/*
 * presentation.h - Custom wp_presentation_time implementation
 *
 * Replaces wlroots' wlr_presentation (which fires presented/discarded
 * based on local commit time, not actual rio-side delivery time).
 * This implementation tracks each wp_presentation_feedback resource
 * through three states:
 *
 *   PENDING   feedback() request received, waiting for next surface commit.
 *             Linked in server->pending_feedbacks.  Two listeners active:
 *               - surface->events.commit  (transition to QUEUED)
 *               - surface->events.destroy (transition to discarded)
 *
 *   QUEUED    Surface committed, feedback is bound to a specific
 *             rio_window's next render cycle.  Linked in
 *             rw->queued_feedbacks.  Surface commit listener removed;
 *             surface destroy listener still active.
 *
 *   INFLIGHT  send_frame() chose a slot, feedback now travels with that
 *             send_buf slot until the send thread completes the frame.
 *             Linked in rw->buf_feedbacks[slot].
 *
 * Terminal events:
 *   presented   send thread completed delivery to rio (present_handler
 *               fires) — fires wp_presentation_feedback.presented with
 *               actual delivery time.
 *   discarded   buffer dropped via send_frame() overwrite, surface
 *               destroyed, rio_window destroyed, send pipeline broken.
 *               Fires wp_presentation_feedback.discarded.
 *
 * The protocol contract is that exactly one of presented/discarded fires
 * for each feedback resource.  Once fired, the feedback resource is
 * destroyed (which fires our resource_destroy listener and frees the
 * struct presentation_feedback).
 */

#ifndef P9WL_PRESENTATION_H
#define P9WL_PRESENTATION_H

#include "../types.h"

/*
 * Initialize the wp_presentation global on the wl_display.  Replaces
 * the wlroots built-in.  Must be called after wl_display_create() and
 * before wl_display_run().
 *
 * Returns 0 on success, -1 on failure (wl_global_create failed).
 */
int presentation_init(struct server *s);

/*
 * Destroy the wp_presentation global and discard any pending feedbacks.
 * Call from server_cleanup().  Idempotent.
 */
void presentation_cleanup(struct server *s);

/*
 * Called from send_frame() under rw->send_lock.  Moves all QUEUED
 * feedbacks from rw->queued_feedbacks to rw->buf_feedbacks[slot].
 *
 * If overwriting != 0, the slot already has feedbacks from a previous
 * frame that's about to be dropped — fires discarded on those first.
 */
void presentation_claim_slot(struct rio_window *rw, int slot, int overwriting);

/*
 * Called from present_handler on the main thread when the send thread
 * has completed a frame.  Fires presented on every feedback in the
 * given slot's buf_feedbacks list with the supplied timestamp.
 *
 * when_ms is the CLOCK_MONOTONIC millisecond timestamp the send thread
 * captured at delivery (drain_throttle returned).
 */
void presentation_fire_slot(struct rio_window *rw, int slot, uint32_t when_ms);

/*
 * Called when a rio_window is being destroyed.  Fires discarded on all
 * queued and inflight feedbacks attached to that window.  Must be
 * called BEFORE the rio_window is freed and BEFORE its toplevels
 * migrate, because the feedbacks reference the window directly.
 *
 * Safe to call multiple times — operates on lists which become empty
 * after the first call.
 */
void presentation_window_died(struct rio_window *rw);

#endif /* P9WL_PRESENTATION_H */
