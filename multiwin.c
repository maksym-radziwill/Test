/*
 * multiwin.c - Multi-window protocol for p9wl
 *
 * Handles creation of secondary rio windows via stdin/stdout protocol.
 * An external script (typically rc on Plan 9) reads NEW_WINDOW requests
 * from stdout, creates rio windows with exportfs, and sends READY
 * responses on stdin with the connection details.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>

#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>

#include "multiwin.h"
#include "p9/p9.h"
#include "p9/p9_tls.h"
#include "draw/draw.h"
#include "draw/send.h"
#include "input/input.h"
#include "wayland/output.h"
#include "wayland/presentation.h"

/* ============== Stdin Protocol Handler ============== */

/*
 * Find a toplevel waiting for a specific window id.
 */
static struct toplevel *find_pending_toplevel(struct server *s, int id) {
    struct toplevel *tl;
    wl_list_for_each(tl, &s->toplevels, link) {
        if (tl->pending_window_id == id)
            return tl;
    }
    return NULL;
}

/*
 * Find a rio_window by id.
 */
static struct rio_window *find_rio_window(struct server *s, int id) {
    struct rio_window *rw;
    wl_list_for_each(rw, &s->rio_windows, link) {
        if (rw->id == id)
            return rw;
    }
    return NULL;
}

/*
 * Parse a READY response and create the secondary window.
 *
 * Format: READY id=<N> host=<H> port=<P>
 */
static void handle_ready(struct server *s, const char *line) {
    int id = 0, port = 0;
    char host[256] = {0};

    /* Parse fields */
    const char *p = line + 5; /* skip "READY" */
    while (*p) {
        while (*p == ' ') p++;
        if (strncmp(p, "id=", 3) == 0) {
            id = atoi(p + 3);
        } else if (strncmp(p, "host=", 5) == 0) {
            const char *start = p + 5;
            const char *end = start;
            while (*end && *end != ' ') end++;
            int len = end - start;
            if (len >= (int)sizeof(host)) len = sizeof(host) - 1;
            memcpy(host, start, len);
            host[len] = '\0';
        } else if (strncmp(p, "port=", 5) == 0) {
            port = atoi(p + 5);
        }
        while (*p && *p != ' ') p++;
    }

    if (id <= 0 || !host[0] || port <= 0) {
        wlr_log(WLR_ERROR, "multiwin: invalid READY: %s", line);
        return;
    }

    wlr_log(WLR_INFO, "multiwin: READY id=%d host=%s port=%d", id, host, port);

    struct toplevel *tl = find_pending_toplevel(s, id);
    if (!tl) {
        wlr_log(WLR_ERROR, "multiwin: no pending toplevel for id=%d", id);
        return;
    }

    struct rio_window *rw = multiwin_create_window(s, id, host, port);
    if (!rw) {
        wlr_log(WLR_ERROR, "multiwin: failed to create window id=%d", id);
        tl->pending_window_id = 0;  /* Give up, stays on primary */
        /* Re-show on primary since the new window failed */
        wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
        tl->window->scene_dirty = 1;
        if (tl->window->output) wlr_output_schedule_frame(tl->window->output);
        return;
    }

    tl->pending_window_id = 0;
    multiwin_migrate_toplevel(tl, rw);
    wlr_log(WLR_INFO, "multiwin: toplevel migrated to window id=%d", id);
}

/*
 * Wayland event loop callback for stdin.
 *
 * Reads lines and dispatches READY responses.
 */
static int stdin_readable(int fd, uint32_t mask, void *data) {
    struct server *s = data;
    static char buf[1024];
    static int buf_len = 0;
    (void)mask;

    ssize_t n = read(fd, buf + buf_len, sizeof(buf) - buf_len - 1);
    if (n <= 0) {
        if (n == 0) {
            wlr_log(WLR_INFO, "multiwin: stdin closed");
            /* Remove event source — script exited */
            if (s->stdin_event) {
                wl_event_source_remove(s->stdin_event);
                s->stdin_event = NULL;
            }
            /* Re-show any toplevels hidden while waiting for a READY
             * that will never arrive.  They fall back to primary. */
            struct toplevel *tl;
            wl_list_for_each(tl, &s->toplevels, link) {
                if (tl->pending_window_id != 0) {
                    tl->pending_window_id = 0;
                    wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
                    wlr_log(WLR_INFO, "multiwin: re-showing pending toplevel on primary");
                }
            }
            s->primary->scene_dirty = 1;
            if (s->primary->output)
                wlr_output_schedule_frame(s->primary->output);
        }
        return 0;
    }
    buf_len += n;
    buf[buf_len] = '\0';

    /* Process complete lines */
    char *line_start = buf;
    char *newline;
    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';

        /* Strip trailing \r */
        if (newline > line_start && *(newline - 1) == '\r')
            *(newline - 1) = '\0';

        wlr_log(WLR_DEBUG, "multiwin: stdin: %s", line_start);

        if (strncmp(line_start, "READY ", 6) == 0) {
            handle_ready(s, line_start);
        } else {
            wlr_log(WLR_INFO, "multiwin: unknown message: %s", line_start);
        }

        line_start = newline + 1;
    }

    /* Shift remaining partial line to front of buffer */
    int remaining = buf_len - (line_start - buf);
    if (remaining > 0 && line_start != buf) {
        memmove(buf, line_start, remaining);
    }
    buf_len = remaining;

    return 0;
}

/* ============== Public API ============== */

int multiwin_init(struct server *s) {
    /* Make stdin non-blocking */
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0) return -1;
    if (fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    s->stdin_event = wl_event_loop_add_fd(
        wl_display_get_event_loop(s->display),
        STDIN_FILENO, WL_EVENT_READABLE,
        stdin_readable, s);

    if (!s->stdin_event) {
        wlr_log(WLR_ERROR, "multiwin: failed to add stdin to event loop");
        return -1;
    }

    wlr_log(WLR_INFO, "multiwin: initialized (reading stdin for READY)");
    return 0;
}

void multiwin_request_window(struct server *s, struct toplevel *tl) {
    int id = ++s->next_window_id;
    struct rio_window *primary = s->primary;

    tl->pending_window_id = id;

    /* Hide the toplevel on primary while it waits for its own rio window.
     * Without this, both the old and new toplevel render at (0,0) in
     * primary's subtree, overlapping each other.  The node is re-enabled
     * in multiwin_migrate_toplevel once the new window is ready, or
     * below if the write fails. */
    wlr_scene_node_set_enabled(&tl->scene_tree->node, false);

    /* Use primary window's visible dimensions as default for the new window */
    clearerr(stdout);
    fprintf(stdout, "NEW_WINDOW id=%d width=%d height=%d\n",
            id, primary->visible_width, primary->visible_height);
    fflush(stdout);

    if (ferror(stdout)) {
        /* Stdout pipe is broken — script has exited.  No READY can
         * ever arrive, so undo the hide and fall back to primary. */
        wlr_log(WLR_INFO, "multiwin: stdout broken (script exited), "
                "toplevel stays on primary");
        tl->pending_window_id = 0;
        wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
        clearerr(stdout);
        return;
    }

    wlr_log(WLR_INFO, "multiwin: requested NEW_WINDOW id=%d (%dx%d)",
            id, primary->visible_width, primary->visible_height);
}

void multiwin_close_window(struct server *s, struct rio_window *rw) {
    if (!rw || rw == s->primary) return;

    /* Delete the rio window on the Plan 9 side via /dev/wctl.
     * This must happen before multiwin_destroy_window shuts down
     * the connections.  Uses the dedicated p9_wctl connection. */
    struct p9conn *p9 = &rw->p9_wctl;
    uint32_t wctl_fid = p9->next_fid++;
    const char *wnames[1] = { "wctl" };

    if (p9_walk(p9, p9->root_fid, wctl_fid, 1, wnames) == 0) {
        if (p9_open(p9, wctl_fid, OWRITE, NULL) == 0) {
            const char *cmd = "delete";
            p9_write(p9, wctl_fid, 0, (uint8_t*)cmd, strlen(cmd));
            wlr_log(WLR_INFO, "multiwin: deleted rio window %d via wctl", rw->id);
        } else {
            wlr_log(WLR_ERROR, "multiwin: failed to open wctl for window %d", rw->id);
        }
    } else {
        wlr_log(WLR_ERROR, "multiwin: failed to walk to wctl for window %d", rw->id);
    }

    fprintf(stdout, "CLOSE_WINDOW id=%d\n", rw->id);
    fflush(stdout);

    wlr_log(WLR_INFO, "multiwin: sent CLOSE_WINDOW id=%d", rw->id);
}

/* Per-thread argument for parallel secondary window connections. */
struct mw_connect_arg {
    struct p9conn *p9;
    const char *host;
    int port;
    struct tls_config *tls_cfg;
    const char *name;
    int result;
};

static void *mw_connect_thread(void *arg) {
    struct mw_connect_arg *a = arg;
    a->result = p9_connect(a->p9, a->host, a->port, a->tls_cfg);
    if (a->result < 0)
        wlr_log(WLR_ERROR, "multiwin: connect failed (%s)", a->name);
    return NULL;
}

struct rio_window *multiwin_create_window(struct server *s, int id,
                                          const char *host, int port) {
    struct rio_window *rw = calloc(1, sizeof(*rw));
    if (!rw) return NULL;

    rw->server = s;
    rw->id = id;

    /* Build TLS config from server settings */
    struct tls_config tls_cfg = {0};
    if (s->use_tls) {
        tls_cfg.cert_file = s->tls_cert_file;
        tls_cfg.cert_fingerprint = s->tls_fingerprint;
        tls_cfg.insecure = s->tls_insecure;
    }

    /* Connect per-window 9P sessions in parallel */
    struct p9conn *conns[] = {
        &rw->p9_draw, &rw->p9_relookup, &rw->p9_mouse, &rw->p9_kbd, &rw->p9_wctl
    };
    const char *names[] = { "draw", "relookup", "mouse", "kbd", "wctl" };
    int nconns = sizeof(conns) / sizeof(conns[0]);

    struct mw_connect_arg mw_args[5];
    pthread_t cthreads[5];
    struct tls_config *tls_ptr = s->use_tls ? &tls_cfg : NULL;

    for (int i = 0; i < nconns; i++) {
        mw_args[i] = (struct mw_connect_arg){
            .p9 = conns[i], .host = host, .port = port,
            .tls_cfg = tls_ptr, .name = names[i]
        };
        pthread_create(&cthreads[i], NULL, mw_connect_thread, &mw_args[i]);
    }
    for (int i = 0; i < nconns; i++)
        pthread_join(cthreads[i], NULL);

    for (int i = 0; i < nconns; i++) {
        if (mw_args[i].result < 0) {
            wlr_log(WLR_ERROR, "multiwin: connect failed (%s) for window %d",
                    names[i], id);
            for (int j = 0; j < nconns; j++)
                if (j != i && mw_args[j].result == 0) p9_disconnect(conns[j]);
            free(rw);
            return NULL;
        }
    }

    /* Initialize draw device */
    if (init_draw(rw) < 0) {
        wlr_log(WLR_ERROR, "multiwin: init_draw failed for window %d", id);
        for (int i = 0; i < nconns; i++) p9_disconnect(conns[i]);
        free(rw);
        return NULL;
    }

    rw->width = rw->draw.width;
    rw->height = rw->draw.height;
    rw->visible_width = rw->draw.visible_width;
    rw->visible_height = rw->draw.visible_height;
    rw->tiles_x = rw->width / TILE_SIZE;
    rw->tiles_y = rw->height / TILE_SIZE;

    /* Allocate framebuffers */
    size_t fb_size = rw->width * rw->height * 4;
    rw->framebuf = calloc(1, fb_size);
    rw->prev_framebuf = calloc(1, fb_size);
    rw->send_buf[0] = calloc(1, fb_size);
    rw->send_buf[1] = calloc(1, fb_size);
    if (!rw->framebuf || !rw->prev_framebuf ||
        !rw->send_buf[0] || !rw->send_buf[1]) {
        wlr_log(WLR_ERROR, "multiwin: alloc failed for window %d", id);
        free(rw->framebuf); free(rw->prev_framebuf);
        free(rw->send_buf[0]); free(rw->send_buf[1]);
        for (int i = 0; i < nconns; i++) p9_disconnect(conns[i]);
        free(rw);
        return NULL;
    }

    rw->force_full_frame = 1;
    rw->frame_dirty = 1;
    rw->pending_buf = -1;
    rw->active_buf = -1;
    pthread_mutex_init(&rw->send_lock, NULL);
    pthread_cond_init(&rw->send_cond, NULL);

    /* v16: wp_presentation feedback lists — see wayland/presentation.h */
    wl_list_init(&rw->queued_feedbacks);
    wl_list_init(&rw->buf_feedbacks[0]);
    wl_list_init(&rw->buf_feedbacks[1]);
    rw->present_seq = 0;
    atomic_store(&rw->present_slot_mask, 0);
    atomic_store(&rw->present_time_ms_slot[0], 0);
    atomic_store(&rw->present_time_ms_slot[1], 0);

    /* Presentation feedback pipe — see comments in main.c. */
    rw->present_pipe[0] = rw->present_pipe[1] = -1;
    if (pipe(rw->present_pipe) < 0) {
        wlr_log(WLR_ERROR, "multiwin: present pipe failed: %s", strerror(errno));
        /* Continue without backpressure — frame_done will fall back to
         * firing on every output_frame for this window only. */
    } else {
        fcntl(rw->present_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(rw->present_pipe[1], F_SETFL, O_NONBLOCK);
    }
    atomic_store(&rw->frame_done_pending, 0);

    /* Start per-window threads */
    pthread_create(&rw->mouse_thread, NULL, mouse_thread_func, rw);
    pthread_create(&rw->kbd_thread, NULL, kbd_thread_func, rw);
    pthread_create(&rw->send_thread, NULL, send_thread_func, rw);

    /* Register present_source on the wlroots event loop.  Done after
     * thread creation so the send thread's first writes have a reader
     * registered (otherwise the byte sits in the pipe buffer until
     * the main loop picks it up — fine, but cleaner this way). */
    if (rw->present_pipe[0] >= 0) {
        rw->present_source = wl_event_loop_add_fd(
            wl_display_get_event_loop(s->display),
            rw->present_pipe[0], WL_EVENT_READABLE,
            present_handler, rw);
    }

    /* Place each window far apart in layout space so no resize can
     * make one output's viewport overlap another's scene nodes.
     * 100000 logical pixels is unreachable by any real window size. */
    rw->layout_x = id * 100000;
    rw->layout_y = 0;

    /* Create wlroots output — set pending so new_output picks it up.
     * new_output sets rw->output and clears pending_rio_window before
     * wlr_output_layout_add fires any callbacks. */
    s->pending_rio_window = rw;
    wlr_headless_add_output(s->backend, rw->visible_width, rw->visible_height);

    /* Add to window list */
    wl_list_insert(&s->rio_windows, &rw->link);

    /* Create background for new window */
    if (rw->output) {
        int lw = focus_phys_to_logical(rw->visible_width, s->scale);
        int lh = focus_phys_to_logical(rw->visible_height, s->scale);
        float gray[4] = { 0.3f, 0.3f, 0.3f, 1.0f };
        rw->background = wlr_scene_rect_create(&s->scene->tree, lw, lh, gray);
        if (rw->background) {
            wlr_scene_node_set_position(&rw->background->node, rw->layout_x, rw->layout_y);
            wlr_scene_node_lower_to_bottom(&rw->background->node);
        }
    }

    /* Per-window scene subtree for toplevel isolation */
    rw->toplevel_tree = wlr_scene_tree_create(&s->scene->tree);
    if (rw->toplevel_tree) {
        wlr_scene_node_set_position(&rw->toplevel_tree->node, rw->layout_x, rw->layout_y);
    }

    wlr_log(WLR_INFO, "multiwin: window %d created (%dx%d visible, %dx%d padded)",
            id, rw->visible_width, rw->visible_height, rw->width, rw->height);

    return rw;
}

void multiwin_migrate_toplevel(struct toplevel *tl, struct rio_window *rw) {
    struct server *s = tl->server;
    struct rio_window *old_rw = tl->window;

    tl->window = rw;

    /* Compute logical dimensions for the new window */
    int logical_w = focus_phys_to_logical(rw->visible_width, s->scale);
    int logical_h = focus_phys_to_logical(rw->visible_height, s->scale);

    /* Reconfigure toplevel with new dimensions */
    if (tl->xdg && tl->xdg->base && tl->xdg->base->initialized) {
        wlr_xdg_toplevel_set_size(tl->xdg, logical_w, logical_h);
        wlr_xdg_toplevel_set_maximized(tl->xdg, true);
        wlr_xdg_toplevel_set_activated(tl->xdg, true);
        wlr_xdg_surface_schedule_configure(tl->xdg->base);
    }

    /*
     * Set keyboard focus on the migrated toplevel via the focus manager
     * rather than calling wlr_seat_keyboard_notify_enter directly.
     *
     * focus_keyboard_set sends the enter event AND, crucially, a
     * follow-up modifier update with fm->modifier_state — see the
     * comment in focus_keyboard_set about kb->modifiers always being
     * the virtual keyboard's (zero) state.  Going through the seat
     * call directly would tell the migrated toplevel "no modifiers
     * held," which is wrong if the user is mid-keystroke.
     *
     * We rely on migrate_toplevels_to having already cleaned the
     * dying window's contribution out of fm->modifier_state, so the
     * resync here delivers a state that excludes any modifiers whose
     * synthetic-release got lost when the rio window's kbd thread
     * died.
     */
    if (tl->surface)
        focus_keyboard_set(&s->focus, tl->surface, FOCUS_REASON_EXPLICIT);

    /* Reparent scene node into new window's subtree.
     * The subtree is positioned at (layout_x, layout_y), so the
     * toplevel sits at (0,0) local — visible only in this output's
     * viewport.  Combined with the enable/disable in output_frame,
     * this guarantees each output renders only its own toplevels. */
    if (rw->toplevel_tree) {
        wlr_scene_node_reparent(&tl->scene_tree->node, rw->toplevel_tree);
        wlr_scene_node_set_position(&tl->scene_tree->node, 0, 0);
        wlr_log(WLR_INFO, "multiwin: scene node reparented to window %d subtree at (%d,%d)",
                rw->id, rw->layout_x, rw->layout_y);
    } else {
        /* Fallback: no subtree, just reposition (shouldn't happen) */
        wlr_scene_node_set_position(&tl->scene_tree->node, rw->layout_x, rw->layout_y);
        wlr_log(WLR_INFO, "multiwin: scene node moved to (%d,%d) for output %d",
                rw->layout_x, rw->layout_y, rw->id);
    }

    /* Re-enable the scene node — it was hidden by multiwin_request_window
     * to prevent it from rendering on primary while waiting. */
    wlr_scene_node_set_enabled(&tl->scene_tree->node, true);

    /*
     * Mark both windows dirty.
     *
     * On the destination we also force a full frame on the next send.
     * Scroll detection in send_thread_func compares framebuf against
     * prev_framebuf to find translations; after migration the
     * destination's prev_framebuf reflects whatever was on it BEFORE
     * the migrated toplevel arrived (a different toplevel, or zeros for
     * a fresh window).  Without force_full_frame, phase correlation
     * could find a spurious "scroll" between two unrelated frames; even
     * if the verification step usually rejects these, a particularly
     * unlucky pair (similar background patterns, similar text vertical
     * frequency) can pass and produce a 'd' command that copies wrong
     * pixels around the destination's image, causing visual corruption.
     *
     * force_full_frame=1 makes the send thread skip scroll detection
     * and emit a complete frame, after which prev_framebuf is in sync
     * with framebuf and normal scroll detection resumes.
     */
    if (old_rw) {
        old_rw->scene_dirty = 1;
        if (old_rw->output) wlr_output_schedule_frame(old_rw->output);
    }
    rw->scene_dirty = 1;
    rw->force_full_frame = 1;
    if (rw->output) wlr_output_schedule_frame(rw->output);
}

void multiwin_destroy_window(struct server *s, struct rio_window *rw) {
    if (!rw || rw == s->primary) return;

    wlr_log(WLR_INFO, "multiwin: destroying window %d", rw->id);

    /*
     * Mark this window as being destroyed BEFORE we close any sockets.
     *
     * The worker threads (mouse, kbd, send) are about to wake from their
     * blocking 9P reads with EOF/error and run their exit paths.  Those
     * paths normally post INPUT_RIO_WINDOW_DIED so the event loop can
     * clean up windows that died externally.  Here we're cleaning up
     * already, so the post would be a stale event referencing the
     * about-to-be-freed rw.  The flag tells them to skip the post.
     */
    atomic_store(&rw->being_destroyed, 1);

    /*
     * Phase 0: Clean up this window's contributions to global focus state.
     *
     * Must happen BEFORE thread teardown.  The kbd/mouse threads for this
     * window have already dispatched events that set bits in the global
     * modifier_state and incremented the seat's button_count.  Those bits
     * will never be cleared by release events because the threads are about
     * to die.  Clean up now while we still know what this window contributed.
     *
     * Also (v16): fire wp_presentation_feedback.discarded on every
     * feedback still attached to this window.  Toplevels migrating to
     * other windows will request fresh feedbacks on those windows;
     * the old ones must terminate so clients don't leak resources.
     */
    focus_manager_window_died(&s->focus, rw);
    presentation_window_died(rw);

    /*
     * Phase 1: Force all threads to exit.
     *
     * shutdown() signals EOF on the sockets without closing the fd,
     * so blocked p9_read calls return immediately.  Setting drain.broken
     * and drain.running ensures both drain and send threads exit their
     * loops cleanly.
     */
    if (rw->p9_mouse.fd >= 0) shutdown(rw->p9_mouse.fd, SHUT_RDWR);
    if (rw->p9_kbd.fd >= 0) shutdown(rw->p9_kbd.fd, SHUT_RDWR);
    if (rw->p9_draw.fd >= 0) shutdown(rw->p9_draw.fd, SHUT_RDWR);
    if (rw->p9_relookup.fd >= 0) shutdown(rw->p9_relookup.fd, SHUT_RDWR);
    if (rw->p9_wctl.fd >= 0) shutdown(rw->p9_wctl.fd, SHUT_RDWR);

    /* Tell drain thread to stop */
    atomic_store(&rw->drain.broken, 1);
    atomic_store(&rw->drain.running, 0);
    pthread_mutex_lock(&rw->drain.lock);
    pthread_cond_broadcast(&rw->drain.cond);
    pthread_cond_broadcast(&rw->drain.done_cond);
    pthread_mutex_unlock(&rw->drain.lock);

    /* Wake send thread so it sees drain.broken and exits */
    pthread_mutex_lock(&rw->send_lock);
    pthread_cond_signal(&rw->send_cond);
    pthread_mutex_unlock(&rw->send_lock);

    /* Phase 2: Join threads */
    if (rw->mouse_thread) { pthread_join(rw->mouse_thread, NULL); rw->mouse_thread = 0; }
    if (rw->kbd_thread)   { pthread_join(rw->kbd_thread, NULL);   rw->kbd_thread = 0; }
    if (rw->send_thread)  { pthread_join(rw->send_thread, NULL);  rw->send_thread = 0; }

    /*
     * Tear down the presentation pipe AFTER joining the send thread.
     * If we closed the pipe before joining, the send thread (still
     * running until it sees drain.broken) could race with us and
     * write(2) to a recycled fd — a textbook write-to-closed-fd bug.
     * pthread_join guarantees the thread has exited; only then is it
     * safe to close.
     *
     * Removing the event source first ensures the wlroots event loop
     * doesn't try to call present_handler after the rio_window is
     * partly destroyed.  By this point the send thread is gone, so
     * no more wakeups are possible anyway, but the cleanup ordering
     * (remove source, then close fd) matches what the wlroots API
     * expects.
     */
    if (rw->present_source) {
        wl_event_source_remove(rw->present_source);
        rw->present_source = NULL;
    }
    if (rw->present_pipe[0] >= 0) { close(rw->present_pipe[0]); rw->present_pipe[0] = -1; }
    if (rw->present_pipe[1] >= 0) { close(rw->present_pipe[1]); rw->present_pipe[1] = -1; }

    /* Phase 3: Remove wlroots resources (must be on event loop thread) */
    if (rw->toplevel_tree) {
        wlr_scene_node_destroy(&rw->toplevel_tree->node);
        rw->toplevel_tree = NULL;
    }
    if (rw->background) {
        wlr_scene_node_destroy(&rw->background->node);
        rw->background = NULL;
    }
    /* scene_output is owned by wlr_scene_attach_output_layout — it will
     * be auto-destroyed when we remove/destroy the output below. */
    rw->scene_output = NULL;
    if (rw->output) {
        wlr_output_layout_remove(s->output_layout, rw->output);
        /* output_destroy listener removes frame/destroy listeners */
        wlr_output_destroy(rw->output);
        rw->output = NULL;
    }

    /* Phase 4: Disconnect 9P and free buffers */
    p9_disconnect(&rw->p9_draw);
    p9_disconnect(&rw->p9_relookup);
    p9_disconnect(&rw->p9_mouse);
    p9_disconnect(&rw->p9_kbd);
    p9_disconnect(&rw->p9_wctl);

    free(rw->framebuf);
    free(rw->prev_framebuf);
    free(rw->send_buf[0]);
    free(rw->send_buf[1]);
    free(rw->dirty_staging);
    free(rw->dirty_tiles[0]);
    free(rw->dirty_tiles[1]);

    pthread_mutex_destroy(&rw->send_lock);
    pthread_cond_destroy(&rw->send_cond);

    /* Phase 5: Drain stale input events referencing this window.
     * After joining its threads, no new events for rw will arrive.
     * Flush the entire queue — losing a few events for other windows
     * is harmless since mouse input is continuous. */
    {
        struct input_event ev;
        while (input_queue_pop(&s->input_queue, &ev)) { /* discard */ }
        char drain_buf[64];
        while (read(s->input_queue.pipe_fd[0], drain_buf, sizeof(drain_buf)) > 0) {}
    }

    /* Phase 6: Remove from window list and free */
    wl_list_remove(&rw->link);
    wlr_log(WLR_INFO, "multiwin: window %d destroyed", rw->id);
    free(rw);
}

void multiwin_relayout(struct server *s) {
    /*
     * Recalculate layout positions for all windows left-to-right.
     * Primary (id=0) is always at (0,0). Secondary windows are placed
     * to its right in creation order (by id).
     *
     * This must be called after any window resize to prevent overlapping
     * viewports — if primary grows wider, its scene_output covers more
     * layout space and renders secondary windows' scene nodes.
     */

    /* Build a sorted array of window pointers (by id, primary first) */
    struct rio_window *sorted[32];
    int nwin = 0;
    struct rio_window *rw;
    wl_list_for_each(rw, &s->rio_windows, link) {
        if (nwin < 32) sorted[nwin++] = rw;
    }
    /* Simple insertion sort by id */
    for (int i = 1; i < nwin; i++) {
        struct rio_window *tmp = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j]->id > tmp->id) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = tmp;
    }

    /* Assign positions left-to-right */
    int x = 0;
    for (int i = 0; i < nwin; i++) {
        rw = sorted[i];
        int old_x = rw->layout_x;
        rw->layout_x = x;
        rw->layout_y = 0;

        int logical_w = focus_phys_to_logical(rw->visible_width, s->scale);

        if (old_x != rw->layout_x && rw->output) {
            wlr_output_layout_remove(s->output_layout, rw->output);
            wlr_output_layout_add(s->output_layout, rw->output,
                                  rw->layout_x, rw->layout_y);

            /* scene_output position is managed by wlr_scene_attach_output_layout
             * via the layout change above, but update manually as fallback */
            if (rw->scene_output)
                wlr_scene_output_set_position(rw->scene_output,
                                              rw->layout_x, rw->layout_y);

            if (rw->background)
                wlr_scene_node_set_position(&rw->background->node,
                                            rw->layout_x, rw->layout_y);

            if (rw->toplevel_tree)
                wlr_scene_node_set_position(&rw->toplevel_tree->node,
                                            rw->layout_x, rw->layout_y);

            wlr_log(WLR_INFO, "multiwin: relayout window %d: x=%d -> %d",
                    rw->id, old_x, rw->layout_x);

            rw->scene_dirty = 1;
            wlr_output_schedule_frame(rw->output);
        }

        x += logical_w;
    }
}

int has_other_live_windows(struct server *s, struct rio_window *self) {
    struct rio_window *rw;
    wl_list_for_each(rw, &s->rio_windows, link) {
        if (rw != self) return 1;
    }
    return 0;
}

/*
 * Find the first rio_window other than `exclude` whose worker threads
 * have not started exiting (alive).  Returns NULL if no such window
 * exists.
 *
 * Used by candidate selection in promotion / migration to avoid handing
 * toplevels to a window that's also dying.
 */
static struct rio_window *find_live_rio_window(struct server *s,
                                                struct rio_window *exclude) {
    struct rio_window *rw;
    wl_list_for_each(rw, &s->rio_windows, link) {
        if (rw != exclude && atomic_load(&rw->threads_exited) == 0)
            return rw;
    }
    return NULL;
}

/*
 * Migrate every toplevel currently sitting on `from` onto `to`.
 *
 * Also fixes up popup_data.window for any popups whose owning window was
 * `from`.  popups created when a toplevel was on `from` cache the
 * rio_window pointer at popup creation time, so without this they'd
 * dangle after `from` is freed.
 *
 * Clears pending_window_id so a stale READY for that id doesn't
 * later trigger a double migration, and re-enables the scene node
 * in case multiwin_request_window's hide-while-pending logic disabled it.
 */
static void migrate_toplevels_to(struct server *s,
                                  struct rio_window *from,
                                  struct rio_window *to) {
    /*
     * Clean up `from`'s contribution to fm->modifier_state BEFORE we
     * migrate any toplevels.
     *
     * multiwin_destroy_window will later call focus_manager_window_died
     * (which does this same cleanup) on its own — but that call comes
     * AFTER migration, so without this, multiwin_migrate_toplevel below
     * would hand the migrated toplevel a stale modifier state via the
     * resync inside focus_keyboard_set.  Doing the cleanup here makes
     * fm->modifier_state correct at migration time; the later call in
     * focus_manager_window_died sees held_modifiers = 0 and is a no-op
     * for the modifier path.
     *
     * The bits that get cleaned here are the ones tracked in
     * rw->held_modifiers — both directly-pressed modifier keys
     * (Kshift, Kctl, ...) and synthetic modifiers fired by uppercase
     * or Ctrl-key typing (see wl_input.c handle_key).
     */
    if (from && from->held_modifiers) {
        uint32_t current = focus_keyboard_get_modifiers(&s->focus);
        uint32_t cleaned = current & ~from->held_modifiers;
        wlr_log(WLR_INFO, "migrate: cleaning modifiers 0x%x from dying win%d "
                "(0x%x -> 0x%x)", from->held_modifiers, from->id, current, cleaned);
        focus_keyboard_set_modifiers(&s->focus, cleaned);
        from->held_modifiers = 0;
    }

    struct toplevel *tl, *tmp;
    wl_list_for_each_safe(tl, tmp, &s->toplevels, link) {
        if (tl->window == from) {
            wlr_log(WLR_INFO, "migrate: toplevel %p (surface=%p) from win%d to win%d",
                    (void*)tl, (void*)tl->surface, from->id, to->id);
            tl->pending_window_id = 0;
            if (tl->scene_tree)
                wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
            multiwin_migrate_toplevel(tl, to);
        }
    }

    /* Re-point any popups that cached the dying window. */
    struct popup_data *pd;
    wl_list_for_each(pd, &s->focus.popup_stack, link) {
        if (pd->window == from) {
            wlr_log(WLR_INFO, "migrate: popup %p from win%d to win%d",
                    (void*)pd->surface, from->id, to->id);
            pd->window = to;
        }
    }
}

void multiwin_promote_primary(struct server *s) {
    struct rio_window *old_primary = s->primary;
    if (!old_primary) return;

    /*
     * Caller (handle_input_events) has already checked
     * `ev.window == s->primary`, which is the idempotency guard:
     * a stale INPUT_RIO_WINDOW_DIED whose target was already promoted
     * away never gets here.
     */

    /* Prefer a window whose threads are still alive.  Falls back to
     * any non-primary window if all secondaries are dying — at least
     * we get the toplevels off the doomed primary and let the cascade
     * (each secondary's own RIO_WINDOW_DIED) sort it out. */
    struct rio_window *candidate = find_live_rio_window(s, old_primary);
    if (!candidate) {
        struct rio_window *rw;
        wl_list_for_each(rw, &s->rio_windows, link) {
            if (rw != old_primary) { candidate = rw; break; }
        }
    }

    if (!candidate) {
        wlr_log(WLR_ERROR, "promote_primary: no surviving secondary, "
                "shutting down compositor");
        s->running = 0;
        wl_display_terminate(s->display);
        return;
    }

    wlr_log(WLR_INFO, "promote_primary: promoting window %d to primary "
            "(old primary id=%d had its connection killed)",
            candidate->id, old_primary->id);

    /*
     * Swap s->primary FIRST, before migrating.
     *
     * This way any new_toplevel / popup_commit / multiwin_request_window
     * etc. that fire during the migration land on the live candidate
     * instead of the zombie we're about to tear down.  multiwin_destroy_window
     * also has an `rw == s->primary` guard that we need to step around;
     * swapping first naturally satisfies that guard for the old primary.
     */
    s->primary = candidate;

    /*
     * Migrate any toplevels still on the old primary onto the new one.
     *
     * In the typical "user killed primary's rio window while Firefox was
     * on a secondary" case there will be zero toplevels here.  But there
     * can be:
     *   - toplevels with pending_window_id != 0 (waiting for a READY
     *     that will never arrive because the script is dead);
     *   - toplevels that were created between the threads dying and the
     *     event loop processing the death event (e.g. a Firefox file
     *     dialog — exactly the freeze case this fix is for).
     */
    migrate_toplevels_to(s, old_primary, candidate);

    /*
     * Tear down the old primary.
     *
     * multiwin_destroy_window:
     *   - skips if `rw == s->primary` — we already swapped, so this is OK.
     *   - sets being_destroyed=1 so any not-yet-exited threads know not
     *     to post a stale INPUT_RIO_WINDOW_DIED.
     *   - shutdowns sockets / sets drain.broken / waits for threads —
     *     the threads have already exited (that's why we got here), so
     *     pthread_join returns immediately.
     *   - destroys wlroots resources, frees buffers, removes from list.
     */
    multiwin_destroy_window(s, old_primary);

    wlr_log(WLR_INFO, "promote_primary: complete, new s->primary id=%d",
            s->primary->id);
}

/*
 * Clean up a secondary whose worker threads have all exited because
 * its rio window was killed externally (Plan 9 side delete).
 *
 * Mirror of multiwin_promote_primary but without the s->primary swap:
 *   - migrate any toplevels off the dying secondary onto a live window
 *     (preferring s->primary, falling back to any other live window);
 *   - destroy the secondary.
 *
 * The user-visible result: the toplevel that was on the dead secondary
 * (e.g. Firefox after rio-deleting its window) reappears on the primary
 * (or another live secondary) so the user can interact with it again
 * instead of seeing a frozen surface.
 */
static void cleanup_dead_secondary(struct server *s, struct rio_window *rw) {
    if (!rw || rw == s->primary) return;

    wlr_log(WLR_INFO, "cleanup_dead_secondary: window %d had its connection killed",
            rw->id);

    /* Prefer s->primary as the destination if it's alive — keeps the
     * UX coherent (most apps expect their main window on primary).
     * Otherwise pick any other live window. */
    struct rio_window *target = NULL;
    if (s->primary && atomic_load(&s->primary->threads_exited) == 0) {
        target = s->primary;
    } else {
        target = find_live_rio_window(s, rw);
    }

    if (!target) {
        /*
         * No live window to migrate to.  Every window died at once
         * (e.g. the network died, taking out every 9P connection).
         * Nothing useful to display — terminate the compositor.
         */
        wlr_log(WLR_ERROR, "cleanup_dead_secondary: no live target window, "
                "terminating compositor");
        s->running = 0;
        wl_display_terminate(s->display);
        return;
    }

    migrate_toplevels_to(s, rw, target);
    multiwin_destroy_window(s, rw);

    wlr_log(WLR_INFO, "cleanup_dead_secondary: complete, toplevels migrated to win%d",
            target->id);
}

void multiwin_handle_window_died(struct server *s, struct rio_window *rw) {
    if (!rw) return;

    /*
     * Verify rw is still in s->rio_windows.  The event might be stale
     * if multiwin_destroy_window already cleaned up via a concurrent
     * path (toplevel_destroy).  Walking the list is cheap and avoids
     * touching freed memory.
     *
     * The list is only modified on the event loop thread, so this
     * check is race-free relative to the worker threads that posted
     * the event.
     */
    struct rio_window *check;
    int found = 0;
    wl_list_for_each(check, &s->rio_windows, link) {
        if (check == rw) { found = 1; break; }
    }
    if (!found) {
        wlr_log(WLR_DEBUG, "handle_window_died: rw=%p already cleaned up, ignoring",
                (void*)rw);
        return;
    }

    if (rw == s->primary) {
        multiwin_promote_primary(s);
    } else {
        cleanup_dead_secondary(s, rw);
    }
}
