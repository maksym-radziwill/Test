/*
 * multiwin.h - Multi-window protocol for p9wl
 *
 * When enabled with -W, p9wl communicates with an external script
 * (typically an rc shell script on Plan 9) to create additional rio
 * windows for secondary toplevels.
 *
 * Protocol (stdout → script):
 *   NEW_WINDOW id=<N> width=<W> height=<H>
 *   CLOSE_WINDOW id=<N>
 *
 * Protocol (stdin ← script):
 *   READY id=<N> host=<H> port=<P>
 *
 * When -W is not set, secondary toplevels share the primary window.
 */

#ifndef P9WL_MULTIWIN_H
#define P9WL_MULTIWIN_H

#include "types.h"

/*
 * Initialize multi-window support.
 *
 * Sets up stdin as a non-blocking event source on the Wayland event loop.
 * Must be called after wl_display_create().
 *
 * Returns 0 on success, -1 on failure.
 */
int multiwin_init(struct server *s);

/*
 * Request a new rio window for a toplevel.
 *
 * Prints NEW_WINDOW to stdout and marks the toplevel as waiting.
 * The toplevel stays on the primary window until READY arrives.
 *
 * Called from toplevel_commit when a secondary toplevel maps.
 */
void multiwin_request_window(struct server *s, struct toplevel *tl);

/*
 * Notify script that a secondary window is no longer needed.
 *
 * Prints CLOSE_WINDOW to stdout. The script should clean up
 * the rio window and exportfs on the Plan 9 side.
 *
 * Called from toplevel_destroy when a secondary-window toplevel dies.
 */
void multiwin_close_window(struct server *s, struct rio_window *rw);

/*
 * Create a secondary rio_window and connect it.
 *
 * Allocates a new rio_window, connects 9P sessions to the given
 * host:port, initializes draw, allocates buffers, starts threads,
 * and creates a wlroots output.
 *
 * Returns the new rio_window on success, NULL on failure.
 */
struct rio_window *multiwin_create_window(struct server *s, int id,
                                          const char *host, int port);

/*
 * Migrate a toplevel from its current window to a new one.
 *
 * Moves the toplevel's scene node to the new output and
 * sends a configure with the new window's dimensions.
 */
void multiwin_migrate_toplevel(struct toplevel *tl, struct rio_window *rw);

/*
 * Destroy a secondary rio_window and free all resources.
 *
 * Tears down threads (by closing 9P fds to unblock reads), removes
 * wlroots scene nodes and output, frees buffers, and removes the
 * window from the server's list.
 *
 * Must NOT be called on the primary window.
 * Called from toplevel_destroy when a secondary-window toplevel dies.
 */
void multiwin_destroy_window(struct server *s, struct rio_window *rw);

/*
 * Recalculate layout positions for all rio windows.
 *
 * Called after any window resize to ensure outputs don't overlap.
 * Repositions windows left-to-right based on current visible widths,
 * updating wlr_output_layout, scene trees, and backgrounds.
 */
void multiwin_relayout(struct server *s);

/*
 * Check whether any rio window other than `self` is still alive.
 *
 * Used by the primary's worker threads (mouse/kbd/send) to decide
 * whether a broken 9P stream should terminate the whole compositor
 * or just exit the affected threads.  In multi-window mode, closing
 * the primary's rio window must NOT kill secondary windows that are
 * still serving Wayland clients (e.g. Firefox, Chromium).
 *
 * "Alive" means the rio window struct is still in s->rio_windows and
 * is not the one being torn down.  Returns 1 if any other window is
 * present, 0 otherwise.
 */
int has_other_live_windows(struct server *s, struct rio_window *self);

/*
 * Promote a surviving secondary to be the new s->primary.
 *
 * Called by multiwin_handle_window_died() when the dead window is the
 * current s->primary.  See multiwin_handle_window_died for the calling
 * convention; do not call this directly from worker threads.
 *
 * Steps:
 *   1. Pick a non-primary rio_window as the new primary, preferring
 *      one whose worker threads are still alive.
 *   2. Swap s->primary to the new primary.
 *   3. Migrate any toplevels still pointing at the old primary onto
 *      the new one (and re-point any popups that cached the old rw).
 *   4. Tear down the old primary via multiwin_destroy_window.
 *
 * Must run on the wlroots event loop thread.  If no surviving window
 * exists, terminates the compositor.
 */
void multiwin_promote_primary(struct server *s);

/*
 * Handle a rio_window whose worker threads have all exited because its
 * 9P connections broke (typically: user deleted the rio window from
 * the Plan 9 side via rio's right-click delete or `wctl delete`).
 *
 * Dispatches to:
 *   - multiwin_promote_primary(s) if rw == s->primary
 *   - cleanup of the dead secondary otherwise (migrate any toplevels
 *     to a live window, destroy the dead rio_window)
 *
 * Idempotent: if rw was already removed from s->rio_windows by a
 * concurrent path (e.g. toplevel_destroy), the call returns without
 * doing anything.  This makes it safe to invoke from
 * INPUT_RIO_WINDOW_DIED handling without racing the normal close path.
 *
 * Must be called only from the wlroots event loop thread.
 */
void multiwin_handle_window_died(struct server *s, struct rio_window *rw);

#endif /* P9WL_MULTIWIN_H */
