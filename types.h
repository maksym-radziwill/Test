/*
 * types.h - Shared type definitions for p9wl
 *
 * This is the AUTHORITATIVE source for:
 *   - TILE_SIZE constant (used by compression, scroll detection, rendering)
 *   - MAX_SCROLL_REGIONS constant
 *   - Core structs: server, rio_window, draw_state, toplevel, input_queue, etc.
 *   - Time utility functions: now_ms(), now_us()
 *
 * Include this header (directly or indirectly) before any draw/compress headers.
 *
 * Dependency Notes:
 *
 *   - This header includes p9.h and focus_manager.h
 *   - Do not include input.h from here (would create circular dependency)
 *   - Input-related code should include input.h which includes this file
 *
 * Usage:
 *
 *   Most source files should include this header indirectly through
 *   other headers (e.g., wayland/wayland.h). Direct inclusion is
 *   appropriate for core infrastructure code.
 */

#ifndef P9WL_TYPES_H
#define P9WL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>   /* Required for clock_gettime, struct timespec in now_ms/now_us */
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/util/log.h>

/* Include p9.h for struct p9conn definition (with TLS support) */
#include "p9/p9.h"

/* Include focus_manager for unified focus state machine */
#include "wayland/focus_manager.h"

/* ============== Configuration Constants ============== */

/*
 * TILE_SIZE - Tile dimension for compression and change detection.
 *
 * This is the ONLY definition. Do not redefine elsewhere.
 * Value of 16 balances compression efficiency with granularity:
 *   - Smaller values = finer change detection but more overhead
 *   - Larger values = coarser detection but less metadata
 */
#define TILE_SIZE           16

/*
 * MAX_SCROLL_REGIONS - Maximum number of scroll detection regions.
 *
 * Screen is divided into a grid; each cell is a scroll region.
 * This caps memory usage for the scroll_regions array in struct rio_window.
 *
 * detect_scroll computes cols = width/256 and rows = height/256, so the
 * total region count grows with screen area.  256 covers up to ~5K
 * resolution (5120x2880 -> 220 regions); for displays beyond that the
 * grid is capped in detect_scroll() so we never overrun the array.
 */
#define MAX_SCROLL_REGIONS  256

/*
 * MAX_SCREEN_DIM - Maximum supported screen dimension.
 *
 * Limits framebuffer allocation. 8K resolution should be sufficient
 * for current and near-future displays.
 */
#define MAX_SCREEN_DIM      8192

/*
 * FRAME_INTERVAL_MS - Minimum milliseconds between frames.
 *
 * Set to 0 to disable throttling (render every frame).
 * Non-zero values can reduce CPU usage at cost of latency.
 */
#define FRAME_INTERVAL_MS   0

/*
 * INPUT_QUEUE_SIZE - Maximum pending input events.
 *
 * Ring buffer size for mouse/keyboard events from input threads.
 * Should be large enough to handle bursts without dropping.
 */
#define INPUT_QUEUE_SIZE    256

/*
 * SCROLL_REGION_SIZE - Dimension of each scroll detection region.
 *
 * Used to divide the screen into a grid for regional scroll detection.
 */
#define SCROLL_REGION_SIZE  512

/* ============== Forward Declarations ============== */

struct server;
struct rio_window;
struct draw_state;
struct toplevel;
struct presentation_feedback;   /* Defined in wayland/presentation.c */
/* Note: struct popup_data is defined in focus_manager.h */

/* ============== Input Event Types ============== */

/*
 * Input event types for the cross-thread input queue.
 */
enum input_type {
    INPUT_MOUSE,             /* Mouse movement or button event */
    INPUT_KEY,               /* Keyboard press or release */
    INPUT_WAKEUP,            /* Wake main loop and schedule a frame (resize, etc.) */
    INPUT_RIO_WINDOW_DIED    /* All worker threads on ev.window have exited
                              * because the rio window's 9P connections broke
                              * (typically: user deleted the rio window from
                              * the Plan 9 side).  Triggers cleanup in the
                              * event loop:
                              *   - if ev.window is s->primary, promote a
                              *     surviving secondary to be the new primary.
                              *   - otherwise migrate any toplevels off it
                              *     to a live window and destroy it. */
};

/*
 * Input event structure for cross-thread communication.
 *
 * Pushed by mouse_thread and kbd_thread, popped by main loop.
 */
struct input_event {
    int type;       /* INPUT_MOUSE, INPUT_KEY, or INPUT_WAKEUP */
    struct rio_window *window;  /* Which rio window this event came from */
    union {
        struct {
            int x, y;       /* Absolute position in window coordinates */
            int buttons;    /* Button mask: bit 0=left, 1=middle, 2=right */
        } mouse;
        struct {
            int rune;       /* Plan 9 rune (Unicode codepoint or Kxxx) */
            int pressed;    /* 1 = key press, 0 = key release */
        } key;
    };
};

/*
 * Thread-safe input queue.
 *
 * Uses a ring buffer with mutex protection. The pipe_fd is used
 * to wake the main Wayland event loop when events are available.
 */
struct input_queue {
    struct input_event events[INPUT_QUEUE_SIZE];
    int head, tail;         /* Ring buffer indices */
    pthread_mutex_t lock;   /* Protects head/tail */
    int pipe_fd[2];         /* [0]=read, [1]=write; for waking handle_input_events */
};

/* ============== Draw State ============== */

/*
 * Draw state for Plan 9 /dev/draw operations.
 *
 * Tracks all the file descriptors and image IDs needed to render
 * frames to a Plan 9 window.
 */
struct draw_state {
    struct p9conn *p9;          /* 9P connection for draw operations */
    struct p9conn *p9_relookup; /* Separate 9P connection for relookup */
    uint32_t draw_fid;          /* fid for /dev/draw directory */
    uint32_t drawnew_fid;       /* fid for /dev/draw/new */
    uint32_t drawdata_fid;      /* fid for /dev/draw/N/data */
    uint32_t drawctl_fid;       /* fid for /dev/draw/N/ctl */
    uint32_t winname_fid;       /* fid for /dev/winname (kept open for resize) */
    /* Relookup fids — on p9_relookup connection, isolated from send/drain */
    uint32_t relookup_draw_fid;    /* fid for /dev/draw on relookup conn */
    uint32_t relookup_data_fid;    /* fid for /dev/draw/N/data on relookup conn */
    uint32_t relookup_ctl_fid;     /* fid for /dev/draw/N/ctl on relookup conn */
    uint32_t relookup_winname_fid; /* fid for /dev/winname on relookup conn */
    int client_id;              /* Draw client ID (from /dev/draw/new) */
    int screen_id;              /* Screen image ID */
    int image_id;               /* Our offscreen buffer (accumulates via XOR) */
    int opaque_id;              /* 1x1 white replicated image for mask */
    int delta_id;               /* Temp image for receiving XOR deltas */
    int width, height;          /* Padded buffer dimensions (TILE_ALIGN_UP) */
    int visible_width;          /* Actual window width (what compositor renders) */
    int visible_height;         /* Actual window height (what compositor renders) */
    int win_minx, win_miny;     /* Window origin for coordinate translation */

    /*
     * Logical (Wayland) dimensions and scale factor.
     *
     * Wayland clients operate in logical coordinates; the framebuffer
     * uses physical pixels. These track the draw-side view of the
     * HiDPI state so that coordinate conversion during rendering
     * doesn't need to reach back into struct server.
     *
     * Kept in sync with server.scale by output resize handling.
     * width/height above are physical; logical = physical / scale.
     */
    int logical_width;          /* width / scale (for Wayland configure) */
    int logical_height;         /* height / scale (for Wayland configure) */
    float scale;                /* Copy of server.scale for draw operations */

    char winname[64];           /* Window name for re-querying geometry */
    int winimage_id;            /* Image ID assigned to the window */
    int xor_enabled;            /* Whether XOR delta mode is active */
    uint32_t iounit;            /* Maximum I/O size for this connection */
};

/* ============== Surface Tracking ============== */

/*
 * Subsurface tracking for Wayland subsurfaces.
 *
 * Wayland subsurfaces are child surfaces that move with their parent.
 * We track them to ensure proper rendering and cleanup.
 */
struct subsurface_track {
    struct wl_list link;            /* Link in toplevel->subsurfaces list */
    struct wlr_subsurface *subsurface;
    struct wl_listener destroy;     /* Cleanup when subsurface destroyed */
    struct wl_listener commit;      /* Track content changes and map/unmap */
    struct server *server;
    struct toplevel *toplevel;      /* Parent toplevel */
    bool mapped;                    /* Track mapped state */
};

/*
 * Toplevel window tracking.
 *
 * Represents a top-level application window (xdg_toplevel in Wayland).
 */
struct toplevel {
    struct wl_list link;            /* Link in server->toplevels list */
    struct wlr_xdg_toplevel *xdg;   /* The wlroots toplevel object */
    struct wlr_scene_tree *scene_tree;
    struct wlr_surface *surface;    /* Stored for focus cleanup in destroy */
    struct wl_listener commit;      /* Handle surface commits */
    struct wl_listener destroy;     /* Cleanup on xdg_surface destroy */
    struct wl_listener xdg_destroy; /* Cleanup on xdg_toplevel destroy (fires first) */
    struct wl_listener request_fullscreen;  /* Fullscreen state toggle */
    struct wl_listener request_maximize;    /* Maximize state toggle */
    struct wl_listener request_minimize;    /* Minimize blocked (no restore path) */
    struct wl_list subsurfaces;     /* List of subsurface_track */
    struct server *server;
    struct rio_window *window;      /* Which rio window this toplevel is on */
    int pending_window_id;          /* Waiting for NEW_WINDOW response (0 = not waiting) */
    bool configured;                /* Have we sent initial configure? */
    bool mapped;                    /* Is surface currently mapped? */
    int commit_count;               /* Per-toplevel commit counter */
};

/* ============== Drain Context ============== */

/*
 * Per-window drain thread context.
 *
 * The drain thread reads 9P Rwrite responses asynchronously,
 * allowing pipelined writes without blocking the send thread.
 * Each rio_window has its own drain_ctx so multiple windows
 * don't interfere with each other.
 */
struct drain_ctx {
    struct p9conn *p9;
    atomic_int pending;
    atomic_int errors;
    atomic_int running;
    atomic_int paused;
    atomic_int broken;      /* Stream desynced — stop all I/O */
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;        /* Wakes drain thread when work available */
    pthread_cond_t done_cond;   /* Signaled when pending decreases */
    uint8_t *recv_buf;

    /*
     * Last-packet timestamps for wp_presentation midpoint correction.
     *
     * last_twrite_send_ms: stomped by send thread on every p9_write_send.
     * last_rwrite_ack_ms:  stomped by drain thread after every Rwrite.
     *
     * After drain_throttle(d, 0) returns, both slots describe the LAST
     * write of the just-completed frame (no further writes have been
     * issued — send thread is unblocked here for the first time).
     * Their midpoint approximates the moment rio applied the frame's
     * tail.  See send.c per-frame block for the read site.
     */
    atomic_uint last_twrite_send_ms;
    atomic_uint last_rwrite_ack_ms;
};

/* ============== Per-Window State (rio_window) ============== */

/*
 * Per-rio-window state.
 *
 * Encapsulates everything specific to a single Plan 9 rio window:
 * 9P connections, draw device, framebuffers, send thread, output,
 * scroll detection, and input threads.
 *
 * The primary window is created at startup from command-line args.
 * Additional windows can be created later (multi-window support).
 *
 * Back-pointer rw->server gives access to global Wayland state.
 */
struct rio_window {
    struct wl_list link;            /* Link in server->rio_windows */
    struct server *server;          /* Back-pointer to global state */
    int id;                         /* Window identifier (for protocol) */

    /* ---- 9P connections for this window ---- */
    struct p9conn p9_draw;          /* For /dev/draw rendering */
    struct p9conn p9_relookup;      /* For relookup (isolated from send/drain) */
    struct p9conn p9_mouse;         /* For /dev/mouse input */
    struct p9conn p9_kbd;           /* For /dev/cons keyboard input */
    struct p9conn p9_wctl;          /* For /dev/wctl window monitoring */

    struct draw_state draw;

    /* ---- Window change detection ---- */
    atomic_int window_changed;          /* Set by mouse thread on geometry change */
    volatile int resize_pending;        /* Need to resize wlroots output (protected by send_lock) */
    volatile int pending_width, pending_height;
    volatile int pending_visible_width, pending_visible_height;
    volatile int pending_minx, pending_miny;
    char pending_winname[64];

    /* ---- Framebuffers ---- */
    int width, height;              /* Padded buffer dimensions (TILE_ALIGN_UP) */
    int visible_width, visible_height;  /* Actual window dimensions */
    uint32_t *framebuf;             /* Current frame */
    uint32_t *prev_framebuf;        /* Previous frame (for delta detection) */

    /* ---- Tile-based rendering ---- */
    int tiles_x, tiles_y;           /* Number of tiles in each dimension */
    int force_full_frame;           /* Force complete redraw */
    int scene_dirty;                /* Scene content changed, needs render */
    int frame_dirty;                /* Frame has changes to send */
    int timer_armed;                /* Send timer is active */
    uint32_t last_frame_ms;         /* Timestamp of last frame */
    struct wl_event_source *send_timer;

    /* ---- Send thread (double buffered) ---- */
    pthread_t send_thread;
    pthread_mutex_t send_lock;
    pthread_cond_t send_cond;
    uint32_t *send_buf[2];          /* Double buffer for send thread */
    int pending_buf;                /* Buffer with new data (-1 = none) */
    int active_buf;                 /* Buffer send thread is using */
    int send_full;                  /* Force full frame flag */

    /* ---- Drain thread (per-window) ---- */
    struct drain_ctx drain;

    /* ---- Damage-based dirty tile tracking ---- */
    uint8_t *dirty_staging;          /* Tile bitmap written by output thread */
    int dirty_staging_valid;         /* 1 if dirty_staging has valid data */
    uint8_t *dirty_tiles[2];         /* Per-send-buffer tile bitmaps */
    int dirty_valid[2];              /* Whether bitmap is valid per buffer */
    int damage_lost;                 /* Set when send_frame's overwrite path
                                      * drops a frame.  The next non-overwriting
                                      * send_frame must invalidate dirty_tiles
                                      * because intermediate output_frame calls
                                      * each reset dirty_staging, so the latest
                                      * staging bitmap reflects only the most
                                      * recent frame's deltas — not the cumulative
                                      * damage across the dropped chain.  Cleared
                                      * after one non-overwriting send_frame. */

    /* ---- Per-region scroll detection ---- */
    struct {
        int x1, y1, x2, y2;         /* Region bounds */
        int detected;               /* Scroll detected? */
        int dx, dy;                 /* Scroll vector */
    } scroll_regions[MAX_SCROLL_REGIONS];
    int num_scroll_regions;
    int scroll_regions_x, scroll_regions_y;  /* Grid dimensions */

    /* ---- Scene subtree (per-window isolation) ---- */
    struct wlr_scene_tree *toplevel_tree;  /* Per-window subtree for toplevels */

    /* ---- Output (one wlr_output per rio window) ---- */
    struct wlr_output *output;
    struct wlr_scene_output *scene_output;
    struct wlr_scene_rect *background;  /* Gray background, resized with window */
    struct wl_listener output_frame;
    struct wl_listener output_destroy;
    int layout_x, layout_y;            /* Position in output layout (logical) */

    /* ---- Input threads for this window ---- */
    pthread_t mouse_thread;
    pthread_t kbd_thread;
    int last_buttons;                   /* Per-window button state for change detection */
    uint32_t held_modifiers;            /* Per-window modifier state (WLR_MODIFIER_* bits) */

    /*
     * Worker thread death counter.
     *
     * Each of mouse/kbd/send threads atomically increments this when it
     * exits because of a broken 9P connection that wasn't initiated by
     * us (i.e. the user killed the rio window from the Plan 9 side, not
     * a client-driven toplevel close that goes through
     * multiwin_destroy_window).  When the count reaches 3 (last thread
     * out), it pushes INPUT_RIO_WINDOW_DIED so the event loop can
     * cleanly tear down the dead window.
     *
     * The being_destroyed flag below tells worker threads when their
     * rw is already being destroyed by the event loop — in that case
     * they MUST NOT post the event because the rw is about to be freed.
     */
    atomic_int threads_exited;

    /*
     * Set by multiwin_destroy_window before it shuts down sockets and
     * joins the worker threads.  Worker threads check this on exit:
     *   - 0: rw died externally (post INPUT_RIO_WINDOW_DIED so the
     *        event loop can clean up)
     *   - 1: event loop is already cleaning up; do not post (the
     *        rw struct is about to be freed)
     */
    atomic_int being_destroyed;

    /*
     * ---- Presentation feedback / frame_done backpressure ----
     *
     * The send thread writes one byte to present_pipe[1] after each
     * frame's final flush has been ACKed by rio (drain_throttle(d, 0)
     * returned).  Before the wakeup write it stamps the per-slot
     * timestamp and OR's its slot bit into present_slot_mask.  The
     * main loop reads present_pipe[0] via present_source and fires
     * both wlr_scene_output_send_frame_done (with the most recent
     * slot's timestamp) and wp_presentation_feedback.presented (per
     * slot, with that slot's specific timestamp).
     *
     * Per-slot timestamps are necessary because the wp_presentation
     * 'presented' event must report the correct delivery time for the
     * specific buffer the feedback was attached to.  v15 used a single
     * present_time_ms which worked for frame_done (advisory) but is
     * wrong for per-buffer presentation feedback.
     *
     * present_slot_mask: bit i set = slot i has a fresh completion the
     * main loop hasn't drained yet.  Send thread atomic_fetch_or's its
     * slot bit; present_handler atomic_exchange's the whole mask to
     * claim and clear pending slots.
     *
     * frame_done_pending is set by output_frame_handler when it called
     * send_frame() and so deferred its own frame_done call.
     * present_handler tests-and-clears it before firing.
     */
    int present_pipe[2];
    struct wl_event_source *present_source;
    atomic_uint present_time_ms_slot[2];
    atomic_int present_slot_mask;
    atomic_int frame_done_pending;

    /*
     * ---- Per-window wp_presentation_feedback lists ----
     *
     * Tracks feedback objects through the QUEUED -> INFLIGHT pipeline.
     * See wayland/presentation.h for the full lifecycle.
     *
     *   queued_feedbacks   committed but not yet sent to the network
     *   buf_feedbacks[i]   attached to send_buf[i], in flight
     *
     * All three lists are wl_list_init'd at rio_window creation and
     * touched only on the main thread (send_frame holds send_lock; all
     * other touches are on the event loop).
     *
     * present_seq is the monotonic per-window sequence number reported
     * in wp_presentation_feedback.presented.
     */
    struct wl_list queued_feedbacks;
    struct wl_list buf_feedbacks[2];
    uint64_t present_seq;
};

/* ============== Main Server State ============== */

/*
 * Main compositor server state (global).
 *
 * Contains shared Wayland objects, the focus manager, input queue,
 * and a list of rio_window instances. The primary window is the
 * initial window created at startup.
 */
struct server {
    /* ---- Wayland/wlroots core ---- */
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_output_layout *output_layout;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xdg_decoration_manager_v1 *decoration_mgr;
    struct wl_listener new_decoration;
    struct wlr_keyboard_shortcuts_inhibit_manager_v1 *kb_shortcuts_inhibit;
    struct wl_listener new_kb_shortcut_inhibitor;
    struct wl_listener kb_inhibitor_destroy;
    struct wlr_keyboard_shortcuts_inhibitor_v1 *active_kb_inhibitor;
    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wlr_keyboard virtual_kb;

    /* ---- Event listeners (global) ---- */
    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_list toplevels;           /* List of struct toplevel */

    /*
     * Unified focus state machine.
     *
     * Consolidates: popup stack, pointer/keyboard focus, deferred focus.
     * See focus_manager.h for API.
     */
    struct focus_manager focus;

    /* ---- Rio windows ---- */
    struct wl_list rio_windows;         /* List of struct rio_window */
    struct rio_window *primary;         /* The initial window */
    struct rio_window *pending_rio_window;  /* Set before wlr_headless_add_output */

    /* ---- Multi-window protocol ---- */
    int multi_window;               /* -W flag: multi-window mode enabled */
    int next_window_id;             /* Counter for window IDs */
    struct wl_event_source *stdin_event;  /* Stdin reader for READY messages */

    /* ---- 9P connections (shared/global) ---- */
    struct p9conn p9_snarf;             /* For /dev/snarf clipboard */

    /* ---- Input handling ---- */
    struct input_queue input_queue;
    struct wl_event_source *input_event;
    volatile int running;               /* False to signal threads to exit */

    /* ---- Clipboard/snarf integration ---- */
    struct wl_listener wayland_to_snarf;
    struct wl_listener wayland_to_snarf_primary;

    /* ---- Toplevel tracking ---- */
    int has_toplevel;               /* Currently have any toplevel? */
    int had_toplevel;               /* Ever had a toplevel? (for exit logic) */

    /* ---- Connection settings ---- */
    const char *host;               /* 9P server hostname */
    int port;                       /* 9P server port */
    int use_tls;                    /* TLS mode enabled */
    char *tls_cert_file;            /* Path to certificate (-c option) */
    char *tls_fingerprint;          /* SHA256 fingerprint (-f option) */
    int tls_insecure;               /* Skip cert verification (-k option) */
    float scale;                    /* Output scale for HiDPI (default: 1.0) */
    enum wlr_log_importance log_level;

    /* ---- wp_presentation_time (custom impl) ---- */
    /* See wayland/presentation.h for design.  pending_feedbacks holds
     * feedbacks awaiting their first surface commit; per-window queued
     * and inflight lists live on rio_window. */
    struct wl_global *presentation_global;
    struct wl_list pending_feedbacks;
};

/* ============== Utility Functions ============== */

/*
 * Get current time in milliseconds (monotonic clock).
 *
 * Uses CLOCK_MONOTONIC for consistent timing unaffected by
 * system time changes.
 *
 * Returns milliseconds since arbitrary epoch.
 */
static inline uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * Get current time in microseconds (monotonic clock).
 *
 * Higher precision version of now_ms() for timing-sensitive code.
 *
 * Returns microseconds since arbitrary epoch.
 */
static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ============== Legacy Compatibility ============== */

/*
 * Legacy compatibility macros.
 *
 * These allow existing code to compile during migration to the
 * focus_manager API. Remove after migration is complete.
 *
 * TODO: grep for usage — these may be dead code now.
 */
#define server_popup_stack(s)         ((s)->focus.popup_stack)
#define server_needs_focus_recheck(s) ((s)->focus.pointer_focus_deferred)

#endif /* P9WL_TYPES_H */
