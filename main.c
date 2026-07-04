/* 
 * main.c - p9wl application entry point
 *
 * Argument parsing, 9P connection setup with optional TLS,
 * wlroots initialization, child process spawning, and main event loop.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "types.h"
#include "p9/p9.h"
#include "p9/p9_tls.h"
#include "input/input.h"
#include "input/clipboard.h"
#include "draw/draw.h"
#include "draw/send.h"
#include "multiwin.h"
#include "wayland/wayland.h"
#include "wayland/presentation.h"

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <plan9-ip>[:<port>] [command [args...]]\n", prog);
    fprintf(stderr, "\nConnection options:\n");
    fprintf(stderr, "  -c <cert>      Path to server certificate (PEM format)\n");
    fprintf(stderr, "  -f <fp>        SHA256 fingerprint of server certificate (hex)\n");
    fprintf(stderr, "  -k             Insecure mode: skip certificate verification\n");
    fprintf(stderr, "  -u <user>      9P username (default: $P9USER, $USER, or 'glenda')\n");
    fprintf(stderr, "\nDisplay options:\n");
    fprintf(stderr, "  -W             Multi-window mode (stdin\/stdout protocol)\n");
    fprintf(stderr, "  -S <scale>     Output scale factor (1.0-4.0, default: 1.0)\n");
    fprintf(stderr, "\nLogging options:\n");
    fprintf(stderr, "  -q             Quiet mode (errors only, default)\n");
    fprintf(stderr, "  -v             Verbose mode (info + errors)\n");
    fprintf(stderr, "  -d             Debug mode (all messages)\n");
    fprintf(stderr, "\nDefault port is %d for plaintext, %d for TLS.\n", P9_PORT, P9_TLS_PORT);
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s 192.168.1.100 foot\n", prog);
    fprintf(stderr, "  %s -c 9front.pem 192.168.1.100:10001 firefox\n", prog);
    fprintf(stderr, "  %s -f aa11bb22cc33... 192.168.1.100 chromium\n", prog);
    fprintf(stderr, "  %s -k 192.168.1.100 librewolf\n", prog);
    fprintf(stderr, "\n9front setup (plaintext):\n");
    fprintf(stderr, "  aux/listen1 -t tcp!*!%d /bin/exportfs -r /dev\n", P9_PORT);
    fprintf(stderr, "\n9front setup (TLS):\n");
    fprintf(stderr, "  auth/rsagen -t 'service=tls owner=*' > /sys/lib/tls/key\n");
    fprintf(stderr, "  auth/rsa2x509 -e 3650 'CN=myhost' /sys/lib/tls/key | \\\n");
    fprintf(stderr, "      auth/pemencode CERTIFICATE > /sys/lib/tls/cert\n");
    fprintf(stderr, "  cat /sys/lib/tls/key > /mnt/factotum/ctl\n");
    fprintf(stderr, "  aux/listen1 -t tcp!*!%d tlssrv -c /sys/lib/tls/cert /bin/exportfs -r /dev\n", P9_TLS_PORT);
    fprintf(stderr, "\n");
}

static int parse_args(int argc, char *argv[], const char **host, int *port,
                      const char **uname, float *scale,
                      enum wlr_log_importance *log_level,
                      struct tls_config *tls_cfg,  
                      char ***exec_argv, int *exec_argc) {
    static char host_buf[256];
    *host = NULL; *port = -1; *uname = NULL; *scale = 1.0f;
    *log_level = WLR_ERROR;
    memset(tls_cfg, 0, sizeof(*tls_cfg));
    *exec_argv = NULL; *exec_argc = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) { tls_cfg->cert_file = argv[++i]; }
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) { tls_cfg->cert_fingerprint = argv[++i]; }
        else if (strcmp(argv[i], "-k") == 0) { tls_cfg->insecure = 1; }
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) { *uname = argv[++i]; }
        else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            *scale = strtof(argv[++i], NULL);
            if (*scale < 1.0f) *scale = 1.0f;
            if (*scale > 4.0f) *scale = 4.0f;
        } else if (strcmp(argv[i], "-q") == 0) { *log_level = WLR_ERROR; }
        else if (strcmp(argv[i], "-v") == 0) { *log_level = WLR_INFO; }
        else if (strcmp(argv[i], "-d") == 0) { *log_level = WLR_DEBUG; }
        else if (strcmp(argv[i], "-W") == 0) { /* handled in main */ }
        else if (strcmp(argv[i], "-h") == 0) { return -1; }
        else if (argv[i][0] == '-') { fprintf(stderr, "Unknown option: %s\n", argv[i]); return -1; }
        else if (!*host) {
            *host = argv[i];
            char *colon = strchr(argv[i], ':');
            if (colon) {
                *port = atoi(colon + 1);
                size_t len = colon - argv[i];
                if (len >= sizeof(host_buf)) len = sizeof(host_buf) - 1;
                memcpy(host_buf, argv[i], len); host_buf[len] = '\0'; *host = host_buf;
            }
        } else { *exec_argv = &argv[i]; *exec_argc = argc - i; break; }
    }
    if (!*host) return -1;
    if (*port < 0) *port = (tls_cfg->cert_file || tls_cfg->cert_fingerprint || tls_cfg->insecure)
                           ? P9_TLS_PORT : P9_PORT;
    if (tls_cfg->insecure && (tls_cfg->cert_file || tls_cfg->cert_fingerprint)) {
        fprintf(stderr, "Warning: -k (insecure) ignores -c and -f options\n");
        tls_cfg->cert_file = NULL; tls_cfg->cert_fingerprint = NULL;
    }
    return 0;
}

/* Per-thread argument for parallel connection setup. */
struct connect_arg {
    struct p9conn *p9;
    const char *host;
    int port;
    struct tls_config *tls_cfg;
    const char *name;
    int result;
};

static void *connect_thread(void *arg) {
    struct connect_arg *a = arg;
    a->result = p9_connect(a->p9, a->host, a->port, a->tls_cfg);
    if (a->result < 0)
        wlr_log(WLR_ERROR, "Failed to connect (%s)", a->name);
    return NULL;
}

/* Connect all 9P sessions in parallel. */
static int connect_all_9p(struct rio_window *rw, struct p9conn *snarf,
                          const char *host, int port,
                          struct tls_config *tls_cfg) {
    struct p9conn *conns[] = {
        &rw->p9_draw, &rw->p9_relookup, &rw->p9_mouse, &rw->p9_kbd,
        &rw->p9_wctl, snarf
    };
    const char *names[] = { "draw", "relookup", "mouse", "kbd", "wctl", "snarf" };
    int n = sizeof(conns) / sizeof(conns[0]);

    struct connect_arg args[6];
    pthread_t threads[6];

    for (int i = 0; i < n; i++) {
        args[i] = (struct connect_arg){
            .p9 = conns[i], .host = host, .port = port,
            .tls_cfg = tls_cfg, .name = names[i]
        };
        pthread_create(&threads[i], NULL, connect_thread, &args[i]);
    }
    for (int i = 0; i < n; i++)
        pthread_join(threads[i], NULL);

    for (int i = 0; i < n; i++) {
        if (args[i].result < 0) {
            for (int j = 0; j < n; j++)
                if (j != i && args[j].result == 0) p9_disconnect(conns[j]);
            return -1;
        }
    }
    return 0;
}

static int init_wayland(struct server *s) {
    struct rio_window *rw = s->primary;
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);

    s->display = wl_display_create();
    if (!s->display) return -1;
    s->backend = wlr_headless_backend_create(wl_display_get_event_loop(s->display));
    if (!s->backend) return -1;
    s->renderer = wlr_renderer_autocreate(s->backend);
    if (!s->renderer) return -1;
    wlr_renderer_init_wl_display(s->renderer, s->display);
    s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
    if (!s->allocator) return -1;

    wlr_compositor_create(s->display, 5, s->renderer);
    wlr_subcompositor_create(s->display);
    wlr_data_device_manager_create(s->display);
    wlr_viewporter_create(s->display);
    wlr_primary_selection_v1_device_manager_create(s->display);
    wlr_idle_notifier_v1_create(s->display);

    s->output_layout = wlr_output_layout_create(s->display);
    wlr_xdg_output_manager_v1_create(s->display, s->output_layout);

    s->scene = wlr_scene_create();
    if (!s->scene) return -1;
    wlr_scene_attach_output_layout(s->scene, s->output_layout);

    int logical_w = focus_phys_to_logical(rw->visible_width, s->scale);
    int logical_h = focus_phys_to_logical(rw->visible_height, s->scale);
    float gray[4] = { 0.3f, 0.3f, 0.3f, 1.0f };
    rw->background = wlr_scene_rect_create(&s->scene->tree, logical_w, logical_h, gray);
    if (rw->background) wlr_scene_node_lower_to_bottom(&rw->background->node);

    /* Per-window scene subtree for toplevel isolation.
     * Each rio_window gets its own subtree positioned at its layout
     * coordinates.  Toplevels are parented here at (0,0) local.
     * Since each output's scene_output viewport only covers its own
     * layout region, build_state naturally renders only its toplevels. */
    rw->toplevel_tree = wlr_scene_tree_create(&s->scene->tree);
    if (!rw->toplevel_tree) return -1;
    wlr_scene_node_set_position(&rw->toplevel_tree->node, rw->layout_x, rw->layout_y);

    s->xdg_shell = wlr_xdg_shell_create(s->display, 5);
    if (!s->xdg_shell) return -1;
    s->new_xdg_toplevel.notify = new_toplevel;
    wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_xdg_toplevel);
    s->new_xdg_popup.notify = new_popup;
    wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_xdg_popup);

    s->decoration_mgr = wlr_xdg_decoration_manager_v1_create(s->display);
    if (s->decoration_mgr) {
        s->new_decoration.notify = handle_new_decoration;
        wl_signal_add(&s->decoration_mgr->events.new_toplevel_decoration, &s->new_decoration);
    }

    /*
     * v16: custom wp_presentation_time implementation.
     *
     * wlroots' wlr_presentation listens to the headless output's
     * present events and fires feedback at local commit time.  For a
     * network compositor that's misleading — the timestamp would lie
     * about when content actually reached rio.  We replace it with a
     * custom global in wayland/presentation.c that fires 'presented'
     * after the send thread reports rio-side delivery, and 'discarded'
     * when frames are dropped via the send_frame() overwrite path or
     * when surfaces/windows die mid-pipeline.
     *
     * We do NOT call wlr_presentation_create — only one wp_presentation
     * global can be advertised on the wl_display.
     */
    if (presentation_init(s) < 0) return -1;

    s->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(s->cursor, s->output_layout);

    s->seat = wlr_seat_create(s->display, "seat0");
    wlr_seat_set_capabilities(s->seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    wlr_keyboard_init(&s->virtual_kb, NULL, "virtual-keyboard");
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(&s->virtual_kb, km);
    xkb_keymap_unref(km); xkb_context_unref(ctx);
    wlr_seat_set_keyboard(s->seat, &s->virtual_kb);

    s->new_output.notify = new_output;
    wl_signal_add(&s->backend->events.new_output, &s->new_output);
    s->new_input.notify = new_input;
    wl_signal_add(&s->backend->events.new_input, &s->new_input);

    /* Set pending so new_output knows which rio_window to populate.
     * new_output clears it after assigning rw->output.  wlroots 0.19
     * may fire new_output during wlr_backend_start instead of during
     * wlr_headless_add_output — either way, pending is still set. */
    s->pending_rio_window = rw;
    wlr_headless_add_output(s->backend, rw->visible_width, rw->visible_height);

    return 0;
}

static const char *setup_socket(struct server *s) {
    struct rio_window *rw = s->primary;

    /*
     * Use a random socket name instead of wl_display_add_socket_auto().
     *
     * The auto-generated names (wayland-0, wayland-1, ...) are sequential
     * and get recycled.  If multiple p9wl instances run and some die,
     * the slot numbers get reused by new instances — and any orphaned
     * child process whose WAYLAND_DISPLAY still points to the old name
     * will silently connect to the wrong compositor.
     *
     * A random name like "p9wl-a7f3b2c1" makes accidental reuse
     * essentially impossible (1 in ~4 billion per name).
     */
    static char sock_name[32];
    uint32_t rnd = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &rnd, sizeof(rnd)) != sizeof(rnd))
            rnd = (uint32_t)getpid() ^ (uint32_t)time(NULL);
        close(fd);
    } else {
        /* Fallback: PID + wall-clock seconds */
        rnd = (uint32_t)getpid() ^ (uint32_t)time(NULL);
    }
    snprintf(sock_name, sizeof(sock_name), "p9wl-%08x", rnd);

    if (wl_display_add_socket(s->display, sock_name) < 0) {
        wlr_log(WLR_ERROR, "Failed to add socket %s, falling back to auto", sock_name);
        const char *auto_sock = wl_display_add_socket_auto(s->display);
        if (!auto_sock) return NULL;
        setenv("WAYLAND_DISPLAY", auto_sock, 1);
        wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s (%dx%d visible, %dx%d padded)",
                auto_sock, rw->visible_width, rw->visible_height, rw->width, rw->height);
        fprintf(stdout, "WAYLAND_DISPLAY=%s\n", auto_sock);
        fflush(stdout);
        return auto_sock;
    }

    setenv("WAYLAND_DISPLAY", sock_name, 1);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s (%dx%d visible, %dx%d padded)",
            sock_name, rw->visible_width, rw->visible_height, rw->width, rw->height);
    fprintf(stdout, "WAYLAND_DISPLAY=%s\n", sock_name);
    fflush(stdout);
    return sock_name;
}

int main(int argc, char *argv[]) {
    const char *host, *uname;
    int port, exec_argc, ret = 1;
    float scale; enum wlr_log_importance log_level;
    struct tls_config tls_cfg; char **exec_argv;

    if (parse_args(argc, argv, &host, &port, &uname, &scale, &log_level,
                   &tls_cfg, &exec_argv, &exec_argc) < 0) {
        print_usage(argv[0]); return 1;
    }
    if (uname) setenv("P9USER", uname, 1);
    signal(SIGPIPE, SIG_IGN);
    wlr_log_init(log_level, NULL);

    int using_tls = tls_cfg.cert_file || tls_cfg.cert_fingerprint || tls_cfg.insecure;
    if (using_tls) {
        if (tls_init() < 0) { wlr_log(WLR_ERROR, "Failed to initialize TLS"); return 1; }
        if (tls_cfg.cert_file) {
            wlr_log(WLR_INFO, "TLS mode: certificate pinning (file: %s)", tls_cfg.cert_file);
            char fp[65];
            if (tls_cert_file_fingerprint(tls_cfg.cert_file, fp, sizeof(fp)) == 0)
                wlr_log(WLR_INFO, "Pinned certificate fingerprint: %s", fp);
        } else if (tls_cfg.cert_fingerprint) { wlr_log(WLR_INFO, "TLS mode: fingerprint pinning"); }
        else if (tls_cfg.insecure) { wlr_log(WLR_ERROR, "WARNING: TLS certificate verification disabled"); }
    }

    struct server s = {0};
    wl_list_init(&s.toplevels);
    wl_list_init(&s.rio_windows);
    focus_manager_init(&s.focus, &s);
    s.host = host; s.port = port; s.running = 1;
    s.use_tls = using_tls; s.scale = scale; s.log_level = log_level;
    if (tls_cfg.cert_file) s.tls_cert_file = strdup(tls_cfg.cert_file);
    if (tls_cfg.cert_fingerprint) s.tls_fingerprint = strdup(tls_cfg.cert_fingerprint);
    s.tls_insecure = tls_cfg.insecure;

    /* Check for -W flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-W") == 0) { s.multi_window = 1; break; }
    }
    if (s.multi_window) {
        /* Stdout goes to a pipe in -W mode; force line-buffering so
         * protocol messages flush immediately on newline instead of
         * accumulating in the default full-buffer. */
        setvbuf(stdout, NULL, _IOLBF, 0);
    }

    /* Allocate primary rio window */
    struct rio_window *rw = calloc(1, sizeof(*rw));
    if (!rw) { wlr_log(WLR_ERROR, "Failed to allocate rio_window"); goto cleanup; }
    rw->server = &s; rw->id = 0;
    s.primary = rw;
    wl_list_insert(&s.rio_windows, &rw->link);

    wlr_log(WLR_INFO, "Connecting to %s:%d", host, port);

    if (connect_all_9p(rw, &s.p9_snarf, host, port, &tls_cfg) < 0) goto cleanup;

    if (init_draw(rw) < 0) { wlr_log(WLR_ERROR, "Failed to initialize draw device"); goto cleanup; }

    rw->width = rw->draw.width; rw->height = rw->draw.height;
    rw->visible_width = rw->draw.visible_width; rw->visible_height = rw->draw.visible_height;
    rw->tiles_x = rw->width / TILE_SIZE; rw->tiles_y = rw->height / TILE_SIZE;

    if (s.scale > 1.0f)
        wlr_log(WLR_INFO, "Visible: %dx%d, Padded: %dx%d, Scale: %.2f, Logical: %dx%d",
                rw->visible_width, rw->visible_height, rw->width, rw->height, s.scale,
                focus_phys_to_logical(rw->visible_width, s.scale),
                focus_phys_to_logical(rw->visible_height, s.scale));

    rw->framebuf = calloc(rw->width * rw->height, 4);
    rw->prev_framebuf = calloc(rw->width * rw->height, 4);
    rw->send_buf[0] = calloc(rw->width * rw->height, 4);
    rw->send_buf[1] = calloc(rw->width * rw->height, 4);
    if (!rw->framebuf || !rw->prev_framebuf || !rw->send_buf[0] || !rw->send_buf[1]) {
        wlr_log(WLR_ERROR, "Memory allocation failed"); goto cleanup;
    }

    rw->force_full_frame = 1; rw->frame_dirty = 1;
    rw->pending_buf = -1; rw->active_buf = -1;
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

    /*
     * Presentation feedback pipe.  Send thread posts here after each
     * frame's final flush has been ACKed by rio; main loop's
     * present_handler reads and fires wlr_scene_output_send_frame_done
     * with the actual delivery timestamp.  See output.c:present_handler
     * and send.c (end of per-frame loop) for details.
     */
    rw->present_pipe[0] = rw->present_pipe[1] = -1;
    if (pipe(rw->present_pipe) < 0) {
        wlr_log(WLR_ERROR, "present pipe: %s", strerror(errno));
        goto cleanup;
    }
    fcntl(rw->present_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(rw->present_pipe[1], F_SETFL, O_NONBLOCK);
    atomic_store(&rw->frame_done_pending, 0);

    input_queue_init(&s.input_queue);

    pthread_create(&rw->mouse_thread, NULL, mouse_thread_func, rw);
    pthread_create(&rw->kbd_thread, NULL, kbd_thread_func, rw);
    pthread_create(&rw->send_thread, NULL, send_thread_func, rw);

    if (init_wayland(&s) < 0) goto cleanup;
    clipboard_init(&s);
    if (s.multi_window && multiwin_init(&s) < 0)
        wlr_log(WLR_ERROR, "Multi-window init failed (continuing single-window)");
    if (!setup_socket(&s)) goto cleanup;

    if (exec_argc > 0) {
        pid_t pid = fork();
        if (pid < 0) { wlr_log(WLR_ERROR, "fork: %s", strerror(errno)); goto cleanup; }
        else if (pid == 0) {
            /* Detach from p9wl's session so a SIGHUP to whatever launched
             * p9wl doesn't cascade into the spawned child.  The child's
             * own pty/session structure (e.g. for terminal emulators) is
             * created independently after exec, so this only affects the
             * inherited session — not anything the child sets up itself. */
            setsid();
            execvp(exec_argv[0], exec_argv);
            fprintf(stderr, "exec %s: %s\n", exec_argv[0], strerror(errno)); _exit(1);
        }
        wlr_log(WLR_INFO, "Spawned child %d: %s", pid, exec_argv[0]);
    }

    s.input_event = wl_event_loop_add_fd(wl_display_get_event_loop(s.display),
                                          s.input_queue.pipe_fd[0], WL_EVENT_READABLE,
                                          handle_input_events, &s);
    rw->send_timer = wl_event_loop_add_timer(wl_display_get_event_loop(s.display),
                                              send_timer_callback, rw);
    rw->present_source = wl_event_loop_add_fd(wl_display_get_event_loop(s.display),
                                              rw->present_pipe[0], WL_EVENT_READABLE,
                                              present_handler, rw);

    if (!wlr_backend_start(s.backend)) { wlr_log(WLR_ERROR, "Backend start failed"); goto cleanup; }

    wlr_log(WLR_INFO, "Running (9P%s)", using_tls ? " over TLS" : "");
    wl_display_run(s.display);
    ret = 0;

cleanup:
    if (s.display) { clipboard_cleanup(&s); wl_display_destroy(s.display); }
    server_cleanup(&s);
    if (using_tls) tls_cleanup();
    return ret;
}
