/*
 * send.c - Frame sending and send thread (refactored)
 *
 * Handles queuing frames, the send thread main loop,
 * tile compression selection, and pipelined writes.
 *
 * Changes from original:
 * - Uses cmd_* helpers from draw_helpers.h
 * - Consolidated draw command building into loop
 * - Extracted drain_wake() helper
 * - Simplified tile bounds with tile_bounds()
 * - Replaced full-frame memcpy in send_frame() with pointer swap
 * - Added damage-based dirty tile tracking to skip unchanged tiles
 *   without pixel scanning (falls back to tile_changed on scroll/errors)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include <wlr/util/log.h>
#include <wayland-server-core.h>

#include "send.h"
#include "compress.h"
#include "scroll.h"
#include "draw/draw.h"
#include "draw_helpers.h"
#include "p9/p9.h"
#include "types.h"
#include "input/input.h"
#include "multiwin.h"
#include "wayland/presentation.h"

/* ============== Drain Thread ============== */

/* Wake drain thread */
static inline void drain_wake(struct drain_ctx *d) {
    pthread_mutex_lock(&d->lock);
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->lock);
}

/* Read one Rwrite response.
 *
 * If the 9P stream desyncs (invalid message length), we set broken=1
 * and force pending to 0.  This immediately unblocks drain_pause and
 * prevents any further reads on the corrupted stream.  Without this,
 * a partial read leaves message body bytes in the socket buffer;
 * the next read interprets them as a message header, producing
 * garbage lengths and an unrecoverable error cascade.
 */
static int drain_recv_one(struct drain_ctx *d) {
    uint8_t *buf = d->recv_buf;
    struct p9conn *p9 = d->p9;
    
    if (atomic_load(&d->broken)) return -1;
    
    if (p9_read_full(p9, buf, 4) != 4) {
        wlr_log(WLR_ERROR, "drain: failed to read message header");
        atomic_store(&d->broken, 1);
        return -1;
    }
    uint32_t rxlen = GET32(buf);
    if (rxlen < 7 || rxlen > p9->msize) {
        wlr_log(WLR_ERROR, "drain: stream desynced (invalid length %u), halting I/O", rxlen);
        atomic_store(&d->broken, 1);
        return -1;
    }
    if (p9_read_full(p9, buf + 4, rxlen - 4) != (int)(rxlen - 4)) {
        wlr_log(WLR_ERROR, "drain: truncated message, halting I/O");
        atomic_store(&d->broken, 1);
        return -1;
    }
    
    int type = buf[4];
    if (type == Rerror) {
        uint16_t elen = GET16(buf + 7);
        char errmsg[256];
        int copylen = (elen < 255) ? elen : 255;
        memcpy(errmsg, buf + 9, copylen);
        errmsg[copylen] = '\0';
        wlr_log(WLR_ERROR, "9P drain error: %s", errmsg);
        if (strstr(errmsg, "unknown id")) atomic_store(&p9->unknown_id_error, 1);
        if (strstr(errmsg, "short")) atomic_store(&p9->draw_error, 1);
        return -1;
    }
    
    return (type == Rwrite) ? (int)GET32(buf + 7) : -1;
}

static void *drain_thread_func(void *arg) {
    struct drain_ctx *d = arg;
    wlr_log(WLR_INFO, "Drain thread started");
    
    while (atomic_load(&d->running)) {
        pthread_mutex_lock(&d->lock);
        while (atomic_load(&d->pending) == 0 && atomic_load(&d->running)) {
            pthread_cond_wait(&d->cond, &d->lock);
        }
        pthread_mutex_unlock(&d->lock);
        
        if (!atomic_load(&d->running)) break;
        if (atomic_load(&d->paused) && atomic_load(&d->pending) == 0) continue;
        
        if (atomic_load(&d->broken)) {
            /* Stream is dead — flush all pending without reading */
            int rem = atomic_exchange(&d->pending, 0);
            if (rem > 0) {
                wlr_log(WLR_INFO, "drain: flushed %d pending (stream broken)", rem);
                pthread_mutex_lock(&d->lock);
                pthread_cond_broadcast(&d->done_cond);
                pthread_mutex_unlock(&d->lock);
            }
            continue;
        }
        
        if (atomic_load(&d->pending) > 0) {
            if (drain_recv_one(d) < 0) atomic_fetch_add(&d->errors, 1);
            if (atomic_load(&d->broken)) {
                /* Just became broken — flush remaining pending */
                int rem = atomic_exchange(&d->pending, 0);
                wlr_log(WLR_ERROR, "drain: stream broke, flushed %d remaining", rem);
                pthread_mutex_lock(&d->lock);
                pthread_cond_broadcast(&d->done_cond);
                pthread_mutex_unlock(&d->lock);
            } else {
                /*
                 * Stamp the ack-time of the just-received Rwrite.
                 * Same idea as last_twrite_send_ms in drain_notify:
                 * after drain_throttle(d, 0) returns, this slot holds
                 * the timestamp of the last Rwrite of the frame.
                 * Stamp BEFORE decrementing pending so the value is
                 * visible when waiters wake.
                 */
                atomic_store(&d->last_rwrite_ack_ms, now_ms());
                atomic_fetch_sub(&d->pending, 1);
                /* Wake drain_throttle / drain_pause */
                pthread_mutex_lock(&d->lock);
                pthread_cond_broadcast(&d->done_cond);
                pthread_mutex_unlock(&d->lock);
            }
        }
    }
    
    wlr_log(WLR_INFO, "Drain thread exiting");
    return NULL;
}

static int drain_start(struct drain_ctx *d, struct p9conn *p9) {
    atomic_store(&d->pending, 0);
    atomic_store(&d->errors, 0);
    atomic_store(&d->running, 1);
    atomic_store(&d->paused, 0);
    atomic_store(&d->broken, 0);
    atomic_store(&d->last_twrite_send_ms, 0);
    atomic_store(&d->last_rwrite_ack_ms, 0);
    d->p9 = p9;
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->cond, NULL);
    pthread_cond_init(&d->done_cond, NULL);
    
    d->recv_buf = malloc(p9->msize);
    if (!d->recv_buf) return -1;
    
    if (pthread_create(&d->thread, NULL, drain_thread_func, d) != 0) {
        free(d->recv_buf);
        return -1;
    }
    return 0;
}

static void drain_stop(struct drain_ctx *d) {
    atomic_store(&d->running, 0);
    drain_wake(d);
    pthread_join(d->thread, NULL);
    
    if (!atomic_load(&d->broken)) {
        while (atomic_load(&d->pending) > 0) {
            drain_recv_one(d);
            atomic_fetch_sub(&d->pending, 1);
        }
    }
    
    free(d->recv_buf);
    d->recv_buf = NULL;
    pthread_mutex_destroy(&d->lock);
    pthread_cond_destroy(&d->cond);
    pthread_cond_destroy(&d->done_cond);
}

static void drain_pause(struct drain_ctx *d) {
    atomic_store(&d->paused, 1);
    drain_wake(d);
    pthread_mutex_lock(&d->lock);
    while (atomic_load(&d->pending) > 0 && !atomic_load(&d->broken)) {
        pthread_cond_wait(&d->done_cond, &d->lock);
    }
    pthread_mutex_unlock(&d->lock);
}

static void drain_resume(struct drain_ctx *d) {
    atomic_store(&d->paused, 0);
    drain_wake(d);
}

static void drain_notify(struct drain_ctx *d) {
    if (atomic_load(&d->broken)) return;
    /*
     * Stamp the send-time of the most recent Twrite.  drain_notify is
     * called after every successful p9_write_send, so this slot always
     * holds the timestamp of the latest write at any moment.  After
     * drain_throttle(d, 0) returns this naturally becomes the timestamp
     * of the last write of the just-completed frame.
     */
    atomic_store(&d->last_twrite_send_ms, now_ms());
    atomic_fetch_add(&d->pending, 1);
    drain_wake(d);
}

static void drain_throttle(struct drain_ctx *d, int max_pending) {
    pthread_mutex_lock(&d->lock);
    while (atomic_load(&d->pending) > max_pending && !atomic_load(&d->broken)) {
        pthread_cond_wait(&d->done_cond, &d->lock);
    }
    pthread_mutex_unlock(&d->lock);
}

/* ============== Frame Sending ============== */

void send_frame(struct rio_window *rw) {
    pthread_mutex_lock(&rw->send_lock);
    
    if (rw->resize_pending) {
        pthread_mutex_unlock(&rw->send_lock);
        return;
    }

    /*
     * Find a free buffer.  We have 3 buffers total:
     *   - framebuf     : compositor renders into this
     *   - active_buf   : currently being sent over 9P (owned by send thread)
     *   - pending_buf  : queued, waiting for send thread to pick up
     *
     * In the common case at least one of send_buf[0]/send_buf[1] is free
     * (= not equal to active_buf or pending_buf).
     */
    int buf = -1;
    for (int i = 0; i < 2; i++) {
        if (i != rw->active_buf && i != rw->pending_buf) {
            buf = i;
            break;
        }
    }

    /*
     * Slow-network path: both send_buf[0] and send_buf[1] are taken
     * (one in flight, one queued).
     *
     * The previous behaviour was to drop the new frame.  That's wrong:
     * the new frame is by definition newer than the queued one, and the
     * queued frame represents an interim state the user has already
     * moved past (the "stuck button" symptom — pressed-state frame
     * queued, release-state frames dropped).
     *
     * Instead, OVERWRITE the queued buffer with the new frame contents.
     * This keeps the invariant "what's queued is the most recent frame
     * the compositor has produced" — so even when we drop frames under
     * load, we never drop the *final* frame.
     *
     * Damage bookkeeping: the queued buffer's dirty_tiles bitmap was
     * computed against the active_buf, not against this new frame's
     * predecessor, so we can't trivially merge it.  Force the send
     * thread to fall back to pixel-comparison damage detection by
     * marking dirty_valid[buf] = 0 (and force_full_frame on resize-like
     * events still flows through unchanged via send_full).
     */
    int overwriting = 0;
    if (buf < 0) {
        buf = rw->pending_buf;
        overwriting = 1;
    }

    /*
     * Swap the framebuffer pointer with the chosen send buffer instead of
     * copying.  After the swap the send thread owns what was framebuf
     * (the just-rendered frame) and the compositor gets a recycled
     * buffer to render the next frame into.  All three buffers
     * (framebuf, send_buf[0], send_buf[1]) must be allocated with the
     * same size and alignment for this to be safe.
     *
     * In the overwrite case the recycled buffer is what was previously
     * queued — the send thread never saw it, so this is safe.
     */
    uint32_t *tmp     = rw->send_buf[buf];
    rw->send_buf[buf]  = rw->framebuf;
    rw->framebuf       = tmp;

    /* Copy dirty tile bitmap from staging area if available.
     *
     * Three cases for damage validity:
     *
     *   1. Overwrite path: dirty_staging only describes the latest frame's
     *      deltas, but the queued buffer needs damage cumulative across the
     *      whole dropped chain.  Force pixel-comparison fallback by clearing
     *      dirty_valid[buf] AND set damage_lost so the NEXT non-overwriting
     *      send_frame also forces fallback (it'll have a freshly-reset
     *      dirty_staging that doesn't cover the dropped chain either).
     *
     *   2. Non-overwrite, but damage_lost is set from a prior overwrite:
     *      same problem — dirty_staging was reset by an output_frame that
     *      we then dropped via overwrite, so the staging bitmap doesn't
     *      span the gap.  Force fallback and clear damage_lost.
     *
     *   3. Non-overwrite and damage_lost clear: normal fast path, copy
     *      dirty_staging into dirty_tiles[buf] and mark valid.
     *
     * Without case 2, a slow-network burst followed by a single new frame
     * would leave stale tiles on rio: tiles dirty in the dropped chain but
     * not in the surviving frame would be skipped by the dirty_map filter
     * at send_thread:603.  Currently masked by the headless backend
     * reporting full-screen damage, but exposed by any backend that uses
     * precise damage. */
    if (overwriting) {
        rw->dirty_valid[buf] = 0;
        rw->dirty_staging_valid = 0;
        rw->damage_lost = 1;
    } else if (rw->damage_lost) {
        rw->dirty_valid[buf] = 0;
        rw->dirty_staging_valid = 0;
        rw->damage_lost = 0;
    } else if (rw->dirty_staging_valid) {
        int ntiles = rw->tiles_x * rw->tiles_y;
        if (!rw->dirty_tiles[buf] && ntiles > 0)
            rw->dirty_tiles[buf] = calloc(1, ntiles);
        if (rw->dirty_tiles[buf] && ntiles > 0) {
            memcpy(rw->dirty_tiles[buf], rw->dirty_staging, ntiles);
            rw->dirty_valid[buf] = 1;
        } else {
            rw->dirty_valid[buf] = 0;
        }
        rw->dirty_staging_valid = 0;
    } else {
        rw->dirty_valid[buf] = 0;
    }

    rw->pending_buf = buf;
    if (rw->force_full_frame) rw->send_full = 1;

    /*
     * v16: bind queued wp_presentation_feedback objects to this slot.
     *
     * Any feedbacks that arrived since the last send_frame are sitting
     * in rw->queued_feedbacks (placed there by surface_commit
     * listeners — see wayland/presentation.c).  Move them to
     * rw->buf_feedbacks[buf], where they'll travel with the frame
     * until the send thread reports rio-side delivery.
     *
     * Overwrite case: the slot already has feedbacks from a previous
     * frame whose buffer is being replaced before delivery — those
     * frames are dropped by definition, so fire 'discarded' on them
     * before claiming the slot.  This is the protocol-correct way to
     * tell mpv "your frame at T-N never made it; stop expecting it."
     */
    presentation_claim_slot(rw, buf, overwriting);

    pthread_cond_signal(&rw->send_cond);
    pthread_mutex_unlock(&rw->send_lock);
}

int send_timer_callback(void *data) {
    struct rio_window *rw = data;
    if (!rw->frame_dirty) return 0;
    rw->frame_dirty = 0;
    send_frame(rw);
    return 0;
}

static int scroll_disabled(struct rio_window *rw) {
    double floor_val;
    return (modf(rw->server->scale, &floor_val) != 0.0);
}

void *send_thread_func(void *arg) {
    struct rio_window *rw = arg;
    struct server *s = rw->server;
    struct draw_state *draw = &rw->draw;
    struct p9conn *p9 = draw->p9;
    struct drain_ctx *d = &rw->drain;
    int send_count = 0;
    
    wlr_log(WLR_INFO, "Send thread started");
    
    if (scroll_disabled(rw)) {
        wlr_log(WLR_INFO, "Scroll optimization disabled (fractional scale: %.2f)", s->scale);
    }
    
    /* Determine max batch size from iounit */
    size_t max_batch = draw->iounit ? draw->iounit : (p9->msize - 24);
    if (max_batch > 23) max_batch -= 23;
    
    wlr_log(WLR_INFO, "Send thread: max_batch=%zu", max_batch);
    
    uint8_t *batch = malloc(max_batch);
    if (!batch) return NULL;
    
    if (drain_start(d, p9) < 0) {
        free(batch);
        return NULL;
    }
    
    /* Initialize parallel compression */
    int nthreads = sysconf(_SC_NPROCESSORS_ONLN) / 2;
    if (compress_pool_init(nthreads) < 0) nthreads = 0;
    
    int max_tiles = (4096 / TILE_SIZE) * (4096 / TILE_SIZE);
    struct tile_work *work = malloc(max_tiles * sizeof(*work));
    struct tile_result *results = malloc(max_tiles * sizeof(*results));
    
    /*
     * After a failed relookup, suppress frame sending to avoid the
     * error→relookup→error cascade that can desync the 9P stream.
     * Cleared on next successful relookup or new window_changed event.
     */
    int draw_suspended = 0;
    if (!work || !results) nthreads = 0;
    
    const size_t comp_buf_size = TILE_SIZE * TILE_SIZE * 4 + 256;
    uint8_t *comp_buf = malloc(comp_buf_size);
    
    while (s->running) {
        /* Wait for work — woken by send_frame(), mouse thread (resize),
         * or multiwin_destroy_window() (closing fd sets drain.broken) */
        pthread_mutex_lock(&rw->send_lock);
        while (rw->pending_buf < 0 && !atomic_load(&rw->window_changed) && s->running
               && !atomic_load(&d->broken)) {
            pthread_cond_wait(&rw->send_cond, &rw->send_lock);
        }
        pthread_mutex_unlock(&rw->send_lock);
        if (!s->running) break;

        pthread_mutex_lock(&rw->send_lock);
        int current_buf = rw->pending_buf;
        int got_frame = (current_buf >= 0);
        if (got_frame) {
            rw->active_buf = current_buf;
            rw->pending_buf = -1;
        }
        int do_full = rw->send_full;
        rw->send_full = 0;
        pthread_mutex_unlock(&rw->send_lock);
        
        uint32_t *send_buf = got_frame ? rw->send_buf[current_buf] : NULL;
        
        /*
         * Clear padding strips to ensure deterministic edge tiles.
         *
         * The compositor renders visible_width × visible_height into the
         * top-left of a buffer with stride = width (padded to TILE_SIZE).
         * The right and bottom padding strips may contain stale data from
         * a previous frame (buffers are recycled via pointer swap).
         * Zero them so edge-tile compression sees stable black pixels
         * instead of random changes.
         */
        if (send_buf &&
            (rw->visible_width < rw->width || rw->visible_height < rw->height)) {
            /* Right-edge padding strip */
            if (rw->visible_width < rw->width) {
                int pad = rw->width - rw->visible_width;
                for (int y = 0; y < rw->visible_height; y++)
                    memset(&send_buf[y * rw->width + rw->visible_width], 0,
                           pad * sizeof(uint32_t));
            }
            /* Bottom padding strip (full row width including right padding) */
            if (rw->visible_height < rw->height) {
                memset(&send_buf[rw->visible_height * rw->width], 0,
                       (rw->height - rw->visible_height) * rw->width
                       * sizeof(uint32_t));
            }
        }
        
        /* Handle errors and window changes */
        
        /*
         * If the 9P stream desynced, all I/O on this connection is dead.
         * Don't attempt relookup (it would read garbage), don't send
         * frames (writes would pile up with no drain), just drop the
         * frame and wait.  Recovery requires reconnecting, which is
         * outside the send thread's scope.
         *
         * For secondary windows, just exit the send thread — the main
         * loop will clean up when the toplevel is destroyed.  Do NOT
         * set s->running = 0, as that would kill ALL threads including
         * the primary window's send thread.
         */
        if (atomic_load(&d->broken)) {
            if (!draw_suspended) {
                draw_suspended = 1;
                if (rw == s->primary) {
                    /* In -W mode the user can close the primary's rio
                     * window while secondaries (Firefox, Chromium, ...)
                     * are still alive and serving Wayland clients.  Don't
                     * shoot down the compositor in that case — just exit
                     * this thread, same as a secondary. */
                    if (s->multi_window && has_other_live_windows(s, rw)) {
                        wlr_log(WLR_INFO, "send: primary 9P draw stream broken; "
                                "secondaries still alive, exiting send thread only");
                        /*
                         * Coordinate with mouse/kbd threads.  Last one out
                         * posts INPUT_RIO_WINDOW_DIED so the event loop
                         * promotes a secondary to be the new primary.
                         * being_destroyed check skips the post when this
                         * rw is already being torn down via the toplevel-
                         * close path (multiwin_destroy_window).
                         */
                        if (s->running && !atomic_load(&rw->being_destroyed) &&
                            atomic_fetch_add(&rw->threads_exited, 1) + 1 >= 3) {
                            struct input_event died = { .type = INPUT_RIO_WINDOW_DIED, .window = rw };
                            input_queue_push(&s->input_queue, &died);
                        }
                    } else {
                        wlr_log(WLR_ERROR, "send: primary 9P draw stream broken, shutting down");
                        s->running = 0;
                        /* Terminate the Wayland event loop — without this,
                         * wl_display_run() keeps the compositor alive as a zombie. */
                        wl_display_terminate(s->display);
                        /* Wake main loop so it can exit promptly even if it
                         * is currently blocked outside the event loop. */
                        struct input_event wakeup = { .type = INPUT_WAKEUP, .window = rw };
                        input_queue_push(&s->input_queue, &wakeup);
                    }
                } else {
                    wlr_log(WLR_INFO, "send[win%d]: secondary 9P stream broken, exiting send thread", rw->id);
                    /*
                     * Secondary case: same coordination as above.
                     *
                     * If the secondary was killed externally (rio window
                     * deleted from the Plan 9 side), the toplevel on it is
                     * still alive on the Wayland side but commits go nowhere.
                     * The event loop will migrate it to a live window when
                     * it sees this event.
                     *
                     * If the secondary is being destroyed via the toplevel-
                     * close path, being_destroyed is already 1 and we skip
                     * the post (the destroy path handles cleanup itself).
                     */
                    if (s->running && !atomic_load(&rw->being_destroyed) &&
                        atomic_fetch_add(&rw->threads_exited, 1) + 1 >= 3) {
                        struct input_event died = { .type = INPUT_RIO_WINDOW_DIED, .window = rw };
                        input_queue_push(&s->input_queue, &died);
                    }
                }
            }
            /* Consume and discard any pending error flags */
            atomic_store(&p9->draw_error, 0);
            atomic_store(&p9->unknown_id_error, 0);
            atomic_store(&rw->window_changed, 0);
            atomic_exchange(&d->errors, 0);
            if (got_frame) {
                pthread_mutex_lock(&rw->send_lock);
                rw->active_buf = -1;
                pthread_mutex_unlock(&rw->send_lock);
            }
            break;  /* Exit send thread */
        }
        
        if (atomic_exchange(&p9->draw_error, 0)) {
            draw->xor_enabled = 0;
            memset(rw->prev_framebuf, 0, rw->width * rw->height * 4);
            do_full = 1;
        }
        
        int drain_errs = atomic_exchange(&d->errors, 0);
        if (drain_errs > 0) {
            memset(rw->prev_framebuf, 0xDE, rw->width * rw->height * 4);
            do_full = 1;
        }
        
        if (atomic_exchange(&rw->window_changed, 0)) {
            drain_pause(d);
            if (relookup_window(rw) == 0) {
                draw_suspended = 0;
            } else {
                draw_suspended = 1;
                wlr_log(WLR_INFO, "send: draw suspended until next window change");
            }
            drain_resume(d);
            /*
             * Wake main loop so output_frame fires.  For resize,
             * output_frame consumes resize_pending.  For move,
             * output_frame renders a frame that the send thread
             * can send with updated win_minx/miny coordinates.
             * Without this, the idle scene_dirty gate prevents
             * output_frame from running.
             */
            struct input_event wakeup = { .type = INPUT_WAKEUP, .window = rw };
            input_queue_push(&s->input_queue, &wakeup);
            if (rw->resize_pending) {
                pthread_mutex_lock(&rw->send_lock);
                rw->active_buf = -1;
                pthread_mutex_unlock(&rw->send_lock);
                continue;
            }
            do_full = 1;
        }
        
        if (atomic_exchange(&p9->unknown_id_error, 0)) {
            if (draw_suspended) {
                /* Already suspended — don't hammer relookup, just wait
                 * for the next window_changed event to try again. */
            } else {
                drain_pause(d);
                if (relookup_window(rw) == 0) {
                    draw_suspended = 0;
                } else {
                    draw_suspended = 1;
                    wlr_log(WLR_INFO, "send: draw suspended until next window change");
                }
                drain_resume(d);
            }
            struct input_event wakeup = { .type = INPUT_WAKEUP, .window = rw };
            input_queue_push(&s->input_queue, &wakeup);
            if (rw->resize_pending) {
                pthread_mutex_lock(&rw->send_lock);
                rw->active_buf = -1;
                pthread_mutex_unlock(&rw->send_lock);
                continue;
            }
            do_full = 1;
        }
        
        if (!got_frame) continue;
        if (rw->resize_pending || draw_suspended) {
            pthread_mutex_lock(&rw->send_lock);
            rw->active_buf = -1;
            pthread_mutex_unlock(&rw->send_lock);
            continue;
        }
        if (rw->force_full_frame) {
            do_full = 1;
            rw->force_full_frame = 0;
        }
        
        /* Detect and apply scroll */
        int scrolled_regions = 0;
        if (!do_full && !scroll_disabled(rw)) {
            detect_scroll(rw, send_buf);
            scrolled_regions = apply_scroll_to_prevbuf(rw);
        }
        
        /* Build batch */
        size_t off = 0;
        if (scrolled_regions > 0) {
            off = write_scroll_commands(rw, batch, max_batch);
        }
        
        int tile_count = 0, batch_count = 0;
        int comp_tiles = 0, delta_tiles = 0;
        size_t bytes_raw = 0, bytes_sent = 0;
        int can_delta = draw->xor_enabled && !do_full && rw->prev_framebuf;
        
        /*
         * Use damage-based dirty map when available.  Tiles outside the
         * damage region are skipped entirely (no memory read).  Tiles
         * inside the damage region are assumed changed — the compositor
         * determined these regions were re-rendered, and we track actual
         * content changes via scene_dirty in output_frame so the damage
         * is reliable.  This eliminates the expensive tile_changed()
         * memcmp that was previously the main CPU cost.
         *
         * Falls back to tile_changed() pixel scanning only when no
         * damage info is available (scroll modified prev_framebuf,
         * errors, or allocation failure).
         */
        uint8_t *dirty_map = NULL;
        if (!do_full && scrolled_regions == 0 &&
            rw->dirty_valid[current_buf] && rw->dirty_tiles[current_buf]) {
            dirty_map = rw->dirty_tiles[current_buf];
        }
        
        /* Collect changed tiles */
        int work_count = 0;
        for (int ty = 0; ty < rw->tiles_y; ty++) {
            for (int tx = 0; tx < rw->tiles_x; tx++) {
                int x1, y1, w, h;
                tile_bounds(tx, ty, rw->width, rw->height, &x1, &y1, &w, &h);
                if (w <= 0 || h <= 0) continue;
                
                int changed;
                if (dirty_map) {
                    /* Skip tiles outside damaged region (no memory read) */
                    if (!dirty_map[ty * rw->tiles_x + tx])
                        continue;
                    /* Verify damaged tiles actually changed — the headless
                     * backend reports full-screen damage on every frame,
                     * so many "damaged" tiles are pixel-identical to
                     * prev_framebuf (e.g. a cursor blink marks the whole
                     * screen dirty but only ~2 tiles differ). */
                    changed = tile_changed(send_buf, rw->prev_framebuf,
                                           rw->width, x1, y1, w, h);
                } else {
                    /* Fallback: check every tile */
                    changed = do_full || tile_changed(send_buf, rw->prev_framebuf,
                                                       rw->width, x1, y1, w, h);
                }
                if (!changed) continue;
                
                if (work_count >= max_tiles) break;
                
                /* Check for scroll-exposed region (marked 0xDEADBEEF) */
                int use_delta = can_delta;
                if (use_delta) {
                    int x2m1 = x1 + w - 1, y2m1 = y1 + h - 1;
                    for (int x = x1; x < x1 + w && use_delta; x++) {
                        if (rw->prev_framebuf[y1 * rw->width + x] == 0xDEADBEEF ||
                            rw->prev_framebuf[y2m1 * rw->width + x] == 0xDEADBEEF)
                            use_delta = 0;
                    }
                    for (int y = y1; y <= y2m1 && use_delta; y++) {
                        if (rw->prev_framebuf[y * rw->width + x1] == 0xDEADBEEF ||
                            rw->prev_framebuf[y * rw->width + x2m1] == 0xDEADBEEF)
                            use_delta = 0;
                    }
                }
                
                work[work_count] = (struct tile_work){
                    .pixels = send_buf, .stride = rw->width,
                    .prev_pixels = use_delta ? rw->prev_framebuf : NULL,
                    .prev_stride = rw->width,
                    .x1 = x1, .y1 = y1, .w = w, .h = h
                };
                work_count++;
            }
        }
        
        /* Compress tiles in parallel */
        if (work_count > 0 && nthreads > 0) {
            compress_tiles_parallel(work, results, work_count);
        }
        
        /*
         * v16 (refined): we report a midpoint timestamp for each frame
         * so wp_presentation 'presented' events approximate the moment
         * rio actually applied the frame, not the moment the ACKs got
         * back to us.
         *
         * The midpoint we use is built from drain_ctx scalar slots:
         *   d->last_twrite_send_ms: stamped by drain_notify on every send
         *   d->last_rwrite_ack_ms:  stamped by the drain thread on every ack
         *
         * After drain_throttle(d, 0) returns at the bottom of this
         * iteration, both slots describe the LAST packet of THIS frame
         * (no later writes have been issued — the send thread is
         * blocked in drain_throttle for the first time since starting
         * this frame).  Their midpoint approximates the moment rio
         * applied the frame's tail, regardless of how many tiles the
         * frame had or how RTT varied across the frame.
         *
         * No frame-level bookkeeping is needed here — the synchronization
         * comes from drain_throttle(d, 0) acting as a barrier.
         */
        drain_throttle(d, 2);
        
        /* Build and send batches */
        for (int i = 0; i < work_count; i++) {
            struct tile_work *tw = &work[i];
            struct tile_result *r = &results[i];
            int x1 = tw->x1, y1 = tw->y1;
            int x2 = x1 + tw->w, y2 = y1 + tw->h;
            int raw_size = tw->w * tw->h * 4;
            
            /* Single-threaded fallback */
            if (nthreads == 0) {
                int res = compress_tile_adaptive(comp_buf, comp_buf_size,
                                                  tw->pixels, tw->stride,
                                                  tw->prev_pixels, tw->prev_stride,
                                                  x1, y1, tw->w, tw->h);
                r->is_delta = (res > 0);
                r->size = (res > 0) ? res : (res < 0) ? -res : 0;
                if (r->size > 0) memcpy(r->data, comp_buf, r->size);
            }
            
            bytes_raw += raw_size;
            
            size_t tile_size = (r->size > 0)
                ? (21 + r->size + (r->is_delta ? ALPHA_DELTA_OVERHEAD : 0))
                : (21 + raw_size);
            
            /* Flush if batch full */
            if (off + tile_size > max_batch && off > 0) {
                if (p9_write_send(p9, draw->drawdata_fid, 0, batch, off) < 0) {
                    memset(rw->prev_framebuf, 0xDE, rw->width * rw->height * 4);
                    rw->send_full = 1;
                }
                drain_notify(d);
                batch_count++;
                off = 0;
            }
            
            /* Write tile command */
            if (r->size > 0) {
                uint32_t img_id = r->is_delta ? draw->delta_id : draw->image_id;
                off += cmd_load_hdr(batch + off, img_id, x1, y1, x2, y2);
                memcpy(batch + off, r->data, r->size);
                off += r->size;
                
                if (r->is_delta) {
                    /* Composite delta onto image */
                    off += cmd_draw(batch + off, draw->image_id, draw->delta_id,
                                   draw->delta_id, x1, y1, x2, y2, x1, y1, x1, y1);
                    bytes_sent += r->size + ALPHA_DELTA_OVERHEAD;
                    delta_tiles++;
                } else {
                    bytes_sent += r->size;
                    comp_tiles++;
                }
            } else {
                /* Uncompressed */
                off += cmd_loadraw_hdr(batch + off, draw->image_id, x1, y1, x2, y2);
                for (int row = 0; row < tw->h; row++) {
                    memcpy(batch + off, &send_buf[(y1 + row) * rw->width + x1], tw->w * 4);
                    off += tw->w * 4;
                }
                bytes_sent += raw_size;
            }
            
            /* Update prev_framebuf */
            for (int row = 0; row < tw->h; row++) {
                memcpy(&rw->prev_framebuf[(y1 + row) * rw->width + x1],
                       &send_buf[(y1 + row) * rw->width + x1], tw->w * 4);
            }
            tile_count++;
        }
        
        /*
         * Final batch: COPY-TO-SCREEN + flush.
         *
         * This was previously gated on `tile_count > 0 || scrolled_regions > 0`,
         * the reasoning being "if nothing changed, don't bother emitting a
         * no-op refresh."  That gate was the cause of the long-running
         * "latest frame doesn't reach Plan 9 until I press a key" bug,
         * particularly with terminals like alacritty.
         *
         * The IMAGE → SCREEN relationship in Plan 9 draw is one-way: SCREEN
         * is whatever the last COPY (or external rio activity) put there.
         * When tile_count == 0 the per-tile loop emitted no `d` writes, so
         * IMAGE is unchanged — but SCREEN can still drift away from IMAGE
         * for several reasons we don't fully control:
         *
         *   - rio repaints the window border on focus / activation changes,
         *     which can touch screen pixels adjacent to our image area.
         *   - When our window is occluded and re-exposed rio composites from
         *     the screen image; if the screen is briefly out-of-date we
         *     paint a stale snapshot until the next COPY.
         *   - Plan 9 draw flushimage() semantics: a `d` followed by `f`
         *     (flush) actually pushes pixels to the display.  Without a
         *     periodic flush, the chain of buffered ops on the rio side can
         *     be perceived as "nothing happened."  The previous gate would
         *     starve the flush whenever consecutive frames produced no
         *     differing tiles.
         *
         * For terminals the failure mode is repeatable: many frames produce
         * pixel-identical output to the last sent frame (cursor blink phases
         * that happen to land on the same pixel pattern, idle redraws after
         * the prompt is displayed, framerate-paced no-op renders).  Each of
         * these came in via toplevel_commit → scene_dirty=1 → output_frame
         * → send_frame, but the send-thread ended up emitting no write at
         * all.  When the user finally pressed a key, the cursor moved and
         * tile_count became 1, the gate finally fired, and the COPY-TO-SCREEN
         * inside it refreshed everything that had silently drifted.  Hence
         * the "key press unsticks the screen" symptom.
         *
         * Cost of unconditional refresh: one COPY (~45 bytes) + one flush
         * (~1 byte) per send-thread iteration.  send_thread only runs when
         * scene_dirty was set, so this is bounded by client commit rate
         * (typically capped by frame_done callbacks).  At 60 fps worst case
         * the overhead is < 3 KB/s.  The compression / delta logic and
         * tile-skip filter for individual tiles are unchanged, so the
         * per-frame cost stays small whenever no pixels actually changed.
         */
        if (got_frame) {
            size_t footer_size = 45 + 1;  /* copy-to-screen + flush */
            if (off + footer_size > max_batch && off > 0) {
                if (p9_write_send(p9, draw->drawdata_fid, 0, batch, off) < 0) {
                    memset(rw->prev_framebuf, 0xDE, rw->width * rw->height * 4);
                    rw->send_full = 1;
                }
                drain_notify(d);
                batch_count++;
                off = 0;
            }
            
            /* Copy visible area to window — Plan 9 clips the rest */
            off += cmd_copy(batch + off, draw->screen_id, draw->image_id,
                           draw->opaque_id,
                           draw->win_minx, draw->win_miny,
                           draw->win_minx + draw->visible_width,
                           draw->win_miny + draw->visible_height,
                           0, 0);
            
            /* Flush */
            off += cmd_flush(batch + off);
            
            if (p9_write_send(p9, draw->drawdata_fid, 0, batch, off) < 0) {
                memset(rw->prev_framebuf, 0xDE, rw->width * rw->height * 4);
                rw->send_full = 1;
            }
            drain_notify(d);
            batch_count++;
            
            if (!draw->xor_enabled && tile_count > 0) {
                draw->xor_enabled = 1;
                wlr_log(WLR_INFO, "Alpha-delta mode enabled (win%d)", rw->id);
            }
            
            send_count++;
            if (send_count % 30 == 0) {
                int ratio = bytes_raw > 0 ? (int)(bytes_sent * 100 / bytes_raw) : 100;
                wlr_log(WLR_INFO, "Send[win%d] #%d: %d tiles (%d comp, %d delta) %zu->%zu (%d%%) [%d batches]",
                        rw->id, send_count, tile_count, comp_tiles, delta_tiles,
                        bytes_raw, bytes_sent, ratio, batch_count);
            }

            /*
             * Wait for the final flush write to be acknowledged by rio
             * (drain_throttle blocks until pending == 0), then post a
             * presentation event so the main loop can fire frame_done
             * with an accurate delivery timestamp.
             *
             * Cost: this serializes frame N's completion with the start
             * of frame N+1's writes — losing inter-frame pipelining.
             * Intra-frame pipelining (drain_throttle(d, 2) earlier in
             * this loop) is preserved, so per-frame throughput is
             * unchanged.  In the steady state, this RTT is hidden:
             * clients are blocked on frame_done before they commit
             * frame N+1, so the cond_wait at the top of the loop would
             * have blocked anyway.
             *
             * If the connection died (drain.broken or connection_dead),
             * drain_throttle returns immediately and we still post the
             * wakeup.  Better to fire frame_done with a stale timestamp
             * than freeze the client by withholding the callback — the
             * client will get a wl_display disconnect on cleanup.
             *
             * Pipe write is non-blocking; EAGAIN means the main loop
             * already has a wake pending, which coalesces our event
             * into the queued one.  Lost wakeups are fine because
             * present_handler drains all queued bytes per call.
             */
            /*
             * v16: per-slot completion bookkeeping for wp_presentation.
             *
             * current_buf is the slot we just finished delivering.
             * Stamp its dedicated timestamp slot, OR our bit into the
             * pending mask, then write the wakeup byte.  The main
             * loop's present_handler atomic_exchange's the mask to
             * claim and clear pending slots, then fires
             * wp_presentation_feedback.presented for each with that
             * slot's specific timestamp.
             *
             * Atomics order: timestamp first (so present_handler sees
             * a valid value), then mask (so present_handler knows it's
             * ready to read).  Pipe wakeup last, after all atomics are
             * visible.
             */
            drain_throttle(d, 0);
            /*
             * After drain_throttle(d, 0) returns, the drain_ctx scalar
             * slots describe the last packet of THIS frame:
             *   d->last_twrite_send_ms = when its Twrite went out
             *   d->last_rwrite_ack_ms  = when its Rwrite came back
             * Their midpoint approximates the moment rio applied the
             * frame's tail, regardless of frame size or per-packet RTT
             * variance during the frame.
             *
             * Fallback to current time if either slot is 0 (no writes
             * happened — defensive, shouldn't occur on a got_frame
             * path with non-empty work).
             */
            uint32_t t_send = atomic_load(&d->last_twrite_send_ms);
            uint32_t t_ack  = atomic_load(&d->last_rwrite_ack_ms);
            uint32_t when_ms;
            if (t_send != 0 && t_ack != 0) {
                when_ms = (uint32_t)(((uint64_t)t_send + (uint64_t)t_ack) / 2);
            } else {
                when_ms = now_ms();
            }
            atomic_store(&rw->present_time_ms_slot[current_buf], when_ms);
            atomic_fetch_or(&rw->present_slot_mask, 1 << current_buf);
            if (rw->present_pipe[1] >= 0) {
                char one = 1;
                ssize_t w = write(rw->present_pipe[1], &one, 1);
                (void)w;  /* EAGAIN/EPIPE both intentionally ignored */
            }
        }
        
        pthread_mutex_lock(&rw->send_lock);
        rw->active_buf = -1;
        pthread_mutex_unlock(&rw->send_lock);
    }
    
    drain_stop(d);
    /* Don't call compress_pool_shutdown() here — the pool is shared
     * across all send threads.  It lives for the process lifetime. */
    free(work);
    free(results);
    free(comp_buf);
    free(batch);
    wlr_log(WLR_INFO, "Send thread exiting");
    return NULL;
}
