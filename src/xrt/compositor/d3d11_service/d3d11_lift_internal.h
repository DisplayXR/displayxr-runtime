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

#include "util/u_lift_autoconv.h"
#include "util/u_lift_depth.h"
#include "util/u_lift_mailbox.h"
#include "util/u_lift_rig.h"
#include "util/u_lift_viewpoint.h"
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

//! Round up to a 64 px multiple — input slots grow, they don't chase every size.
static inline uint32_t
round_cap(uint32_t v)
{
	return (v + 63u) & ~63u;
}

static const char *
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


/*
 *
 * Stream.
 *
 */

//! One input slot: created on the SERVICE device, opened on the LIFT device.
struct lift_in_slot
{
	ID3D11Texture2D *svc_tex = nullptr;
	ID3D11RenderTargetView *svc_rtv = nullptr;
	IDXGIKeyedMutex *svc_km = nullptr;
	HANDLE share = nullptr;
	ID3D11Texture2D *lift_tex = nullptr;
	IDXGIKeyedMutex *lift_km = nullptr;
	uint32_t cap_w = 0, cap_h = 0;

	// Parameters of the frame currently in this slot.
	xrt_dp_lift_params params = {};
	float viewpoints[3 * XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS] = {};
	uint32_t viewpoint_floats = 0;
	//! The part of the submitted rect this frame holds (letterbox crop):
	//! x0, y0, x1, y1 normalised to the rect. {0,0,1,1} = the whole rect.
	float active[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	//! ADR-048: the stream's viewpoint policy and the region this frame came
	//! from, as of this frame's submit.
	xrt_lift_view_control vc = {};
	xrt_lift_rect_frame frame = {};
	//! XR_DXR_lift v3: the stream's app rig as of this frame (type NONE = none).
	xrt_lift_rig rig = {};
};

//! One output ring slot: created on the LIFT device, opened on the SERVICE device.
struct lift_out_slot
{
	ID3D11Texture2D *lift_tex = nullptr;
	IDXGIKeyedMutex *lift_km = nullptr;
	HANDLE share = nullptr;
	ID3D11Texture2D *svc_tex = nullptr;
	ID3D11ShaderResourceView *svc_srv = nullptr;
	IDXGIKeyedMutex *svc_km = nullptr;
	uint32_t w = 0, h = 0, format = 0, view_count = 0;
	std::shared_ptr<std::vector<uint8_t>> blob; //!< GAUSSIANS
	uint32_t blob_format = 0;
	float active[4] = {0.0f, 0.0f, 1.0f, 1.0f}; //!< the input's lift_in_slot::active
	//! ADR-048 echo: the viewpoints this result was synthesized for.
	uint32_t vp_count = 0;
	float vps[3 * XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS] = {};
	float rect_center[3] = {};
	float rect_size[2] = {};
	//! XR_DXR_lift v3 auxiliary depth: a second texture of this slot (lift
	//! device, keyed mutex, opened on the service device) + its metadata.
	ID3D11Texture2D *d_lift_tex = nullptr;
	IDXGIKeyedMutex *d_lift_km = nullptr;
	HANDLE d_share = nullptr;
	ID3D11Texture2D *d_svc_tex = nullptr;
	IDXGIKeyedMutex *d_svc_km = nullptr;
	uint32_t d_w = 0, d_h = 0, d_format = 0;
	bool has_depth = false;         //!< this slot's result carries depth
	xrt_lift_depth_info depth = {}; //!< metadata (valid / realloc set at acquire)
	//! ADR-046 Amendment 1: the module's relief mapping for this result
	//! (xrt_dp_lift_depth::relief_*), service-internal — not exported to apps.
	bool relief_valid = false;
	float relief_scale = 0.0f;
	float relief_offset = 0.0f;
};

//! One letterbox profile readback (service device, producer thread only).
struct lift_lb_readback
{
	ID3D11Texture2D *staging = nullptr;
	bool pending = false;
	uint32_t w = 0, h = 0;   //!< rect dims it measured
	uint32_t nr = 0, nc = 0; //!< buckets used
};

struct lift_stream
{
	uint64_t id = 0;
	uint64_t owner = 0;
	xrt_dp_lift_stream_info info = {};
	u_lift_mailbox mb = {};
	lift_in_slot in[U_LIFT_INPUT_SLOTS];
	lift_out_slot out[U_LIFT_RING_SIZE];
	xrt_dp_lift_params last_params = {};
	float last_viewpoints[3 * XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS] = {};
	uint32_t last_viewpoint_floats = 0;
	//! ADR-048: the sticky viewpoint policy (XrLiftViewControlDXR), set at
	//! stream create to the defaults; replaced by every submit that carries one.
	xrt_lift_view_control vc = {};
	//! XR_DXR_lift v3: the app rig, riding with the options (see submit_locked).
	xrt_lift_rig rig = {};

	uint32_t priority = U_LIFT_PRIORITY_NORMAL; //!< XrLiftPriorityDXR
	bool dead = false;                          //!< destroy requested; the lift thread reaps it
	bool converting = false;                    //!< the lift thread is inside a conversion for it

	// Lift thread only.
	uint64_t dp_id = 0;
	bool dp_created = false;
	bool dp_failed = false;
	ID3D11Texture2D *exact_tex = nullptr;
	uint32_t exact_w = 0, exact_h = 0;
	//! ADR-048 recenter filter (lift thread only). Zero = reset: stream create.
	u_lift_recenter recenter = {};
	//! The last policy WARNed about (lift thread only): WARN on change, never per frame.
	xrt_lift_view_control logged_vc = {};
	bool logged_vc_valid = false;
	//! The last rig WARNed about (lift thread only).
	xrt_lift_rig logged_rig = {};
	//! XR_DXR_lift v3: one-shot WARNs about the auxiliary depth (lift thread).
	bool depth_logged = false;
	bool depth_missing_logged = false;

	// Caller-input import cache (service device, producer thread only).
	HANDLE imp_handle = nullptr;
	bool imp_dxgi = false;
	ID3D11Texture2D *imp_tex = nullptr;
	ID3D11ShaderResourceView *imp_srv = nullptr;
	IDXGIKeyedMutex *imp_km = nullptr;
	uint32_t imp_w = 0, imp_h = 0;

	// Client export (service device) — the weave output pattern.
	ID3D11Texture2D *exp_tex = nullptr;
	HANDLE exp_handle = nullptr;
	ID3D11Fence *exp_fence = nullptr;
	HANDLE exp_fence_handle = nullptr;
	uint64_t exp_fence_value = 0;
	uint32_t exp_w = 0, exp_h = 0, exp_format = 0;
	//! XR_DXR_lift v3: the depth export texture (same fence as exp_tex).
	ID3D11Texture2D *exp_depth_tex = nullptr;
	HANDLE exp_depth_handle = nullptr;
	uint32_t exp_depth_w = 0, exp_depth_h = 0, exp_depth_format = 0;

	// Blob latch (xrAcquireLiftBlobDXR two-call idiom).
	std::shared_ptr<std::vector<uint8_t>> blob_latched;
	u_lift_frame_meta blob_latched_meta = {};
	uint32_t blob_latched_format = 0;

	// Throttled INFO.
	uint64_t last_stats_log_ns = 0;

	// Snapshot cap: the last capped size WARNed about (producer thread, under mtx).
	uint32_t cap_logged_w = 0, cap_logged_h = 0;

	// Letterbox crop (producer thread only; the crop is read under mtx at submit).
	u_lift_letterbox lb = {};
	lift_lb_readback lb_rb[LB_RING];
	uint32_t lb_next = 0; //!< next ring entry to issue into

	// Runtime auto-convergence (u_lift_autoconv.h), lift thread only.
	//! The module was asked for the depth of each conversion for the policy
	//! (beyond what the app asked for).
	bool ac_depth = false;
	u_lift_autoconv_filter ac = {};
	u_lift_autoconv_estimate ac_last = {}; //!< last measurement (logged)
	//! One depth readback in flight (lift device, staging).
	ID3D11Texture2D *ac_staging = nullptr;
	uint32_t ac_w = 0, ac_h = 0, ac_fmt = 0;
	bool ac_pending = false;
	bool ac_inverse = true;
	float ac_scale = 1.0f, ac_offset = 0.0f;
	uint64_t ac_issued_ns = 0;
	std::vector<float> ac_scratch; //!< R16F -> float rows
	float ac_value = -1.0f;        //!< last delivered convergence; < 0 = none yet
	uint64_t ac_applied = 0;       //!< conversions that carried the runtime's value
	bool ac_locked_logged = false;
	bool ac_nodepth_logged = false;
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
	//! G7: the panel's eyes, published by the service (never pulled from here).
	std::mutex eyes_mtx;
	xrt_eye_positions eyes = {};
	uint64_t eyes_ns = 0;
	void (*eyes_refresh)(void *ud) = nullptr;
	void *eyes_refresh_ud = nullptr;
	//! The display's nominal viewing distance (ADR-048 straight-on viewer).
	float nominal_z = U_LIFT_NOMINAL_Z_DEFAULT_M;
	//! DXR_LIFT_AUTO_CONVERGENCE, read once at create: who picks the
	//! convergence of a stream whose app leaves it AUTO (default: the runtime).
	u_lift_autoconv_source autoconv = U_LIFT_AUTOCONV_SOURCE_RUNTIME;
	u_lift_autoconv_params ac_params = {};
	u_lift_autoconv_filter_params ac_fparams = {};
	//! DXR_LIFT_RECENTER=off, read once at create: force recentering OFF for
	//! every stream (A/B against the ADR-048 default ease-back).
	bool recenter_off = false;

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
	bool activate_requested = false;
	bool activated = false;
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
	uint64_t next_stream_id = 0;
	uint32_t live_streams = 0;
	std::map<uint64_t, std::unique_ptr<lift_stream>> streams;
	u_lift_sched sched = {}; //!< cross-stream priority scheduling (u_lift_mailbox.h)

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

	// The weave path's bridge to core-routed streams.
	std::mutex bridge_mtx;
	d3d11_lift_core_bridge bridge = {};

	std::thread thread;
};

//! A snapshot destination: the pre-core input slot, or a core slot image (#2003).
struct blit_dst
{
	IDXGIKeyedMutex *svc_km;
	ID3D11RenderTargetView *svc_rtv;
};

//! IEEE half -> float (the depth may be R16_FLOAT).
static inline float
half_to_float(uint16_t h)
{
	const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
	uint32_t exp = (h >> 10) & 0x1fu;
	uint32_t man = h & 0x3ffu;
	uint32_t bits;
	if (exp == 0) {
		if (man == 0) {
			bits = sign;
		} else {
			// Subnormal: normalise.
			exp = 127 - 15 + 1;
			while ((man & 0x400u) == 0) {
				man <<= 1;
				exp--;
			}
			man &= 0x3ffu;
			bits = sign | (exp << 23) | (man << 13);
		}
	} else if (exp == 0x1f) {
		bits = sign | 0x7f800000u | (man << 13); // inf / NaN
	} else {
		bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
	}
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}
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

// d3d11_lift.cpp: producer side, pull fresh eyes when nothing published them lately.
void
eyes_refresh_if_stale(d3d11_lift *l);

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

//! Letterbox measurement (explicitly instantiated for lift_stream and core_weave_state).
template <typename STATE>
void
letterbox_step(d3d11_lift *l, STATE &st, ID3D11ShaderResourceView *src, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
