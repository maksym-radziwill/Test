# p9wl - Wayland compositor for Plan 9 (v16)
#
# Build: make
# Clean: make clean
#
# Changes from v15:
#   - Custom wp_presentation_time implementation in wayland/presentation.{h,c}.
#     Replaces wlroots' wlr_presentation, which fires feedback at local
#     commit time and lies about delivery for a network compositor.  Our
#     impl tracks each wp_presentation_feedback through PENDING -> QUEUED
#     -> INFLIGHT, fires presented after rio-side delivery, and fires
#     discarded on dropped frames (send_frame overwrite path) and
#     surface/window destruction.  Required for correct mpv playback.
#
#   - Per-slot completion timestamps (present_time_ms_slot[2] +
#     present_slot_mask) replace v15's single present_time_ms, because
#     each feedback must report the correct timestamp for its specific
#     buffer.
#
#   - Build-time generation of presentation-time-protocol.{h,c} via
#     wayland-scanner.  Requires wayland-protocols and wayland-scanner.
#
# Changes from v14 (carried forward):
#   - Frame backpressure via deferred wlr_scene_output_send_frame_done.

CC = gcc
CFLAGS = -O3 -Wall -Wextra -DWLR_USE_UNSTABLE -I. -Isrc
CFLAGS += $(shell pkg-config --cflags wlroots-0.19 wayland-server xkbcommon pixman-1)
LDFLAGS = $(shell pkg-config --libs wlroots-0.19 wayland-server xkbcommon pixman-1)
LDFLAGS += -lpthread -lm -lssl -lcrypto -lfftw3f

# wayland-scanner setup for protocol generation.
#
# We invoke wayland-scanner directly from PATH rather than going through
# pkg-config.  The --variable=wayland_scanner approach requires
# wayland-scanner.pc, which on some distros (notably NixOS, where
# wayland-scanner is its own derivation) isn't on PKG_CONFIG_PATH even
# when the binary is.  Override either var on the command line if you
# have a non-standard install: `make WAYLAND_SCANNER=/path/to/wayland-scanner`.
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
PRESENTATION_TIME_XML = $(WAYLAND_PROTOCOLS)/stable/presentation-time/presentation-time.xml

# Generated protocol files (created by wayland-scanner from XML)
GEN_HEADERS = wayland/presentation-time-protocol.h
GEN_SOURCES = wayland/presentation-time-protocol.c
GEN_OBJS = $(GEN_SOURCES:.c=.o)

# Source files
SRCS = main.c multiwin.c \
       p9/p9.c p9/p9_tls.c \
       input/input.c input/clipboard.c \
       draw/draw.c draw/compress.c draw/scroll.c draw/send.c \
       draw/phase_correlate.c draw/parallel.c \
       wayland/focus_manager.c wayland/popup.c wayland/toplevel.c \
       wayland/wl_input.c wayland/output.c wayland/client.c \
       wayland/presentation.c
OBJS = $(SRCS:.c=.o) $(GEN_OBJS)

# Headers
HDRS = types.h multiwin.h \
       p9/p9.h p9/p9_tls.h \
       input/input.h input/clipboard.h \
       draw/draw.h draw/compress.h draw/scroll.h draw/send.h \
       draw/phase_correlate.h draw/parallel.h \
       wayland/focus_manager.h wayland/popup.h wayland/toplevel.h \
       wayland/wl_input.h wayland/output.h wayland/client.h wayland/wayland.h \
       wayland/presentation.h

TARGET = p9wl

.PHONY: all clean

all: $(TARGET)

# Generated protocol header — server-header form, no marshalling code.
wayland/presentation-time-protocol.h: $(PRESENTATION_TIME_XML)
	$(WAYLAND_SCANNER) server-header $< $@

# Generated protocol marshalling code.  "private-code" form bundles the
# wl_message tables into the .c file directly (no need for a separate
# protocol registry).
wayland/presentation-time-protocol.c: $(PRESENTATION_TIME_XML)
	$(WAYLAND_SCANNER) private-code $< $@

# Make sure the generated header is present before any .c that includes
# it gets compiled.  presentation.c is the only consumer.
wayland/presentation.o: wayland/presentation-time-protocol.h

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) $(GEN_HEADERS) $(GEN_SOURCES)

# For now, build from monolithic file
monolithic: p9wl.c
	$(CC) $(CFLAGS) -o p9wl p9wl.c $(LDFLAGS)
