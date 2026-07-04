/*
 * presentation.c - Custom wp_presentation_time implementation
 *
 * See presentation.h for design overview.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "presentation.h"
#include "focus_manager.h"
#include "../types.h"

/*
 * Generated from wayland-protocols/stable/presentation-time/presentation-time.xml
 * via wayland-scanner.  See Makefile rules.
 */
#include "presentation-time-protocol.h"

/* We support v1 of wp_presentation. */
#define PRESENTATION_VERSION 1

/* ============== Feedback state ============== */

enum feedback_state {
    FB_PENDING,    /* Awaiting next surface commit */
    FB_QUEUED,     /* Surface committed, awaiting send_frame */
    FB_INFLIGHT,   /* Assigned to a send_buf slot */
};

/*
 * One per outstanding wp_presentation_feedback resource.
 *
 * Lifetime begins in presentation_handle_feedback (response to client
 * feedback() request).  Ends when one of:
 *   - presented event fires (send thread delivered the frame)
 *   - discarded event fires (any of: client destroys resource, surface
 *     destroyed, frame overwritten, window died)
 * Both terminal paths call wl_resource_destroy(), which triggers our
 * resource_destroy listener, which calls feedback_free() to clean up
 * the struct itself.
 */
struct presentation_feedback {
    struct wl_resource *resource;       /* wp_presentation_feedback resource;
                                         * NULL after a terminal event has
                                         * been queued/sent. */
    struct wlr_surface *surface;        /* Source surface; NULL if destroyed
                                         * before terminal event fired. */
    struct server *server;              /* Back-reference for find_window */
    struct rio_window *window;          /* Target window once committed;
                                         * NULL while PENDING. */
    enum feedback_state state;
    int slot;                           /* send_buf slot when INFLIGHT;
                                         * -1 otherwise. */

    struct wl_listener resource_destroy;
    struct wl_listener surface_commit;  /* Active in PENDING only */
    struct wl_listener surface_destroy; /* Active until terminal event */

    struct wl_list link;                /* In server->pending_feedbacks
                                         * or rw->queued_feedbacks
                                         * or rw->buf_feedbacks[slot] */
};

/* ============== Helpers ============== */

/* Walk subsurface parent chain to find the root surface. */
static struct wlr_surface *root_surface(struct wlr_surface *s) {
    while (s) {
        struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(s);
        if (!sub) break;
        s = sub->parent;
    }
    return s;
}

/*
 * Map a surface to the rio_window that will eventually display it.
 * Returns NULL for surfaces with no render target (cursors, surfaces
 * during teardown) — caller fires discarded for those.
 */
static struct rio_window *find_window(struct server *s,
                                      struct wlr_surface *surface) {
    if (!surface) return NULL;

    /* Try the toplevel path first (handles subsurface walk). */
    struct toplevel *tl = focus_toplevel_from_surface(&s->focus, surface);
    if (tl && tl->window) return tl->window;

    /*
     * Popups: walk up the popup parent chain until we reach a toplevel.
     * Capped at 8 hops to prevent runaway loops on malformed input.
     */
    struct wlr_surface *cur = root_surface(surface);
    for (int hop = 0; hop < 8 && cur; hop++) {
        struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(cur);
        if (!xdg) break;
        if (xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
            tl = focus_toplevel_from_surface(&s->focus, cur);
            if (tl && tl->window) return tl->window;
            break;
        }
        if (xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg->popup) {
            cur = xdg->popup->parent;
            if (!cur) break;
            cur = root_surface(cur);
            continue;
        }
        break;
    }

    return NULL;
}

/* ============== Feedback lifecycle ============== */

/*
 * Free the struct presentation_feedback.  Removes from whatever list
 * it's on and detaches all listeners.  Called from the resource
 * destroy listener (which fires after wl_resource_destroy is invoked).
 */
static void feedback_free(struct presentation_feedback *fb) {
    if (fb->link.prev && fb->link.next)
        wl_list_remove(&fb->link);
    /* All listeners' links are wl_list_init'd at creation, so
     * wl_list_remove is safe even if never attached to a signal. */
    wl_list_remove(&fb->resource_destroy.link);
    wl_list_remove(&fb->surface_commit.link);
    wl_list_remove(&fb->surface_destroy.link);
    free(fb);
}

/*
 * Fire wp_presentation_feedback.discarded and tear down.
 * Idempotent against fb->resource being NULL.
 */
static void feedback_send_discarded(struct presentation_feedback *fb) {
    if (fb->resource) {
        wp_presentation_feedback_send_discarded(fb->resource);
        struct wl_resource *r = fb->resource;
        fb->resource = NULL;
        wl_resource_destroy(r);
        /* resource_destroy listener calls feedback_free */
    } else {
        feedback_free(fb);
    }
}

/*
 * Fire wp_presentation_feedback.presented and tear down.
 *
 * The protocol's timestamp is split across two u32s for tv_sec
 * (allowing 64-bit Unix time) and one u32 for tv_nsec.  Sequence is
 * similarly split.  Refresh = 0 = unknown vsync interval (rio is
 * asynchronous).  Flags = 0 — no VSYNC alignment, no HW_CLOCK
 * accuracy, no ZERO_COPY semantics to claim.
 */
static void feedback_send_presented(struct presentation_feedback *fb,
                                    uint32_t when_ms, uint64_t seq) {
    if (!fb->resource) {
        feedback_free(fb);
        return;
    }

    uint64_t when_ns = (uint64_t)when_ms * 1000000ULL;
    uint64_t when_sec = when_ns / 1000000000ULL;
    uint32_t tv_sec_hi = (uint32_t)(when_sec >> 32);
    uint32_t tv_sec_lo = (uint32_t)(when_sec & 0xFFFFFFFFu);
    uint32_t tv_nsec = (uint32_t)(when_ns % 1000000000ULL);
    uint32_t seq_hi = (uint32_t)(seq >> 32);
    uint32_t seq_lo = (uint32_t)(seq & 0xFFFFFFFFu);

    wp_presentation_feedback_send_presented(fb->resource,
        tv_sec_hi, tv_sec_lo, tv_nsec,
        0,                  /* refresh: unknown */
        seq_hi, seq_lo,
        0);                 /* flags: nothing honest to claim */

    struct wl_resource *r = fb->resource;
    fb->resource = NULL;
    wl_resource_destroy(r);
}

/* ============== Listeners ============== */

static void on_resource_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct presentation_feedback *fb =
        wl_container_of(l, fb, resource_destroy);
    fb->resource = NULL;
    feedback_free(fb);
}

static void on_surface_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct presentation_feedback *fb =
        wl_container_of(l, fb, surface_destroy);
    fb->surface = NULL;
    /* Disable our own surface_destroy listener — we're handling it now,
     * and feedback_free's wl_list_remove must be on a self-link only. */
    wl_list_remove(&fb->surface_destroy.link);
    wl_list_init(&fb->surface_destroy.link);
    feedback_send_discarded(fb);
}

static void on_surface_commit(struct wl_listener *l, void *data) {
    (void)data;
    struct presentation_feedback *fb =
        wl_container_of(l, fb, surface_commit);

    if (fb->state != FB_PENDING) return;  /* Defensive */

    /* Disable commit listener — only fires once per feedback. */
    wl_list_remove(&fb->surface_commit.link);
    wl_list_init(&fb->surface_commit.link);

    struct rio_window *rw = find_window(fb->server, fb->surface);
    if (!rw) {
        /* Surface doesn't map to a render target — discard. */
        feedback_send_discarded(fb);
        return;
    }

    /* Migrate from server->pending_feedbacks to rw->queued_feedbacks. */
    wl_list_remove(&fb->link);
    wl_list_insert(&rw->queued_feedbacks, &fb->link);
    fb->window = rw;
    fb->state = FB_QUEUED;
}

/* ============== wp_presentation_feedback resource ============== */

/*
 * wp_presentation_feedback has no requests in the protocol — it's
 * a send-only resource (server fires presented or discarded; that's
 * it).  wayland-scanner therefore doesn't generate a
 * 'struct wp_presentation_feedback_interface' dispatcher struct, so
 * we pass NULL as the implementation in wl_resource_set_implementation
 * below.  Resource teardown is handled via wl_resource_add_destroy_listener
 * elsewhere in this file.
 */

/* ============== wp_presentation requests ============== */

static void presentation_handle_destroy(struct wl_client *client,
                                        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void presentation_handle_feedback(struct wl_client *client,
                                         struct wl_resource *resource,
                                         struct wl_resource *surface_resource,
                                         uint32_t callback) {
    struct server *s = wl_resource_get_user_data(resource);
    struct wlr_surface *surface =
        surface_resource ? wlr_surface_from_resource(surface_resource) : NULL;

    struct presentation_feedback *fb = calloc(1, sizeof(*fb));
    if (!fb) {
        wl_client_post_no_memory(client);
        return;
    }
    fb->state = FB_PENDING;
    fb->slot = -1;
    fb->server = s;
    fb->surface = surface;
    wl_list_init(&fb->link);
    wl_list_init(&fb->resource_destroy.link);
    wl_list_init(&fb->surface_commit.link);
    wl_list_init(&fb->surface_destroy.link);

    fb->resource = wl_resource_create(client,
        &wp_presentation_feedback_interface,
        wl_resource_get_version(resource),
        callback);
    if (!fb->resource) {
        free(fb);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(fb->resource, NULL,
                                   fb, NULL);

    fb->resource_destroy.notify = on_resource_destroy;
    wl_resource_add_destroy_listener(fb->resource, &fb->resource_destroy);

    if (!surface) {
        /* Bad surface — fire discarded immediately. */
        feedback_send_discarded(fb);
        return;
    }

    fb->surface_commit.notify = on_surface_commit;
    wl_signal_add(&surface->events.commit, &fb->surface_commit);
    fb->surface_destroy.notify = on_surface_destroy;
    wl_signal_add(&surface->events.destroy, &fb->surface_destroy);

    wl_list_insert(&s->pending_feedbacks, &fb->link);
}

static const struct wp_presentation_interface presentation_implementation = {
    .destroy = presentation_handle_destroy,
    .feedback = presentation_handle_feedback,
};

/* ============== Global bind ============== */

static void presentation_bind(struct wl_client *client, void *data,
                              uint32_t version, uint32_t id) {
    struct server *s = data;
    uint32_t v = version > PRESENTATION_VERSION ? PRESENTATION_VERSION : version;
    struct wl_resource *resource = wl_resource_create(client,
        &wp_presentation_interface, v, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &presentation_implementation,
                                   s, NULL);

    /* Tell the client which clock our timestamps are in.  We use
     * CLOCK_MONOTONIC throughout (now_ms / now_us). */
    wp_presentation_send_clock_id(resource, (uint32_t)CLOCK_MONOTONIC);
}

/* ============== Public API ============== */

int presentation_init(struct server *s) {
    wl_list_init(&s->pending_feedbacks);

    s->presentation_global = wl_global_create(s->display,
        &wp_presentation_interface, PRESENTATION_VERSION,
        s, presentation_bind);
    if (!s->presentation_global) {
        wlr_log(WLR_ERROR, "presentation: wl_global_create failed");
        return -1;
    }
    wlr_log(WLR_INFO, "wp_presentation v%d global created (custom impl)",
            PRESENTATION_VERSION);
    return 0;
}

void presentation_cleanup(struct server *s) {
    /*
     * By the time server_cleanup runs, wl_display_destroy has typically
     * already destroyed all clients and resources, which fired our
     * destroy listeners and emptied pending_feedbacks via feedback_free.
     * Defensively drain anything left.
     */
    if (s->pending_feedbacks.prev && s->pending_feedbacks.next) {
        struct presentation_feedback *fb, *tmp;
        wl_list_for_each_safe(fb, tmp, &s->pending_feedbacks, link) {
            feedback_send_discarded(fb);
        }
    }
    if (s->presentation_global) {
        wl_global_destroy(s->presentation_global);
        s->presentation_global = NULL;
    }
}

void presentation_claim_slot(struct rio_window *rw, int slot, int overwriting) {
    if (slot < 0 || slot >= 2) return;

    /*
     * Overwrite case: the slot already has feedbacks from a previous
     * frame whose buffer is about to be replaced before delivery.
     * Per protocol, fire discarded on those — that's exactly what
     * 'discarded' means.
     */
    if (overwriting) {
        struct presentation_feedback *fb, *tmp;
        wl_list_for_each_safe(fb, tmp, &rw->buf_feedbacks[slot], link) {
            feedback_send_discarded(fb);
        }
    }

    /* Move queued -> buf_feedbacks[slot]. */
    struct presentation_feedback *fb, *tmp;
    wl_list_for_each_safe(fb, tmp, &rw->queued_feedbacks, link) {
        wl_list_remove(&fb->link);
        wl_list_insert(&rw->buf_feedbacks[slot], &fb->link);
        fb->state = FB_INFLIGHT;
        fb->slot = slot;
    }
}

void presentation_fire_slot(struct rio_window *rw, int slot, uint32_t when_ms) {
    if (slot < 0 || slot >= 2) return;

    /* Each frame gets a fresh seq.  Wraps after 2^64 frames. */
    uint64_t seq = ++rw->present_seq;

    struct presentation_feedback *fb, *tmp;
    wl_list_for_each_safe(fb, tmp, &rw->buf_feedbacks[slot], link) {
        feedback_send_presented(fb, when_ms, seq);
    }
}

void presentation_window_died(struct rio_window *rw) {
    struct presentation_feedback *fb, *tmp;

    wl_list_for_each_safe(fb, tmp, &rw->queued_feedbacks, link) {
        feedback_send_discarded(fb);
    }
    for (int i = 0; i < 2; i++) {
        wl_list_for_each_safe(fb, tmp, &rw->buf_feedbacks[i], link) {
            feedback_send_discarded(fb);
        }
    }
}
