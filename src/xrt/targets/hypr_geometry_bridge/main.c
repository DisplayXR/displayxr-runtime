// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-hypr-geometry-bridge: org.displayxr.WindowGeometry1 on
 *         Hyprland (Omarchy), read from the compositor's own IPC.
 *
 * docs/specs/runtime/wayland-window-geometry.md, "Hyprland publisher", and
 * docs/roadmap/omarchy-support.md. Hyprland has no in-process publisher of
 * window geometry, but it answers `j/clients` and `j/monitors` on its request
 * socket and announces window changes on its event socket. This process turns
 * that into the canonical snapshot (hgb_payload.c) and serves it with
 * wgeom_bus, so native-Wayland DisplayXR apps can weave windowed.
 *
 * Refresh policy:
 * - any event that can move a window (os_hyprland_event_moves_windows)
 *   marks the snapshot dirty; dirty snapshots are re-queried at most once per
 *   16 ms;
 * - Hyprland sends no event while a floating window is dragged or resized
 *   with the mouse, nor for a keyboard resize, so it also polls: every 50 ms
 *   while a visible floating window exists, otherwise once a second;
 * - wgeom_bus only signals WindowsChanged when the text changes, so polling
 *   is quiet on the bus.
 *
 * If the compositor goes away (a Hyprland restart), it serves an empty
 * snapshot and reconnects every 2 s, re-resolving the instance. Like every
 * bridge it steps aside for a real publisher (NameLost) and refuses to start
 * outside a Hyprland session (DXR_HGB_ANY_DESKTOP=1 overrides).
 *
 * @ingroup targets
 */

#include "hgb_payload.h"
#include "wgeom_bus.h"

#include "os/os_hyprland.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HGB_LOG "displayxr-hypr-geometry-bridge"
#define HGB_REQUEST_TIMEOUT_MS 500
#define HGB_MIN_REFRESH_NS (16 * 1000000LL)
#define HGB_FLOATING_POLL_NS (50 * 1000000LL)
#define HGB_IDLE_POLL_NS (1000 * 1000000LL)
#define HGB_RECONNECT_NS (2000 * 1000000LL)

static volatile sig_atomic_t g_quit = 0;

static void
on_signal(int sig)
{
	(void)sig;
	g_quit = 1;
}

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

struct hgb
{
	struct wgeom_bus *bus;
	char dir[512];
	int ev_fd;
	char ev_buf[8192];
	size_t ev_len;
	bool dirty;
	bool floating;
	int64_t last_refresh;
	int64_t next_reconnect;
	bool warned_build;
};

static void
disconnect(struct hgb *h, const char *why)
{
	if (h->ev_fd >= 0) {
		close(h->ev_fd);
		fprintf(stderr, HGB_LOG ": lost Hyprland (%s) — serving no windows until it is back\n", why);
	}
	h->ev_fd = -1;
	h->ev_len = 0;
	h->dir[0] = '\0';
	h->floating = false;
	wgeom_bus_publish(h->bus, WGEOM_EMPTY_SNAPSHOT);
	h->next_reconnect = now_ns() + HGB_RECONNECT_NS;
}

static bool
try_connect(struct hgb *h)
{
	if (!os_hyprland_socket_dir(h->dir, sizeof(h->dir))) {
		h->dir[0] = '\0';
		return false;
	}
	h->ev_fd = os_hyprland_event_connect(h->dir);
	if (h->ev_fd < 0) {
		h->dir[0] = '\0';
		return false;
	}
	fprintf(stderr, HGB_LOG ": following Hyprland at %s\n", h->dir);
	h->dirty = true;
	return true;
}

static void
refresh(struct hgb *h)
{
	h->last_refresh = now_ns();
	h->dirty = false;
	char *clients = os_hyprland_request(h->dir, "j/clients", HGB_REQUEST_TIMEOUT_MS);
	char *monitors = clients != NULL ? os_hyprland_request(h->dir, "j/monitors", HGB_REQUEST_TIMEOUT_MS) : NULL;
	if (clients == NULL || monitors == NULL) {
		free(clients);
		free(monitors);
		disconnect(h, "request socket");
		return;
	}
	struct hgb_snapshot_info info;
	char *snap = hgb_build_snapshot(clients, monitors, &info);
	free(clients);
	free(monitors);
	if (snap == NULL) {
		if (!h->warned_build) {
			h->warned_build = true;
			fprintf(stderr, HGB_LOG ": Hyprland's j/clients or j/monitors reply is not the expected JSON "
			                        "(further failures are not logged)\n");
		}
		return;
	}
	h->floating = info.any_floating_visible;
	wgeom_bus_publish(h->bus, snap);
	hgb_free(snap);
}

//! Drain the event socket; mark dirty on any line that can move a window.
static void
read_events(struct hgb *h)
{
	for (;;) {
		if (h->ev_len >= sizeof(h->ev_buf) - 1) {
			h->ev_len = 0; // a line longer than the buffer: drop it, refresh anyway
			h->dirty = true;
		}
		const ssize_t r = read(h->ev_fd, h->ev_buf + h->ev_len, sizeof(h->ev_buf) - 1 - h->ev_len);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			break;
		}
		if (r <= 0) {
			disconnect(h, "event socket closed");
			return;
		}
		h->ev_len += (size_t)r;
		h->ev_buf[h->ev_len] = '\0';
		char *line = h->ev_buf;
		char *nl;
		while ((nl = strchr(line, '\n')) != NULL) {
			*nl = '\0';
			if (os_hyprland_event_moves_windows(line)) {
				h->dirty = true;
			}
			line = nl + 1;
		}
		const size_t rest = h->ev_len - (size_t)(line - h->ev_buf);
		memmove(h->ev_buf, line, rest);
		h->ev_len = rest;
	}
}

int
main(int argc, char **argv)
{
	if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
		printf("usage: %s\n"
		       "Serves " WGEOM_BUS_IFACE
		       " on Hyprland (Omarchy), from Hyprland's IPC sockets.\n"
		       "Run as a systemd user service; Hyprland sessions only (DXR_HGB_ANY_DESKTOP=1 overrides).\n",
		       argv[0]);
		return 0;
	}

	const char *any = getenv("DXR_HGB_ANY_DESKTOP");
	const bool forced = any != NULL && strcmp(any, "1") == 0;
	char probe[512];
	if (!forced &&
	    !os_hyprland_session_detected(getenv("XDG_CURRENT_DESKTOP"), getenv("HYPRLAND_INSTANCE_SIGNATURE")) &&
	    !os_hyprland_socket_dir(probe, sizeof(probe))) {
		fprintf(stderr,
		        HGB_LOG ": not a Hyprland session (XDG_CURRENT_DESKTOP=%s, no live Hyprland socket) — not "
		                "taking " WGEOM_BUS_NAME ", which this desktop's own publisher may need\n",
		        getenv("XDG_CURRENT_DESKTOP") != NULL ? getenv("XDG_CURRENT_DESKTOP") : "unset");
		return 0;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	struct hgb h;
	memset(&h, 0, sizeof(h));
	h.ev_fd = -1;
	h.bus = wgeom_bus_open(HGB_LOG, NULL, NULL, NULL);
	if (h.bus == NULL) {
		return 0;
	}
	fprintf(stderr, HGB_LOG ": serving " WGEOM_BUS_NAME " (geometry only)\n");

	if (!try_connect(&h)) {
		fprintf(stderr, HGB_LOG ": no live Hyprland instance yet — retrying every 2 s\n");
		h.next_reconnect = now_ns() + HGB_RECONNECT_NS;
	}

	while (!g_quit) {
		const int64_t now = now_ns();
		if (h.ev_fd < 0 && now >= h.next_reconnect) {
			if (!try_connect(&h)) {
				h.next_reconnect = now + HGB_RECONNECT_NS;
			}
		}

		// When the next refresh is due.
		int64_t due;
		if (h.ev_fd < 0) {
			due = h.next_reconnect;
		} else if (h.dirty) {
			due = h.last_refresh + HGB_MIN_REFRESH_NS;
		} else {
			due = h.last_refresh + (h.floating ? HGB_FLOATING_POLL_NS : HGB_IDLE_POLL_NS);
		}
		int64_t wait_ns = due - now;
		if (wait_ns < 0) {
			wait_ns = 0;
		}
		const int timeout_ms = (int)((wait_ns + 999999) / 1000000);

		struct pollfd fds[2];
		int nfds = 0;
		const int bus_fd = wgeom_bus_fd(h.bus);
		if (bus_fd >= 0) {
			fds[nfds++] = (struct pollfd){.fd = bus_fd, .events = POLLIN};
		}
		const int ev_slot = h.ev_fd >= 0 ? nfds : -1;
		if (h.ev_fd >= 0) {
			fds[nfds++] = (struct pollfd){.fd = h.ev_fd, .events = POLLIN};
		}
		const int pr = poll(fds, (nfds_t)nfds, timeout_ms);
		if (pr < 0 && errno != EINTR) {
			fprintf(stderr, HGB_LOG ": poll failed (%s)\n", strerror(errno));
			break;
		}

		if (!wgeom_bus_dispatch(h.bus, 0)) {
			break; // bus gone, or a real publisher took the name
		}
		if (ev_slot >= 0 && pr > 0 && (fds[ev_slot].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			read_events(&h);
		}
		if (h.ev_fd >= 0 && now_ns() >= (h.dirty ? h.last_refresh + HGB_MIN_REFRESH_NS : due)) {
			refresh(&h);
		}
	}

	if (h.ev_fd >= 0) {
		close(h.ev_fd);
	}
	wgeom_bus_close(&h.bus);
	return 0;
}
