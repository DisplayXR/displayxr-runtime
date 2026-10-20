// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME screen capture through libpipewire-0.3, dlopen'd. See
 *         u_desktop_gnome_pipewire.h.
 *
 * Only the stable C ABI is used: opaque handles, struct pw_stream_events at
 * version 0 (every PipeWire 0.3.x and 1.x accepts it), and the public buffer
 * structs (spa/buffer/buffer.h, pipewire/stream.h — unchanged since 0.3.0). The
 * SPA pods are hand-built (u_desktop_spa_pod.c). Ubuntu 22.04 ships PipeWire
 * 0.3.48, 24.04 1.0.x, 26.04 1.4.x.
 */

#include "u_desktop_gnome_pipewire.h"
#include "u_desktop_spa_pod.h"

#include "util/u_logging.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * The ABI this file relies on (pipewire/stream.h, spa/buffer/buffer.h).
 */

struct pw_thread_loop;
struct pw_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_properties;
struct spa_dict;

struct spa_chunk_abi
{
	uint32_t offset;
	uint32_t size;
	int32_t stride;
	int32_t flags;
};

struct spa_data_abi
{
	uint32_t type;
	uint32_t flags;
	int64_t fd;
	uint32_t mapoffset;
	uint32_t maxsize;
	void *data;
	struct spa_chunk_abi *chunk;
};

struct spa_buffer_abi
{
	uint32_t n_metas;
	uint32_t n_datas;
	void *metas;
	struct spa_data_abi *datas;
};

struct pw_buffer_abi
{
	struct spa_buffer_abi *buffer;
	void *user_data;
	uint64_t size;
};

//! struct spa_hook is 6 pointers today; the listener gets more room than it needs.
struct spa_hook_abi
{
	void *p[16];
};

//! struct pw_stream_events, version 0 (PW_VERSION_STREAM_EVENTS 0 fields).
struct pw_stream_events_v0
{
	uint32_t version;
	void (*destroy)(void *data);
	void (*state_changed)(void *data, int old, int state, const char *error);
	void (*control_info)(void *data, uint32_t id, const void *control);
	void (*io_changed)(void *data, uint32_t id, void *area, uint32_t size);
	void (*param_changed)(void *data, uint32_t id, const void *param);
	void (*add_buffer)(void *data, struct pw_buffer_abi *buffer);
	void (*remove_buffer)(void *data, struct pw_buffer_abi *buffer);
	void (*process)(void *data);
	void (*drained)(void *data);
};

#define PWX_DIRECTION_INPUT 0
#define PWX_STREAM_FLAG_AUTOCONNECT (1u << 0)
#define PWX_STREAM_FLAG_MAP_BUFFERS (1u << 2)
#define PWX_STREAM_FLAG_DONT_RECONNECT (1u << 7)
#define PWX_STREAM_STATE_ERROR (-1)

struct pwlib
{
	bool ok;
	void (*init)(int *argc, char ***argv);
	struct pw_thread_loop *(*thread_loop_new)(const char *name, const struct spa_dict *props);
	void (*thread_loop_destroy)(struct pw_thread_loop *loop);
	int (*thread_loop_start)(struct pw_thread_loop *loop);
	void (*thread_loop_stop)(struct pw_thread_loop *loop);
	void (*thread_loop_lock)(struct pw_thread_loop *loop);
	void (*thread_loop_unlock)(struct pw_thread_loop *loop);
	struct pw_loop *(*thread_loop_get_loop)(struct pw_thread_loop *loop);
	struct pw_context *(*context_new)(struct pw_loop *loop, struct pw_properties *props, size_t user_data_size);
	void (*context_destroy)(struct pw_context *ctx);
	struct pw_core *(*context_connect_fd)(struct pw_context *ctx,
	                                      int fd,
	                                      struct pw_properties *props,
	                                      size_t user_data_size);
	int (*core_disconnect)(struct pw_core *core);
	struct pw_properties *(*properties_new)(const char *key, ...);
	struct pw_stream *(*stream_new)(struct pw_core *core, const char *name, struct pw_properties *props);
	void (*stream_add_listener)(struct pw_stream *s,
	                            struct spa_hook_abi *listener,
	                            const struct pw_stream_events_v0 *events,
	                            void *data);
	int (*stream_connect)(struct pw_stream *s,
	                      int direction,
	                      uint32_t target_id,
	                      uint32_t flags,
	                      const void **params,
	                      uint32_t n_params);
	int (*stream_update_params)(struct pw_stream *s, const void **params, uint32_t n_params);
	struct pw_buffer_abi *(*stream_dequeue_buffer)(struct pw_stream *s);
	int (*stream_queue_buffer)(struct pw_stream *s, struct pw_buffer_abi *b);
	int (*stream_disconnect)(struct pw_stream *s);
	void (*stream_destroy)(struct pw_stream *s);
};

static struct pwlib g_pw;
static pthread_once_t g_pw_once = PTHREAD_ONCE_INIT;

static void
pwlib_load(void)
{
	// Never dlclose'd: the process keeps it for its lifetime.
	void *h = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
	if (h == NULL) {
		U_LOG_I("desktop/gnome: libpipewire-0.3.so.0 not found — no screen capture");
		return;
	}
	bool ok = true;
#define PW_SYM(field, name)                                                                                            \
	do {                                                                                                           \
		void *s = dlsym(h, name);                                                                              \
		if (s == NULL) {                                                                                       \
			U_LOG_W("desktop/gnome: libpipewire-0.3.so.0 lacks %s — no screen capture", name);             \
			ok = false;                                                                                    \
		}                                                                                                      \
		memcpy(&g_pw.field, &s, sizeof(s));                                                                    \
	} while (0)
	PW_SYM(init, "pw_init");
	PW_SYM(thread_loop_new, "pw_thread_loop_new");
	PW_SYM(thread_loop_destroy, "pw_thread_loop_destroy");
	PW_SYM(thread_loop_start, "pw_thread_loop_start");
	PW_SYM(thread_loop_stop, "pw_thread_loop_stop");
	PW_SYM(thread_loop_lock, "pw_thread_loop_lock");
	PW_SYM(thread_loop_unlock, "pw_thread_loop_unlock");
	PW_SYM(thread_loop_get_loop, "pw_thread_loop_get_loop");
	PW_SYM(context_new, "pw_context_new");
	PW_SYM(context_destroy, "pw_context_destroy");
	PW_SYM(context_connect_fd, "pw_context_connect_fd");
	PW_SYM(core_disconnect, "pw_core_disconnect");
	PW_SYM(properties_new, "pw_properties_new");
	PW_SYM(stream_new, "pw_stream_new");
	PW_SYM(stream_add_listener, "pw_stream_add_listener");
	PW_SYM(stream_connect, "pw_stream_connect");
	PW_SYM(stream_update_params, "pw_stream_update_params");
	PW_SYM(stream_dequeue_buffer, "pw_stream_dequeue_buffer");
	PW_SYM(stream_queue_buffer, "pw_stream_queue_buffer");
	PW_SYM(stream_disconnect, "pw_stream_disconnect");
	PW_SYM(stream_destroy, "pw_stream_destroy");
#undef PW_SYM
	if (ok) {
		g_pw.init(NULL, NULL);
	}
	g_pw.ok = ok;
}

bool
gnome_pw_available(void)
{
	pthread_once(&g_pw_once, pwlib_load);
	return g_pw.ok;
}

static void
set_msg(char *msg, const char *fmt, ...)
{
	if (msg == NULL) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, U_DESKTOP_MSG_SIZE, fmt, ap);
	va_end(ap);
}

static uint64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/*
 * One stream's state: touched by the loop thread's callbacks (which run with the
 * thread-loop lock held) and read by the caller under that lock.
 */
struct grab
{
	struct pw_stream *stream;
	struct spa_hook_abi hook;
	uint32_t node;
	uint32_t w, h, format; //!< negotiated
	bool have_format;
	bool done;
	bool failed;
	char why[96];
	struct gnome_pw_frame frame;
};

static enum u_desktop_pixfmt
pixfmt_of(uint32_t spa_format, bool *ok)
{
	*ok = true;
	switch (spa_format) {
	case U_SPA_VIDEO_FORMAT_BGRx: return U_DESKTOP_PIXFMT_BGRX;
	case U_SPA_VIDEO_FORMAT_BGRA: return U_DESKTOP_PIXFMT_BGRA;
	case U_SPA_VIDEO_FORMAT_RGBx: return U_DESKTOP_PIXFMT_RGBX;
	case U_SPA_VIDEO_FORMAT_RGBA: return U_DESKTOP_PIXFMT_RGBA;
	default: *ok = false; return U_DESKTOP_PIXFMT_BGRX;
	}
}

static void
on_state_changed(void *data, int old, int state, const char *error)
{
	(void)old;
	struct grab *g = (struct grab *)data;
	if (state == PWX_STREAM_STATE_ERROR && !g->done) {
		g->failed = true;
		snprintf(g->why, sizeof(g->why), "the stream failed (%s)", error != NULL ? error : "?");
	}
}

static void
on_param_changed(void *data, uint32_t id, const void *param)
{
	struct grab *g = (struct grab *)data;
	if (id != U_SPA_PARAM_Format || param == NULL) {
		return;
	}
	uint32_t size = 0;
	memcpy(&size, param, 4);
	uint32_t w = 0, h = 0, fmt = 0;
	if (!u_spa_parse_video_format(param, (size_t)size + 8, &w, &h, &fmt)) {
		g->failed = true;
		snprintf(g->why, sizeof(g->why), "unreadable stream format");
		return;
	}
	g->w = w;
	g->h = h;
	g->format = fmt;
	g->have_format = true;
	// Shared memory only.
	uint8_t buf[U_SPA_POD_BUF] __attribute__((aligned(8)));
	size_t n = u_spa_build_buffers(buf, sizeof(buf));
	if (n > 0) {
		const void *params[1] = {buf};
		g_pw.stream_update_params(g->stream, params, 1);
	}
}

static void
on_process(void *data)
{
	struct grab *g = (struct grab *)data;
	struct pw_buffer_abi *b = g_pw.stream_dequeue_buffer(g->stream);
	if (b == NULL) {
		return;
	}
	if (!g->done && !g->failed && g->have_format && b->buffer != NULL && b->buffer->n_datas > 0) {
		struct spa_data_abi *d = &b->buffer->datas[0];
		struct spa_chunk_abi *c = d->chunk;
		bool fmt_ok = false;
		enum u_desktop_pixfmt pf = pixfmt_of(g->format, &fmt_ok);
		if (!fmt_ok) {
			g->failed = true;
			snprintf(g->why, sizeof(g->why), "unsupported pixel format %u", g->format);
		} else if (d->data != NULL && c != NULL && c->size > 0 && (c->flags & U_SPA_CHUNK_FLAG_CORRUPTED) == 0) {
			uint32_t stride = c->stride > 0 ? (uint32_t)c->stride : g->w * 4;
			uint64_t need = (uint64_t)stride * (g->h - 1) + (uint64_t)g->w * 4;
			if (stride >= g->w * 4 && (uint64_t)c->offset + need <= d->maxsize) {
				uint8_t *copy = (uint8_t *)malloc((size_t)stride * g->h);
				if (copy != NULL) {
					memcpy(copy, (const uint8_t *)d->data + c->offset, (size_t)need);
					g->frame.data = copy;
					g->frame.w = g->w;
					g->frame.h = g->h;
					g->frame.stride = stride;
					g->frame.fmt = pf;
					g->done = true;
				}
			}
			// A short / empty buffer is skipped: the next one is awaited.
		}
	}
	g_pw.stream_queue_buffer(g->stream, b);
}

static const struct pw_stream_events_v0 k_events = {
    .version = 0,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

void
gnome_pw_frame_free(struct gnome_pw_frame *f)
{
	if (f != NULL) {
		free(f->data);
		memset(f, 0, sizeof(*f));
	}
}

enum u_desktop_result
gnome_pw_grab(int pw_fd,
              const struct gnome_pw_stream *streams,
              uint32_t n,
              uint32_t timeout_ms,
              struct gnome_pw_frame *out,
              char *msg)
{
	memset(out, 0, sizeof(*out) * n);
	if (!gnome_pw_available()) {
		set_msg(msg, "no libpipewire-0.3 on this system");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	if (n == 0) {
		set_msg(msg, "no screen stream to read");
		return U_DESKTOP_ERR_FAILED;
	}
	int fd = fcntl(pw_fd, F_DUPFD_CLOEXEC, 3);
	if (fd < 0) {
		set_msg(msg, "the PipeWire remote is gone");
		return U_DESKTOP_ERR_FAILED;
	}
	struct grab *gs = (struct grab *)calloc(n, sizeof(*gs));
	struct pw_thread_loop *loop = g_pw.thread_loop_new("dxr-capture", NULL);
	if (gs == NULL || loop == NULL) {
		close(fd);
		free(gs);
		if (loop != NULL) {
			g_pw.thread_loop_destroy(loop);
		}
		set_msg(msg, "out of memory");
		return U_DESKTOP_ERR_FAILED;
	}
	struct pw_context *ctx = g_pw.context_new(g_pw.thread_loop_get_loop(loop), NULL, 0);
	enum u_desktop_result r = U_DESKTOP_OK;
	struct pw_core *core = NULL;
	if (ctx == NULL || g_pw.thread_loop_start(loop) != 0) {
		close(fd);
		set_msg(msg, "could not start a PipeWire loop");
		r = U_DESKTOP_ERR_FAILED;
		goto teardown;
	}

	g_pw.thread_loop_lock(loop);
	core = g_pw.context_connect_fd(ctx, fd, NULL, 0); // takes the fd
	if (core == NULL) {
		g_pw.thread_loop_unlock(loop);
		set_msg(msg, "could not connect to the portal's PipeWire remote");
		r = U_DESKTOP_ERR_FAILED;
		goto teardown;
	}
	uint8_t pod[U_SPA_POD_BUF] __attribute__((aligned(8)));
	size_t pod_len = u_spa_build_enum_format(pod, sizeof(pod));
	for (uint32_t i = 0; i < n && r == U_DESKTOP_OK; i++) {
		struct grab *g = &gs[i];
		g->node = streams[i].node;
		struct pw_properties *props = g_pw.properties_new("media.type", "Video", "media.category", "Capture",
		                                                  "media.role", "Screen", NULL);
		g->stream = g_pw.stream_new(core, "dxr-desktop-capture", props);
		if (g->stream == NULL || pod_len == 0) {
			set_msg(msg, "could not create a PipeWire stream");
			r = U_DESKTOP_ERR_FAILED;
			break;
		}
		g_pw.stream_add_listener(g->stream, &g->hook, &k_events, g);
		const void *params[1] = {pod};
		// target_id = the portal's node id (as pipewiresrc's `path` / OBS do).
		if (g_pw.stream_connect(g->stream, PWX_DIRECTION_INPUT, g->node,
		                        PWX_STREAM_FLAG_AUTOCONNECT | PWX_STREAM_FLAG_MAP_BUFFERS |
		                            PWX_STREAM_FLAG_DONT_RECONNECT,
		                        params, 1) < 0) {
			set_msg(msg, "could not connect to screen stream %u", g->node);
			r = U_DESKTOP_ERR_FAILED;
		}
	}
	g_pw.thread_loop_unlock(loop);

	// Wait for one frame from every stream (the first buffer of a fresh stream).
	uint64_t deadline = now_ms() + timeout_ms;
	while (r == U_DESKTOP_OK) {
		bool all = true;
		g_pw.thread_loop_lock(loop);
		for (uint32_t i = 0; i < n; i++) {
			if (gs[i].failed) {
				set_msg(msg, "screen stream %u: %s", gs[i].node, gs[i].why);
				r = U_DESKTOP_ERR_FAILED;
				break;
			}
			all = all && gs[i].done;
		}
		g_pw.thread_loop_unlock(loop);
		if (r != U_DESKTOP_OK || all) {
			break;
		}
		if (now_ms() >= deadline) {
			set_msg(msg, "no frame from the screen streams within %u ms", timeout_ms);
			r = U_DESKTOP_ERR_TIMEOUT;
			break;
		}
		struct timespec ts = {0, 5 * 1000000L};
		nanosleep(&ts, NULL);
	}

	g_pw.thread_loop_lock(loop);
	for (uint32_t i = 0; i < n; i++) {
		if (gs[i].stream != NULL) {
			g_pw.stream_disconnect(gs[i].stream);
			g_pw.stream_destroy(gs[i].stream); // removes the listener too
			gs[i].stream = NULL;
		}
		if (r == U_DESKTOP_OK) {
			out[i] = gs[i].frame;
		} else {
			gnome_pw_frame_free(&gs[i].frame);
		}
	}
	if (core != NULL) {
		g_pw.core_disconnect(core);
	}
	g_pw.thread_loop_unlock(loop);

teardown:
	g_pw.thread_loop_stop(loop);
	if (ctx != NULL) {
		g_pw.context_destroy(ctx);
	}
	g_pw.thread_loop_destroy(loop);
	if (r != U_DESKTOP_OK) {
		for (uint32_t i = 0; i < n; i++) {
			gnome_pw_frame_free(&gs[i].frame);
		}
	}
	free(gs);
	return r;
}
