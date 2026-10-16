// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: control loop, lane workers, ticker.
 * @ingroup conv_host
 */

#include "xrt/xrt_config_os.h"

#include "conv_host_serve.h"
#include "conv_host_exit.h"
#include "conv_host_log.h"
#include "conv_host_os.h"
#include "conv_host_shm.h"

#ifdef XRT_OS_WINDOWS
#include "conv_host_d3d11.h"
#endif

#include "util/u_shm_ring.h"
#include "os/os_process.h"
#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_git_tag.h"
#include "util/u_logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STREAMS 64u
#define MAX_INPUTS 256u
#define MAX_SYNCS 16u
#define MAX_OUTPUTS 256u
#define MAX_LANES 4u
#define MAX_SETS 4u
#define MAX_PENDING_WARMS 16u
#define MAX_KEPT_BLOBS 4u

//! A worker waits on its wake signal at most this long before re-checking stop.
#define LANE_WAIT_MS 100u
//! Longest a worker waits for room in a full completion ring.
#define COMPLETE_FULL_WAIT_NS (1000ull * 1000ull * 1000ull)
//! The orderly-shutdown budget (§3.4: exit 0 within 3 s).
#define SHUTDOWN_BUDGET_MS 3000u

struct out_image
{
	bool used;
	uint64_t stream; //!< service stream id
	uint32_t role;   //!< CONV_HOST_WIRE_OUT_*
	struct xrt_conv_image img;
	uint64_t shm_handle; //!< CPU: the shm section (kept open: Windows pulls it)
	void *map;
	uint64_t map_bytes;
#ifdef XRT_OS_WINDOWS
	struct conv_host_d3d11_image tex;
#endif
};

struct stream
{
	bool used;
	bool ready;           //!< stream_create completed
	bool close_requested; //!< STREAM_CLOSE arrived while creating
	uint32_t close_request_id;
	uint64_t id;     //!< service id
	uint64_t handle; //!< provider's
	uint32_t cap_id;
	struct xrt_conv_stream_info info;
	bool reset_pending; //!< STREAM_RESET: FRAME_RESET on the next convert
	uint32_t set_count;
	uint32_t next_set;
	uint32_t sets[MAX_SETS][CONV_HOST_WIRE_OUT_COUNT]; //!< output indices; NO_INDEX = none
};

struct in_image
{
	bool used;
	uint32_t generation;
	struct xrt_conv_image img; //!< handle = local value; cpu_ptr resolved per lane
	uint64_t shm_offset;
	uint64_t sync_handle; //!< handles[1], when the image carried its own sync object
};

struct in_sync
{
	bool used;
	uint32_t kind;
	uint64_t handle;
};

struct lane
{
	bool used;
	uint32_t index;
	uint64_t shm_handle;
	void *map;
	uint64_t shm_bytes;
	struct u_shm_ring submit;   //!< consumer
	struct u_shm_ring complete; //!< producer
	struct os_wake wake_in;  //!< the service signals: the command ring is non-empty
	struct os_wake wake_out; //!< we signal: the completion ring is non-empty
	struct os_thread thread;
	volatile bool stop;
	struct conv_host_serve *srv;
};

struct pending
{
	struct conv_host_cmd cmd;
	struct conv_host_serve *srv;
	uint32_t request_id;
	uint32_t msg;
	uint64_t stream_id;
	volatile bool cancelled;
	struct xrt_conv_stream_info info;
	/* JOB */
	uint64_t job_id;
	struct xrt_conv_job_in job_in;
	struct xrt_conv_params params;
	struct xrt_conv_job_control ctl;
	struct xrt_conv_job_out job_out;
	struct xrt_conv_splat_result splat_result; //!< ADR-057: job_out.splat_result for a splat job
	struct xrt_conv_blob_sink sinks[5]; //!< one per role: a provider matches sink.role EXACTLY
	uint8_t *blob;
	uint64_t blob_bytes, blob_cap;
	uint32_t blob_format;
	uint64_t last_progress_ns;
};

struct conv_host_serve
{
	struct conv_host_chan *chan;
	const struct conv_host_serve_config *cfg;
	struct conv_host *host;

	struct os_mutex mtx; //!< guards everything below
	struct stream streams[MAX_STREAMS];
	struct in_image inputs[MAX_INPUTS];
	struct in_sync syncs[MAX_SYNCS];
	struct out_image outputs[MAX_OUTPUTS];
	struct lane lanes[MAX_LANES];
	struct pending *warms[MAX_PENDING_WARMS];
	struct pending *jobs[MAX_PENDING_WARMS];
	uint32_t warm_pins[XRT_CONV_MAX_CAPABILITIES]; //!< cap ids warmed (0 = free)
	uint32_t commands_in_flight;
	uint32_t input_generation;
	bool fence_announced;
	struct
	{
		uint64_t job_id;
		uint64_t handle;
	} kept_blobs[MAX_KEPT_BLOBS]; //!< Windows: results the service has not JOB_RELEASEd yet
	uint32_t kept_blob_next;
	uint64_t last_bound_ns;

	struct os_thread ticker;
	volatile bool stopping;
};

static struct
{
	struct os_mutex mtx;
	bool mtx_ok;
	int code;
	bool requested;
	struct conv_host_chan *chan;
} g_exit;

void
conv_host_serve_prepare(void)
{
	if (!g_exit.mtx_ok) {
		g_exit.mtx_ok = os_mutex_init(&g_exit.mtx) == 0;
	}
}

void
conv_host_serve_request_exit(int exit_code)
{
	if (!g_exit.mtx_ok) {
		return;
	}
	os_mutex_lock(&g_exit.mtx);
	if (!g_exit.requested) {
		g_exit.requested = true;
		g_exit.code = exit_code;
		conv_host_chan_shutdown(g_exit.chan); // unblocks the control thread
	}
	os_mutex_unlock(&g_exit.mtx);
}


/*
 *
 * Small helpers.
 *
 */

static bool
send_msg(struct conv_host_serve *s, uint32_t type, uint32_t rid, const void *payload, uint32_t bytes)
{
	return conv_host_chan_send(s->chan, type, rid, payload, bytes, NULL, 0);
}

static void
send_status(struct conv_host_serve *s, uint32_t req_type, uint32_t rid, xrt_result_t r)
{
	struct conv_host_wire_status st;
	conv_host_wire_init(&st, sizeof(st));
	st.result = (int32_t)r;
	(void)send_msg(s, req_type | CONV_HOST_MSG_REPLY_BIT, rid, &st, sizeof(st));
}

static struct stream *
stream_find_locked(struct conv_host_serve *s, uint64_t id)
{
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		if (s->streams[i].used && s->streams[i].id == id) {
			return &s->streams[i];
		}
	}
	return NULL;
}

static uint32_t
bytes_per_texel(uint32_t fmt)
{
	switch (fmt) {
	case XRT_CONV_FMT_R8_UNORM: return 1;
	case XRT_CONV_FMT_R16_FLOAT:
	case XRT_CONV_FMT_R16_UNORM: return 2;
	case XRT_CONV_FMT_RGBA16_FLOAT: return 8;
	case XRT_CONV_FMT_RGBA8_UNORM:
	case XRT_CONV_FMT_RGBA8_SRGB:
	case XRT_CONV_FMT_BGRA8_UNORM:
	case XRT_CONV_FMT_BGRA8_SRGB:
	case XRT_CONV_FMT_R32_FLOAT: return 4;
	default: return 0;
	}
}

#ifdef XRT_OS_WINDOWS
static uint32_t
dxgi_of(uint32_t fmt)
{
	switch (fmt) {
	case XRT_CONV_FMT_RGBA8_UNORM: return 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
	case XRT_CONV_FMT_RGBA8_SRGB: return 29;   // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
	case XRT_CONV_FMT_BGRA8_UNORM: return 87;  // DXGI_FORMAT_B8G8R8A8_UNORM
	case XRT_CONV_FMT_BGRA8_SRGB: return 91;   // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
	case XRT_CONV_FMT_R8_UNORM: return 61;     // DXGI_FORMAT_R8_UNORM
	case XRT_CONV_FMT_R16_FLOAT: return 54;    // DXGI_FORMAT_R16_FLOAT
	case XRT_CONV_FMT_R16_UNORM: return 56;    // DXGI_FORMAT_R16_UNORM
	case XRT_CONV_FMT_R32_FLOAT: return 41;    // DXGI_FORMAT_R32_FLOAT
	case XRT_CONV_FMT_RGBA16_FLOAT: return 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
	default: return 0;
	}
}
#endif

static uint32_t
bound_tasks_locked(const struct conv_host_serve *s) // calls, streams AND warm pins: "no bound task and no prewarm" (§3.4)
{
	uint32_t n = s->commands_in_flight;
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		n += s->streams[i].used ? 1u : 0u;
	}
	for (uint32_t i = 0; i < XRT_CONV_MAX_CAPABILITIES; i++) {
		n += s->warm_pins[i] != 0 ? 1u : 0u;
	}
	return n;
}

static void
pin_locked(struct conv_host_serve *s, uint32_t cap_id, bool on)
{
	for (uint32_t i = 0; i < XRT_CONV_MAX_CAPABILITIES; i++) {
		if (s->warm_pins[i] == cap_id) {
			if (!on) {
				s->warm_pins[i] = 0;
			}
			return;
		}
	}
	for (uint32_t i = 0; on && i < XRT_CONV_MAX_CAPABILITIES; i++) {
		if (s->warm_pins[i] == 0) {
			s->warm_pins[i] = cap_id;
			return;
		}
	}
}

static uint32_t
cap_import_kinds(struct conv_host_serve *s, uint32_t cap_id)
{
	uint32_t count = 0;
	if (conv_host_describe_cap(s->host, 0, NULL, &count) != XRT_SUCCESS) {
		return 0;
	}
	for (uint32_t i = 0; i < count && i < XRT_CONV_MAX_CAPABILITIES; i++) {
		struct xrt_conv_capability c;
		memset(&c, 0, sizeof(c));
		c.struct_size = sizeof(c);
		if (conv_host_describe_cap(s->host, i, &c, NULL) == XRT_SUCCESS && c.cap_id == cap_id) {
			return c.import_memory_kinds;
		}
	}
	return 0;
}


/*
 *
 * Outputs.
 *
 */

static void
output_free_locked(struct conv_host_serve *s, uint32_t idx)
{
	struct out_image *o = &s->outputs[idx];
	if (!o->used) {
		return;
	}
	if (o->map != NULL) {
		conv_host_shm_unmap(o->map, o->map_bytes);
	}
	conv_host_handle_close(o->shm_handle);
#ifdef XRT_OS_WINDOWS
	if (o->tex.texture != NULL) {
		conv_host_d3d11_free_image(s->cfg->d3d11, &o->tex);
	}
#endif
	memset(o, 0, sizeof(*o));
}

static void
stream_free_sets_locked(struct conv_host_serve *s, struct stream *st)
{
	for (uint32_t k = 0; k < MAX_SETS; k++) {
		for (uint32_t r = 0; r < CONV_HOST_WIRE_OUT_COUNT; r++) {
			if (st->sets[k][r] != CONV_HOST_WIRE_NO_INDEX && st->sets[k][r] < MAX_OUTPUTS) {
				output_free_locked(s, st->sets[k][r]);
			}
			st->sets[k][r] = CONV_HOST_WIRE_NO_INDEX;
		}
	}
	st->set_count = 0;
	st->next_set = 0;
}

/*!
 * Allocate one output image of @p w x @p h x @p layers in @p fmt; returns its
 * index or NO_INDEX. Lock held.
 */
static uint32_t
output_alloc_locked(struct conv_host_serve *s,
                    uint64_t stream,
                    uint32_t role,
                    uint32_t w,
                    uint32_t h,
                    uint32_t layers,
                    uint32_t fmt,
                    bool gpu)
{
	uint32_t idx = CONV_HOST_WIRE_NO_INDEX;
	for (uint32_t i = 0; i < MAX_OUTPUTS; i++) {
		if (!s->outputs[i].used) {
			idx = i;
			break;
		}
	}
	if (idx == CONV_HOST_WIRE_NO_INDEX || w == 0 || h == 0) {
		return CONV_HOST_WIRE_NO_INDEX;
	}
	struct out_image *o = &s->outputs[idx];
	memset(o, 0, sizeof(*o));
	o->img.width = w;
	o->img.height = h;
	o->img.format = fmt;
	o->img.color_space = role == CONV_HOST_WIRE_OUT_VIEWS ? XRT_CONV_CS_SRGB : XRT_CONV_CS_DATA;
	o->img.import_key = ((uint64_t)0xC0u << 56) | ((uint64_t)s->input_generation << 16) | idx;

#ifdef XRT_OS_WINDOWS
	if (gpu && s->cfg->d3d11 != NULL) {
		const uint32_t dx = dxgi_of(fmt);
		if (dx == 0 || !conv_host_d3d11_alloc_image(s->cfg->d3d11, w, h, layers, dx, &o->tex)) {
			return CONV_HOST_WIRE_NO_INDEX;
		}
		o->img.memory_kind = XRT_CONV_MEM_WIN32_NT_HANDLE;
		o->img.handle = o->tex.nt_handle;
		o->img.adapter_luid = s->cfg->adapter_luid;
		o->used = true;
		o->stream = stream;
		o->role = role;
		return idx;
	}
#else
	(void)gpu;
#endif
	const uint32_t bpt = bytes_per_texel(fmt);
	if (bpt == 0) {
		return CONV_HOST_WIRE_NO_INDEX;
	}
	const uint64_t stride = (uint64_t)w * bpt;
	const uint64_t bytes = stride * h * (layers > 0 ? layers : 1);
	void *map = NULL;
	const uint64_t hshm = conv_host_shm_create(bytes, &map);
	if (hshm == 0) {
		return CONV_HOST_WIRE_NO_INDEX;
	}
	o->img.memory_kind = XRT_CONV_MEM_CPU;
	o->img.row_stride = (uint32_t)stride;
	o->img.byte_size = bytes;
	o->img.cpu_ptr = map;
	o->shm_handle = hshm;
	o->map = map;
	o->map_bytes = bytes;
	o->used = true;
	o->stream = stream;
	o->role = role;
	return idx;
}

static void
handle_alloc_outputs(struct conv_host_serve *s, uint32_t rid, const void *payload, uint32_t bytes)
{
	struct conv_host_wire_alloc_outputs a;
	if (!conv_host_wire_receive(&a, sizeof(a), payload, bytes)) {
		send_status(s, CONV_HOST_MSG_ALLOC_OUTPUTS, rid, XRT_ERROR_IPC_FAILURE);
		return;
	}
	const uint32_t sets = a.set_count == 0 ? 1u : (a.set_count > MAX_SETS ? MAX_SETS : a.set_count);

	os_mutex_lock(&s->mtx);
	struct stream *st = stream_find_locked(s, a.stream);
	const bool ready = st != NULL && st->ready;
	const uint64_t handle = ready ? st->handle : 0;
	const uint32_t cap_id = ready ? st->cap_id : 0;
	const struct xrt_conv_stream_info info = ready ? st->info : (struct xrt_conv_stream_info){0};
	os_mutex_unlock(&s->mtx);
	if (!ready) {
		send_status(s, CONV_HOST_MSG_ALLOC_OUTPUTS, rid, XRT_ERROR_IPC_FAILURE);
		return;
	}

	struct xrt_conv_output_desc d;
	memset(&d, 0, sizeof(d));
	d.struct_size = sizeof(d);
	xrt_result_t r = conv_host_get_output_desc(s->host, handle, a.width, a.height, &d);
	if (r != XRT_SUCCESS) {
		send_status(s, CONV_HOST_MSG_ALLOC_OUTPUTS, rid, r);
		return;
	}
	const bool gpu = s->cfg->d3d11 != NULL && (cap_import_kinds(s, cap_id) & XRT_CONV_MEM_WIN32_NT_HANDLE) != 0;
	const uint32_t layers = info.views_layout == XRT_CONV_VIEWS_ARRAY && info.view_count > 0 ? info.view_count : 1;

	// Allocate the new sets; the previous ones go after the reply (§ wire).
	uint32_t newsets[MAX_SETS][CONV_HOST_WIRE_OUT_COUNT];
	uint32_t old_set_count = 0;
	uint32_t oldsets[MAX_SETS][CONV_HOST_WIRE_OUT_COUNT];
	os_mutex_lock(&s->mtx);
	st = stream_find_locked(s, a.stream);
	for (uint32_t k = 0; k < sets && st != NULL && r == XRT_SUCCESS; k++) {
		for (uint32_t role = 0; role < CONV_HOST_WIRE_OUT_COUNT; role++) {
			uint32_t w = 0;
			uint32_t h = 0;
			uint32_t f = 0;
			uint32_t lay = 1;
			switch (role) {
			case CONV_HOST_WIRE_OUT_VIEWS:
				w = d.views_width, h = d.views_height, f = d.views_format, lay = layers;
				break;
			case CONV_HOST_WIRE_OUT_DEPTH: w = d.depth_width, h = d.depth_height, f = d.depth_format; break;
			case CONV_HOST_WIRE_OUT_CONFIDENCE:
				w = d.confidence_width, h = d.confidence_height, f = d.confidence_format;
				break;
			default: w = d.disparity_width, h = d.disparity_height, f = d.disparity_format; break;
			}
			newsets[k][role] = CONV_HOST_WIRE_NO_INDEX;
			if (w == 0 || h == 0) {
				continue;
			}
			newsets[k][role] = output_alloc_locked(s, a.stream, role, w, h, lay, f, gpu);
			if (newsets[k][role] == CONV_HOST_WIRE_NO_INDEX) {
				r = XRT_ERROR_ALLOCATION;
			}
		}
	}
	if (st == NULL) {
		r = XRT_ERROR_IPC_FAILURE;
	}
	if (r == XRT_SUCCESS) {
		memcpy(oldsets, st->sets, sizeof(oldsets));
		old_set_count = st->set_count;
		for (uint32_t k = 0; k < MAX_SETS; k++) {
			for (uint32_t role = 0; role < CONV_HOST_WIRE_OUT_COUNT; role++) {
				st->sets[k][role] = k < sets ? newsets[k][role] : CONV_HOST_WIRE_NO_INDEX;
			}
		}
		st->set_count = sets;
		st->next_set = 0;
	}
	os_mutex_unlock(&s->mtx);

	if (r != XRT_SUCCESS) {
		os_mutex_lock(&s->mtx);
		for (uint32_t k = 0; k < sets; k++) {
			for (uint32_t role = 0; role < CONV_HOST_WIRE_OUT_COUNT; role++) {
				if (k < MAX_SETS && newsets[k][role] < MAX_OUTPUTS) {
					output_free_locked(s, newsets[k][role]);
				}
			}
		}
		os_mutex_unlock(&s->mtx);
		send_status(s, CONV_HOST_MSG_ALLOC_OUTPUTS, rid, r);
		return;
	}

#ifdef XRT_OS_WINDOWS
	// The output fence, announced once (the service duplicates it once).
	uint64_t fence_h = 0;
	if (gpu && !s->fence_announced && conv_host_d3d11_fence(s->cfg->d3d11, &fence_h)) {
		struct conv_host_wire_sync sy;
		conv_host_wire_init(&sy, sizeof(sy));
		sy.index = 0;
		sy.kind = XRT_CONV_SYNC_FENCE_NT;
		sy.direction = CONV_HOST_SYNC_HOST_SIGNALS;
		sy.initial_value = conv_host_d3d11_completed(s->cfg->d3d11);
		if (conv_host_chan_send(s->chan, CONV_HOST_MSG_SYNC, rid, &sy, sizeof(sy), &fence_h, 1)) {
			s->fence_announced = true;
		}
	}
#endif

	for (uint32_t k = 0; k < sets; k++) {
		for (uint32_t role = 0; role < CONV_HOST_WIRE_OUT_COUNT; role++) {
			const uint32_t idx = newsets[k][role];
			if (idx == CONV_HOST_WIRE_NO_INDEX) {
				continue;
			}
			os_mutex_lock(&s->mtx);
			const struct out_image o = s->outputs[idx];
			os_mutex_unlock(&s->mtx);
			struct conv_host_wire_output_image oi;
			conv_host_wire_init(&oi, sizeof(oi));
			oi.stream = a.stream;
			oi.index = idx;
			oi.role = role;
			xrt_conv_wire_pack_image(&o.img, role, 0, &oi.image);
			const uint64_t h = o.img.memory_kind == XRT_CONV_MEM_CPU ? o.shm_handle : o.img.handle;
			(void)conv_host_chan_send(s->chan, CONV_HOST_MSG_OUTPUT_IMAGE, rid, &oi, sizeof(oi), &h, 1);
		}
	}
	send_status(s, CONV_HOST_MSG_ALLOC_OUTPUTS, rid, XRT_SUCCESS);

	os_mutex_lock(&s->mtx);
	for (uint32_t k = 0; k < old_set_count && k < MAX_SETS; k++) {
		for (uint32_t role = 0; role < CONV_HOST_WIRE_OUT_COUNT; role++) {
			if (oldsets[k][role] < MAX_OUTPUTS) {
				output_free_locked(s, oldsets[k][role]);
			}
		}
	}
	os_mutex_unlock(&s->mtx);
}


/*
 *
 * Async [S] commands (warm / evict / stream create / destroy / job).
 *
 */

static bool
pending_is_cancelled(void *ctx)
{
	const struct pending *p = (const struct pending *)ctx;
	return p->cancelled || p->srv->stopping;
}

static void
track_locked(struct pending **arr, struct pending *p, bool add)
{
	for (uint32_t i = 0; i < MAX_PENDING_WARMS; i++) {
		if (add ? arr[i] == NULL : arr[i] == p) {
			arr[i] = add ? p : NULL;
			return;
		}
	}
}

static void
pending_done(struct conv_host_cmd *cmd)
{
	struct pending *p = (struct pending *)cmd->user;
	struct conv_host_serve *s = p->srv;
	const xrt_result_t r = cmd->result;

	switch (p->msg) {
	case CONV_HOST_MSG_WARM:
		os_mutex_lock(&s->mtx);
		track_locked(s->warms, p, false);
		if (r == XRT_SUCCESS) {
			pin_locked(s, cmd->cap_id, true);
		}
		os_mutex_unlock(&s->mtx);
		send_status(s, p->msg, p->request_id, r);
		break;
	case CONV_HOST_MSG_EVICT:
		os_mutex_lock(&s->mtx);
		pin_locked(s, cmd->cap_id, false);
		os_mutex_unlock(&s->mtx);
		send_status(s, p->msg, p->request_id, r);
		break;
	case CONV_HOST_MSG_STREAM_OPEN: {
		bool close_now = false;
		uint32_t close_rid = 0;
		os_mutex_lock(&s->mtx);
		struct stream *st = stream_find_locked(s, p->stream_id);
		if (st != NULL) {
			if (r == XRT_SUCCESS) {
				st->ready = true;
				st->handle = cmd->stream;
				close_now = st->close_requested;
				close_rid = st->close_request_id;
			} else {
				// The close that overtook a failed create is answered too.
				if (st->close_requested) {
					close_rid = st->close_request_id;
				}
				memset(st, 0, sizeof(*st));
			}
		}
		os_mutex_unlock(&s->mtx);
		send_status(s, p->msg, p->request_id, r);
		if (r != XRT_SUCCESS && close_rid != 0) {
			send_status(s, CONV_HOST_MSG_STREAM_CLOSE, close_rid, XRT_SUCCESS);
		}
		if (close_now) {
			// A STREAM_CLOSE overtook the create: destroy it now.
			struct pending *q = calloc(1, sizeof(*q));
			if (q != NULL) {
				q->srv = s;
				q->request_id = close_rid;
				q->msg = CONV_HOST_MSG_STREAM_CLOSE;
				q->stream_id = p->stream_id;
				q->cmd.op = CONV_OP_STREAM_DESTROY;
				q->cmd.cap_id = cmd->cap_id;
				q->cmd.stream = cmd->stream;
				q->cmd.on_complete = pending_done;
				q->cmd.user = q;
				os_mutex_lock(&s->mtx);
				s->commands_in_flight++;
				os_mutex_unlock(&s->mtx);
				if (conv_host_submit(s->host, &q->cmd) != XRT_SUCCESS) {
					q->cmd.result = XRT_ERROR_IPC_FAILURE;
					pending_done(&q->cmd);
				}
			}
		}
		break;
	}
	case CONV_HOST_MSG_STREAM_CLOSE:
		os_mutex_lock(&s->mtx);
		{
			struct stream *st = stream_find_locked(s, p->stream_id);
			if (st != NULL) {
				stream_free_sets_locked(s, st);
				memset(st, 0, sizeof(*st));
			}
		}
		os_mutex_unlock(&s->mtx);
		send_status(s, p->msg, p->request_id, r);
		break;
	case CONV_HOST_MSG_JOB_SUBMIT: {
		struct conv_host_wire_job_done jd;
		conv_host_wire_init(&jd, sizeof(jd));
		jd.result = (int32_t)r;
		jd.written = p->job_out.written;
		jd.job_id = p->job_id;
		jd.blob_format = p->blob_format;
		xrt_conv_wire_pack_splat_info(&p->job_out.splat_info, &jd.splat_info);
		xrt_conv_wire_pack_depth_info(&p->job_out.depth_info, &jd.depth_info);
		xrt_conv_wire_pack_views_info(&p->job_out.views_info, &jd.views_info);
		conv_host_wire_pack_splat_result(p->job_out.splat_result, &jd.splat_result);
		uint64_t h = 0;
		if (r == XRT_SUCCESS && p->blob_bytes > 0) {
			void *map = NULL;
			h = conv_host_shm_create(p->blob_bytes, &map);
			if (h != 0) {
				memcpy(map, p->blob, (size_t)p->blob_bytes);
				conv_host_shm_unmap(map, p->blob_bytes);
				jd.blob_bytes = p->blob_bytes;
			} else {
				jd.result = (int32_t)XRT_ERROR_ALLOCATION;
			}
		}
		(void)conv_host_chan_send(s->chan, CONV_HOST_MSG_JOB_SUBMIT | CONV_HOST_MSG_REPLY_BIT, p->request_id,
		                          &jd, sizeof(jd), &h, h != 0 ? 1u : 0u);
		if (h != 0) {
#ifdef XRT_OS_WINDOWS
			// The service PULLS it from this process: keep the last few
			// open (there is no release message), close the oldest.
			os_mutex_lock(&s->mtx);
			// Fallback when the service never sends JOB_RELEASE.
			conv_host_handle_close(s->kept_blobs[s->kept_blob_next].handle);
			s->kept_blobs[s->kept_blob_next].job_id = p->job_id;
			s->kept_blobs[s->kept_blob_next].handle = h;
			s->kept_blob_next = (s->kept_blob_next + 1) % MAX_KEPT_BLOBS;
			os_mutex_unlock(&s->mtx);
#else
			conv_host_handle_close(h); // SCM_RIGHTS duplicated it
#endif
		}
		os_mutex_lock(&s->mtx);
		track_locked(s->jobs, p, false);
		os_mutex_unlock(&s->mtx);
		free(p->blob);
		break;
	}
	default: send_status(s, p->msg, p->request_id, r); break;
	}

	os_mutex_lock(&s->mtx);
	if (s->commands_in_flight > 0) {
		s->commands_in_flight--;
	}
	s->last_bound_ns = os_monotonic_get_ns();
	os_mutex_unlock(&s->mtx);
	free(p);
}

static void
submit_pending(struct conv_host_serve *s, struct pending *p)
{
	p->cmd.on_complete = pending_done;
	p->cmd.user = p;
	os_mutex_lock(&s->mtx);
	s->commands_in_flight++;
	os_mutex_unlock(&s->mtx);
	if (conv_host_submit(s->host, &p->cmd) != XRT_SUCCESS) {
		p->cmd.result = XRT_ERROR_IPC_FAILURE;
		pending_done(&p->cmd);
	}
}

static bool
job_sink_begin(void *ctx, uint32_t blob_format, uint64_t size_hint)
{
	struct pending *p = (struct pending *)ctx;
	p->blob_format = blob_format;
	p->blob_bytes = 0;
	if (size_hint > p->blob_cap && size_hint < (1ull << 31)) {
		uint8_t *nb = realloc(p->blob, (size_t)size_hint);
		if (nb != NULL) {
			p->blob = nb;
			p->blob_cap = size_hint;
		}
	}
	return true;
}

static bool
job_sink_write(void *ctx, const void *bytes, uint64_t count)
{
	struct pending *p = (struct pending *)ctx;
	if (p->blob_bytes + count > p->blob_cap) {
		uint64_t cap = p->blob_cap > 0 ? p->blob_cap : 65536u;
		while (cap < p->blob_bytes + count) {
			cap *= 2;
		}
		if (cap >= (1ull << 31)) {
			return false;
		}
		uint8_t *nb = realloc(p->blob, (size_t)cap);
		if (nb == NULL) {
			return false;
		}
		p->blob = nb;
		p->blob_cap = cap;
	}
	memcpy(p->blob + p->blob_bytes, bytes, (size_t)count);
	p->blob_bytes += count;
	return true;
}

static void
job_progress(void *ctx, float fraction, const char *stage)
{
	struct pending *p = (struct pending *)ctx;
	const uint64_t now = os_monotonic_get_ns();
	if (fraction < 1.0f && now - p->last_progress_ns < 100ull * 1000ull * 1000ull) {
		return; // runtime throttles: 10 Hz
	}
	p->last_progress_ns = now;
	struct conv_host_wire_job_progress jp;
	conv_host_wire_init(&jp, sizeof(jp));
	jp.job_id = p->job_id;
	jp.fraction = fraction;
	conv_host_wire_set_str(jp.stage, sizeof(jp.stage), stage);
	(void)send_msg(p->srv, CONV_HOST_MSG_JOB_PROGRESS, 0, &jp, sizeof(jp));
}


/*
 *
 * Inputs.
 *
 */

//! The shm CPU input images live in: the lowest-index open lane's section.
static bool
resolve_cpu_locked(struct conv_host_serve *s, const struct lane *prefer, struct xrt_conv_image *img, uint64_t off)
{
	const struct lane *l = prefer;
	for (uint32_t i = 0; l == NULL && i < MAX_LANES; i++) {
		if (s->lanes[i].used) {
			l = &s->lanes[i];
		}
	}
	if (l == NULL || l->map == NULL || off > l->shm_bytes || img->byte_size > l->shm_bytes - off) {
		return false;
	}
	img->cpu_ptr = (uint8_t *)l->map + off;
	return true;
}

/*! Build an input image from a registered index. Lock held. */
static bool
input_get_locked(struct conv_host_serve *s,
                 const struct lane *lane,
                 uint32_t idx,
                 uint32_t sync_idx,
                 uint64_t fence_value,
                 struct xrt_conv_image *out)
{
	if (idx >= MAX_INPUTS || !s->inputs[idx].used) {
		return false;
	}
	const struct in_image *in = &s->inputs[idx];
	*out = in->img;
	if (out->memory_kind == XRT_CONV_MEM_CPU && !resolve_cpu_locked(s, lane, out, in->shm_offset)) {
		return false;
	}
	if (sync_idx != CONV_HOST_WIRE_NO_INDEX) {
		if (sync_idx >= MAX_SYNCS || !s->syncs[sync_idx].used) {
			return false;
		}
		out->sync.kind = s->syncs[sync_idx].kind;
		out->sync.handle = s->syncs[sync_idx].handle;
		out->sync.import_key = ((uint64_t)0x5Cu << 56) | sync_idx;
		out->sync.acquire_value = fence_value;
		out->sync.release_value = 0;
	} else if (out->memory_kind == XRT_CONV_MEM_CPU || in->sync_handle == 0) {
		out->sync.kind = XRT_CONV_SYNC_NONE;
	} else {
		out->sync.handle = in->sync_handle;
	}
	return true;
}


/*
 *
 * Lane worker.
 *
 */

static void
lane_convert(struct lane *l, const struct conv_host_wire_submit_record *rec, struct conv_host_wire_complete_record *c)
{
	struct conv_host_serve *s = l->srv;
	memset(c, 0, sizeof(*c));
	c->seq = rec->seq;
	for (uint32_t r = 0; r < CONV_HOST_WIRE_OUT_COUNT; r++) {
		c->out_image_idx[r] = CONV_HOST_WIRE_NO_INDEX;
	}
	c->out_sync_idx = CONV_HOST_WIRE_NO_INDEX;

	struct xrt_conv_frame_in fin;
	struct xrt_conv_frame_out fout;
	struct xrt_conv_params params;
	memset(&fin, 0, sizeof(fin));
	memset(&fout, 0, sizeof(fout));
	fin.struct_size = sizeof(fin);
	fout.struct_size = sizeof(fout);
	xrt_conv_wire_unpack_params(&rec->params, &params);
	xrt_conv_wire_unpack_depth_info(&rec->depth_in_info, &fin.depth_in_info);

	uint32_t set[CONV_HOST_WIRE_OUT_COUNT] = {CONV_HOST_WIRE_NO_INDEX, CONV_HOST_WIRE_NO_INDEX,
	                                          CONV_HOST_WIRE_NO_INDEX, CONV_HOST_WIRE_NO_INDEX};
	uint64_t handle = 0;
	uint32_t cap_id = 0;
	bool gpu_out = false;
	xrt_result_t r = XRT_SUCCESS;

	os_mutex_lock(&s->mtx);
	struct stream *st = stream_find_locked(s, rec->stream);
	if (rec->op != CONV_HOST_RING_OP_CONVERT || st == NULL || !st->ready || st->set_count == 0 ||
	    rec->image_count == 0 || rec->image_count > XRT_CONV_MAX_FRAME_IMAGES) {
		r = XRT_ERROR_IPC_FAILURE;
	} else {
		handle = st->handle;
		cap_id = st->cap_id;
		if (st->reset_pending) {
			params.frame_flags |= XRT_CONV_FRAME_RESET;
			st->reset_pending = false;
		}
		memcpy(set, st->sets[st->next_set], sizeof(set));
		st->next_set = (st->next_set + 1) % st->set_count;
		fin.image_count = rec->image_count;
		for (uint32_t i = 0; i < rec->image_count && r == XRT_SUCCESS; i++) {
			fin.image_role[i] = rec->in_image_role[i];
			// The input fence guards every input of the record.
			if (!input_get_locked(s, l, rec->in_image_idx[i], rec->in_sync_idx, rec->in_fence_value,
			                      &fin.images[i])) {
				r = XRT_ERROR_IPC_FAILURE;
			}
		}
		struct xrt_conv_image *targets[CONV_HOST_WIRE_OUT_COUNT] = {&fout.views, &fout.depth, &fout.confidence,
		                                                            &fout.disparity};
		for (uint32_t k = 0; k < CONV_HOST_WIRE_OUT_COUNT && r == XRT_SUCCESS; k++) {
			if (set[k] < MAX_OUTPUTS && s->outputs[set[k]].used) {
				*targets[k] = s->outputs[set[k]].img;
				gpu_out = gpu_out || targets[k]->memory_kind != XRT_CONV_MEM_CPU;
			}
		}
	}
	os_mutex_unlock(&s->mtx);

	uint64_t fence_value = 0;
#ifdef XRT_OS_WINDOWS
	uint64_t fence_h = 0;
	if (r == XRT_SUCCESS && gpu_out && conv_host_d3d11_fence(s->cfg->d3d11, &fence_h)) {
		fence_value = conv_host_d3d11_reserve(s->cfg->d3d11);
		struct xrt_conv_image *targets[CONV_HOST_WIRE_OUT_COUNT] = {&fout.views, &fout.depth, &fout.confidence,
		                                                            &fout.disparity};
		for (uint32_t k = 0; k < CONV_HOST_WIRE_OUT_COUNT; k++) {
			if (targets[k]->memory_kind == XRT_CONV_MEM_WIN32_NT_HANDLE) {
				targets[k]->sync.kind = XRT_CONV_SYNC_FENCE_NT;
				targets[k]->sync.handle = fence_h;
				targets[k]->sync.import_key = (uint64_t)0xFEu << 56;
				targets[k]->sync.release_value = fence_value;
			}
		}
	}
#endif

	c->start_ns = os_monotonic_get_ns();
	if (r == XRT_SUCCESS) {
		struct conv_host_cmd cmd;
		memset(&cmd, 0, sizeof(cmd));
		cmd.op = CONV_OP_CONVERT;
		cmd.cap_id = cap_id;
		cmd.stream = handle;
		cmd.frame_in = &fin;
		cmd.params = &params;
		cmd.frame_out = &fout;
		r = conv_host_call(s->host, &cmd);
	}
	c->end_ns = os_monotonic_get_ns();

	c->status = (int32_t)r;
	if (r == XRT_SUCCESS) {
		c->written = fout.written;
		const uint32_t bits[CONV_HOST_WIRE_OUT_COUNT] = {XRT_CONV_OUT_VIEWS, XRT_CONV_OUT_DEPTH,
		                                                 XRT_CONV_OUT_CONFIDENCE, XRT_CONV_OUT_DISPARITY};
		for (uint32_t k = 0; k < CONV_HOST_WIRE_OUT_COUNT; k++) {
			if ((fout.written & bits[k]) != 0 && set[k] < MAX_OUTPUTS) {
				c->out_image_idx[k] = set[k];
			}
		}
		xrt_conv_wire_pack_views_info(&fout.views_info, &c->views_info);
		xrt_conv_wire_pack_depth_info(&fout.depth_info, &c->depth_info);
	}
#ifdef XRT_OS_WINDOWS
	if (fence_value != 0) {
		if (r != XRT_SUCCESS) {
			// Nobody else will write this value: the service's CPU
			// observe must not wait for it until its timeout.
			(void)conv_host_d3d11_signal_value(s->cfg->d3d11, fence_value);
		}
		c->out_sync_idx = 0;
		c->out_fence_value = fence_value;
	}
#endif
}

static void
lane_push_completion(struct lane *l, const struct conv_host_wire_complete_record *c)
{
	const uint64_t give_up = os_monotonic_get_ns() + COMPLETE_FULL_WAIT_NS;
	for (;;) {
		bool wake = false;
		const enum u_shm_ring_result pr = u_shm_ring_push(&l->complete, c, &wake);
		if (pr == U_SHM_RING_OK) {
			if (wake) {
				os_wake_signal(&l->wake_out);
			}
			return;
		}
		if (pr == U_SHM_RING_CORRUPT || l->stop || os_monotonic_get_ns() > give_up) {
			U_LOG_W("conversion host: lane %u completion ring %s; dropping seq %llu", l->index,
			        pr == U_SHM_RING_CORRUPT ? "corrupt" : "full", (unsigned long long)c->seq);
			return;
		}
		os_wake_signal(&l->wake_out); // the service is behind: nudge it
		os_nanosleep(1000 * 1000);
	}
}

static void *
lane_thread(void *ptr)
{
	struct lane *l = (struct lane *)ptr;
	struct conv_host_wire_submit_record rec;
	struct conv_host_wire_complete_record comp;
	while (!l->stop) {
		const enum u_shm_ring_result pr = u_shm_ring_pop(&l->submit, &rec);
		if (pr == U_SHM_RING_OK) {
			lane_convert(l, &rec, &comp);
			lane_push_completion(l, &comp);
			continue;
		}
		if (pr == U_SHM_RING_CORRUPT) {
			// The service scribbled on its own ring? Treat like a dead peer.
			U_LOG_E("conversion host: lane %u command ring corrupt; stopping the lane", l->index);
			break;
		}
		if (u_shm_ring_prepare_wait(&l->submit)) {
			(void)os_wake_wait(&l->wake_in, LANE_WAIT_MS);
			u_shm_ring_finish_wait(&l->submit);
		}
	}
	return NULL;
}

static void
lane_close_locked_out(struct conv_host_serve *s, struct lane *l)
{
	// Called WITHOUT s->mtx: joins the worker, which takes it.
	(void)s;
	if (!l->used) {
		return;
	}
	l->stop = true;
	os_thread_join(&l->thread);
	os_thread_destroy(&l->thread);
	if (l->map != NULL) {
		conv_host_shm_unmap(l->map, l->shm_bytes);
	}
	conv_host_handle_close(l->shm_handle);
	os_wake_destroy(&l->wake_in);
	os_wake_destroy(&l->wake_out);
	memset(l, 0, sizeof(*l));
}

static void
handle_lane_open(struct conv_host_serve *s,
                 uint32_t rid,
                 const struct conv_host_wire_frame *f,
                 const void *payload,
                 uint32_t bytes)
{
	struct conv_host_wire_lane_open lo;
	xrt_result_t r = XRT_SUCCESS;
	if (!conv_host_wire_receive(&lo, sizeof(lo), payload, bytes) || f->handle_count < 3) {
		r = XRT_ERROR_IPC_FAILURE;
	} else if (lo.lane >= MAX_LANES || lo.lane_kind != CONV_HOST_LANE_STREAM ||
	           lo.submit_record_bytes != sizeof(struct conv_host_wire_submit_record) ||
	           lo.complete_record_bytes != sizeof(struct conv_host_wire_complete_record)) {
		r = XRT_ERROR_IPC_FAILURE;
	}
	struct lane *l = r == XRT_SUCCESS ? &s->lanes[lo.lane] : NULL;
	if (l != NULL && l->used) {
		lane_close_locked_out(s, l); // re-open replaces
	}
	if (r == XRT_SUCCESS) {
		const size_t sb = u_shm_ring_bytes(lo.submit_record_bytes, lo.ring_capacity);
		const size_t cb = u_shm_ring_bytes(lo.complete_record_bytes, lo.ring_capacity);
		if (sb == 0 || cb == 0 || lo.submit_ring_offset > lo.shm_bytes || sb > lo.shm_bytes - lo.submit_ring_offset ||
		    lo.complete_ring_offset > lo.shm_bytes || cb > lo.shm_bytes - lo.complete_ring_offset ||
		    (lo.submit_ring_offset % 8) != 0 || (lo.complete_ring_offset % 8) != 0) {
			r = XRT_ERROR_IPC_FAILURE;
		} else {
			void *map = conv_host_shm_map(f->handles[0], lo.shm_bytes);
			if (map == NULL) {
				r = XRT_ERROR_IPC_FAILURE;
			} else {
				memset(l, 0, sizeof(*l));
				l->map = map;
				l->shm_bytes = lo.shm_bytes;
				l->shm_handle = f->handles[0];
				os_wake_from_native(&l->wake_in, f->handles[1]);
				os_wake_from_native(&l->wake_out, f->handles[2]);
				l->index = lo.lane;
				l->srv = s;
				if (!u_shm_ring_attach(&l->submit, (uint8_t *)map + lo.submit_ring_offset, sb,
				                       lo.submit_record_bytes, lo.ring_capacity) ||
				    !u_shm_ring_attach(&l->complete, (uint8_t *)map + lo.complete_ring_offset, cb,
				                       lo.complete_record_bytes, lo.ring_capacity)) {
					conv_host_shm_unmap(map, lo.shm_bytes);
					memset(l, 0, sizeof(*l));
					r = XRT_ERROR_IPC_FAILURE;
				}
			}
		}
	}
	if (r == XRT_SUCCESS) {
		os_mutex_lock(&s->mtx);
		l->used = true;
		os_mutex_unlock(&s->mtx);
		(void)os_thread_init(&l->thread);
		if (os_thread_start(&l->thread, lane_thread, l) != 0) {
			r = XRT_ERROR_THREADING_INIT_FAILURE;
			lane_close_locked_out(s, l);
		}
	} else {
		for (uint32_t i = 0; i < f->handle_count; i++) {
			conv_host_handle_close(f->handles[i]);
		}
	}
	send_status(s, CONV_HOST_MSG_LANE_OPEN, rid, r);
}


/*
 *
 * Control dispatch.
 *
 */

static void
close_handles(const struct conv_host_wire_frame *f)
{
	for (uint32_t i = 0; i < f->handle_count; i++) {
		conv_host_handle_close(f->handles[i]);
	}
}

//! Returns false when the loop must end (SHUTDOWN).
static bool
dispatch(struct conv_host_serve *s, const struct conv_host_wire_frame *f, const void *pl)
{
	const uint32_t rid = f->request_id;
	const uint32_t n = f->payload_bytes;

	switch (f->type) {
	case CONV_HOST_MSG_DESCRIBE: {
		struct conv_host_wire_index q;
		struct conv_host_wire_describe_reply rep;
		conv_host_wire_init(&rep, sizeof(rep));
		if (!conv_host_wire_receive(&q, sizeof(q), pl, n)) {
			rep.result = XRT_ERROR_IPC_FAILURE;
		} else {
			struct xrt_conv_capability cap;
			memset(&cap, 0, sizeof(cap));
			cap.struct_size = sizeof(cap);
			uint32_t count = 0;
			rep.result = (int32_t)conv_host_describe_cap(s->host, 0, NULL, &count);
			rep.count = count;
			if (rep.result == XRT_SUCCESS) {
				rep.result = (int32_t)conv_host_describe_cap(s->host, q.index, &cap, NULL);
			}
			if (rep.result == XRT_SUCCESS) {
				conv_host_wire_pack_capability(&cap, &rep.capability);
			}
			rep.generation = conv_host_get_generation(s->host);
		}
		(void)send_msg(s, f->type | CONV_HOST_MSG_REPLY_BIT, rid, &rep, sizeof(rep));
		return true;
	}
	case CONV_HOST_MSG_GET_PROVIDER_STATUS: {
		// The provider's own view of its platform (FALLBACK flag, state, hint):
		// the service ranks and reports a satellite provider by it (#2034).
		struct conv_host_wire_provider_status rep;
		conv_host_wire_init(&rep, sizeof(rep));
		const struct xrt_conversion_plugin_iface *iface =
		    s->cfg->provider->entry != NULL ? s->cfg->provider->entry->iface : NULL;
		struct xrt_conv_provider_status st;
		memset(&st, 0, sizeof(st));
		st.struct_size = sizeof(st);
		if (iface != NULL && XRT_CONV_IFACE_HAS(iface, get_provider_status) && iface->get_provider_status != NULL &&
		    iface->get_provider_status(&st)) {
			rep.reported = 1;
			rep.state = st.state;
			rep.flags = st.flags;
			conv_host_wire_set_str(rep.hint, sizeof(rep.hint), st.hint);
		}
		rep.result = XRT_SUCCESS;
		(void)send_msg(s, f->type | CONV_HOST_MSG_REPLY_BIT, rid, &rep, sizeof(rep));
		return true;
	}
	case CONV_HOST_MSG_GET_STATE: {
		struct conv_host_wire_cap_request q;
		struct conv_host_wire_state_reply rep;
		conv_host_wire_init(&rep, sizeof(rep));
		if (!conv_host_wire_receive(&q, sizeof(q), pl, n)) {
			rep.result = XRT_ERROR_IPC_FAILURE;
		} else {
			struct xrt_conv_state stv;
			memset(&stv, 0, sizeof(stv));
			stv.struct_size = sizeof(stv);
			rep.cap_id = q.cap_id;
			rep.result = (int32_t)conv_host_get_state(s->host, q.cap_id, &stv);
			conv_host_wire_pack_state(&stv, &rep.state);
		}
		(void)send_msg(s, f->type | CONV_HOST_MSG_REPLY_BIT, rid, &rep, sizeof(rep));
		return true;
	}
	case CONV_HOST_MSG_WARM:
	case CONV_HOST_MSG_EVICT: {
		struct conv_host_wire_cap_request q;
		struct pending *p = calloc(1, sizeof(*p));
		if (p == NULL || !conv_host_wire_receive(&q, sizeof(q), pl, n)) {
			free(p);
			send_status(s, f->type, rid, XRT_ERROR_IPC_FAILURE);
			return true;
		}
		p->srv = s;
		p->request_id = rid;
		p->msg = f->type;
		p->cmd.op = f->type == CONV_HOST_MSG_WARM ? CONV_OP_WARM : CONV_OP_EVICT;
		p->cmd.cap_id = q.cap_id;
		p->cmd.is_cancelled = pending_is_cancelled;
		p->cmd.cancel_ctx = p;
		if (f->type == CONV_HOST_MSG_WARM) {
			os_mutex_lock(&s->mtx);
			track_locked(s->warms, p, true);
			os_mutex_unlock(&s->mtx);
		}
		submit_pending(s, p);
		return true;
	}
	case CONV_HOST_MSG_CANCEL: {
		struct conv_host_wire_cancel q;
		if (conv_host_wire_receive(&q, sizeof(q), pl, n)) {
			os_mutex_lock(&s->mtx);
			for (uint32_t i = 0; i < MAX_PENDING_WARMS; i++) {
				if (s->warms[i] != NULL && s->warms[i]->request_id == q.request_id) {
					s->warms[i]->cancelled = true;
				}
			}
			os_mutex_unlock(&s->mtx);
		}
		return true; // no reply: the cancelled WARM replies
	}
	case CONV_HOST_MSG_STREAM_OPEN: {
		struct conv_host_wire_stream_open q;
		struct pending *p = calloc(1, sizeof(*p));
		if (p == NULL || !conv_host_wire_receive(&q, sizeof(q), pl, n)) {
			free(p);
			send_status(s, f->type, rid, XRT_ERROR_IPC_FAILURE);
			return true;
		}
		os_mutex_lock(&s->mtx);
		struct stream *st = stream_find_locked(s, q.stream);
		const bool dup = st != NULL; // the service id is already open (or opening)
		for (uint32_t i = 0; st == NULL && i < MAX_STREAMS; i++) {
			if (s->streams[i].used) {
				continue;
			}
			st = &s->streams[i];
			memset(st, 0, sizeof(*st));
			st->used = true;
			st->id = q.stream;
			for (uint32_t k = 0; k < MAX_SETS; k++) {
				for (uint32_t r = 0; r < CONV_HOST_WIRE_OUT_COUNT; r++) {
					st->sets[k][r] = CONV_HOST_WIRE_NO_INDEX;
				}
			}
		}
		if (st != NULL && !dup) {
			st->cap_id = q.cap_id;
			st->info.struct_size = sizeof(st->info);
			st->info.input_kind = q.input_kind;
			st->info.outputs = q.outputs;
			st->info.content_hint = q.content_hint;
			st->info.view_count = q.view_count;
			st->info.views_layout = q.views_layout;
			st->info.max_input_width = q.max_input_width;
			st->info.max_input_height = q.max_input_height;
			p->info = st->info;
		}
		s->last_bound_ns = os_monotonic_get_ns();
		os_mutex_unlock(&s->mtx);
		if (st == NULL || dup) {
			free(p);
			send_status(s, f->type, rid, st == NULL ? XRT_ERROR_ALLOCATION : XRT_ERROR_IPC_FAILURE);
			return true;
		}
		p->srv = s;
		p->request_id = rid;
		p->msg = f->type;
		p->stream_id = q.stream;
		p->cmd.op = CONV_OP_STREAM_CREATE;
		p->cmd.cap_id = q.cap_id;
		p->cmd.stream_info = &p->info;
		submit_pending(s, p);
		return true;
	}
	case CONV_HOST_MSG_STREAM_RESET:
	case CONV_HOST_MSG_STREAM_CLOSE: {
		struct conv_host_wire_stream_ref q;
		if (!conv_host_wire_receive(&q, sizeof(q), pl, n)) {
			send_status(s, f->type, rid, XRT_ERROR_IPC_FAILURE);
			return true;
		}
		os_mutex_lock(&s->mtx);
		struct stream *st = stream_find_locked(s, q.stream);
		xrt_result_t r = st != NULL ? XRT_SUCCESS : XRT_ERROR_IPC_FAILURE;
		bool destroy = false;
		uint64_t handle = 0;
		uint32_t cap_id = 0;
		if (st != NULL && f->type == CONV_HOST_MSG_STREAM_RESET) {
			st->reset_pending = true; // FRAME_RESET on the next convert (no reset slot in the ABI)
		} else if (st != NULL && !st->ready) {
			st->close_requested = true; // the create's on_complete destroys it
			st->close_request_id = rid;
		} else if (st != NULL) {
			destroy = true;
			handle = st->handle;
			cap_id = st->cap_id;
			st->ready = false; // no more converts
		}
		os_mutex_unlock(&s->mtx);
		if (destroy) {
			struct pending *p = calloc(1, sizeof(*p));
			if (p == NULL) {
				send_status(s, f->type, rid, XRT_ERROR_ALLOCATION);
				return true;
			}
			p->srv = s;
			p->request_id = rid;
			p->msg = f->type;
			p->stream_id = q.stream;
			p->cmd.op = CONV_OP_STREAM_DESTROY;
			p->cmd.cap_id = cap_id;
			p->cmd.stream = handle;
			submit_pending(s, p);
		} else if (f->type == CONV_HOST_MSG_STREAM_RESET || st == NULL) {
			send_status(s, f->type, rid, r);
		}
		return true;
	}
	case CONV_HOST_MSG_GET_OUTPUT_DESC: {
		struct conv_host_wire_output_desc_request q;
		struct conv_host_wire_output_desc_reply rep;
		conv_host_wire_init(&rep, sizeof(rep));
		if (!conv_host_wire_receive(&q, sizeof(q), pl, n)) {
			rep.result = XRT_ERROR_IPC_FAILURE;
		} else {
			os_mutex_lock(&s->mtx);
			struct stream *st = stream_find_locked(s, q.stream);
			const bool ok = st != NULL && st->ready;
			const uint64_t handle = ok ? st->handle : 0;
			os_mutex_unlock(&s->mtx);
			struct xrt_conv_output_desc d;
			memset(&d, 0, sizeof(d));
			d.struct_size = sizeof(d);
			rep.result = ok ? (int32_t)conv_host_get_output_desc(s->host, handle, q.width, q.height, &d)
			                : (int32_t)XRT_ERROR_IPC_FAILURE;
			if (rep.result == XRT_SUCCESS) {
				conv_host_wire_pack_output_desc(&d, &rep);
			}
		}
		(void)send_msg(s, f->type | CONV_HOST_MSG_REPLY_BIT, rid, &rep, sizeof(rep));
		return true;
	}
	case CONV_HOST_MSG_REGISTER_IMAGE: {
		struct conv_host_wire_register_image q;
		xrt_result_t r = XRT_SUCCESS;
		if (!conv_host_wire_receive(&q, sizeof(q), pl, n) || q.index >= MAX_INPUTS) {
			close_handles(f);
			send_status(s, f->type, rid, XRT_ERROR_IPC_FAILURE);
			return true;
		}
		const bool cpu = q.image.memory_kind == XRT_CONV_MEM_CPU;
		if (!cpu && f->handle_count < 1) {
			r = XRT_ERROR_IPC_FAILURE;
		}
		os_mutex_lock(&s->mtx);
		struct in_image *in = &s->inputs[q.index];
		if (r == XRT_SUCCESS) {
			if (in->used) {
				conv_host_handle_close(in->img.handle);
				conv_host_handle_close(in->sync_handle);
			}
			memset(in, 0, sizeof(*in));
			uint64_t off = 0;
			xrt_conv_wire_unpack_image(&q.image, &in->img, NULL, &off);
			in->shm_offset = off;
			uint32_t hi = 0;
			if (!cpu) {
				in->img.handle = f->handles[hi++];
			}
			if (hi < f->handle_count) {
				in->sync_handle = f->handles[hi++];
			}
			in->generation = ++s->input_generation;
			// Stable per slot lifetime: the provider caches imports by key.
			in->img.import_key = ((uint64_t)0x1Au << 56) | ((uint64_t)in->generation << 16) | q.index;
			in->used = true;
		}
		const uint64_t log_handle = r == XRT_SUCCESS ? in->img.handle : 0;
		const uint64_t log_sync = r == XRT_SUCCESS ? in->sync_handle : 0;
		const uint32_t log_gen = r == XRT_SUCCESS ? in->generation : 0;
		os_mutex_unlock(&s->mtx);
		if (r == XRT_SUCCESS) {
			// The duplicated handle values a provider will open: the one
			// oracle for an OpenSharedResource1 failure on the provider side.
			U_LOG_I("conversion host: image slot %u gen %u: %ux%u fmt %u kind %u handle 0x%llx sync 0x%llx",
			        q.index, log_gen, q.image.width, q.image.height, q.image.format, q.image.memory_kind,
			        (unsigned long long)log_handle, (unsigned long long)log_sync);
		}
		if (r != XRT_SUCCESS) {
			close_handles(f);
		}
		send_status(s, f->type, rid, r);
		return true;
	}
	case CONV_HOST_MSG_UNREGISTER_IMAGE: {
		struct conv_host_wire_index q;
		xrt_result_t r = XRT_ERROR_IPC_FAILURE;
		if (conv_host_wire_receive(&q, sizeof(q), pl, n) && q.index < MAX_INPUTS) {
			os_mutex_lock(&s->mtx);
			struct in_image *in = &s->inputs[q.index];
			if (in->used) {
				conv_host_handle_close(in->img.handle);
				conv_host_handle_close(in->sync_handle);
				memset(in, 0, sizeof(*in));
				r = XRT_SUCCESS;
			}
			os_mutex_unlock(&s->mtx);
		}
		send_status(s, f->type, rid, r);
		return true;
	}
	case CONV_HOST_MSG_REGISTER_SYNC: {
		struct conv_host_wire_sync q;
		xrt_result_t r = XRT_ERROR_IPC_FAILURE;
		if (conv_host_wire_receive(&q, sizeof(q), pl, n) && q.index < MAX_SYNCS && f->handle_count == 1 &&
		    q.direction == CONV_HOST_SYNC_SERVICE_SIGNALS) {
			os_mutex_lock(&s->mtx);
			if (s->syncs[q.index].used) {
				conv_host_handle_close(s->syncs[q.index].handle);
			}
			s->syncs[q.index].used = true;
			s->syncs[q.index].kind = q.kind;
			s->syncs[q.index].handle = f->handles[0];
			os_mutex_unlock(&s->mtx);
			r = XRT_SUCCESS;
		} else {
			close_handles(f);
		}
		send_status(s, f->type, rid, r);
		return true;
	}
	case CONV_HOST_MSG_ALLOC_OUTPUTS: handle_alloc_outputs(s, rid, pl, n); return true;
	case CONV_HOST_MSG_LANE_OPEN: handle_lane_open(s, rid, f, pl, n); return true;
	case CONV_HOST_MSG_LANE_CLOSE: {
		struct conv_host_wire_index q;
		xrt_result_t r = XRT_ERROR_IPC_FAILURE;
		if (conv_host_wire_receive(&q, sizeof(q), pl, n) && q.index < MAX_LANES && s->lanes[q.index].used) {
			lane_close_locked_out(s, &s->lanes[q.index]);
			r = XRT_SUCCESS;
		}
		send_status(s, f->type, rid, r);
		return true;
	}
	case CONV_HOST_MSG_JOB_SUBMIT: {
		struct conv_host_wire_job_submit q;
		struct pending *p = calloc(1, sizeof(*p));
		xrt_result_t r = XRT_SUCCESS;
		if (p == NULL || !conv_host_wire_receive(&q, sizeof(q), pl, n) || q.image_count == 0 ||
		    q.image_count > XRT_CONV_MAX_FRAME_IMAGES) {
			r = XRT_ERROR_IPC_FAILURE;
		}
		if (r == XRT_SUCCESS) {
			p->job_in.struct_size = sizeof(p->job_in);
			p->job_in.input_kind = q.input_kind;
			p->job_in.image_count = q.image_count;
			p->job_in.requested_blob_format = q.requested_blob_format;
			p->job_in.job_flags = q.job_flags;
			p->job_in.max_input_long_side = q.max_input_long_side;
			os_mutex_lock(&s->mtx);
			for (uint32_t i = 0; i < q.image_count && r == XRT_SUCCESS; i++) {
				p->job_in.image_role[i] = q.image_role[i];
				if (!input_get_locked(s, NULL, q.image_idx[i], CONV_HOST_WIRE_NO_INDEX, 0,
				                      &p->job_in.images[i])) {
					r = XRT_ERROR_IPC_FAILURE;
				}
			}
			os_mutex_unlock(&s->mtx);
		}
		if (r != XRT_SUCCESS) {
			struct conv_host_wire_job_done jd;
			conv_host_wire_init(&jd, sizeof(jd));
			jd.result = (int32_t)r;
			jd.job_id = p != NULL ? q.job_id : 0;
			(void)send_msg(s, f->type | CONV_HOST_MSG_REPLY_BIT, rid, &jd, sizeof(jd));
			free(p);
			return true;
		}
		p->srv = s;
		p->request_id = rid;
		p->msg = f->type;
		p->job_id = q.job_id;
		xrt_conv_wire_unpack_params(&q.params, &p->params);
		// One sink per output role (a provider picks the sink whose role
		// equals what it emits); all write the one blob this job returns.
		static const uint32_t roles[5] = {XRT_CONV_OUT_SPLAT_BLOB, XRT_CONV_OUT_DEPTH, XRT_CONV_OUT_VIEWS,
		                                  XRT_CONV_OUT_DISPARITY, XRT_CONV_OUT_CONFIDENCE};
		for (uint32_t k = 0; k < 5; k++) {
			p->sinks[k].struct_size = sizeof(p->sinks[k]);
			p->sinks[k].role = roles[k];
			p->sinks[k].ctx = p;
			p->sinks[k].begin = job_sink_begin;
			p->sinks[k].write = job_sink_write;
		}
		p->ctl.struct_size = sizeof(p->ctl);
		p->ctl.sink_count = 5;
		p->ctl.sinks = p->sinks;
		p->ctl.ctx = p;
		p->ctl.progress = job_progress;
		p->ctl.is_cancelled = pending_is_cancelled;
		p->job_out.struct_size = sizeof(p->job_out);
		// ADR-057: the service sends a splat result back for every job; the
		// provider fills it only for a splat task (it knows its cap's task).
		p->splat_result.struct_size = sizeof(p->splat_result);
		p->job_out.splat_result = &p->splat_result;
		p->cmd.op = CONV_OP_JOB;
		p->cmd.cap_id = q.cap_id;
		p->cmd.job_in = &p->job_in;
		p->cmd.params = &p->params;
		p->cmd.job_ctl = &p->ctl;
		p->cmd.job_out = &p->job_out;
		os_mutex_lock(&s->mtx);
		track_locked(s->jobs, p, true);
		os_mutex_unlock(&s->mtx);
		submit_pending(s, p);
		return true;
	}
	case CONV_HOST_MSG_JOB_CANCEL: {
		struct conv_host_wire_job_ref q;
		if (conv_host_wire_receive(&q, sizeof(q), pl, n)) {
			os_mutex_lock(&s->mtx);
			for (uint32_t i = 0; i < MAX_PENDING_WARMS; i++) {
				if (s->jobs[i] != NULL && s->jobs[i]->job_id == q.job_id) {
					s->jobs[i]->cancelled = true;
				}
			}
			os_mutex_unlock(&s->mtx);
		}
		return true; // the job itself replies
	}
	case CONV_HOST_MSG_JOB_RELEASE: {
		struct conv_host_wire_job_release q;
		if (conv_host_wire_receive(&q, sizeof(q), pl, n)) {
			os_mutex_lock(&s->mtx);
			for (uint32_t i = 0; i < MAX_KEPT_BLOBS; i++) {
				if (s->kept_blobs[i].handle != 0 && s->kept_blobs[i].job_id == q.job_id) {
					conv_host_handle_close(s->kept_blobs[i].handle);
					s->kept_blobs[i].handle = 0;
					s->kept_blobs[i].job_id = 0;
				}
			}
			os_mutex_unlock(&s->mtx);
		}
		return true; // no reply (POSIX: our copy was closed right after the send)
	}
	case CONV_HOST_MSG_SHUTDOWN: {
		struct conv_host_wire_shutdown q;
		(void)conv_host_wire_receive(&q, sizeof(q), pl, n);
		U_LOG_W("conversion host: SHUTDOWN (%s)", q.orderly ? "orderly" : "immediate");
		// Reply after the teardown (run()), so "replied" means "drained".
		return false;
	}
	case CONV_HOST_MSG_HELLO_ACK: return true; // a late duplicate: ignore
	default:
		U_LOG_W("conversion host: unknown message 0x%04x (request %u); ignored", f->type, rid);
		close_handles(f);
		if (rid != 0) {
			send_status(s, f->type, rid, XRT_ERROR_IPC_FAILURE);
		}
		return true;
	}
}


/*
 *
 * Ticker: heartbeat, logs, state changes, idle exit.
 *
 */

static void
forward_log(void *ctx, uint32_t level, const char *line)
{
	struct conv_host_serve *s = (struct conv_host_serve *)ctx;
	struct conv_host_wire_log lg;
	conv_host_wire_init(&lg, sizeof(lg));
	lg.level = level;
	conv_host_wire_set_str(lg.text, sizeof(lg.text), line);
	(void)send_msg(s, CONV_HOST_MSG_LOG, 0, &lg, sizeof(lg));
}

static void
send_state_changed(struct conv_host_serve *s, uint32_t cap_id)
{
	struct conv_host_wire_state_changed sc;
	conv_host_wire_init(&sc, sizeof(sc));
	sc.cap_id = cap_id;
	sc.generation = conv_host_get_generation(s->host);
	if (cap_id != 0) {
		struct xrt_conv_state stv;
		memset(&stv, 0, sizeof(stv));
		stv.struct_size = sizeof(stv);
		(void)conv_host_get_state(s->host, cap_id, &stv);
		conv_host_wire_pack_state(&stv, &sc.state);
	}
	(void)send_msg(s, CONV_HOST_MSG_STATE_CHANGED, 0, &sc, sizeof(sc));
}

static void *
ticker_thread(void *ptr)
{
	struct conv_host_serve *s = (struct conv_host_serve *)ptr;
	uint64_t next_hb = 0;
	while (!s->stopping) {
		const uint64_t now = os_monotonic_get_ns();

		uint32_t dropped = 0;
		(void)conv_host_log_drain(forward_log, s, &dropped);
		if (dropped > 0) {
			char msg[96];
			(void)snprintf(msg, sizeof(msg), "(%u log lines dropped by the conversion host)", dropped);
			forward_log(s, U_LOGGING_WARN, msg);
		}

		uint32_t cap = 0;
		if (conv_host_provider_take_changed(&cap)) {
			send_state_changed(s, cap);
		}

		if (now >= next_hb) {
			next_hb = now + (uint64_t)CONV_HOST_WIRE_HEARTBEAT_PERIOD_MS * 1000ull * 1000ull;
			struct conv_host_heartbeat hb;
			memset(&hb, 0, sizeof(hb));
			conv_host_get_heartbeat(s->host, &hb);
			struct conv_host_wire_heartbeat w;
			conv_host_wire_init(&w, sizeof(w));
			w.in_flight = hb.in_flight;
			w.queued = hb.queued;
			w.oldest_op = hb.oldest_op;
			w.oldest_cap_id = hb.oldest_cap_id;
			w.oldest_running_ns = hb.oldest_since_ns != 0 && now > hb.oldest_since_ns ? now - hb.oldest_since_ns : 0;
			w.calls_total = hb.calls_total;
			w.host_time_ns = now;
			w.exec_threads = hb.exec_threads;
			os_mutex_lock(&s->mtx);
			w.bound_tasks = bound_tasks_locked(s);
			if (w.bound_tasks > 0) {
				s->last_bound_ns = now;
			}
			const uint64_t idle_ns = now - s->last_bound_ns;
			os_mutex_unlock(&s->mtx);
			if (!send_msg(s, CONV_HOST_MSG_HEARTBEAT, 0, &w, sizeof(w))) {
				conv_host_serve_request_exit(CONV_HOST_EXIT_ORDERLY); // the service is gone
			}
			if (w.bound_tasks == 0 && idle_ns >= (uint64_t)s->cfg->idle_exit_ms * 1000ull * 1000ull) {
				U_LOG_W("conversion host: idle for %u ms with no bound task; exiting", s->cfg->idle_exit_ms);
				conv_host_serve_request_exit(CONV_HOST_EXIT_ORDERLY);
			}
		}
		os_nanosleep(50ll * 1000 * 1000); // 20 Hz: log / state latency
	}
	return NULL;
}


/*
 *
 * Run.
 *
 */

static int g_backstop_code;

static void *
exit_backstop_thread(void *ptr)
{
	(void)ptr;
	os_nanosleep((int64_t)SHUTDOWN_BUDGET_MS * 1000 * 1000);
	U_LOG_W("conversion host: teardown exceeded %u ms (a provider call is stuck); exiting %d now",
	        SHUTDOWN_BUDGET_MS, g_backstop_code);
	conv_host_log_shutdown();
	_Exit(g_backstop_code);
	return NULL;
}

static void
start_exit_backstop(int code)
{
	static struct os_thread t; // never joined: the process ends first
	g_backstop_code = code;
	(void)os_thread_init(&t);
	(void)os_thread_start(&t, exit_backstop_thread, NULL);
}

static bool
hello(struct conv_host_serve *s)
{
	struct conv_host_wire_hello h;
	conv_host_wire_init(&h, sizeof(h));
	h.host_pid = (uint32_t)os_process_current_pid();
	h.provider_api_version = s->cfg->provider->entry != NULL ? s->cfg->provider->entry->plugin_api_version : 0;
	h.flags = 0;
	uint32_t count = 0;
	(void)conv_host_describe_cap(s->host, 0, NULL, &count);
	h.capability_count = count;
	h.generation = conv_host_get_generation(s->host);
	h.adapter_luid = s->cfg->adapter_luid;
	conv_host_wire_set_str(h.git_tag, sizeof(h.git_tag), u_git_tag);
	conv_host_wire_set_str(h.provider_id, sizeof(h.provider_id), s->cfg->provider_id);
	if (!send_msg(s, CONV_HOST_MSG_HELLO, 0, &h, sizeof(h))) {
		U_LOG_E("conversion host: sending HELLO failed");
		return false;
	}
	return true;
}

int
conv_host_serve_run(struct conv_host_chan *chan, const struct conv_host_serve_config *cfg)
{
	struct conv_host_serve *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		return CONV_HOST_EXIT_NEGOTIATE_FAILED;
	}
	s->chan = chan;
	s->cfg = cfg;
	s->host = cfg->provider->host;
	(void)os_mutex_init(&s->mtx);
	s->last_bound_ns = os_monotonic_get_ns();

	conv_host_serve_prepare();
	os_mutex_lock(&g_exit.mtx);
	g_exit.chan = chan;
	const bool early = g_exit.requested;
	os_mutex_unlock(&g_exit.mtx);

	uint8_t *payload = malloc(CONV_HOST_WIRE_MAX_PAYLOAD);
	int code = CONV_HOST_EXIT_ORDERLY;
	if (payload == NULL || early) {
		code = early ? g_exit.code : CONV_HOST_EXIT_NEGOTIATE_FAILED;
		goto out;
	}

	// 1. HELLO, then the ack (the git-tag gate is the service's).
	if (!hello(s)) {
		code = CONV_HOST_EXIT_CHANNEL;
		goto out;
	}
	struct conv_host_wire_frame f;
	if (!conv_host_chan_recv(chan, &f, payload)) {
		code = g_exit.requested ? g_exit.code : CONV_HOST_EXIT_CHANNEL;
		goto out;
	}
	struct conv_host_wire_hello_ack ack;
	if (f.type != CONV_HOST_MSG_HELLO_ACK || !conv_host_wire_receive(&ack, sizeof(ack), payload, f.payload_bytes) ||
	    !ack.accepted) {
		U_LOG_E("conversion host: hello refused (type 0x%04x, reason %u, service tag '%.64s', host tag '%s')",
		        f.type, f.type == CONV_HOST_MSG_HELLO_ACK ? ack.reject_reason : 0u,
		        f.type == CONV_HOST_MSG_HELLO_ACK ? ack.git_tag : "", u_git_tag);
		code = CONV_HOST_EXIT_ABI_REJECT;
		goto out;
	}
	U_LOG_W("conversion host: hello accepted by the service (tag %s)", u_git_tag);

	(void)os_thread_init(&s->ticker);
	(void)os_thread_start(&s->ticker, ticker_thread, s);

	// 2. Serve.
	bool shutdown_msg = false;
	uint32_t shutdown_rid = 0;
	while (conv_host_chan_recv(chan, &f, payload)) {
		if (!dispatch(s, &f, payload)) {
			shutdown_msg = true;
			shutdown_rid = f.request_id;
			break;
		}
	}
	os_mutex_lock(&g_exit.mtx);
	if (g_exit.requested) {
		code = g_exit.code;
	} else {
		g_exit.requested = true; // a late request no longer touches the channel
		g_exit.code = CONV_HOST_EXIT_ORDERLY;
		code = CONV_HOST_EXIT_ORDERLY;
		if (!shutdown_msg) {
			U_LOG_W("conversion host: control channel closed by the service; exiting");
		}
	}
	os_mutex_unlock(&g_exit.mtx);

	// 3. Teardown: cancel (is_cancelled turns true), stop lanes, drain the
	//    provider (destroy waits for every exec thread), then reply. A hung
	//    provider must not hold the exit past the budget (§3.4: 3 s).
	s->stopping = true;
	start_exit_backstop(code);
	for (uint32_t i = 0; i < MAX_LANES; i++) {
		lane_close_locked_out(s, &s->lanes[i]);
	}
	if (code != CONV_HOST_EXIT_UPDATE) {
		conv_host_provider_unload(cfg->provider);
	}
	if (shutdown_msg) {
		send_status(s, CONV_HOST_MSG_SHUTDOWN, shutdown_rid, XRT_SUCCESS);
	}
	os_thread_join(&s->ticker);
	os_thread_destroy(&s->ticker);
	(void)conv_host_log_drain(forward_log, s, NULL);

out:
	os_mutex_lock(&s->mtx);
	for (uint32_t i = 0; i < MAX_OUTPUTS; i++) {
		output_free_locked(s, i);
	}
	for (uint32_t i = 0; i < MAX_INPUTS; i++) {
		if (s->inputs[i].used) {
			conv_host_handle_close(s->inputs[i].img.handle);
			conv_host_handle_close(s->inputs[i].sync_handle);
		}
	}
	for (uint32_t i = 0; i < MAX_SYNCS; i++) {
		if (s->syncs[i].used) {
			conv_host_handle_close(s->syncs[i].handle);
		}
	}
	os_mutex_unlock(&s->mtx);
	free(payload);
	// s stays allocated: an abandoned exec thread may still complete into it.
	return code;
}
