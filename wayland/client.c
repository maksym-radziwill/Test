/*
 * client.c - Client handling, decoration, and cleanup
 *
 * Handles XDG decorations (server-side) and server resource cleanup.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/util/log.h>

#include "client.h"
#include "../draw/draw.h"
#include "../p9/p9.h"
#include "presentation.h"

/* ============== Decoration Handling ============== */

struct decoration_data {
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wl_listener destroy;
    struct wl_listener request_mode;
    struct wl_listener surface_commit;
    bool mode_set;
};

static void decoration_handle_destroy(struct wl_listener *listener, void *data) {
    struct decoration_data *dd = wl_container_of(listener, dd, destroy);
    (void)data;
    wl_list_remove(&dd->destroy.link);
    wl_list_remove(&dd->request_mode.link);
    if (dd->surface_commit.link.next) {
        wl_list_remove(&dd->surface_commit.link);
    }
    free(dd);
}

static void decoration_set_mode_if_ready(struct decoration_data *dd) {
    struct wlr_xdg_toplevel *toplevel = dd->decoration->toplevel;
    
    if (!toplevel || !toplevel->base || !toplevel->base->initialized) {
        wlr_log(WLR_DEBUG, "Decoration: surface not initialized yet, deferring");
        return;
    }
    
    if (dd->mode_set) {
        return;
    }
    
    wlr_log(WLR_INFO, "Decoration mode set to server-side");
    wlr_xdg_toplevel_decoration_v1_set_mode(dd->decoration, 
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    dd->mode_set = true;
    
    if (dd->surface_commit.link.next) {
        wl_list_remove(&dd->surface_commit.link);
        dd->surface_commit.link.next = NULL;
    }
}

static void decoration_handle_surface_commit(struct wl_listener *listener, void *data) {
    struct decoration_data *dd = wl_container_of(listener, dd, surface_commit);
    (void)data;
    decoration_set_mode_if_ready(dd);
}

static void decoration_handle_request_mode(struct wl_listener *listener, void *data) {
    struct decoration_data *dd = wl_container_of(listener, dd, request_mode);
    (void)data;
    
    decoration_set_mode_if_ready(dd);
    
    if (!dd->mode_set && dd->decoration->toplevel && 
        dd->decoration->toplevel->base && dd->decoration->toplevel->base->surface) {
        if (!dd->surface_commit.link.next) {
            dd->surface_commit.notify = decoration_handle_surface_commit;
            wl_signal_add(&dd->decoration->toplevel->base->surface->events.commit, 
                          &dd->surface_commit);
        }
    }
}

void handle_new_decoration(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, new_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
    (void)s;
    
    wlr_log(WLR_INFO, "New decoration object created");
    
    struct decoration_data *dd = calloc(1, sizeof(*dd));
    if (!dd) {
        wlr_log(WLR_ERROR, "Failed to allocate decoration_data");
        return;
    }
    
    dd->decoration = decoration;
    dd->mode_set = false;
    dd->surface_commit.link.next = NULL;
    
    dd->destroy.notify = decoration_handle_destroy;
    wl_signal_add(&decoration->events.destroy, &dd->destroy);
    
    dd->request_mode.notify = decoration_handle_request_mode;
    wl_signal_add(&decoration->events.request_mode, &dd->request_mode);
}

/* ============== Keyboard Shortcuts Inhibit ============== */

static void kb_inhibitor_destroy(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, kb_inhibitor_destroy);
    (void)data;
    
    wlr_log(WLR_INFO, "Keyboard shortcuts inhibitor destroyed");
    s->active_kb_inhibitor = NULL;
    wl_list_remove(&s->kb_inhibitor_destroy.link);
}

void handle_new_kb_inhibitor(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, new_kb_shortcut_inhibitor);
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;
    
    /* Only allow one at a time */
    if (s->active_kb_inhibitor) {
        wlr_keyboard_shortcuts_inhibitor_v1_deactivate(s->active_kb_inhibitor);
    }
    
    s->active_kb_inhibitor = inhibitor;
    wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
    
    s->kb_inhibitor_destroy.notify = kb_inhibitor_destroy;
    wl_signal_add(&inhibitor->events.destroy, &s->kb_inhibitor_destroy);
    
    wlr_log(WLR_INFO, "Keyboard shortcuts inhibitor activated");
}

/* ============== Server Cleanup ============== */

static void rio_window_cleanup(struct rio_window *rw) {
    /*
     * v16: discard any presentation feedbacks still attached to this
     * window before tearing down threads.  After the wl_display has
     * been destroyed, this is mostly defensive — destroy listeners
     * fired and emptied the lists.  But during multiwin_destroy_window
     * called from the event loop with the display still alive, this is
     * the only way feedbacks get terminated cleanly.
     */
    presentation_window_died(rw);

    /* Stop per-window threads */
    pthread_mutex_lock(&rw->send_lock);
    pthread_cond_signal(&rw->send_cond);
    pthread_mutex_unlock(&rw->send_lock);

    /* Shut down sockets BEFORE joining threads.
     * Mouse and kbd threads may be stuck in blocking read() calls on a
     * still-healthy socket (e.g. user-initiated shutdown via toplevel
     * close).  shutdown() makes those reads return immediately with an
     * error, so pthread_join doesn't deadlock.  multiwin_destroy_window
     * does the same thing for the per-secondary teardown path. */
    if (rw->p9_mouse.fd    >= 0) shutdown(rw->p9_mouse.fd,    SHUT_RDWR);
    if (rw->p9_kbd.fd      >= 0) shutdown(rw->p9_kbd.fd,      SHUT_RDWR);
    if (rw->p9_draw.fd     >= 0) shutdown(rw->p9_draw.fd,     SHUT_RDWR);
    if (rw->p9_relookup.fd >= 0) shutdown(rw->p9_relookup.fd, SHUT_RDWR);
    if (rw->p9_wctl.fd     >= 0) shutdown(rw->p9_wctl.fd,     SHUT_RDWR);

    if (rw->mouse_thread) pthread_join(rw->mouse_thread, NULL);
    if (rw->kbd_thread)   pthread_join(rw->kbd_thread, NULL);
    if (rw->send_thread)  pthread_join(rw->send_thread, NULL);

    /*
     * Close the presentation pipe.  By the time server_cleanup runs,
     * wl_display_destroy has already auto-destroyed all event sources
     * registered on the loop, so present_source itself is gone — we
     * just need to close the underlying fds.  The send thread has
     * already been joined above so no writes are in flight.
     */
    if (rw->present_pipe[0] >= 0) { close(rw->present_pipe[0]); rw->present_pipe[0] = -1; }
    if (rw->present_pipe[1] >= 0) { close(rw->present_pipe[1]); rw->present_pipe[1] = -1; }
    rw->present_source = NULL;  /* Already invalidated by wl_display_destroy */

    /* Free per-window buffers */
    free(rw->framebuf);
    free(rw->prev_framebuf);
    free(rw->send_buf[0]);
    free(rw->send_buf[1]);
    free(rw->dirty_staging);
    free(rw->dirty_tiles[0]);
    free(rw->dirty_tiles[1]);

    pthread_mutex_destroy(&rw->send_lock);
    pthread_cond_destroy(&rw->send_cond);

    /* Disconnect per-window 9P connections */
    p9_disconnect(&rw->p9_draw);
    p9_disconnect(&rw->p9_relookup);
    p9_disconnect(&rw->p9_mouse);
    p9_disconnect(&rw->p9_kbd);
    p9_disconnect(&rw->p9_wctl);
}

void server_cleanup(struct server *s) {
    s->running = 0;

    /* v16: tear down wp_presentation global and discard any pending
     * feedbacks not yet bound to a rio_window.  Per-window feedbacks
     * are handled by rio_window_cleanup below. */
    presentation_cleanup(s);

    /* Clean up all rio windows */
    struct rio_window *rw, *tmp;
    wl_list_for_each_safe(rw, tmp, &s->rio_windows, link) {
        rio_window_cleanup(rw);
        wl_list_remove(&rw->link);
        free(rw);
    }

    wlr_keyboard_finish(&s->virtual_kb);

    /* Cleanup focus manager */
    focus_manager_cleanup(&s->focus);

    /* Disconnect global 9P connections */
    p9_disconnect(&s->p9_snarf);

    close(s->input_queue.pipe_fd[0]);
    close(s->input_queue.pipe_fd[1]);
    pthread_mutex_destroy(&s->input_queue.lock);

    free(s->tls_cert_file);
    free(s->tls_fingerprint);
}
