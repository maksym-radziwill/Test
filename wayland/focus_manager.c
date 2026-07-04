/*
 * focus_manager.c - Unified focus state machine
 *
 * Manages pointer focus, keyboard focus, and the popup grab stack.
 * All focus transitions go through this module so that deferred focus
 * (during drags), popup grab/ungrab, and surface lifecycle events are
 * handled consistently.
 *
 * See focus_manager.h for API documentation and design overview.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include "types.h"

#define SEAT(fm)         ((fm)->server->seat)
#define CURSOR(fm)       ((fm)->server->cursor)
#define BUTTONS_HELD(fm) (SEAT(fm)->pointer_state.button_count > 0)

void focus_manager_init(struct focus_manager *fm, struct server *server) {
    memset(fm, 0, sizeof(*fm));
    fm->server = server;
    wl_list_init(&fm->popup_stack);
}

void focus_manager_cleanup(struct focus_manager *fm) {
    wlr_log(WLR_INFO, "Focus: %d changes", fm->focus_change_count);
}

/* Walk subsurface parents to find root surface. */
static struct wlr_surface *root_surface(struct wlr_surface *surface) {
    while (surface) {
        struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(surface);
        if (!sub) break;
        surface = sub->parent;
    }
    return surface;
}

/* Find any mapped surface to give focus to, skipping `skip`. */
static struct wlr_surface *fallback_surface(struct focus_manager *fm,
                                             struct wlr_surface *skip) {
    struct popup_data *pd;
    wl_list_for_each(pd, &fm->popup_stack, link) {
        if (pd->mapped && pd->surface != skip)
            return pd->surface;
    }
    struct toplevel *tl;
    wl_list_for_each(tl, &fm->server->toplevels, link) {
        if (tl->mapped && tl->surface != skip)
            return tl->surface;
    }
    return NULL;
}

/* ============== Surface Queries ============== */

struct wlr_surface *focus_surface_at_cursor(struct focus_manager *fm,
                                            double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(
        &fm->server->scene->tree.node, CURSOR(fm)->x, CURSOR(fm)->y, sx, sy);

    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
        if (ss && ss->surface && ss->surface->mapped)
            return ss->surface;
    }

    /*
     * No buffer node under the cursor — gray background area, the gap
     * between outputs in multi-window mode, or a non-buffer scene node.
     *
     * Earlier this function fell back to the first mapped toplevel and
     * stamped sx/sy with CURSOR(fm)->x and CURSOR(fm)->y, which are in
     * LAYOUT space.  Callers feed sx/sy directly into
     * wlr_seat_pointer_notify_enter as surface-local coordinates.  For
     * the primary window (layout_x = 0) layout space and surface-local
     * space coincide so the bug was invisible; on any secondary window
     * (layout_x = id * 100000, see multiwin.c) the receiving surface
     * was told the cursor was ~100000 pixels outside itself.  This
     * surfaced as popups and dropdowns inside secondary windows
     * behaving as though the cursor was nowhere near them — items
     * didn't highlight, hover targeting was off, and the only fix was
     * to dismiss and re-open so a fresh hit test could correct the
     * pointer position.
     *
     * The right behavior is to report "no surface here" and let the
     * caller decide.  Every caller already handles a NULL return.
     */
    *sx = 0;
    *sy = 0;
    return NULL;
}

struct toplevel *focus_toplevel_from_surface(struct focus_manager *fm,
                                             struct wlr_surface *surface) {
    if (!surface) return NULL;
    struct wlr_surface *root = root_surface(surface);
    struct toplevel *tl;
    wl_list_for_each(tl, &fm->server->toplevels, link) {
        if (tl->surface == root) return tl;
    }
    return NULL;
}

struct toplevel *focus_toplevel_at_cursor(struct focus_manager *fm) {
    double sx, sy;
    struct wlr_surface *s = focus_surface_at_cursor(fm, &sx, &sy);
    return s ? focus_toplevel_from_surface(fm, s) : NULL;
}

/* ============== Pointer Focus ============== */

void focus_pointer_set(struct focus_manager *fm, struct wlr_surface *surface,
                       double sx, double sy, enum focus_reason reason) {
    if (surface == SEAT(fm)->pointer_state.focused_surface)
        return;

    /* Defer if dragging (except forced cases) */
    if (BUTTONS_HELD(fm) && reason != FOCUS_REASON_EXPLICIT &&
        reason != FOCUS_REASON_SURFACE_DESTROY) {
        fm->pointer_focus_deferred = true;
        fm->deferred_pointer_target = surface;
        fm->deferred_sx = sx;
        fm->deferred_sy = sy;
        return;
    }

    fm->focus_change_count++;
    fm->pointer_focus = surface;

    if (surface)
        wlr_seat_pointer_notify_enter(SEAT(fm), surface, sx, sy);
    else
        wlr_seat_pointer_notify_clear_focus(SEAT(fm));
    wlr_seat_pointer_notify_frame(SEAT(fm));
}

void focus_pointer_motion(struct focus_manager *fm, double sx, double sy,
                          uint32_t time_msec) {
    wlr_seat_pointer_notify_motion(SEAT(fm), time_msec, sx, sy);
}

/*
 * Re-evaluate pointer focus after geometry changes.
 * If buttons are held, do nothing — focus_pointer_set will defer.
 * Otherwise, always do a fresh hit test. This fixes the old bug where
 * a stale/NULL deferred_pointer_target caused the hit test to be skipped.
 */
void focus_pointer_recheck(struct focus_manager *fm) {
    if (BUTTONS_HELD(fm))
        return;

    fm->pointer_focus_deferred = false;
    fm->deferred_pointer_target = NULL;

    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    if (surface != SEAT(fm)->pointer_state.focused_surface)
        focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_MOTION);
}

void focus_pointer_button_pressed(struct focus_manager *fm) {
    (void)fm;
}

void focus_pointer_button_released(struct focus_manager *fm) {
    if (fm->pointer_focus_deferred && !BUTTONS_HELD(fm))
        focus_pointer_recheck(fm);
}

/* ============== Keyboard Focus ============== */

void focus_keyboard_set(struct focus_manager *fm, struct wlr_surface *surface,
                        enum focus_reason reason) {
    (void)reason;
    if (surface == SEAT(fm)->keyboard_state.focused_surface)
        return;

    fm->keyboard_focus = surface;
    fm->focus_change_count++;

    if (surface) {
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(SEAT(fm));
        if (kb)
            wlr_seat_keyboard_notify_enter(SEAT(fm), surface,
                kb->keycodes, kb->num_keycodes, &kb->modifiers);
        /* Re-send actual modifier state — kb->modifiers is the virtual
         * keyboard's state (always zero), not the real modifier state
         * tracked in fm->modifier_state. Without this, entering a surface
         * clears any held Ctrl/Shift/Alt from the client's perspective. */
        if (fm->modifier_state) {
            struct wlr_keyboard_modifiers mods = { .depressed = fm->modifier_state };
            wlr_seat_keyboard_notify_modifiers(SEAT(fm), &mods);
        }
    } else {
        wlr_seat_keyboard_notify_clear_focus(SEAT(fm));
    }
}

void focus_keyboard_set_modifiers(struct focus_manager *fm, uint32_t modifiers) {
    fm->modifier_state = modifiers;
    struct wlr_keyboard_modifiers mods = { .depressed = modifiers };
    wlr_seat_keyboard_notify_modifiers(SEAT(fm), &mods);
}

uint32_t focus_keyboard_get_modifiers(struct focus_manager *fm) {
    return fm->modifier_state;
}

/* ============== Toplevel Focus ============== */

void focus_toplevel(struct focus_manager *fm, struct toplevel *tl,
                    enum focus_reason reason) {
    if (!tl || !tl->xdg) return;
    if (tl->surface == SEAT(fm)->keyboard_state.focused_surface) return;

    /* Deactivate previous */
    struct toplevel *prev = focus_get_focused_toplevel(fm);
    if (prev && prev->xdg)
        wlr_xdg_toplevel_set_activated(prev->xdg, false);

    /* Raise, reorder, activate */
    wlr_scene_node_raise_to_top(&tl->scene_tree->node);
    wl_list_remove(&tl->link);
    wl_list_insert(&fm->server->toplevels, &tl->link);
    wlr_xdg_toplevel_set_activated(tl->xdg, true);
    focus_keyboard_set(fm, tl->surface, reason);
}

struct toplevel *focus_get_focused_toplevel(struct focus_manager *fm) {
    struct wlr_surface *s = SEAT(fm)->keyboard_state.focused_surface;
    return s ? focus_toplevel_from_surface(fm, s) : NULL;
}

/* ============== Popup Management ============== */

void focus_popup_register(struct focus_manager *fm, struct popup_data *pd) {
    wl_list_insert(&fm->popup_stack, &pd->link);
}

void focus_popup_mapped(struct focus_manager *fm, struct popup_data *pd) {
    if (pd->has_grab)
        focus_keyboard_set(fm, pd->surface, FOCUS_REASON_POPUP_GRAB);
    focus_pointer_recheck(fm);
}

void focus_popup_unmapped(struct focus_manager *fm, struct popup_data *pd) {
    if (SEAT(fm)->pointer_state.focused_surface != pd->surface)
        return;

    /*
     * Re-evaluate pointer focus from the current cursor position so the
     * receiving surface gets surface-local sx/sy from the scene-graph
     * hit test rather than raw layout coordinates.  Sending CURSOR->x/y
     * (layout space) as sx/sy was correct only for the primary window
     * at layout_x = 0; on secondaries it shifted the cursor ~100000
     * pixels off the receiving surface.
     */
    double sx = 0, sy = 0;
    struct wlr_surface *new_focus = focus_surface_at_cursor(fm, &sx, &sy);
    focus_pointer_set(fm, new_focus, sx, sy, FOCUS_REASON_SURFACE_UNMAP);
}

/*
 * Popup destroyed — restore focus.
 * Uses focus_pointer_set instead of direct wlr_seat calls so that
 * fm->pointer_focus stays in sync and deferral logic is respected.
 */
void focus_popup_unregister(struct focus_manager *fm, struct popup_data *pd) {
    bool had_grab = pd->has_grab;
    struct wlr_surface *pd_surface = pd->surface;

    wl_list_remove(&pd->link);
    wl_list_init(&pd->link);

    /* Clear pointer focus for destroyed popup */
    focus_pointer_set(fm, NULL, 0, 0, FOCUS_REASON_EXPLICIT);

    /*
     * Pointer focus: re-evaluate from the current cursor position.  The
     * scene-graph hit test gives us correct surface-local sx/sy for
     * whatever is now under the cursor (often the parent menu in a
     * submenu-dismiss case).  This previously used CURSOR(fm)->x and
     * CURSOR(fm)->y as if they were surface-local, which was wrong on
     * any secondary window — see focus_surface_at_cursor for the full
     * explanation.
     */
    double sx = 0, sy = 0;
    struct wlr_surface *new_pointer = focus_surface_at_cursor(fm, &sx, &sy);
    if (new_pointer)
        focus_pointer_set(fm, new_pointer, sx, sy, FOCUS_REASON_POPUP_DISMISS);

    /*
     * Keyboard focus: hand the grab off to a fallback surface (parent
     * popup or a toplevel).  Independent of cursor position, so still
     * uses fallback_surface rather than the hit test.
     */
    if (had_grab) {
        struct wlr_surface *target = fallback_surface(fm, pd_surface);
        if (target)
            focus_keyboard_set(fm, target, FOCUS_REASON_POPUP_DISMISS);
    }

    /* Re-activate toplevel if popup stack empty */
    if (wl_list_empty(&fm->popup_stack)) {
        struct toplevel *tl;
        wl_list_for_each(tl, &fm->server->toplevels, link) {
            if (tl->mapped && tl->xdg) {
                wlr_xdg_toplevel_set_activated(tl->xdg, true);
                if (!had_grab)
                    focus_keyboard_set(fm, tl->surface, FOCUS_REASON_POPUP_DISMISS);
                break;
            }
        }
    }
}

struct popup_data *focus_popup_get_topmost(struct focus_manager *fm) {
    if (wl_list_empty(&fm->popup_stack)) return NULL;
    struct popup_data *pd;
    return wl_container_of(fm->popup_stack.next, pd, link);
}

struct popup_data *focus_popup_from_surface(struct focus_manager *fm,
                                             struct wlr_surface *surface) {
    struct wlr_surface *root = root_surface(surface);
    struct popup_data *pd;
    wl_list_for_each(pd, &fm->popup_stack, link) {
        if (pd->surface == surface || pd->surface == root)
            return pd;
    }
    return NULL;
}

void focus_popup_dismiss_all(struct focus_manager *fm) {
    struct popup_data *pd, *tmp;
    wl_list_for_each_safe(pd, tmp, &fm->popup_stack, link) {
        wlr_xdg_popup_destroy(pd->popup);
    }
}

bool focus_popup_dismiss_topmost_grabbed(struct focus_manager *fm) {
    if (wl_list_empty(&fm->popup_stack)) return false;
    struct popup_data *pd;
    pd = wl_container_of(fm->popup_stack.next, pd, link);
    if (!pd->has_grab) return false;
    wlr_xdg_popup_destroy(pd->popup);
    return true;
}

bool focus_popup_stack_empty(struct focus_manager *fm) {
    return wl_list_empty(&fm->popup_stack);
}

/* ============== Surface Lifecycle ============== */

void focus_on_surface_map(struct focus_manager *fm, struct wlr_surface *surface,
                          bool is_toplevel) {
    if (is_toplevel) {
        struct toplevel *tl = focus_toplevel_from_surface(fm, surface);
        if (tl) focus_toplevel(fm, tl, FOCUS_REASON_SURFACE_MAP);
    }

    double sx, sy;
    struct wlr_surface *under = focus_surface_at_cursor(fm, &sx, &sy);
    if (under == surface)
        focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_SURFACE_MAP);
}

void focus_on_surface_unmap(struct focus_manager *fm, struct wlr_surface *surface) {
    struct wlr_surface *ptr_focused = SEAT(fm)->pointer_state.focused_surface;
    struct wlr_surface *kbd_focused = SEAT(fm)->keyboard_state.focused_surface;

    if (ptr_focused != surface && kbd_focused != surface)
        return;

    if (ptr_focused == surface) {
        /*
         * Pointer focus: re-evaluate from the current cursor position
         * via a fresh scene-graph hit test.  This gives the receiving
         * surface correct surface-local sx/sy.  Sending CURSOR(fm)->x
         * and CURSOR(fm)->y here was wrong for any non-primary window
         * (layout_x = id * 100000) — see focus_surface_at_cursor for
         * the full rationale.
         */
        double sx = 0, sy = 0;
        struct wlr_surface *new_pointer = focus_surface_at_cursor(fm, &sx, &sy);
        focus_pointer_set(fm, new_pointer, sx, sy, FOCUS_REASON_SURFACE_UNMAP);
    }

    if (kbd_focused == surface) {
        /* Keyboard focus is not tied to cursor position — keep using
         * fallback_surface to find a sensible new target. */
        struct wlr_surface *target = fallback_surface(fm, surface);
        focus_keyboard_set(fm, target, FOCUS_REASON_SURFACE_UNMAP);
    }
}

void focus_on_surface_destroy(struct focus_manager *fm, struct wlr_surface *surface) {
    focus_on_surface_unmap(fm, surface);

    if (fm->deferred_pointer_target == surface) {
        fm->deferred_pointer_target = NULL;
        fm->pointer_focus_deferred = false;
    }
}

/* ============== Click Handling ============== */

struct wlr_surface *focus_handle_click(struct focus_manager *fm,
                                        struct wlr_surface *clicked,
                                        double sx, double sy,
                                        uint32_t button) {
    (void)button;

    /* Click on popup — keep it focused */
    if (focus_popup_from_surface(fm, clicked))
        return clicked;

    /* Click outside popup stack — dismiss all */
    if (!wl_list_empty(&fm->popup_stack)) {
        focus_popup_dismiss_all(fm);
        struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
        if (surface)
            focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_CLICK);
        return surface;
    }

    /* Click on toplevel */
    struct toplevel *tl = focus_toplevel_from_surface(fm, clicked);
    if (tl) focus_toplevel(fm, tl, FOCUS_REASON_POINTER_CLICK);

    return clicked;
}

/* ============== Window Lifecycle ============== */

/*
 * Button codes for seat cleanup.  Must match the mapping in wl_input.c.
 */
static const struct { int mask; uint32_t button; } window_died_buttons[] = {
    { 1, BTN_LEFT }, { 2, BTN_MIDDLE }, { 4, BTN_RIGHT },
};

void focus_manager_window_died(struct focus_manager *fm, struct rio_window *rw) {
    /*
     * Remove this window's modifier contributions from global state.
     *
     * Each window's kbd thread independently sets bits in the global
     * modifier_state via handle_key → focus_keyboard_set_modifiers.
     * rw->held_modifiers tracks which bits came from this window.
     * Clearing just those bits leaves other windows' modifiers intact.
     */
    if (rw->held_modifiers) {
        uint32_t current = fm->modifier_state;
        uint32_t cleaned = current & ~rw->held_modifiers;
        wlr_log(WLR_INFO, "focus: window %d died, modifiers 0x%x -> 0x%x "
                "(removing 0x%x)", rw->id, current, cleaned, rw->held_modifiers);
        focus_keyboard_set_modifiers(fm, cleaned);
        rw->held_modifiers = 0;
    }

    /*
     * Release any buttons the seat thinks are held from this window.
     *
     * rw->last_buttons tracks the button state as last dispatched by
     * handle_mouse.  The seat's button_count was incremented for each
     * PRESSED and never decremented because no RELEASED will arrive.
     * Send synthetic releases to bring button_count back down.
     */
    int held = rw->last_buttons & 7;
    if (held) {
        uint32_t t = now_ms();
        for (int i = 0; i < 3; i++) {
            if (held & window_died_buttons[i].mask) {
                wlr_seat_pointer_notify_button(SEAT(fm), t,
                    window_died_buttons[i].button,
                    WL_POINTER_BUTTON_STATE_RELEASED);
            }
        }
        wlr_seat_pointer_notify_frame(SEAT(fm));
        rw->last_buttons = 0;
        wlr_log(WLR_INFO, "focus: window %d died, released stuck buttons 0x%x",
                rw->id, held);
    }

    /*
     * Clear deferred pointer state if it was targeting a surface on
     * this window.  The surface destroy path normally handles this,
     * but if the surface was already destroyed before we get here
     * (race between toplevel_destroy and stale event dispatch),
     * the pointer could reference freed memory.
     */
    if (fm->pointer_focus_deferred && fm->deferred_pointer_target) {
        struct toplevel *tl = focus_toplevel_from_surface(
            fm, fm->deferred_pointer_target);
        if (tl && tl->window == rw) {
            fm->deferred_pointer_target = NULL;
            fm->pointer_focus_deferred = false;
            wlr_log(WLR_INFO, "focus: cleared deferred pointer for window %d",
                    rw->id);
        }
    }

    /* Recheck focus so surviving windows pick up pointer correctly */
    focus_pointer_recheck(fm);
}
