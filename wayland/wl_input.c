/*
 * wl_input.c - Translate Plan 9 input events to Wayland
 *
 * Consumes events from the input queue (fed by mouse_thread_func and
 * kbd_thread_func in input.c) and delivers them to Wayland clients
 * via wlroots seat notifications.
 *
 * Keyboard: Plan 9 runes are mapped to Linux keycodes via keymap_lookup().
 * Modifier state is tracked through the focus manager.
 *
 * Mouse: Plan 9 absolute coordinates and button bitmask are translated
 * to Wayland pointer motion, button, and scroll axis events.
 */

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "wl_input.h"
#include "../types.h"
#include "../input/input.h"
#include "../input/clipboard.h"
#include "../multiwin.h"

/* ============== Button Mapping Tables ============== */

/* Mouse button mapping: bitmask -> Linux button code */
static const struct {
    int mask;
    uint32_t button;
} button_map[] = {
    { 1, BTN_LEFT },
    { 2, BTN_MIDDLE },
    { 4, BTN_RIGHT },
};
#define NUM_BUTTONS (sizeof(button_map) / sizeof(button_map[0]))

/* Scroll axis mapping: bitmask -> axis, direction, discrete step */
static const struct {
    int mask;
    enum wl_pointer_axis axis;
    int direction;       /* -1 or +1 */
    int32_t discrete;    /* ±120 per notch (Wayland axis_value120 convention) */
} scroll_map[] = {
    { 8,  WL_POINTER_AXIS_VERTICAL_SCROLL,   -1, -120 },  /* Scroll up */
    { 16, WL_POINTER_AXIS_VERTICAL_SCROLL,    1,  120 },  /* Scroll down */
    { 32, WL_POINTER_AXIS_HORIZONTAL_SCROLL, -1, -120 },  /* Scroll left */
    { 64, WL_POINTER_AXIS_HORIZONTAL_SCROLL,  1,  120 },  /* Scroll right */
};
#define NUM_SCROLLS (sizeof(scroll_map) / sizeof(scroll_map[0]))

/*
 * Scroll source.
 *
 * Plan 9's /dev/mouse delivers scroll as discrete button bits (8/16/32/64)
 * regardless of the physical device. Even trackpad swipes arrive as
 * individual button events. We always report SOURCE_WHEEL with discrete
 * step counts since that's what the events look like by the time they
 * reach us.
 */

/* ============== Keyboard Handling ============== */

void handle_key(struct server *s, struct rio_window *rw, uint32_t rune, int pressed) {
    struct focus_manager *fm = &s->focus;
    
    /* Guard against stale events from a destroyed window (matches handle_mouse) */
    if (rw && !rw->output) return;
    
    /* Handle Escape for popup dismissal (unless keyboard shortcuts are inhibited,
     * e.g. during fullscreen video — let the client handle Escape itself) */
    if (rune == 0x1B && pressed) {
        if (!s->active_kb_inhibitor && focus_popup_dismiss_topmost_grabbed(fm))
            return;
    }
    
    /* Handle modifier keys - use keymapmod() as single source of truth */
    uint32_t mod = keymapmod(rune);
    if (mod) {
        uint32_t current = focus_keyboard_get_modifiers(fm);
        focus_keyboard_set_modifiers(fm, pressed ? (current | mod) : (current & ~mod));
        /* Track per-window so focus_manager_window_died can clean up */
        if (rw) {
            if (pressed) rw->held_modifiers |= mod;
            else         rw->held_modifiers &= ~mod;
        }
        return;
    }
    
    /*
     * Multi-window keyboard focus: keys come from a specific rio window's
     * /dev/kbd thread.  If the current keyboard focus is on a surface that
     * belongs to a different window, switch focus to a mapped toplevel on
     * the source window.  Without this, after multiwin_migrate_toplevel
     * moves focus to a secondary window, typing on the primary rio window
     * sends keys to the wrong toplevel.
     */
    if (rw && s->multi_window && pressed) {
        struct wlr_surface *focused = s->seat->keyboard_state.focused_surface;
        int focus_on_this_window = 0;
        struct toplevel *target_tl = NULL;
        struct toplevel *tl;
        wl_list_for_each(tl, &s->toplevels, link) {
            if (tl->window == rw && tl->mapped) {
                target_tl = tl;
                if (tl->surface == focused) {
                    focus_on_this_window = 1;
                    break;
                }
            }
        }
        if (!focus_on_this_window && target_tl && target_tl->surface) {
            wlr_log(WLR_INFO, "Keyboard focus switch: -> win%d toplevel %p",
                    rw->id, (void*)target_tl->surface);
            focus_keyboard_set(fm, target_tl->surface, FOCUS_REASON_EXPLICIT);
        }
    }
    
    /* Check keyboard focus */
    struct wlr_surface *focused = s->seat->keyboard_state.focused_surface;
    if (!focused) {
        wlr_log(WLR_DEBUG, "No keyboard focus for rune=0x%04x", rune);
        return;
    }
    
    /* Look up key mapping */
    const struct key_map *km = keymap_lookup(rune);
    if (!km) {
        if (rune >= 0x80)
            wlr_log(WLR_ERROR, "No keymap entry for rune=0x%04x", rune);
        return;
    }
    
    wlr_log(WLR_DEBUG, "Key: rune=0x%04x -> keycode=%d shift=%d", 
            rune, km->keycode, km->shift);
    
    uint32_t t = now_ms();
    wlr_seat_set_keyboard(s->seat, &s->virtual_kb);
    
    /* Handle temporary modifiers from keymap */
    uint32_t key_mods = 0;
    if (km->shift) key_mods |= WLR_MODIFIER_SHIFT;
    if (km->ctrl) key_mods |= WLR_MODIFIER_CTRL;
    
    /*
     * Track synthetic modifiers in the source window's held_modifiers.
     *
     * Plan 9 reports a single rune for keys that include a Shift or
     * Ctrl modifier (e.g. 'A' for Shift+a, 0x01 for Ctrl+a).  We
     * simulate the modifier by sending a press before and a release
     * after the key.  If the rio window's kbd thread dies between
     * the synthetic press and the synthetic release (Plan 9 deletes
     * the window mid-keystroke), the release never fires and
     * fm->modifier_state stays stuck with the modifier.
     *
     * Recording these synthetic press/releases against rw->held_modifiers
     * lets focus_manager_window_died (called from multiwin_destroy_window
     * and from migrate_toplevels_to before any migration) clean up the
     * stuck bit when the originating window dies — matching the
     * symmetric path that already runs for direct modifier keys (Kshift,
     * Kctl, ...) handled in the keymapmod() branch above.
     */
    if (key_mods && pressed) {
        uint32_t current = focus_keyboard_get_modifiers(fm);
        focus_keyboard_set_modifiers(fm, current | key_mods);
        if (rw) rw->held_modifiers |= key_mods;
    }
    
    uint32_t state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED 
                             : WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_seat_keyboard_notify_key(s->seat, t, km->keycode, state);
    
    if (key_mods && !pressed) {
        uint32_t current = focus_keyboard_get_modifiers(fm);
        focus_keyboard_set_modifiers(fm, current & ~key_mods);
        if (rw) rw->held_modifiers &= ~key_mods;
    }
    
    /* Schedule frame on all windows — the key may trigger visual
     * changes on whichever window owns the focused surface. */
    struct rio_window *w;
    wl_list_for_each(w, &s->rio_windows, link) {
        w->scene_dirty = 1;
        if (w->output) wlr_output_schedule_frame(w->output);
    }
}

/* ============== Mouse Handling ============== */

/*
 * Send button events for all changed buttons.
 * Uses table-driven approach for cleaner code.
 */
static void send_button_events(struct server *s, uint32_t t, 
                               int buttons, int changed) {
    struct wlr_surface *surface = s->seat->pointer_state.focused_surface;
    if (!surface || !surface->mapped) return;
    
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        if (changed & button_map[i].mask) {
            uint32_t state = (buttons & button_map[i].mask)
                ? WL_POINTER_BUTTON_STATE_PRESSED
                : WL_POINTER_BUTTON_STATE_RELEASED;
            wlr_seat_pointer_notify_button(s->seat, t, button_map[i].button, state);
        }
    }
}

/*
 * Send scroll events for all active scroll buttons.
 */
static void send_scroll_events(struct server *s, uint32_t t,
                               int buttons, int changed) {
    struct focus_manager *fm = &s->focus;
    int scroll_changed = changed & 0x78;
    int scroll_active = buttons & 0x78;
    
    if (!scroll_changed || !scroll_active) return;
    
    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    if (!surface || !surface->mapped) return;
    
    /* Ensure focus is on scroll target */
    struct wlr_surface *current = s->seat->pointer_state.focused_surface;
    if (surface != current) {
        focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_MOTION);
    }
    if (s->seat->pointer_state.focused_surface == surface)
        focus_pointer_motion(fm, sx, sy, t);
    
    /* Send scroll events */
    for (size_t i = 0; i < NUM_SCROLLS; i++) {
        if ((changed & scroll_map[i].mask) && (buttons & scroll_map[i].mask)) {
            wlr_seat_pointer_notify_axis(s->seat, t, scroll_map[i].axis,
                scroll_map[i].direction * 15.0,
                scroll_map[i].discrete,
                WL_POINTER_AXIS_SOURCE_WHEEL,
                WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        }
    }
}

void handle_mouse(struct server *s, struct rio_window *rw, int mx, int my, int buttons) {
    struct focus_manager *fm = &s->focus;
    
    /* Guard against stale events from a destroyed secondary window */
    if (!rw->output) return;
    
    if (rw->id > 0) {
        static int log_count = 0;
        if (++log_count % 100 == 1)
            wlr_log(WLR_DEBUG, "handle_mouse[win%d]: mx=%d my=%d btn=%d", rw->id, mx, my, buttons);
    }
    
    /* Translate to window-local coordinates */
    int local_x = mx - rw->draw.win_minx;
    int local_y = my - rw->draw.win_miny;
    
    /* Clamp to visible window bounds (not padded buffer bounds) */
    int vis_w = rw->visible_width;
    int vis_h = rw->visible_height;
    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    if (local_x >= vis_w) local_x = vis_w - 1;
    if (local_y >= vis_h) local_y = vis_h - 1;
    
    /* Update cursor — convert window-local physical coords to layout logical coords.
     * Each output occupies a region starting at (rw->layout_x, rw->layout_y). */
    double layout_x = (double)local_x / s->scale + rw->layout_x;
    double layout_y = (double)local_y / s->scale + rw->layout_y;
    wlr_cursor_warp_closest(s->cursor, NULL, layout_x, layout_y);
    
    /* Find surface under cursor */
    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    
    uint32_t t = now_ms();
    
    /*
     * Per-window button state tracking.
     *
     * Each rio window has its own /dev/mouse that reports independent
     * button state. Using a global last_buttons caused cross-window
     * interference: events from an empty primary window (buttons=0)
     * would generate phantom releases for buttons held on a secondary.
     */
    int last_buttons = rw->last_buttons;
    int changed = buttons ^ last_buttons;
    bool releasing_all = (last_buttons & 7) && !(buttons & 7);
    
    /* Handle click for focus changes */
    if ((changed & 1) && (buttons & 1) && surface) {
        surface = focus_handle_click(fm, surface, sx, sy, BTN_LEFT);
        if (surface) {
            struct wlr_surface *new_surface = focus_surface_at_cursor(fm, &sx, &sy);
            if (new_surface != surface)
                surface = new_surface;
        }
    }
    
    /* Handle pointer focus and motion */
    if (surface) {
        struct wlr_surface *focused = s->seat->pointer_state.focused_surface;
        if (surface != focused)
            focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_MOTION);
        /*
         * Only send motion when the focused surface matches the surface
         * under cursor.  If focus_pointer_set deferred (button held),
         * the seat's focused surface is still the OLD surface, but sx/sy
         * are surface-local for the NEW surface.  Sending those coords
         * to the old surface makes the cursor jump — e.g. popup-relative
         * coords sent to the toplevel shifts the cursor way up.
         */
        if (s->seat->pointer_state.focused_surface == surface)
            focus_pointer_motion(fm, sx, sy, t);
    } else {
        /*
         * No surface under cursor (gray background area).
         *
         * Release any buttons that were pressed while a surface was
         * focused.  We ALWAYS send the releases to the seat, even if
         * focused_surface is already NULL (e.g. the surface was
         * destroyed while the button was held).  wlr_seat still
         * decrements button_count internally, which prevents
         * BUTTONS_HELD() from getting permanently stuck — without
         * this, all future focus changes are deferred forever and
         * the mouse appears frozen.
         */
        int held = last_buttons & 7;
        if (held) {
            for (size_t i = 0; i < NUM_BUTTONS; i++) {
                if (held & button_map[i].mask) {
                    wlr_seat_pointer_notify_button(s->seat, t,
                        button_map[i].button,
                        WL_POINTER_BUTTON_STATE_RELEASED);
                }
            }
            wlr_seat_pointer_notify_frame(s->seat);
            /* Mark buttons as released so we don't double-release */
            buttons &= ~7;
            changed = buttons ^ last_buttons;
        }
        
        if ((changed & 1) && (buttons & 1) && !focus_popup_stack_empty(fm))
            focus_popup_dismiss_all(fm);
        focus_pointer_set(fm, NULL, 0, 0, FOCUS_REASON_EXPLICIT);
    }
    
    /* Button and scroll events */
    send_button_events(s, t, buttons, changed);
    send_scroll_events(s, t, buttons, changed);
    
    /*
     * Resolve deferred focus AFTER sending button events.
     *
     * focus_pointer_button_released → focus_pointer_recheck checks
     * BUTTONS_HELD (seat->pointer_state.button_count > 0).  The
     * releases must already be sent so button_count is decremented
     * before the check, otherwise the recheck always bails out.
     */
    if (releasing_all)
        focus_pointer_button_released(fm);
    
    rw->last_buttons = buttons & ~0x78;  /* Scroll bits are instantaneous, not holdable */
    wlr_seat_pointer_notify_frame(s->seat);
    
    /* Ensure the target window re-renders after mouse interaction.
     * Without this, mouse events on a secondary window won't trigger
     * visual updates (hover effects, focus changes, etc.) because
     * scene_dirty is only set by client commits. */
    rw->scene_dirty = 1;
    wlr_output_schedule_frame(rw->output);
}

/* ============== Event Queue Handler ============== */

int handle_input_events(int fd, uint32_t mask, void *data) {
    struct server *s = data;
    struct input_event ev;
    char buf[32];
    
    (void)mask;
    
    /* Drain pipe */
    while (read(fd, buf, sizeof(buf)) > 0);
    
    /* If a worker thread signaled shutdown (e.g. primary 9P stream died),
     * terminate the Wayland event loop.  Without this, wl_display_run()
     * keeps the compositor alive as a zombie that accepts Wayland clients
     * but can't render anything. */
    if (!s->running) {
        wl_display_terminate(s->display);
        return 0;
    }
    
    /* Process all queued events */
    while (input_queue_pop(&s->input_queue, &ev)) {
        switch (ev.type) {
        case INPUT_MOUSE:
            handle_mouse(s, ev.window, ev.mouse.x, ev.mouse.y, ev.mouse.buttons);
            break;
        case INPUT_KEY:
            handle_key(s, ev.window, ev.key.rune, ev.key.pressed);
            break;
        case INPUT_WAKEUP:
            {
                struct rio_window *wake_rw = ev.window ? ev.window : s->primary;
                if (wake_rw->output)
                    wlr_output_schedule_frame(wake_rw->output);
            }
            break;
        case INPUT_RIO_WINDOW_DIED:
            /*
             * A rio window's 9P connections broke (typically: user
             * deleted the rio window from the Plan 9 side) and its
             * worker threads all exited.  Hand off to multiwin which
             * either promotes a secondary (if the dead window was
             * primary) or migrates the toplevels off and tears down
             * the dead window (if it was a secondary).
             *
             * Idempotency: the dispatcher walks s->rio_windows to
             * confirm rw is still present.  Stale events whose target
             * was already cleaned up are dropped silently.
             */
            multiwin_handle_window_died(s, ev.window);
            break;
        }
    }
    
    return 0;
}
