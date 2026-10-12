// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Transport fake for tests_oxr_lift_startup (see the .cpp file header).
 *
 * The REAL oxr_lift.c and the REAL ipc_client_lift.c are compiled into the
 * test; everything below them is faked here, at the narrowest seams that exist
 * today:
 *
 *  - the generated wire calls (`ipc_call_lift_*`, the blob send/receive pair
 *    and `ipc_receive`), which ipc_client_lift.c calls — every message an app's
 *    lift call puts on the wire lands in the trace, with its arguments;
 *  - the ADR-049 lift-only link (`ipc_client_lift_link_*`), which oxr_lift.c
 *    calls for an in-process D3D11 / D3D12 session — its state is scripted;
 *  - `comp_ipc_client_compositor_lift_connection`, the IPC session's bridge.
 *
 * C, not C++: it builds the oxr_instance / oxr_system / oxr_session objects the
 * entry points verify, with the state tracker's own headers and handle code.
 */

#include "tests_oxr_lift_startup_fake.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_dp_lift.h"
#include "xrt/xrt_lift.h"

#include "os/os_threading.h"
#include "util/u_misc.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_handle.h"

#include "shared/ipc_protocol.h"
#include "client/ipc_client.h"
#include "client/ipc_client_lift_link.h"
#include "ipc_client_generated.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Trace.
 *
 */

static char s_trace[LF_TRACE_MAX][LF_TRACE_LINE];
static size_t s_trace_count;

static void
trace(const char *fmt, ...)
{
	if (s_trace_count >= LF_TRACE_MAX) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	vsnprintf(s_trace[s_trace_count], LF_TRACE_LINE, fmt, args);
	va_end(args);
	s_trace_count++;
}

size_t
lf_trace_count(void)
{
	return s_trace_count;
}

const char *
lf_trace_line(size_t i)
{
	return i < s_trace_count ? s_trace[i] : "";
}

void
lf_trace_clear(void)
{
	s_trace_count = 0;
}


/*
 *
 * Scripted service.
 *
 */

static struct
{
	struct ipc_lift_properties props;
	xrt_result_t props_xret;
	xrt_result_t create_xret;
	uint64_t next_stream_id;
	xrt_result_t submit_xret;
	uint64_t next_frame_id;
	xrt_result_t acquire_xret;
	bool result_ready;
	struct ipc_lift_result result;
	bool output_realloc_pending;
	xrt_result_t destroy_xret;
	struct ipc_arg_lift_submit last_submit;
} s_svc;

static struct ipc_connection s_conn;
static bool s_conn_init;

static struct
{
	enum ipc_client_lift_link_state state;
	uint32_t gen;
	int created;
} s_link;

//! The one link object the fake hands out (opaque to oxr_lift.c).
static int s_link_object;

void
lf_reset(void)
{
	lf_trace_clear();
	memset(&s_svc, 0, sizeof(s_svc));
	s_svc.next_stream_id = 0x51;
	s_svc.next_frame_id = 1;
	memset(&s_link, 0, sizeof(s_link));
	s_link.state = IPC_CLIENT_LIFT_LINK_CONNECTING;
	if (!s_conn_init) {
		memset(&s_conn, 0, sizeof(s_conn));
		os_mutex_init(&s_conn.mutex);
		s_conn_init = true;
	}
}

void
lf_set_properties(uint32_t state, uint32_t modes, uint32_t max_views, uint32_t aux_outputs, uint32_t aux_depth_semantics)
{
	memset(&s_svc.props, 0, sizeof(s_svc.props));
	s_svc.props.modes = modes;
	s_svc.props.max_streams = 4;
	s_svc.props.max_views = max_views;
	s_svc.props.depth_semantics = XRT_DP_LIFT_DEPTH_RELATIVE;
	s_svc.props.state = state;
	s_svc.props.aux_outputs = aux_outputs;
	s_svc.props.aux_depth_semantics = aux_depth_semantics;
	s_svc.props.typical_latency_ns = 30000000;
	snprintf(s_svc.props.backend, sizeof(s_svc.props.backend), "fake-module");
}

void
lf_set_properties_xret(int xret)
{
	s_svc.props_xret = (xrt_result_t)xret;
}

void
lf_set_create_xret(int xret)
{
	s_svc.create_xret = (xrt_result_t)xret;
}

void
lf_set_submit_xret(int xret)
{
	s_svc.submit_xret = (xrt_result_t)xret;
}

void
lf_set_acquire_xret(int xret)
{
	s_svc.acquire_xret = (xrt_result_t)xret;
}

void
lf_set_result_ready(bool ready, uint32_t width, uint32_t height, uint32_t view_count)
{
	s_svc.result_ready = ready;
	memset(&s_svc.result, 0, sizeof(s_svc.result));
	if (ready) {
		s_svc.result.frame_id = 1;
		s_svc.result.source_time = 777;
		s_svc.result.fence_value = 9;
		s_svc.result.latency_ns = 25000000;
		s_svc.result.width = width;
		s_svc.result.height = height;
		s_svc.result.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
		s_svc.result.view_count = view_count;
		s_svc.result.output_realloc = s_svc.output_realloc_pending ? 1u : 0u;
		s_svc.output_realloc_pending = false;
	}
}

void
lf_set_output_realloc_next(bool realloc)
{
	s_svc.output_realloc_pending = realloc;
}

uint32_t
lf_last_submit_view_count(void)
{
	return s_svc.last_submit.params.view_count;
}

void
lf_set_link(int state, uint32_t gen)
{
	s_link.state = (enum ipc_client_lift_link_state)state;
	s_link.gen = gen;
}


/*
 *
 * Objects the entry points verify.
 *
 */

struct lf_objects
{
	struct oxr_instance *inst;
	struct oxr_system *sys;
	struct oxr_session *sess;
	struct xrt_compositor_native *xcn;
	int xsysc_dummy;
};

static XrResult
lf_session_destroy_cb(struct oxr_logger *log, struct oxr_handle_base *hb)
{
	(void)log;
	struct oxr_session *sess = (struct oxr_session *)hb;
	oxr_lift_session_fini(sess);
	// The objects are freed by lf_session_destroy (hb is the session).
	return XR_SUCCESS;
}

static struct lf_objects s_obj;

XrSession
lf_session_create(enum lf_session_kind kind, bool lift_enabled)
{
	memset(&s_obj, 0, sizeof(s_obj));
	s_obj.inst = U_TYPED_CALLOC(struct oxr_instance);
	s_obj.sys = U_TYPED_CALLOC(struct oxr_system);
	s_obj.sess = U_TYPED_CALLOC(struct oxr_session);
	s_obj.xcn = U_TYPED_CALLOC(struct xrt_compositor_native);

	s_obj.inst->extensions.DXR_lift = lift_enabled;
	s_obj.sys->inst = s_obj.inst;
	// Never dereferenced by oxr_lift.c: only "is there a system compositor".
	s_obj.sys->xsysc = (struct xrt_system_compositor *)&s_obj.xsysc_dummy;

	struct oxr_logger log;
	oxr_log_init(&log, "lf_session_create");
	oxr_handle_init(&log, &s_obj.sess->handle, OXR_XR_DEBUG_SESSION, lf_session_destroy_cb, NULL);
	s_obj.sess->sys = s_obj.sys;
	s_obj.sess->xcn = s_obj.xcn;
	switch (kind) {
	case LF_SESSION_IPC: break; // no in-process native flag: the IPC route
	case LF_SESSION_INPROCESS_D3D11: s_obj.sess->is_d3d11_native_compositor = true; break;
	case LF_SESSION_INPROCESS_D3D12: s_obj.sess->is_d3d12_native_compositor = true; break;
	case LF_SESSION_INPROCESS_GL: s_obj.sess->is_gl_native_compositor = true; break;
	}
	return (XrSession)(uintptr_t)s_obj.sess;
}

bool
lf_session_lost(void)
{
	return s_obj.sess != NULL && s_obj.sess->has_lost;
}

void
lf_session_destroy(void)
{
	if (s_obj.sess == NULL) {
		return;
	}
	// Destroys the session's lift streams first (children), as at runtime.
	struct oxr_logger log;
	oxr_log_init(&log, "lf_session_destroy");
	oxr_handle_destroy(&log, &s_obj.sess->handle);
	free(s_obj.sess);
	free(s_obj.xcn);
	free(s_obj.sys);
	free(s_obj.inst);
	memset(&s_obj, 0, sizeof(s_obj));
}


/*
 *
 * Seam 1: the IPC session's connection (ipc_client_compositor.c in the runtime).
 *
 */

struct ipc_connection *
comp_ipc_client_compositor_lift_connection(struct xrt_compositor *xc)
{
	(void)xc;
	trace("bridge:session_connection");
	return &s_conn;
}


/*
 *
 * Seam 2: the ADR-049 lift-only link (ipc_client_lift_link.c in the runtime).
 *
 */

xrt_result_t
ipc_client_lift_link_create(const char *label, struct ipc_client_lift_link **out_link)
{
	(void)label;
	s_link.created++;
	trace("link:create");
	*out_link = (struct ipc_client_lift_link *)&s_link_object;
	return XRT_SUCCESS;
}

void
ipc_client_lift_link_destroy(struct ipc_client_lift_link **link_ptr)
{
	if (link_ptr == NULL || *link_ptr == NULL) {
		return;
	}
	trace("link:destroy");
	*link_ptr = NULL;
}

static struct ipc_connection *
link_acquire(const char *what, uint32_t timeout_ms, enum ipc_client_lift_link_state *out_state, uint32_t *out_gen)
{
	trace("link:%s(timeout_ms=%u) -> %s gen=%u", what, timeout_ms,
	      s_link.state == IPC_CLIENT_LIFT_LINK_CONNECTING  ? "CONNECTING"
	      : s_link.state == IPC_CLIENT_LIFT_LINK_CONNECTED ? "CONNECTED"
	      : s_link.state == IPC_CLIENT_LIFT_LINK_FAILED    ? "FAILED"
	                                                       : "DEAD",
	      s_link.gen);
	*out_state = s_link.state;
	*out_gen = s_link.gen;
	return s_link.state == IPC_CLIENT_LIFT_LINK_CONNECTED ? &s_conn : NULL;
}

struct ipc_connection *
ipc_client_lift_link_acquire(struct ipc_client_lift_link *link,
                             enum ipc_client_lift_link_state *out_state,
                             uint32_t *out_gen)
{
	(void)link;
	return link_acquire("acquire", 0, out_state, out_gen);
}

struct ipc_connection *
ipc_client_lift_link_acquire_wait(struct ipc_client_lift_link *link,
                                  uint32_t timeout_ms,
                                  enum ipc_client_lift_link_state *out_state,
                                  uint32_t *out_gen)
{
	(void)link;
	return link_acquire("acquire_wait", timeout_ms, out_state, out_gen);
}

void
ipc_client_lift_link_release(struct ipc_client_lift_link *link, uint32_t gen, bool ipc_failed)
{
	(void)link;
	trace("link:release(gen=%u, ipc_failed=%d)", gen, ipc_failed ? 1 : 0);
	if (ipc_failed && gen == s_link.gen) {
		s_link.state = IPC_CLIENT_LIFT_LINK_DEAD;
	}
}


/*
 *
 * Seam 3: the generated wire calls (ipc_client_generated.c in the runtime).
 *
 */

xrt_result_t
ipc_call_lift_get_properties(struct ipc_connection *ipc_c, struct ipc_lift_properties *out_props)
{
	(void)ipc_c;
	trace("wire:lift_get_properties -> state=%u modes=0x%x max_views=%u aux=0x%x", s_svc.props.state,
	      s_svc.props.modes, s_svc.props.max_views, s_svc.props.aux_outputs);
	if (s_svc.props_xret != XRT_SUCCESS) {
		return s_svc.props_xret;
	}
	*out_props = s_svc.props;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_lift_stream_create(struct ipc_connection *ipc_c,
                            uint32_t mode,
                            uint32_t content_hint,
                            float input_scale,
                            uint32_t aux_outputs,
                            uint32_t aux_depth_format,
                            uint64_t *out_stream_id)
{
	(void)ipc_c;
	trace("wire:lift_stream_create(mode=%u, hint=%u, scale=%.2f, aux=%u, aux_fmt=%u)", mode, content_hint,
	      (double)input_scale, aux_outputs, aux_depth_format);
	if (s_svc.create_xret != XRT_SUCCESS) {
		return s_svc.create_xret;
	}
	*out_stream_id = s_svc.next_stream_id++;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_lift_stream_destroy(struct ipc_connection *ipc_c, uint64_t stream_id)
{
	(void)ipc_c;
	trace("wire:lift_stream_destroy(id=0x%llx)", (unsigned long long)stream_id);
	return s_svc.destroy_xret;
}

xrt_result_t
ipc_call_lift_submit_frame(struct ipc_connection *ipc_c,
                           const struct ipc_arg_lift_submit *args,
                           const xrt_graphics_buffer_handle_t *handles,
                           const uint32_t handle_count,
                           uint64_t *out_frame_id)
{
	(void)ipc_c;
	(void)handles;
	s_svc.last_submit = *args;
	trace("wire:lift_submit_frame(id=0x%llx, %ux%u, t=%lld, has_params=%u, view_count=%u, viewpoints=%u, "
	      "handles=%u)",
	      (unsigned long long)args->stream_id, args->width, args->height, (long long)args->source_time,
	      args->has_params, args->params.view_count, args->params.viewpoint_count, handle_count);
	if (s_svc.submit_xret != XRT_SUCCESS) {
		return s_svc.submit_xret;
	}
	*out_frame_id = s_svc.next_frame_id++;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_lift_acquire_result(struct ipc_connection *ipc_c,
                             uint64_t stream_id,
                             bool *out_ready,
                             struct ipc_lift_result *out_lift_result)
{
	(void)ipc_c;
	trace("wire:lift_acquire_result(id=0x%llx) -> ready=%d realloc=%u", (unsigned long long)stream_id,
	      s_svc.result_ready ? 1 : 0, s_svc.result.output_realloc);
	if (s_svc.acquire_xret != XRT_SUCCESS) {
		return s_svc.acquire_xret;
	}
	*out_ready = s_svc.result_ready;
	*out_lift_result = s_svc.result;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_lift_get_output(struct ipc_connection *ipc_c,
                         uint64_t stream_id,
                         bool *out_have_output,
                         uint32_t *out_width,
                         uint32_t *out_height,
                         uint32_t *out_format,
                         xrt_graphics_buffer_handle_t *handles,
                         uint32_t handle_count)
{
	(void)ipc_c;
	trace("wire:lift_get_output(id=0x%llx, max_handles=%u)", (unsigned long long)stream_id, handle_count);
	*out_have_output = true;
	*out_width = s_svc.result.width;
	*out_height = s_svc.result.height;
	*out_format = s_svc.result.format;
	handles[0] = (xrt_graphics_buffer_handle_t)(intptr_t)LF_OUTPUT_HANDLE;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_lift_get_depth_output(struct ipc_connection *ipc_c,
                               uint64_t stream_id,
                               bool *out_have_depth,
                               uint32_t *out_width,
                               uint32_t *out_height,
                               uint32_t *out_format,
                               xrt_graphics_buffer_handle_t *handles,
                               uint32_t handle_count)
{
	(void)ipc_c;
	(void)handles;
	trace("wire:lift_get_depth_output(id=0x%llx, max_handles=%u)", (unsigned long long)stream_id, handle_count);
	*out_have_depth = false;
	*out_width = 0;
	*out_height = 0;
	*out_format = 0;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_lift_get_fence(struct ipc_connection *ipc_c,
                        uint64_t stream_id,
                        bool *out_have_fence,
                        xrt_graphics_sync_handle_t *handles,
                        uint32_t handle_count)
{
	(void)ipc_c;
	trace("wire:lift_get_fence(id=0x%llx, max_handles=%u)", (unsigned long long)stream_id, handle_count);
	*out_have_fence = true;
	handles[0] = (xrt_graphics_sync_handle_t)(intptr_t)LF_FENCE_HANDLE;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_send_lift_acquire_blob_locked(struct ipc_connection *ipc_c, uint64_t stream_id, uint64_t capacity)
{
	(void)ipc_c;
	trace("wire:lift_acquire_blob(id=0x%llx, capacity=%llu)", (unsigned long long)stream_id,
	      (unsigned long long)capacity);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_receive_lift_acquire_blob_locked(struct ipc_connection *ipc_c,
                                     bool *out_ready,
                                     uint64_t *out_frame_id,
                                     int64_t *out_source_time,
                                     uint32_t *out_format,
                                     uint64_t *out_byte_count)
{
	(void)ipc_c;
	*out_ready = false;
	*out_frame_id = 0;
	*out_source_time = 0;
	*out_format = 0;
	*out_byte_count = 0;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_receive(struct ipc_message_channel *imc, void *out_data, size_t size)
{
	(void)imc;
	memset(out_data, 0, size);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_lift_stream_set_priority(struct ipc_connection *ipc_c, uint64_t stream_id, uint32_t priority)
{
	(void)ipc_c;
	trace("wire:lift_stream_set_priority(id=0x%llx, %u)", (unsigned long long)stream_id, priority);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_lift_stream_stats(struct ipc_connection *ipc_c, uint64_t stream_id, struct xrt_lift_stream_stats *out_stats)
{
	(void)ipc_c;
	trace("wire:lift_stream_stats(id=0x%llx)", (unsigned long long)stream_id);
	memset(out_stats, 0, sizeof(*out_stats));
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_lift_weave_rect_viewpoints(struct ipc_connection *ipc_c, const struct ipc_arg_lift_rect_viewpoints *args)
{
	(void)ipc_c;
	(void)args;
	trace("wire:lift_weave_rect_viewpoints");
	return XRT_SUCCESS;
}

xrt_result_t
ipc_call_lift_weave_rects(struct ipc_connection *ipc_c, const struct ipc_arg_lift_weave_rects *args)
{
	(void)ipc_c;
	trace("wire:lift_weave_rects(count=%u)", args->count);
	return XRT_SUCCESS;
}
