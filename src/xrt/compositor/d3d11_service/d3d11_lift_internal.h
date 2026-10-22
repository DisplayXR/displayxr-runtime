// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Private to comp_d3d11_lift: the d3d11_lift object shared by
 *         d3d11_lift.cpp (the lift thread + the dp-lift backend) and
 *         d3d11_conv_gpu.cpp (the conversion core's D3D11 glue on the service
 *         device). Not an API: include from those two files only.
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include "d3d11_lift.h"
#include "d3d11_conv_viewsyn.h"

#include "xrt/xrt_display_processor_d3d11.h"

#include "conversion/conv_core.h"

#include "util/u_lift_mailbox.h"
#include "util/u_lift_rig.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <d3d11_1.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <thread>
#include <vector>


/*
 *
 * Small helpers.
 *
 */

template <typename T>
static inline void
rel(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

static inline void
close_handle(HANDLE &h)
{
	if (h != nullptr && h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
	}
	h = nullptr;
}

static inline const char *
state_str(uint32_t s)
{
	switch (s) {
	case XRT_DP_LIFT_STATE_READY: return "READY";
	case XRT_DP_LIFT_STATE_ACTIVATING: return "ACTIVATING";
	default: return "UNAVAILABLE";
	}
}

//! CompareObjectHandles (Windows 10+), resolved at runtime so older SDKs build.
static inline bool
same_kernel_object(HANDLE a, HANDLE b)
{
	typedef BOOL(WINAPI * pfn_t)(HANDLE, HANDLE);
	static pfn_t fn = []() -> pfn_t {
		HMODULE m = GetModuleHandleA("kernelbase.dll");
		return m != nullptr ? (pfn_t)(void *)GetProcAddress(m, "CompareObjectHandles") : nullptr;
	}();
	if (fn == nullptr || a == nullptr || b == nullptr) {
		return false;
	}
	return fn(a, b) != FALSE;
}

static inline DXGI_FORMAT
typed_srv_format(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return f;
	}
}

//! The fullscreen-triangle-strip vertex shader (snapshot blit, lift-side output blit).
static const char *const k_snap_vs = R"(
struct VSO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSO main(uint id : SV_VertexID) {
	VSO o;
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
)";

//! Staging readbacks in flight per stream (async: never stall the weave).
#define LB_RING 3


//! One letterbox profile readback (service device, the weave's producer thread only).
struct lift_lb_readback
{
	ID3D11Texture2D *staging = nullptr;
	bool pending = false;
	uint32_t w = 0, h = 0;   //!< rect dims it measured
	uint32_t nr = 0, nc = 0; //!< buckets used
};

/*
 *
 * The dp-lift backend (#2003): commands the provider's exec thread hands to
 * the lift thread, and the lift thread's own view of runtime images.
 *
 */

enum lift_cmd_op
{
	LIFT_CMD_ACTIVATE,
	LIFT_CMD_POLL,
	LIFT_CMD_STREAM_CREATE,
	LIFT_CMD_STREAM_DESTROY,
	LIFT_CMD_CONVERT,
};

struct lift_cmd
{
	lift_cmd_op op = LIFT_CMD_POLL;
	bool retry = false;
	const xrt_dp_lift_stream_info *info = nullptr;
	uint64_t dp_id = 0;
	conv_dp_lift_convert *io = nullptr;
	xrt_result_t result = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	bool done = false;
};

//! A runtime image (glue-allocated, shared NT handle) opened on the LIFT device.
struct lift_open_img
{
	ID3D11Texture2D *tex = nullptr;
	IDXGIKeyedMutex *km = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
};

//! Per module stream of a core-routed conversion (lift thread only).
struct lift_core_stream
{
	ID3D11Texture2D *exact_tex = nullptr;
	uint32_t exact_w = 0, exact_h = 0;
	bool depth_logged = false;
};

//! A glue-allocated image on the SERVICE device.
//! ADR-058: one stream's late views on the weave path (render thread only).
struct d3d11_late_state
{
	struct conv_late_cache cache = {};
	ID3D11Texture2D *tex = nullptr; //!< N views side by side, R8G8B8A8_UNORM
	ID3D11ShaderResourceView *srv = nullptr;
	uint32_t w = 0, h = 0;
	bool written = false;
	bool warned = false;
	uint64_t used_ns = 0;
};

struct glue_img
{
	ID3D11Texture2D *tex = nullptr;
	ID3D11ShaderResourceView *srv = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
	IDXGIKeyedMutex *km = nullptr;
	HANDLE share = nullptr;
	uint64_t stream_id = 0; //!< core stream
	uint32_t w = 0, h = 0, layers = 1;
	DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
};

//! Per v3 (wire) stream on the core route: the client import cache + exports.
struct glue_stream
{
	// Caller-input import cache (producer thread only).
	HANDLE imp_handle = nullptr;
	bool imp_dxgi = false;
	ID3D11Texture2D *imp_tex = nullptr;
	ID3D11ShaderResourceView *imp_srv = nullptr;
	IDXGIKeyedMutex *imp_km = nullptr;
	uint32_t imp_w = 0, imp_h = 0;
	// Client export (the weave output pattern).
	ID3D11Texture2D *exp_tex = nullptr;
	HANDLE exp_handle = nullptr;
	ID3D11Fence *exp_fence = nullptr;
	HANDLE exp_fence_handle = nullptr;
	uint64_t exp_fence_value = 0;
	uint32_t exp_w = 0, exp_h = 0, exp_format = 0;
	ID3D11Texture2D *exp_depth_tex = nullptr;
	HANDLE exp_depth_handle = nullptr;
	uint32_t exp_depth_w = 0, exp_depth_h = 0, exp_depth_format = 0;
	// Spec 4 (#2054): the caller's shared fence, CPU-input upload, CPU export.
	ID3D11Fence *imp_fence = nullptr;
	HANDLE imp_fence_handle = nullptr;
	ID3D11Texture2D *cpu_tex = nullptr;
	ID3D11ShaderResourceView *cpu_srv = nullptr;
	uint32_t cpu_w = 0, cpu_h = 0;
	DXGI_FORMAT cpu_fmt = DXGI_FORMAT_UNKNOWN;
	HANDLE cpu_map = nullptr;
	void *cpu_view = nullptr;
	uint64_t cpu_map_bytes = 0;
	ID3D11Texture2D *cpu_stage = nullptr;
	uint32_t cpu_stage_w = 0, cpu_stage_h = 0;
	DXGI_FORMAT cpu_stage_fmt = DXGI_FORMAT_UNKNOWN;
};

//! S6 / S7 depth readback of one core stream (dispatcher thread).
struct glue_rb
{
	ID3D11Texture2D *staging = nullptr;
	uint32_t w = 0, h = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	bool pending = false;
	uint64_t issued_ns = 0;
	std::vector<float> samples;
};

//! The weave path's per-rect producer state for a core-routed stream.
struct core_weave_state
{
	uint64_t id = 0; //!< the v3 wire id (logs)
	u_lift_letterbox lb = {};
	lift_lb_readback lb_rb[LB_RING];
	uint32_t lb_next = 0;
	uint32_t cap_logged_w = 0, cap_logged_h = 0;
	bool has_last_params = false;
	xrt_dp_lift_params last_params = {};
};

struct d3d11_lift
{
	// Service side (borrowed).
	ID3D11Device *svc_device = nullptr;
	ID3D11Device1 *svc_device1 = nullptr;
	ID3D11DeviceContext *svc_context = nullptr;
	ID3D11DeviceContext4 *svc_context4 = nullptr;
	std::mutex *svc_ctx_mutex = nullptr;
	//! G5: the factory pair, re-resolved at every activation attempt.
	d3d11_lift_resolve_factories_fn resolve = nullptr;
	void *resolve_ud = nullptr;
	// Snapshot blit (service device).
	ID3D11VertexShader *snap_vs = nullptr;
	ID3D11PixelShader *snap_ps = nullptr;
	ID3D11PixelShader *snap_ps_scaled = nullptr;
	ID3D11SamplerState *snap_sampler = nullptr;
	ID3D11SamplerState *snap_sampler_linear = nullptr;
	ID3D11Buffer *snap_cb = nullptr;
	bool snap_ok = false;
	//! DXR_LIFT_MAX_INPUT_EDGE, read once at create: long-edge cap on weave-rect
	//! snapshots (0 = off). Never applied to xrSubmitLiftFrameDXR frames.
	uint32_t max_input_edge = U_LIFT_MAX_INPUT_EDGE_DEFAULT;
	//! DXR_LIFT_LETTERBOX, read once at create (default on). Weave-rect
	//! snapshots only, like the cap.
	bool letterbox = true;
	ID3D11PixelShader *lb_ps = nullptr;
	ID3D11Texture2D *lb_tex = nullptr; //!< R32F BINS_MAX x 2 profile target
	ID3D11RenderTargetView *lb_rtv = nullptr;
	ID3D11Buffer *lb_cbuf = nullptr;
	bool lb_ok = false;

	// Lift side (lift thread only, after activation).
	ID3D11Device *lift_device = nullptr;
	ID3D11Device1 *lift_device1 = nullptr;
	ID3D11DeviceContext *lift_context = nullptr;
	struct xrt_display_processor_d3d11 *dp = nullptr;

	// Shared state, under mtx.
	std::mutex mtx;
	std::condition_variable cv;
	bool stop = false;
	bool activate_requested = false; //!< an ACTIVATE command ran (the stall WARN's clock)
	bool activated = false;          //!< an activation attempt finished
	xrt_dp_lift_caps caps = {};
	uint32_t logged_state = 0xffffffffu;
	//! #1812: when the current ACTIVATING stretch began (0 = not activating /
	//! nobody asked yet), and whether its one-shot stall WARN was emitted.
	uint64_t activating_since_ns = 0;
	bool activating_warned = false;
	//! The vendor call the lift thread is inside right now (nullptr = none),
	//! so the stall WARN can say whether the thread itself is blocked there.
	const char *vendor_call = nullptr;
	uint64_t vendor_call_since_ns = 0;

	// The dp-lift backend (#2003), under mtx unless noted.
	std::deque<lift_cmd *> cmds;
	std::condition_variable cmd_cv;
	char unavail_detail[96] = {};     //!< why there is no module (the last UNAVAILABLE reason)
	bool has_depth_slot = false;      //!< the lift DP carries lift_get_depth
	bool module_alive = false;        //!< the lift DP is up and carries a module
	uint64_t change_count = 0;        //!< bumps whenever activation / caps changed
	std::vector<uint64_t> lift_evict; //!< freed image keys the lift thread drops from lift_open
	// Lift thread only.
	std::map<uint64_t, lift_open_img> lift_open;       //!< import_key -> lift-device view
	std::map<uint64_t, lift_core_stream> core_streams; //!< module stream id -> state
	ID3D11PixelShader *blit_ps = nullptr;
	ID3D11VertexShader *blit_vs = nullptr;
	ID3D11SamplerState *blit_samp = nullptr;
	bool blit_tried = false;

	// The core's D3D11 glue (service device). glue_mtx is a leaf.
	std::mutex glue_mtx;
	std::condition_variable glue_cv;
	uint32_t glue_calls = 0; //!< glue entry points in flight (d3d11_lift_destroy waits)
	bool glue_closed = false;
	std::map<uint64_t, glue_img> glue_imgs; //!< import_key ->
	uint64_t glue_next_key = 0;
	std::map<uint64_t, glue_stream> glue_streams;    //!< v3 wire id ->
	std::map<uint64_t, glue_rb> glue_rbs;            //!< core stream id ->
	std::map<uint64_t, core_weave_state> core_weave; //!< v3 wire id -> (weave producer)
	uint64_t adapter_luid = 0;
	std::mutex viewsyn_mtx;
	d3d11_conv_viewsyn *viewsyn = nullptr;
	bool viewsyn_tried = false;
	//! #2030: S5 records here (dispatcher thread, under viewsyn_mtx) and only
	//! the replay touches the shared immediate context. nullptr = the device
	//! refused one: S5 draws on the immediate context, as before.
	ID3D11DeviceContext *viewsyn_deferred = nullptr;
	//! ADR-058 late view synthesis on the weave path (render thread only,
	//! under the service's immediate-context mutex the weave holds): its own
	//! stage + deferred context, so the dispatcher's S5 is never shared.
	d3d11_conv_viewsyn *late_vs = nullptr;
	bool late_vs_tried = false;
	ID3D11DeviceContext *late_deferred = nullptr;
	std::map<uint64_t, struct d3d11_late_state> late_states; //!< core stream id ->
	//! #2030: the still-image content check (its own device; created on the
	//! first PHOTO submit, under glue_mtx).
	struct d3d11_conv_content_check *content_check = nullptr;
	bool content_check_tried = false;

	//! G7, producer side: when the service last published the panel's eyes
	//! (the conversion core holds the eyes themselves), and how a producer
	//! refreshes them when that is older than 50 ms (it may take render_mutex).
	std::mutex eyes_mtx;
	uint64_t eyes_ns = 0;
	void (*eyes_refresh)(void *ud) = nullptr;
	void *eyes_refresh_ud = nullptr;

	// The weave path's bridge to core-routed streams.
	std::mutex bridge_mtx;
	d3d11_lift_core_bridge bridge = {};

	std::thread thread;
};

//! A snapshot destination: a core slot image on the service device.
struct blit_dst
{
	IDXGIKeyedMutex *svc_km;
	ID3D11RenderTargetView *svc_rtv;
};

//! XRT_CONV_FMT_* -> DXGI_FORMAT (UNKNOWN = not representable).
static inline DXGI_FORMAT
conv_fmt_to_dxgi(uint32_t f)
{
	switch (f) {
	case XRT_CONV_FMT_RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case XRT_CONV_FMT_RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case XRT_CONV_FMT_BGRA8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case XRT_CONV_FMT_BGRA8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case XRT_CONV_FMT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
	case XRT_CONV_FMT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
	case XRT_CONV_FMT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
	case XRT_CONV_FMT_RGBA16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case XRT_CONV_FMT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

//! DXGI_FORMAT -> XRT_CONV_FMT_* (UNKNOWN = none).
static inline uint32_t
dxgi_to_conv_fmt(uint32_t d)
{
	switch ((DXGI_FORMAT)d) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM: return XRT_CONV_FMT_RGBA8_UNORM;
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return XRT_CONV_FMT_RGBA8_SRGB;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM: return XRT_CONV_FMT_BGRA8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return XRT_CONV_FMT_BGRA8_SRGB;
	case DXGI_FORMAT_R8_UNORM: return XRT_CONV_FMT_R8_UNORM;
	case DXGI_FORMAT_R16_FLOAT: return XRT_CONV_FMT_R16_FLOAT;
	case DXGI_FORMAT_R32_FLOAT: return XRT_CONV_FMT_R32_FLOAT;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return XRT_CONV_FMT_RGBA16_FLOAT;
	case DXGI_FORMAT_R16_UNORM: return XRT_CONV_FMT_R16_UNORM;
	default: return XRT_CONV_FMT_UNKNOWN;
	}
}

//! A keyed-mutex acquire with the image's sync keys (no mutex = nothing to wait for).
static inline bool
img_acquire(IDXGIKeyedMutex *km, const xrt_conv_image *img, DWORD ms)
{
	if (km == nullptr) {
		return true;
	}
	const uint64_t key =
	    img != nullptr && img->sync.kind == XRT_CONV_SYNC_KEYED_MUTEX ? img->sync.acquire_value : 0;
	HRESULT hr = km->AcquireSync(key, ms);
	return SUCCEEDED(hr) && hr != (HRESULT)WAIT_TIMEOUT;
}

static inline void
img_release(IDXGIKeyedMutex *km, const xrt_conv_image *img)
{
	if (km != nullptr) {
		km->ReleaseSync(img != nullptr && img->sync.kind == XRT_CONV_SYNC_KEYED_MUTEX ? img->sync.release_value
		                                                                              : 0);
	}
}


/*
 *
 * Shared between the two files.
 *
 */

// d3d11_conv_gpu.cpp: service-device resources (snapshot / letterbox
// pipelines, adapter LUID, env policy). conv_gpu_init returns snap_ok.
bool
conv_gpu_init(d3d11_lift *l);
void
conv_gpu_fini(d3d11_lift *l);

// d3d11_conv_gpu.cpp: the glue gate, the bridge and the core weave branch.
void
glue_close(d3d11_lift *l);
void
glue_release_all(d3d11_lift *l);
bool
bridge_resolve(
    d3d11_lift *l, uint64_t owner, uint64_t wire_id, conv_core **out_core, uint64_t *out_core_id, uint32_t *out_mode);
xrt_result_t
core_weave_submit_locked(d3d11_lift *l,
                         uint64_t owner,
                         uint64_t id,
                         ID3D11ShaderResourceView *src,
                         uint32_t src_tw,
                         uint32_t src_th,
                         uint32_t x,
                         uint32_t y,
                         uint32_t w,
                         uint32_t h,
                         int64_t source_time,
                         const xrt_dp_lift_params *params,
                         const float *viewpoints,
                         uint32_t viewpoint_floats,
                         const xrt_lift_view_control *view_control,
                         const xrt_lift_rig *rig,
                         const xrt_lift_rect_frame *frame,
                         uint64_t *out_frame_id);
bool
core_pin_latest(d3d11_lift *l, uint64_t owner, uint64_t id, d3d11_lift_pin *out);
void
core_unpin(d3d11_lift *l, d3d11_lift_pin *pin);

/*!
 * Snapshot @p src region (@p x, @p y, @p w, @p h) into the top-left
 * @p dst_w x @p dst_h of @p s's service texture. Caller holds the service
 * context mutex; takes the destination's keyed mutex itself.
 */
bool
snapshot_blit(d3d11_lift *l,
              blit_dst &s,
              ID3D11ShaderResourceView *src,
              uint32_t src_tw,
              uint32_t src_th,
              uint32_t x,
              uint32_t y,
              uint32_t w,
              uint32_t h,
              uint32_t dst_w,
              uint32_t dst_h);
