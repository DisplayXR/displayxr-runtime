// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  External background-capture receiver for compose-under (#1073 T2).
 * @author David Fattal
 * @ingroup comp_util
 *
 * See comp_bg2d_capture.h for the protocol and for why the runtime is the
 * listener. This file is just the socket: accept one producer, read frames,
 * keep the newest.
 */

#include "comp_bg2d_capture.h"

#include "os/os_threading.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_trace_marker.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(XRT_OS_LINUX)
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif


#if !defined(XRT_OS_LINUX)

/*
 * Abstract-namespace unix sockets are a Linux extension, and the only platforms
 * that need an external producer (Android, desktop Linux) are Linux. Everywhere
 * else this compiles to "no producer", which is the correct answer rather than
 * a missing symbol.
 */

bool
comp_bg2d_capture_start(const char *socket_name)
{
	(void)socket_name;
	return false;
}

bool
comp_bg2d_capture_acquire(struct comp_bg2d_capture_frame *out, uint32_t last_seq)
{
	(void)out;
	(void)last_seq;
	return false;
}

void
comp_bg2d_capture_release(void)
{}

void
comp_bg2d_capture_stop(void)
{}

#else

//! Refuse absurd frames rather than trusting a length off the wire.
#define BG2D_CAPTURE_MAX_DIM 4096u
#define BG2D_CAPTURE_MAX_BYTES (BG2D_CAPTURE_MAX_DIM * BG2D_CAPTURE_MAX_DIM * 4u)

struct bg2d_capture
{
	struct os_thread_helper oth;

	int listen_fd;

	//! Newest complete frame, owned here, guarded by `oth`'s mutex.
	uint8_t *pixels;
	size_t pixels_capacity;
	uint32_t width;
	uint32_t height;
	//! Panel extent the live frame was captured against (v2 producers only; 0
	//! when the producer speaks v1 and does not say).
	uint32_t panel_w;
	uint32_t panel_h;
	//! Our own delivery counter, NOT the producer's `seq`. It starts at 1 (so
	//! "0" can mean "nothing uploaded yet") and never resets, which makes the
	//! consumer immune both to a producer whose first frame is seq 0 and to a
	//! producer restart that begins renumbering from the start.
	uint32_t delivered;
	bool have_frame;

	//! Thread-owned scratch the producer streams into. Frames are published by
	//! swapping this with `pixels`, so a partially received frame can never be
	//! sampled — the compositor only ever sees a complete one.
	uint8_t *scratch;
	size_t scratch_capacity;

	bool logged_first_frame;
};

//! Process-global: one screen, one background, one producer.
static struct bg2d_capture g_capture = {0};
static bool g_started = false;


/*
 *
 * Socket helpers.
 *
 */

//! Read exactly @p len bytes unless the peer goes away or we are asked to stop.
static bool
read_full(struct bg2d_capture *c, int fd, void *dst, size_t len)
{
	uint8_t *p = (uint8_t *)dst;
	size_t got = 0;
	while (got < len) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		int pr = poll(&pfd, 1, 250);
		if (pr < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		if (pr == 0) {
			// Idle producer (a static screen delivers nothing) — only a
			// stop request ends the wait.
			if (!os_thread_helper_is_running(&c->oth)) {
				return false;
			}
			continue;
		}
		ssize_t n = read(fd, p + got, len - got);
		if (n > 0) {
			got += (size_t)n;
			continue;
		}
		if (n == 0) {
			return false; // producer closed
		}
		if (errno == EINTR || errno == EAGAIN) {
			continue;
		}
		return false;
	}
	return true;
}

/*!
 * Consume one frame header + payload and publish it.
 *
 * Repacks to a tight `width * 4` stride here rather than in the uploader: the
 * compositor thread holds the lock while it uploads, so anything we can do on
 * this thread instead is time it does not spend blocked.
 */
static bool
read_one_frame(struct bg2d_capture *c, int fd, uint32_t version)
{
	uint32_t hdr[9];
	// The negotiated version fixes the header length for the whole connection
	// (v1 = 7 words, v2 = 9), so this is a length lookup and never a guess — a
	// header we sized from the frame's own contents could not be re-synced if
	// it were ever wrong.
	const size_t words = (version >= 2u) ? 9u : 7u;
	if (!read_full(c, fd, hdr, words * sizeof(hdr[0]))) {
		return false;
	}
	if (hdr[0] != COMP_BG2D_CAPTURE_MAGIC_FRAME) {
		U_LOG_E("bg2d capture: framing lost (magic 0x%08x) — dropping the producer", hdr[0]);
		return false;
	}
	uint32_t seq = hdr[1];
	uint32_t w = hdr[2];
	uint32_t h = hdr[3];
	uint32_t stride = hdr[4];
	uint32_t format = hdr[5];
	uint32_t payload = hdr[6];
	uint32_t panel_w = (words >= 9u) ? hdr[7] : 0u;
	uint32_t panel_h = (words >= 9u) ? hdr[8] : 0u;

	// A panel extent is advisory metadata, not a length: a nonsensical one must
	// not cost us the frame, so drop the claim and keep the pixels. The consumer
	// treats 0/0 as "the producer did not say" and falls back to the v1 aspect
	// heuristic, which is exactly the right behaviour for a producer that said
	// something impossible.
	if (panel_w == 0 || panel_h == 0) {
		panel_w = 0;
		panel_h = 0;
	}

	if (w == 0 || h == 0 || w > BG2D_CAPTURE_MAX_DIM || h > BG2D_CAPTURE_MAX_DIM || format != 0 ||
	    stride < w * 4u || payload != stride * h || payload > BG2D_CAPTURE_MAX_BYTES) {
		U_LOG_E("bg2d capture: implausible frame %ux%u stride=%u fmt=%u payload=%u", w, h, stride, format,
		        payload);
		return false;
	}

	const size_t tight = (size_t)w * h * 4u;
	if (c->scratch_capacity < tight) {
		uint8_t *n = realloc(c->scratch, tight);
		if (n == NULL) {
			return false;
		}
		c->scratch = n;
		c->scratch_capacity = tight;
	}

	if (stride == w * 4u) {
		if (!read_full(c, fd, c->scratch, tight)) {
			return false;
		}
	} else {
		// Padded rows: read and drop the padding. `setSize` output from
		// SurfaceFlinger is commonly padded to the buffer's stride.
		uint8_t *pad = malloc(stride - w * 4u);
		if (pad == NULL) {
			return false;
		}
		for (uint32_t y = 0; y < h; y++) {
			if (!read_full(c, fd, c->scratch + (size_t)y * w * 4u, (size_t)w * 4u) ||
			    !read_full(c, fd, pad, stride - w * 4u)) {
				free(pad);
				return false;
			}
		}
		free(pad);
	}

	// Publish: swap the completed scratch in as the live frame. O(1), and the
	// compositor's lock hold is a pointer assignment rather than a copy.
	os_thread_helper_lock(&c->oth);
	uint8_t *old = c->pixels;
	size_t old_cap = c->pixels_capacity;
	c->pixels = c->scratch;
	c->pixels_capacity = c->scratch_capacity;
	c->width = w;
	c->height = h;
	c->panel_w = panel_w;
	c->panel_h = panel_h;
	c->delivered++;
	c->have_frame = true;
	os_thread_helper_unlock(&c->oth);
	c->scratch = old;
	c->scratch_capacity = old_cap;

	if (!c->logged_first_frame) {
		c->logged_first_frame = true;
		if (panel_w != 0) {
			U_LOG_W(
			    "bg2d capture(#1073 T2): FIRST frame from the producer — %ux%u of a %ux%u panel, seq %u", w,
			    h, panel_w, panel_h, seq);
		} else {
			U_LOG_W(
			    "bg2d capture(#1073 T2): FIRST frame from the producer — %ux%u, seq %u (v1 producer: no "
			    "capture-time panel extent, so a rotation can only be detected by aspect)",
			    w, h, seq);
		}
	}
	return true;
}

static void
serve_producer(struct bg2d_capture *c, int fd)
{
	uint32_t hello[2];
	if (!read_full(c, fd, hello, sizeof(hello))) {
		return;
	}
	if (hello[0] != COMP_BG2D_CAPTURE_MAGIC_HELLO || hello[1] < COMP_BG2D_CAPTURE_VERSION_MIN ||
	    hello[1] > COMP_BG2D_CAPTURE_VERSION_CURRENT) {
		U_LOG_E("bg2d capture: bad hello (magic 0x%08x version %u) — expected 'DXRB' v%u..v%u", hello[0],
		        hello[1], COMP_BG2D_CAPTURE_VERSION_MIN, COMP_BG2D_CAPTURE_VERSION_CURRENT);
		return;
	}
	const uint32_t version = hello[1];
	U_LOG_W("bg2d capture(#1073 T2): producer connected (protocol v%u)", version);

	// A reconnecting producer may speak a different version, and its first
	// frame has not landed yet — so clear the panel extent the PREVIOUS
	// producer stated rather than let it describe someone else's pixels.
	os_thread_helper_lock(&c->oth);
	c->panel_w = 0;
	c->panel_h = 0;
	os_thread_helper_unlock(&c->oth);

	while (os_thread_helper_is_running(&c->oth)) {
		if (!read_one_frame(c, fd, version)) {
			break;
		}
	}
	U_LOG_W("bg2d capture(#1073 T2): producer disconnected — falling back to no background");
}

static void *
capture_thread(void *ptr)
{
	struct bg2d_capture *c = (struct bg2d_capture *)ptr;
	U_TRACE_SET_THREAD_NAME("bg2d capture");

	while (os_thread_helper_is_running(&c->oth)) {
		struct pollfd pfd = {.fd = c->listen_fd, .events = POLLIN};
		int pr = poll(&pfd, 1, 250);
		if (pr <= 0) {
			continue;
		}
		int fd = accept(c->listen_fd, NULL, NULL);
		if (fd < 0) {
			continue;
		}
		serve_producer(c, fd);
		close(fd);

		// Deliberately keep the last frame after a disconnect: a producer
		// restart (or a `once` capture that has done its job and exited)
		// should not blink the background away.
	}
	return NULL;
}


/*
 *
 * Public API.
 *
 */

bool
comp_bg2d_capture_start(const char *socket_name)
{
	struct bg2d_capture *c = &g_capture;
	if (g_started) {
		return true;
	}
	c->listen_fd = -1;

	const char *name = (socket_name != NULL && socket_name[0] != '\0') ? socket_name : COMP_BG2D_CAPTURE_SOCKET;

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	// Abstract namespace: leading NUL, name follows, length excludes the tail.
	const size_t name_len = strlen(name);
	if (name_len + 1 >= sizeof(addr.sun_path)) {
		U_LOG_E("bg2d capture: socket name '%s' too long", name);
		return false;
	}
	addr.sun_path[0] = '\0';
	memcpy(addr.sun_path + 1, name, name_len);
	const socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		U_LOG_E("bg2d capture: socket() failed: %s", strerror(errno));
		return false;
	}
	if (bind(fd, (struct sockaddr *)&addr, addr_len) != 0) {
		U_LOG_E("bg2d capture: bind(@%s) failed: %s", name, strerror(errno));
		close(fd);
		return false;
	}
	if (listen(fd, 1) != 0) {
		U_LOG_E("bg2d capture: listen(@%s) failed: %s", name, strerror(errno));
		close(fd);
		return false;
	}

	c->listen_fd = fd;
	if (os_thread_helper_init(&c->oth) != 0) {
		close(fd);
		c->listen_fd = -1;
		return false;
	}
	if (os_thread_helper_start(&c->oth, capture_thread, c) != 0) {
		os_thread_helper_destroy(&c->oth);
		close(fd);
		c->listen_fd = -1;
		return false;
	}

	g_started = true;
	U_LOG_W("bg2d capture(#1073 T2): listening on abstract socket @%s — waiting for a producer", name);
	return true;
}

bool
comp_bg2d_capture_acquire(struct comp_bg2d_capture_frame *out, uint32_t last_seq)
{
	struct bg2d_capture *c = &g_capture;
	if (!g_started || out == NULL) {
		return false;
	}
	os_thread_helper_lock(&c->oth);
	if (!c->have_frame || c->delivered == last_seq) {
		os_thread_helper_unlock(&c->oth);
		return false;
	}
	out->pixels = c->pixels;
	out->width = c->width;
	out->height = c->height;
	out->panel_w = c->panel_w;
	out->panel_h = c->panel_h;
	out->seq = c->delivered;
	return true; // lock intentionally held until _release
}

void
comp_bg2d_capture_release(void)
{
	if (!g_started) {
		return;
	}
	os_thread_helper_unlock(&g_capture.oth);
}

void
comp_bg2d_capture_stop(void)
{
	struct bg2d_capture *c = &g_capture;
	if (!g_started) {
		return;
	}
	g_started = false;
	os_thread_helper_stop_and_wait(&c->oth);
	if (c->listen_fd >= 0) {
		close(c->listen_fd);
		c->listen_fd = -1;
	}
	free(c->pixels);
	c->pixels = NULL;
	c->pixels_capacity = 0;
	free(c->scratch);
	c->scratch = NULL;
	c->scratch_capacity = 0;
	c->have_frame = false;
	c->panel_w = 0;
	c->panel_h = 0;
	os_thread_helper_destroy(&c->oth);
}

#endif // XRT_OS_LINUX
