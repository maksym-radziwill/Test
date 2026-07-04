/*
 * output.c - Output creation, frame rendering, and resize handling
 *
 * Creates the headless wlroots output sized to match the Plan 9 window,
 * runs the frame loop that renders the scene graph into a framebuffer
 * for the send thread, and handles dynamic window resizes.
 *
 * See output.h for the frame loop description and damage tracking design.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "output.h"
#include "../draw/send.h"
#include "../draw/draw_cmd.h"
#include "../p9/p9.h"

static void output_destroy(struct wl_listener *listener, void *data) {
    struct rio_window *rw = wl_container_of(listener, rw, output_destroy);
    (void)data;
    wl_list_remove(&rw->output_frame.link);
    wl_list_remove(&rw->output_destroy.link);
}

/*
 * Reallocate Plan 9 draw images after resize.
 * Uses alloc_image_cmd helper instead of manual byte construction.
 */
static void reallocate_draw_images(struct draw_state *draw, int new_w, int new_h) {
    struct p9conn *p9 = draw->p9;
    uint8_t cmd[64];
    int off;
    
    /* Free old images */
    off = free_image_cmd(cmd, draw->image_id);
    p9_write(p9, draw->drawdata_fid, 0, cmd, off);
    
    off = free_image_cmd(cmd, draw->delta_id);
    p9_write(p9, draw->drawdata_fid, 0, cmd, off);
    
    /* Reallocate framebuffer image (XRGB32) */
    off = alloc_image_cmd(cmd, draw->image_id, CHAN_XRGB32, 0,
                          0, 0, new_w, new_h, 0x00000000);
    p9_write(p9, draw->drawdata_fid, 0, cmd, off);
    
    /* Reallocate delta image (ARGB32 for alpha compositing) */
    off = alloc_image_cmd(cmd, draw->delta_id, CHAN_ARGB32, 0,
                          0, 0, new_w, new_h, 0x00000000);
    p9_write(p9, draw->drawdata_fid, 0, cmd, off);
}

static void output_frame(struct wl_listener *listener, void *data) {
    struct rio_window *rw = wl_container_of(listener, rw, output_frame);
    struct server *s = rw->server;
    struct wlr_scene_output *so = rw->scene_output;
    static int frame_count = 0;
    (void)data;
    
    /* Check if window was resized - read atomically under lock */
    pthread_mutex_lock(&rw->send_lock);
    int resize_pending = rw->resize_pending;
    int new_w = rw->pending_width;
    int new_h = rw->pending_height;
    int new_vis_w = rw->pending_visible_width;
    int new_vis_h = rw->pending_visible_height;
    int new_minx = rw->pending_minx;
    int new_miny = rw->pending_miny;
    if (resize_pending) {
        rw->resize_pending = 0;
    }
    pthread_mutex_unlock(&rw->send_lock);
    
    if (resize_pending) {
        if (new_w == rw->width && new_h == rw->height) {
            struct draw_state *draw = &rw->draw;
            draw->win_minx = new_minx;
            draw->win_miny = new_miny;
            /* Visible dimensions may change even without padding change */
            rw->visible_width = new_vis_w;
            rw->visible_height = new_vis_h;
            draw->visible_width = new_vis_w;
            draw->visible_height = new_vis_h;
            /* Need a render so the send thread gets a frame with updated
             * draw coordinates (win_minx/win_miny). */
            rw->scene_dirty = 1;
            rw->force_full_frame = 1;
            wlr_log(WLR_DEBUG, "Position update only: (%d,%d)", new_minx, new_miny);
        } else {
            wlr_log(WLR_INFO, "Main thread handling resize: %dx%d -> %dx%d (visible %dx%d)",
                    rw->width, rw->height, new_w, new_h, new_vis_w, new_vis_h);
            
            size_t fb_size = new_w * new_h * sizeof(uint32_t);
            uint32_t *new_framebuf = calloc(1, fb_size);
            uint32_t *new_prev_framebuf = calloc(1, fb_size);
            uint32_t *new_send_buf0 = calloc(1, fb_size);
            uint32_t *new_send_buf1 = calloc(1, fb_size);
            
            if (!new_framebuf || !new_prev_framebuf || !new_send_buf0 || !new_send_buf1) {
                wlr_log(WLR_ERROR, "Resize failed: could not allocate buffers");
                free(new_framebuf);
                free(new_prev_framebuf);
                free(new_send_buf0);
                free(new_send_buf1);
            } else {
                uint32_t *old_framebuf = rw->framebuf;
                uint32_t *old_prev_framebuf = rw->prev_framebuf;
                uint32_t *old_send_buf0, *old_send_buf1;
                struct draw_state *draw = &rw->draw;
                
                pthread_mutex_lock(&rw->send_lock);
                old_send_buf0 = rw->send_buf[0];
                old_send_buf1 = rw->send_buf[1];
                
                rw->framebuf = new_framebuf;
                rw->prev_framebuf = new_prev_framebuf;
                rw->send_buf[0] = new_send_buf0;
                rw->send_buf[1] = new_send_buf1;
                rw->pending_buf = -1;
                rw->active_buf = -1;
                
                rw->width = new_w;
                rw->height = new_h;
                rw->visible_width = new_vis_w;
                rw->visible_height = new_vis_h;
                /* Padded dimensions are always exact multiples of TILE_SIZE */
                rw->tiles_x = new_w / TILE_SIZE;
                rw->tiles_y = new_h / TILE_SIZE;
                
                /* Reallocate dirty tile bitmaps */
                uint8_t *old_dirty0 = rw->dirty_tiles[0];
                uint8_t *old_dirty1 = rw->dirty_tiles[1];
                int ntiles = rw->tiles_x * rw->tiles_y;
                rw->dirty_tiles[0] = ntiles > 0 ? calloc(1, ntiles) : NULL;
                rw->dirty_tiles[1] = ntiles > 0 ? calloc(1, ntiles) : NULL;
                rw->dirty_valid[0] = 0;
                rw->dirty_valid[1] = 0;

                /* Reallocate staging buffer under send_lock.
                 *
                 * Previously this lived OUTSIDE the lock (after the unlock at
                 * the end of this block) with a comment claiming it was safe
                 * because the output thread is sole writer.  But send_frame
                 * reads dirty_staging (and tiles_x/tiles_y to size the read)
                 * under send_lock — if send_frame ever runs after we unlock
                 * but before we free/realloc, it would memcpy from a stale
                 * pointer using the new dimensions, or worse, after free.
                 *
                 * Today both run on the wlroots main thread so they can't
                 * interleave, but the locking discipline shouldn't depend on
                 * an undocumented same-thread invariant.  Move it inside.
                 */
                uint8_t *old_dirty_staging = rw->dirty_staging;
                rw->dirty_staging = ntiles > 0 ? calloc(1, ntiles) : NULL;
                rw->dirty_staging_valid = 0;

                /* Clear damage_lost: the buffers are now fresh and forced
                 * full-frame on the next send anyway (force_full_frame is
                 * set elsewhere in the resize path), so there is no
                 * historical damage to preserve. */
                rw->damage_lost = 0;

                draw->width = new_w;
                draw->height = new_h;
                draw->visible_width = new_vis_w;
                draw->visible_height = new_vis_h;
                draw->win_minx = new_minx;
                draw->win_miny = new_miny;
                pthread_mutex_unlock(&rw->send_lock);

                free(old_framebuf);
                free(old_prev_framebuf);
                free(old_send_buf0);
                free(old_send_buf1);
                free(old_dirty0);
                free(old_dirty1);
                free(old_dirty_staging);

                /* Reallocate Plan 9 images using helper */
                reallocate_draw_images(draw, new_w, new_h);
                
                /* Resize wlroots output to VISIBLE dimensions.
                 * The wlroots buffer will be visible_width × visible_height;
                 * the padded framebuf is larger (stride = width). */
                struct wlr_output_state state;
                wlr_output_state_init(&state);
                wlr_output_state_set_custom_mode(&state, new_vis_w, new_vis_h, 0);
                if (s->scale > 1.0f) {
                    wlr_output_state_set_scale(&state, s->scale);
                }
                wlr_output_commit_state(rw->output, &state);
                wlr_output_state_finish(&state);
                
                /* Compute logical dimensions from VISIBLE size for Wayland clients */
                int logical_w = (int)(new_vis_w / s->scale + 0.5f);
                int logical_h = (int)(new_vis_h / s->scale + 0.5f);
                
                /* Send configure to toplevels on THIS window only */
                struct toplevel *tl;
                wl_list_for_each(tl, &s->toplevels, link) {
                    if (tl->window == rw && tl->xdg && tl->xdg->base && tl->xdg->base->initialized) {
                        wlr_xdg_toplevel_set_size(tl->xdg, logical_w, logical_h);
                    }
                }
                
                /* Resize background (scene uses logical coordinates) */
                if (rw->background) {
                    wlr_scene_rect_set_size(rw->background, logical_w, logical_h);
                }
                
                draw->xor_enabled = 0;
                rw->force_full_frame = 1;
                rw->scene_dirty = 1;
                
                wlr_log(WLR_INFO, "Resize complete: %dx%d visible (%dx%d padded), %dx%d logical at (%d,%d)",
                        new_vis_w, new_vis_h, new_w, new_h,
                        logical_w, logical_h, new_minx, new_miny);
            }
        }
    }
    
    uint32_t now = now_ms();
#if FRAME_INTERVAL_MS > 0
    if (now - rw->last_frame_ms < FRAME_INTERVAL_MS) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        wlr_scene_output_send_frame_done(so, &ts);
        return;
    }
#endif
    rw->last_frame_ms = now;
    frame_count++;
    
    /*
     * Skip rendering entirely when the scene hasn't changed.
     * scene_dirty is set by toplevel_commit/subsurface_commit when a
     * client submits new content, and by the resize handler above.
     *
     * force_full_frame is NOT checked here — it only controls whether
     * the send thread does a full send vs delta.  Every code path that
     * sets force_full_frame also sets scene_dirty, so we don't need it
     * as a render gate.  Using it here caused an infinite render loop:
     * force_full_frame stayed set (cleared by send thread on a different
     * cadence) and kept bypassing the idle gate at 60fps.
     */
    if (!rw->scene_dirty) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        wlr_scene_output_send_frame_done(so, &ts);
        return;
    }
    wlr_log(WLR_DEBUG, "output_frame[win%d]: rendering (dirty=%d full=%d)",
            rw->id, rw->scene_dirty, rw->force_full_frame);
    rw->scene_dirty = 0;
    
    struct wlr_output_state ostate;
    wlr_output_state_init(&ostate);
    struct wlr_scene_output_state_options opts = {0};
    
    if (!wlr_scene_output_build_state(so, &ostate, &opts)) {
        if (frame_count <= 10 || frame_count % 60 == 0) {
            wlr_log(WLR_DEBUG, "Frame %d: build_state failed", frame_count);
        }
        wlr_output_state_finish(&ostate);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        wlr_scene_output_send_frame_done(so, &ts);
        return;
    }
    
    rw->dirty_staging_valid = 0;  /* Reset; set below if damage extracted */
    int has_dirty = 0;           /* Set if any damage rects found */
    
    struct wlr_buffer *buffer = ostate.buffer;
    if (buffer) {
        void *data_ptr;
        uint32_t format;
        size_t stride;
        
        if (wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                              &data_ptr, &format, &stride)) {
            int w = rw->width;
            int h = rw->height;
            uint32_t *fb = rw->framebuf;
            int valid_fb = (fb && w > 0 && h > 0 && w <= MAX_SCREEN_DIM && h <= MAX_SCREEN_DIM);
            
            /*
             * Extract damage BEFORE copying pixels.  This lets us skip
             * the buffer copy entirely on idle frames (nrects == 0) and
             * copy only damaged rows on active frames.
             *
             * We read ostate.damage unconditionally rather than checking
             * ostate.committed & WLR_OUTPUT_STATE_DAMAGE.  The committed
             * flag tracks fields set by the caller, not fields populated
             * by wlr_scene_output_build_state().  wlr_output_state_init()
             * initializes damage empty, and the scene builder fills it
             * with actual changed regions.
             */
            int nrects = 0;
            pixman_box32_t *rects = pixman_region32_rectangles(
                &ostate.damage, &nrects);
            
            /* Build dirty tile bitmap.
             *
             * Writes here are NOT under send_lock, even though send_frame
             * reads dirty_staging/dirty_staging_valid under send_lock.
             * This relies on output_frame and send_frame both running on
             * the wlroots main thread — they're invoked from the same
             * event loop, never concurrently.  Resize is the only place
             * where dimensions change, and that path takes send_lock for
             * the realloc (see the resize block above).
             *
             * If anything in the future schedules send_frame from another
             * thread (e.g., a 9P-side reconnect helper that synthesizes
             * a frame), this becomes a real race and these writes need
             * to move under send_lock too. */
            if (!rw->dirty_staging && rw->tiles_x > 0 && rw->tiles_y > 0)
                rw->dirty_staging = calloc(1, rw->tiles_x * rw->tiles_y);
            if (rw->dirty_staging && rw->tiles_x > 0 && rw->tiles_y > 0) {
                int ntiles = rw->tiles_x * rw->tiles_y;
                memset(rw->dirty_staging, 0, ntiles);
                for (int r = 0; r < nrects; r++) {
                    int tx0 = rects[r].x1 / TILE_SIZE;
                    int ty0 = rects[r].y1 / TILE_SIZE;
                    int tx1 = (rects[r].x2 + TILE_SIZE - 1) / TILE_SIZE;
                    int ty1 = (rects[r].y2 + TILE_SIZE - 1) / TILE_SIZE;
                    if (tx0 < 0) tx0 = 0;
                    if (ty0 < 0) ty0 = 0;
                    if (tx1 > rw->tiles_x) tx1 = rw->tiles_x;
                    if (ty1 > rw->tiles_y) ty1 = rw->tiles_y;
                    for (int ty = ty0; ty < ty1; ty++)
                        for (int tx = tx0; tx < tx1; tx++)
                            rw->dirty_staging[ty * rw->tiles_x + tx] = 1;
                }
                rw->dirty_staging_valid = 1;
                has_dirty = (nrects > 0);
            }
            
            /*
             * Copy rendered pixels from wlroots buffer to framebuf.
             *
             * This runs only when scene_dirty was set (a client committed
             * new content), so on an idle screen this code never executes.
             * We always do a full-frame copy because:
             *
             *   - send_frame() swaps framebuf with a recycled send_buf,
             *     so the new framebuf has stale data from ~2 frames ago.
             *     A partial copy would leave stale rows in the buffer.
             *
             *   - The send thread copies per-tile data from send_buf
             *     into prev_framebuf (line ~544). If send_buf has stale
             *     rows from incomplete copy, prev_framebuf gets corrupted,
             *     breaking XOR delta encoding.
             *
             *   - Damage rects are pixel-precise but tiles are 16x16.
             *     Copying only damaged rows can leave stale data within
             *     a tile that the send thread reads in full.
             *
             * The scene_dirty check above already ensures we skip idle
             * frames entirely, so the full copy only runs when content
             * actually changed — not 60 times per second.
             */
            if (valid_fb) {
                int buf_w = buffer->width;
                int buf_h = buffer->height;
                int vis_w = rw->visible_width;
                int vis_h = rw->visible_height;
                /* Copy min of buffer and visible dims; framebuf stride is w (padded) */
                int copy_w = (buf_w < vis_w) ? buf_w : vis_w;
                int copy_h = (buf_h < vis_h) ? buf_h : vis_h;
                
                pthread_mutex_lock(&rw->send_lock);
                for (int y = 0; y < copy_h; y++) {
                    memcpy(&fb[y * w],
                           (uint8_t*)data_ptr + y * stride,
                           copy_w * 4);
                }
                pthread_mutex_unlock(&rw->send_lock);
            }
            
            wlr_buffer_end_data_ptr_access(buffer);
        } else {
            if (frame_count <= 10 || frame_count % 60 == 0)
                wlr_log(WLR_ERROR, "Frame %d: buffer access failed", frame_count);
        }
    } else {
        if (frame_count <= 10 || frame_count % 60 == 0)
            wlr_log(WLR_DEBUG, "Frame %d: no buffer in state", frame_count);
    }
    
    wlr_output_commit_state(rw->output, &ostate);
    wlr_output_state_finish(&ostate);
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    wlr_scene_output_send_frame_done(so, &ts);
    
    /*
     * Only wake the send thread when there's actual work:
     *   - force_full_frame: resize/error recovery needs full resend
     *   - has_dirty: compositor reported pixel changes
     *   - !dirty_staging_valid: damage extraction failed (alloc error),
     *     send thread must fall back to pixel scanning
     */
    if (rw->force_full_frame || has_dirty || !rw->dirty_staging_valid) {
        wlr_log(WLR_DEBUG, "output_frame[win%d]: sending (full=%d dirty=%d staging=%d)",
                rw->id, rw->force_full_frame, has_dirty, rw->dirty_staging_valid);
        send_frame(rw);
    }
}

void new_output(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_output);
    struct rio_window *rw = s->pending_rio_window;
    struct wlr_output *out = d;
    
    /* new_output can fire during wlr_headless_add_output or during
     * wlr_backend_start (wlroots 0.19 fires it at start). Skip if
     * no pending window or if output already assigned. */
    if (!rw || rw->output) return;
    
    wlr_output_init_render(out, s->allocator, s->renderer);
    
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, rw->visible_width, rw->visible_height, 60000);
    if (s->scale > 1.0f) {
        wlr_output_state_set_scale(&state, s->scale);
    }
    wlr_output_commit_state(out, &state);
    wlr_output_state_finish(&state);
    
    /* Assign output BEFORE layout_add so the rw->output guard is live
     * before wlr_output_layout_add fires callbacks through
     * wlr_scene_attach_output_layout.  Without this, a re-entrant or
     * deferred new_output could see rw->output == NULL and try to
     * claim a second output for the same rio_window. */
    rw->output = out;
    s->pending_rio_window = NULL;

    wlr_output_layout_add(s->output_layout, out, rw->layout_x, rw->layout_y);

    /* wlr_scene_attach_output_layout auto-creates a wlr_scene_output at the
     * correct layout position when wlr_output_layout_add fires above.
     * Look it up — do NOT call wlr_scene_output_create or we get a
     * duplicate at (0,0) which renders from the wrong viewport. */
    rw->scene_output = wlr_scene_get_scene_output(s->scene, out);
    if (!rw->scene_output) {
        /* Fallback for wlroots versions where auto-creation might not work */
        wlr_log(WLR_ERROR, "scene_output not auto-created, creating manually at (%d,%d)",
                rw->layout_x, rw->layout_y);
        rw->scene_output = wlr_scene_output_create(s->scene, out);
        wlr_scene_output_set_position(rw->scene_output, rw->layout_x, rw->layout_y);
    }
    wlr_log(WLR_INFO, "Output added at layout (%d,%d), scene_output=%p",
            rw->layout_x, rw->layout_y, (void*)rw->scene_output);
    
    rw->output_frame.notify = output_frame;
    wl_signal_add(&out->events.frame, &rw->output_frame);
    rw->output_destroy.notify = output_destroy;
    wl_signal_add(&out->events.destroy, &rw->output_destroy);
    
    if (s->scale > 1.0f) {
        int logical_w = (int)(rw->visible_width / s->scale + 0.5f);
        int logical_h = (int)(rw->visible_height / s->scale + 0.5f);
        wlr_log(WLR_INFO, "Output ready: %dx%d visible (%dx%d padded), scale=%.2f, %dx%d logical",
                rw->visible_width, rw->visible_height, rw->width, rw->height,
                s->scale, logical_w, logical_h);
    } else {
        wlr_log(WLR_INFO, "Output ready: %dx%d visible (%dx%d padded)",
                rw->visible_width, rw->visible_height, rw->width, rw->height);
    }
}

void new_input(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_input);
    struct wlr_input_device *dev = d;
    if (dev->type == WLR_INPUT_DEVICE_POINTER)
        wlr_cursor_attach_input_device(s->cursor, dev);
}
