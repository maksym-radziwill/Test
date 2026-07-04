/*
 * toplevel.c - XDG toplevel and subsurface lifecycle
 *
 * Handles creation, commit, and destruction of toplevel windows and
 * their subsurfaces. Coordinates with focus_manager for focus transitions
 * on map/unmap/destroy events.
 *
 * See toplevel.h for lifecycle description and subsurface tracking design.
 */

#include <stdlib.h>
#include <stdio.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include "toplevel.h"
#include "../multiwin.h"
#include "types.h"
#include "draw/draw_helpers.h"
#include "draw/draw.h"
#include "p9/p9.h"

/* Forward declaration */
static void check_new_subsurfaces(struct toplevel *tl);

/* ============== Subsurface Iteration Macro ============== */

/*
 * Iterate over both below and above subsurface lists.
 * Usage: FOR_EACH_SUBSURFACE(surface, sub) { ... }
 */
#define FOR_EACH_SUBSURFACE(surface, sub) \
    for (int _list_idx = 0; _list_idx < 2; _list_idx++) \
        wl_list_for_each(sub, \
            (_list_idx == 0) ? &(surface)->current.subsurfaces_below \
                             : &(surface)->current.subsurfaces_above, \
            current.link)

/* ============== Subsurface Tracking ============== */

static void subsurface_commit(struct wl_listener *l, void *d) {
    struct subsurface_track *st = wl_container_of(l, st, commit);
    (void)d;
    
    struct wlr_surface *surface = st->subsurface->surface;
    bool has_buffer = wlr_surface_has_buffer(surface);
    
    if (has_buffer && !st->mapped) {
        st->mapped = true;
        wlr_log(WLR_INFO, "Subsurface mapped: %p", surface);
        focus_pointer_recheck(&st->server->focus);
    } else if (!has_buffer && st->mapped) {
        st->mapped = false;
        wlr_log(WLR_INFO, "Subsurface unmapped: %p", surface);
        focus_pointer_recheck(&st->server->focus);
    }
    
    /* Match the order used by toplevel_commit / popup_commit: set the
     * dirty flag BEFORE scheduling the frame.  schedule_frame just adds
     * an idle source (it never re-enters output_frame synchronously),
     * but consistent ordering across commit handlers keeps the contract
     * obvious: by the time output_frame actually runs, scene_dirty is
     * already 1. */
    st->toplevel->window->scene_dirty = 1;
    wlr_output_schedule_frame(st->toplevel->window->output);
}

static void subsurface_destroy(struct wl_listener *l, void *d) {
    struct subsurface_track *st = wl_container_of(l, st, destroy);
    (void)d;
    
    /*
     * Mark the owning rio window dirty so the area where this subsurface
     * was rendered gets re-painted.  Same reasoning as popup_destroy:
     * scene-graph removals don't go through commit, so output_frame's
     * scene_dirty gate would otherwise skip the next render.  Force a
     * full frame too, so the send thread doesn't delta-encode against
     * a prev_framebuf that still contains the now-destroyed subsurface's
     * pixels.
     */
    if (st->toplevel && st->toplevel->window) {
        struct rio_window *rw = st->toplevel->window;
        if (rw->output) {
            rw->scene_dirty = 1;
            rw->force_full_frame = 1;
            wlr_output_schedule_frame(rw->output);
        }
    }
    
    wl_list_remove(&st->destroy.link);
    wl_list_remove(&st->commit.link);
    wl_list_remove(&st->link);
    free(st);
}

static bool is_subsurface_tracked(struct toplevel *tl, struct wlr_subsurface *sub) {
    struct subsurface_track *st;
    wl_list_for_each(st, &tl->subsurfaces, link) {
        if (st->subsurface == sub) return true;
    }
    return false;
}

static void track_subsurface(struct toplevel *tl, struct wlr_subsurface *sub) {
    wlr_log(WLR_INFO, "New subsurface: parent=%p surface=%p", sub->parent, sub->surface);
    
    struct subsurface_track *st = calloc(1, sizeof(*st));
    if (!st) return;
    
    st->subsurface = sub;
    st->server = tl->server;
    st->toplevel = tl;
    st->mapped = false;
    
    st->destroy.notify = subsurface_destroy;
    wl_signal_add(&sub->events.destroy, &st->destroy);
    
    st->commit.notify = subsurface_commit;
    wl_signal_add(&sub->surface->events.commit, &st->commit);
    
    wl_list_insert(&tl->subsurfaces, &st->link);
    focus_pointer_recheck(&tl->server->focus);
}

static void check_new_subsurfaces(struct toplevel *tl) {
    struct wlr_surface *surface = tl->xdg->base->surface;
    struct wlr_subsurface *sub;
    
    FOR_EACH_SUBSURFACE(surface, sub) {
        if (!is_subsurface_tracked(tl, sub)) {
            track_subsurface(tl, sub);
        }
    }
}

/* ============== Toplevel Handlers ============== */

static void toplevel_request_fullscreen(struct wl_listener *l, void *d) {
    struct toplevel *tl = wl_container_of(l, tl, request_fullscreen);
    (void)d;
    
    if (!tl->xdg->base->initialized) return;
    
    /* Already filling the whole window — just acknowledge the state change
     * so the Fullscreen API promise resolves in the browser. */
    wlr_xdg_toplevel_set_fullscreen(tl->xdg, tl->xdg->requested.fullscreen);
    wlr_xdg_surface_schedule_configure(tl->xdg->base);
    wlr_log(WLR_INFO, "Fullscreen %s",
            tl->xdg->requested.fullscreen ? "granted" : "released");
}

static void toplevel_request_maximize(struct wl_listener *l, void *d) {
    struct toplevel *tl = wl_container_of(l, tl, request_maximize);
    (void)d;
    
    if (!tl->xdg->base->initialized) return;
    
    /* Already maximized — acknowledge so client state stays in sync. */
    wlr_xdg_toplevel_set_maximized(tl->xdg, true);
    wlr_xdg_surface_schedule_configure(tl->xdg->base);
    wlr_log(WLR_INFO, "Maximize acknowledged");
}

static void toplevel_request_minimize(struct wl_listener *l, void *d) {
    struct toplevel *tl = wl_container_of(l, tl, request_minimize);
    (void)d;
    
    if (!tl->xdg || !tl->xdg->base->initialized) return;
    
    /* Headless compositor has no taskbar — minimize is unrecoverable.
     * Re-assert activated + maximized state and schedule a configure
     * so the client knows we refused and keeps rendering. Without this,
     * clients (Firefox, Chromium, etc.) throttle frame commits after
     * calling set_minimized, causing a visible hang until something
     * else (e.g., resize) triggers a new configure. */
    wlr_xdg_toplevel_set_activated(tl->xdg, true);
    wlr_xdg_toplevel_set_maximized(tl->xdg, true);
    wlr_xdg_surface_schedule_configure(tl->xdg->base);
    wlr_log(WLR_INFO, "Minimize request denied — re-asserted active state");
}

/*
 * Handle xdg_toplevel destruction.
 *
 * wlroots destroys the xdg_toplevel BEFORE the xdg_surface, and asserts
 * that all toplevel event listener lists are empty. Our main
 * toplevel_destroy listens on xdg_surface destroy (too late). This
 * handler fires on xdg_toplevel destroy to remove toplevel-specific
 * listeners before the assertion.
 */
static void toplevel_xdg_destroy(struct wl_listener *l, void *d) {
    struct toplevel *tl = wl_container_of(l, tl, xdg_destroy);
    (void)d;
    
    wl_list_remove(&tl->request_fullscreen.link);
    wl_list_remove(&tl->request_maximize.link);
    wl_list_remove(&tl->request_minimize.link);
    wl_list_remove(&tl->xdg_destroy.link);
    tl->xdg = NULL;
}

static void toplevel_commit(struct wl_listener *l, void *d) {
    struct toplevel *tl = wl_container_of(l, tl, commit);
    struct server *s = tl->server;
    struct rio_window *rw = tl->window;
    (void)d;
    
    if (!tl->xdg) return;
    
    struct wlr_xdg_surface *xdg_surface = tl->xdg->base;
    struct wlr_surface *surface = xdg_surface->surface;
    
    if (xdg_surface->initial_commit) {
        int logical_w = focus_phys_to_logical(rw->visible_width, s->scale);
        int logical_h = focus_phys_to_logical(rw->visible_height, s->scale);
        
        wlr_xdg_toplevel_set_size(tl->xdg, logical_w, logical_h);
        wlr_xdg_toplevel_set_maximized(tl->xdg, true);
        wlr_xdg_toplevel_set_activated(tl->xdg, true);
        wlr_xdg_surface_schedule_configure(xdg_surface);
        
        tl->configured = true;
        wlr_log(WLR_INFO, "Initial commit: scheduled configure %dx%d",
                logical_w, logical_h);
        return;
    }
    
    if (!surface->mapped) return;
    
    tl->commit_count++;
    bool has_buffer = wlr_surface_has_buffer(surface);
    
    /* Track map/unmap state changes */
    if (has_buffer && !tl->mapped) {
        tl->mapped = true;
        wlr_log(WLR_INFO, "Toplevel MAPPED!");
        
        focus_on_surface_map(&s->focus, surface, true);
        
        /* Multi-window: if another toplevel is already mapped,
         * request a new rio window for this one.
         * If the script has exited (stdin closed), the NEW_WINDOW
         * message goes to stdout anyway — if nobody responds with
         * READY, the toplevel stays on the primary window (graceful
         * fallback).  SIGPIPE is already ignored. */
        if (s->multi_window && tl->pending_window_id == 0) {
            int mapped_count = 0;
            struct toplevel *other;
            wl_list_for_each(other, &s->toplevels, link) {
                if (other->mapped && other != tl) mapped_count++;
            }
            if (mapped_count > 0) {
                multiwin_request_window(s, tl);
            }
        }
        
    } else if (!has_buffer && tl->mapped) {
        tl->mapped = false;
        focus_on_surface_unmap(&s->focus, surface);
    }
    
    check_new_subsurfaces(tl);
    focus_pointer_recheck(&s->focus);
    rw->scene_dirty = 1;
    wlr_output_schedule_frame(rw->output);
}

static void toplevel_destroy(struct wl_listener *l, void *d) {
    struct toplevel *tl = wl_container_of(l, tl, destroy);
    struct server *s = tl->server;
    struct rio_window *rw = tl->window;
    (void)d;
    
    wlr_log(WLR_INFO, "Toplevel destroyed: surface=%p", (void*)tl->surface);
    
    /*
     * Mark the owning rio window dirty BEFORE doing anything else.
     * If rw survives this destroy (other toplevels still on it), we
     * need a re-render to clear the area the toplevel occupied.
     * If rw gets torn down later in this function the flag is moot —
     * harmless either way.  See popup_destroy for the full rationale,
     * including why force_full_frame is needed alongside scene_dirty.
     */
    if (rw && rw->output) {
        rw->scene_dirty = 1;
        rw->force_full_frame = 1;
        wlr_output_schedule_frame(rw->output);
    }
    
    focus_on_surface_destroy(&s->focus, tl->surface);
    
    /* Notify script and prepare to destroy secondary window */
    struct rio_window *secondary_to_destroy = NULL;
    if (s->multi_window && rw != s->primary) {
        multiwin_close_window(s, rw);
        secondary_to_destroy = rw;
    }
    
    /* Clean up subsurface tracking */
    struct subsurface_track *st, *tmp;
    wl_list_for_each_safe(st, tmp, &tl->subsurfaces, link) {
        wl_list_remove(&st->destroy.link);
        wl_list_remove(&st->commit.link);
        wl_list_remove(&st->link);
        free(st);
    }
    
    wl_list_remove(&tl->commit.link);
    wl_list_remove(&tl->destroy.link);
    
    /* xdg_destroy handler may have already removed these; if xdg is
     * still set it means the toplevel outlived its xdg_toplevel destroy
     * signal (shouldn't happen, but be safe). */
    if (tl->xdg) {
        wl_list_remove(&tl->request_fullscreen.link);
        wl_list_remove(&tl->request_maximize.link);
        wl_list_remove(&tl->request_minimize.link);
        wl_list_remove(&tl->xdg_destroy.link);
    }
    
    wl_list_remove(&tl->link);
    free(tl);
    
    /* In multi-window mode, close the primary rio window when it loses
     * its last toplevel — same as secondary windows.
     *
     * If a secondary survives, delete primary and promote the secondary
     * (existing behavior).  If primary is the only window left, the
     * compositor has nothing to show — terminate the display loop so
     * main() runs the existing cleanup path and exits.  ssh returns to
     * the script; the script's `while(read)` loop ends; the script's
     * sigexit handler runs; rc takes back the launching window.  The
     * Wayland socket goes away, but that's fine: there's no surviving
     * rio window for any new client to render into anyway.
     *
     * (Older versions kept the primary alive showing a gray background
     * so subsequent clients could attach, but in -W mode every new
     * toplevel needs a NEW_WINDOW round-trip with the script — and the
     * script is dead by the time we get here.  So holding the gray
     * window open buys nothing and confuses the user.)
     */
    if (s->multi_window && rw == s->primary) {
        int primary_has_toplevels = 0;
        struct toplevel *check;
        wl_list_for_each(check, &s->toplevels, link) {
            if (check->window == s->primary) {
                primary_has_toplevels = 1;
                break;
            }
        }
        if (!primary_has_toplevels) {
            if (has_other_live_windows(s, s->primary)) {
                /*
                 * Was this primary the launching rio window?
                 *
                 * The original primary created in main.c has id == 0;
                 * any rio window that became primary later via promote
                 * has a non-zero id (assigned by ++s->next_window_id
                 * in multiwin_request_window).  Only the id-0 case
                 * corresponds to the Plan 9 window from which p9wl was
                 * launched — that window pre-existed p9wl and the user
                 * expects rc to take it back when we detach.  We must
                 * NOT tell rio to delete it.
                 *
                 * Just promote synchronously: swap s->primary to a live
                 * secondary, migrate any pending toplevels, then call
                 * multiwin_destroy_window on the old primary, which
                 * shuts down our 9P sockets and joins the kbd/mouse/
                 * send threads.  The rio window itself stays alive on
                 * the Plan 9 side; rio will redisplay cons content on
                 * the next interaction with that window.
                 */
                focus_keyboard_set_modifiers(&s->focus, 0);
                if (s->primary->id == 0) {
                    wlr_log(WLR_INFO, "Primary empty, it's the launcher "
                            "(id=0) — detaching without closing rio window");
                    multiwin_promote_primary(s);
                } else {
                    wlr_log(WLR_INFO, "Primary window empty, deleting rio "
                            "window (secondary will be promoted)");
                    delete_rio_window(&s->primary->p9_wctl);
                    /* Use the current primary's id, not a hardcoded 0.
                     * After multiwin_promote_primary() runs, s->primary
                     * may be a formerly-secondary window with its
                     * original non-zero id. */
                    fprintf(stdout, "CLOSE_WINDOW id=%d\n", s->primary->id);
                    fflush(stdout);
                }
            } else {
                wlr_log(WLR_INFO, "Last toplevel destroyed in multi-window mode "
                        "(primary is the only rio window left) — terminating");
                focus_keyboard_set_modifiers(&s->focus, 0);
                /*
                 * If the surviving primary is a script-created window
                 * (id != 0, i.e. it became primary via a previous
                 * promotion from a NEW_WINDOW-spawned secondary), tell
                 * rio to close it so the rio window goes away on
                 * termination.  Otherwise the window lingers showing
                 * 'aux/listen1' output once exportfs exits — the script
                 * didn't put a wrapper around listen1 to close the
                 * window itself, so we have to do it.
                 *
                 * If primary is the launcher (id == 0), DON'T send
                 * delete: that window pre-existed p9wl, the user wants
                 * rc to take it back.  This branch matches the
                 * symmetric detach-from-launcher case above.
                 */
                if (s->primary->id != 0) {
                    delete_rio_window(&s->primary->p9_wctl);
                    fprintf(stdout, "CLOSE_WINDOW id=%d\n", s->primary->id);
                    fflush(stdout);
                }
                s->running = 0;
                wl_display_terminate(s->display);
            }
        }
    }
    
    /* Exit when last toplevel is destroyed — single-window mode only.
     *
     * In multi-window mode (-W), the compositor stays alive so new
     * Wayland clients can connect and trigger NEW_WINDOW requests.
     * The user sees a gray background until a new app opens.
     *
     * In single-window mode, the compositor is useless without a
     * toplevel, so shut down cleanly.  We just signal the event loop
     * to exit; main() runs the existing cleanup path on the way out,
     * which avoids both the abrupt exit(0) (skipped destructors,
     * leaked sockets) and the ad-hoc thread-join sequence here. */
    if (s->had_toplevel && wl_list_empty(&s->toplevels) && !s->multi_window) {
        wlr_log(WLR_INFO, "Last toplevel destroyed - initiating shutdown");
        s->running = 0;
        wl_display_terminate(s->display);
    }
    
    /* Destroy secondary window after toplevel is fully cleaned up.
     * This tears down threads, 9P connections, scene nodes, and output.
     * Happens after the exit check so we don't bother if shutting down. */
    if (secondary_to_destroy) {
        multiwin_destroy_window(s, secondary_to_destroy);
    }
}

void new_toplevel(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg = d;
    
    wlr_log(WLR_INFO, "New XDG toplevel created");
    
    s->has_toplevel = 1;
    s->had_toplevel = 1;
    
    struct toplevel *tl = calloc(1, sizeof(*tl));
    if (!tl) {
        wlr_log(WLR_ERROR, "Failed to allocate toplevel");
        return;
    }
    
    tl->xdg = xdg;
    tl->server = s;
    tl->window = s->primary;
    tl->surface = xdg->base->surface;

    /* Parent under the window's per-output subtree for isolation.
     * Position (0,0) is local to the subtree, which itself is placed
     * at the window's layout coordinates. */
    struct wlr_scene_tree *parent = s->primary->toplevel_tree
                                    ? s->primary->toplevel_tree
                                    : &s->scene->tree;
    tl->scene_tree = wlr_scene_xdg_surface_create(parent, xdg->base);
    
    if (!tl->scene_tree) {
        wlr_log(WLR_ERROR, "Failed to create scene tree");
        free(tl);
        return;
    }
    
    xdg->base->data = tl->scene_tree;
    tl->scene_tree->node.data = tl;
    wlr_scene_node_set_position(&tl->scene_tree->node, 0, 0);
    
    wl_list_init(&tl->subsurfaces);
    wl_list_insert(&s->toplevels, &tl->link);
    
    tl->commit.notify = toplevel_commit;
    wl_signal_add(&xdg->base->surface->events.commit, &tl->commit);
    
    tl->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg->base->events.destroy, &tl->destroy);
    
    tl->xdg_destroy.notify = toplevel_xdg_destroy;
    wl_signal_add(&xdg->events.destroy, &tl->xdg_destroy);
    
    tl->request_fullscreen.notify = toplevel_request_fullscreen;
    wl_signal_add(&xdg->events.request_fullscreen, &tl->request_fullscreen);
    
    tl->request_maximize.notify = toplevel_request_maximize;
    wl_signal_add(&xdg->events.request_maximize, &tl->request_maximize);
    
    tl->request_minimize.notify = toplevel_request_minimize;
    wl_signal_add(&xdg->events.request_minimize, &tl->request_minimize);
    
    wlr_log(WLR_INFO, "XDG surface scene tree created at (0,0)");
}
