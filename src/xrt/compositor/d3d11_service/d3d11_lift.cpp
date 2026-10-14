// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift (ADR-042) on the D3D11 service — see d3d11_lift.h.
 *
 * Deliberately raw COM (no WIL) so the file is portable to the MinGW
 * compile-check (scripts/build-mingw-check.sh cannot build WIL).
 *
 * @ingroup comp_d3d11_service
 */

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
static void
rel(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

static void
close_handle(HANDLE &h)
{
	if (h != nullptr && h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
	}
	h = nullptr;
}

//! Round up to a 64 px multiple — input slots grow, they don't chase every size.
static uint32_t
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
static bool
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

static DXGI_FORMAT
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


/*
 *
 * Snapshot blit (service device): sample a source sub-rect into a slot — 1:1
 * (point) when the snapshot is uncapped, box-filtered down when the service's
 * DXR_LIFT_MAX_INPUT_EDGE cap engages (u_lift_cap_dims).
 *
 */

static const char *k_snap_vs = R"(
struct VSO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSO main(uint id : SV_VertexID) {
	VSO o;
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
)";

// src_rect = (x, y, w, h) in source texels, src_size = (tw, th).
static const char *k_snap_ps = R"(
cbuffer C : register(b0) { float4 src_rect; float4 src_size; };
Texture2D src : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float2 p = src_rect.xy + uv * src_rect.zw;
	float4 c = src.SampleLevel(samp, p / src_size.xy, 0);
	return float4(c.rgb, 1.0);
}
)";

/*
 * Downscaling variant (capped snapshot). One destination texel covers
 * step = src_rect.zw / dst_size.xy source texels; four bilinear taps at the
 * quarter points of that footprint are an exact box filter up to a 4x reduction
 * (7680 -> 1920) and a much better one than a single tap beyond it. Taps are
 * clamped to texel centres inside the sub-rect, so nothing outside the lifted
 * rect bleeds in at its edges.
 */
static const char *k_snap_ps_scaled = R"(
cbuffer C : register(b0) { float4 src_rect; float4 src_size; float4 dst_size; };
Texture2D src : register(t0);
SamplerState samp : register(s0);
float3 tap(float2 p, float2 lo, float2 hi) {
	return src.SampleLevel(samp, clamp(p, lo, hi) / src_size.xy, 0).rgb;
}
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float2 p = src_rect.xy + uv * src_rect.zw;
	float2 q = 0.25 * src_rect.zw / dst_size.xy;
	float2 lo = src_rect.xy + 0.5;
	float2 hi = src_rect.xy + src_rect.zw - 0.5;
	float3 c = tap(p + float2(-q.x, -q.y), lo, hi) + tap(p + float2(q.x, -q.y), lo, hi) +
	           tap(p + float2(-q.x, q.y), lo, hi) + tap(p + float2(q.x, q.y), lo, hi);
	return float4(c * 0.25, 1.0);
}
)";

struct snap_cb
{
	float src_rect[4];
	float src_size[4];
	float dst_size[4]; //!< read by k_snap_ps_scaled only
};

/*
 * Letterbox profile (DXR_LIFT_LETTERBOX). Target: R32_FLOAT,
 * U_LIFT_LETTERBOX_BINS_MAX x 2 — row 0 = the rect's row buckets top to bottom,
 * row 1 = its column buckets left to right. Each texel is the non-black
 * fraction of its bucket: the max over the bucket's first, middle and last
 * line, each line sampled at 96 points across the rect. Taking the max over
 * the first and LAST line means a bucket holding the picture's first row is
 * already picture, so a bar never swallows a picture row.
 * bins = (row buckets, column buckets, -, -).
 */
static const char *k_lb_ps = R"(
cbuffer C : register(b0) { float4 src_rect; float4 bins; };
Texture2D src : register(t0);
float line_frac(bool rows, float a, float span) {
	float cnt = 0.0;
	[loop] for (int k = 0; k < 96; k++) {
		float b = floor((k + 0.5) * span / 96.0);
		int2 p = rows ? int2(src_rect.x + b, src_rect.y + a) : int2(src_rect.x + a, src_rect.y + b);
		float3 c = src.Load(int3(p, 0)).rgb;
		cnt += dot(c, float3(0.2126, 0.7152, 0.0722)) > 0.07 ? 1.0 : 0.0;
	}
	return cnt / 96.0;
}
float4 main(float4 pos : SV_Position) : SV_Target {
	uint i = (uint)pos.x;
	bool rows = pos.y < 1.0;
	float n = rows ? bins.x : bins.y;
	if ((float)i >= n) {
		return 0.0;
	}
	float len = rows ? src_rect.w : src_rect.z;  // along the profile
	float span = rows ? src_rect.z : src_rect.w; // across it
	float a0 = floor(i * len / n);
	float a1 = max(a0, floor((i + 1) * len / n) - 1.0);
	float am = floor((a0 + a1) * 0.5);
	return max(line_frac(rows, a0, span), max(line_frac(rows, am, span), line_frac(rows, a1, span)));
}
)";

struct lb_cb
{
	float src_rect[4];
	float bins[4];
};

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

static void
caps_set_state(d3d11_lift *l, uint32_t state, const char *why)
{
	// Caller holds mtx. WARN once per state CHANGE, never per poll.
	l->caps.state = state;
	l->change_count++;
	if (state == XRT_DP_LIFT_STATE_UNAVAILABLE && why != nullptr) {
		snprintf(l->unavail_detail, sizeof(l->unavail_detail), "%s", why);
	}
	if (l->logged_state != state) {
		l->logged_state = state;
		U_LOG_W("[lift] module state -> %s (modes=0x%x backend='%s' max_streams=%u max_views=%u)%s%s",
		        state_str(state), l->caps.modes, l->caps.backend, l->caps.max_streams, l->caps.max_views,
		        why != nullptr ? " — " : "", why != nullptr ? why : "");
	}
	if (state != XRT_DP_LIFT_STATE_ACTIVATING) {
		l->activating_since_ns = 0;
		l->activating_warned = false;
	} else if (l->activating_since_ns == 0 && l->activate_requested) {
		l->activating_since_ns = os_monotonic_get_ns();
	}
}

//! How long the module may stay ACTIVATING after first being asked for before
//! the one-shot stall WARN (#1812). A healthy module warms up in seconds.
static const uint64_t k_activating_warn_ns = 30ull * 1000 * 1000 * 1000;

/*!
 * One-shot WARN when the module has been ACTIVATING for longer than
 * k_activating_warn_ns since someone first asked for it (#1812: a vendor
 * conversion runtime whose files were half-removed under the running service).
 * Callers keep getting the ACTIVATING snapshot without waiting — this only
 * makes the stall visible. Caller holds mtx; called from the lift thread and
 * from every caps query, so it fires even while the lift thread itself is
 * blocked inside a vendor call.
 */
static void
lift_check_activating_stall(d3d11_lift *l, uint64_t now)
{
	if (l->caps.state != XRT_DP_LIFT_STATE_ACTIVATING || !l->activate_requested) {
		return;
	}
	if (l->activating_since_ns == 0) {
		l->activating_since_ns = now;
		return;
	}
	if (l->activating_warned || now - l->activating_since_ns < k_activating_warn_ns) {
		return;
	}
	l->activating_warned = true;
	if (l->vendor_call != nullptr) {
		U_LOG_W(
		    "[lift] module still ACTIVATING after %.0f s; the lift thread is blocked in the plug-in's %s "
		    "(for %.0f s). Caps queries keep answering ACTIVATING, streams wait. Usual cause: a broken or "
		    "half-removed vendor conversion runtime install. Repair it, then restart the service.",
		    (double)(now - l->activating_since_ns) / 1e9, l->vendor_call,
		    (double)(now - l->vendor_call_since_ns) / 1e9);
	} else {
		U_LOG_W(
		    "[lift] module still ACTIVATING after %.0f s; the plug-in keeps reporting ACTIVATING (its module "
		    "load has not finished). Caps queries keep answering ACTIVATING, streams wait. Usual cause: a "
		    "broken or half-removed vendor conversion runtime install.",
		    (double)(now - l->activating_since_ns) / 1e9);
	}
}

//! Mark the lift thread as inside / out of a (possibly blocking) vendor call.
static void
lift_vendor_call(d3d11_lift *l, const char *what)
{
	std::lock_guard<std::mutex> g(l->mtx);
	l->vendor_call = what;
	l->vendor_call_since_ns = what != nullptr ? os_monotonic_get_ns() : 0;
}


/*
 *
 * Slot allocation.
 *
 */

static void
in_slot_release(lift_in_slot &s)
{
	rel(s.lift_km);
	rel(s.lift_tex);
	close_handle(s.share);
	rel(s.svc_km);
	rel(s.svc_rtv);
	rel(s.svc_tex);
	s.cap_w = s.cap_h = 0;
}

static void
out_slot_release(lift_out_slot &s)
{
	rel(s.svc_km);
	rel(s.svc_srv);
	rel(s.svc_tex);
	close_handle(s.share);
	rel(s.lift_km);
	rel(s.lift_tex);
	s.w = s.h = s.format = s.view_count = 0;
	s.blob.reset();
	rel(s.d_svc_km);
	rel(s.d_svc_tex);
	close_handle(s.d_share);
	rel(s.d_lift_km);
	rel(s.d_lift_tex);
	s.d_w = s.d_h = s.d_format = 0;
	s.has_depth = false;
}

/*!
 * XR_DXR_lift v3: make out slot @p s's DEPTH texture @p w x @p h @p format —
 * the views' pattern (created on the lift device with a keyed mutex, opened
 * on the service device). Lift thread only.
 */
static bool
depth_slot_ensure(d3d11_lift *l, lift_out_slot &s, uint32_t w, uint32_t h, uint32_t format)
{
	if (s.d_lift_tex != nullptr && s.d_w == w && s.d_h == h && s.d_format == format) {
		return true;
	}
	rel(s.d_svc_km);
	rel(s.d_svc_tex);
	close_handle(s.d_share);
	rel(s.d_lift_km);
	rel(s.d_lift_tex);
	s.d_w = s.d_h = s.d_format = 0;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = (DXGI_FORMAT)format;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	HRESULT hr = l->lift_device->CreateTexture2D(&td, nullptr, &s.d_lift_tex);
	if (SUCCEEDED(hr)) {
		hr = s.d_lift_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&s.d_lift_km);
	}
	IDXGIResource1 *r1 = nullptr;
	if (SUCCEEDED(hr)) {
		hr = s.d_lift_tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
	}
	if (SUCCEEDED(hr)) {
		hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
		                            &s.d_share);
	}
	rel(r1);
	if (SUCCEEDED(hr)) {
		hr = l->svc_device1->OpenSharedResource1(s.d_share, __uuidof(ID3D11Texture2D), (void **)&s.d_svc_tex);
	}
	if (SUCCEEDED(hr)) {
		hr = s.d_svc_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&s.d_svc_km);
	}
	if (FAILED(hr)) {
		U_LOG_E("[lift] depth slot %ux%u fmt=%u create/share failed: 0x%08lx", w, h, format, (unsigned long)hr);
		rel(s.d_svc_km);
		rel(s.d_svc_tex);
		close_handle(s.d_share);
		rel(s.d_lift_km);
		rel(s.d_lift_tex);
		return false;
	}
	s.d_w = w;
	s.d_h = h;
	s.d_format = format;
	U_LOG_I("[lift] depth slot %ux%u fmt=%u ready", w, h, format);
	return true;
}

/*!
 * Make input slot @p s hold at least @p w x @p h. Created on the service device
 * (RGBA8, RT for the snapshot blit, keyed mutex, NT handle), opened on the lift
 * device. Needs the lift device, so a stream can only be fed after activation.
 */
static bool
in_slot_ensure(d3d11_lift *l, lift_in_slot &s, uint32_t w, uint32_t h)
{
	if (s.svc_tex != nullptr && s.cap_w >= w && s.cap_h >= h) {
		return true;
	}
	if (l->lift_device1 == nullptr) {
		return false;
	}
	uint32_t cw = round_cap(w > s.cap_w ? w : s.cap_w);
	uint32_t ch = round_cap(h > s.cap_h ? h : s.cap_h);
	in_slot_release(s);

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = cw;
	td.Height = ch;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	HRESULT hr = l->svc_device->CreateTexture2D(&td, nullptr, &s.svc_tex);
	if (SUCCEEDED(hr)) {
		hr = l->svc_device->CreateRenderTargetView(s.svc_tex, nullptr, &s.svc_rtv);
	}
	if (SUCCEEDED(hr)) {
		hr = s.svc_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&s.svc_km);
	}
	IDXGIResource1 *r1 = nullptr;
	if (SUCCEEDED(hr)) {
		hr = s.svc_tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
	}
	if (SUCCEEDED(hr)) {
		hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
		                            &s.share);
	}
	rel(r1);
	if (SUCCEEDED(hr)) {
		hr = l->lift_device1->OpenSharedResource1(s.share, __uuidof(ID3D11Texture2D), (void **)&s.lift_tex);
	}
	if (SUCCEEDED(hr)) {
		hr = s.lift_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&s.lift_km);
	}
	if (FAILED(hr)) {
		U_LOG_E("[lift] input slot %ux%u create/share failed: 0x%08lx", cw, ch, (unsigned long)hr);
		in_slot_release(s);
		return false;
	}
	s.cap_w = cw;
	s.cap_h = ch;
	U_LOG_I("[lift] input slot %ux%u ready", cw, ch);
	return true;
}

/*!
 * Make output slot @p s exactly @p w x @p h in @p format. Created on the lift
 * device (keyed mutex, NT handle), opened on the service device. Lift thread.
 */
static bool
out_slot_ensure(d3d11_lift *l, lift_out_slot &s, uint32_t w, uint32_t h, uint32_t format)
{
	if (s.lift_tex != nullptr && s.w == w && s.h == h && s.format == format) {
		return true;
	}
	std::shared_ptr<std::vector<uint8_t>> keep_blob = s.blob;
	out_slot_release(s);
	s.blob = keep_blob;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = (DXGI_FORMAT)format;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	HRESULT hr = l->lift_device->CreateTexture2D(&td, nullptr, &s.lift_tex);
	if (SUCCEEDED(hr)) {
		hr = s.lift_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&s.lift_km);
	}
	IDXGIResource1 *r1 = nullptr;
	if (SUCCEEDED(hr)) {
		hr = s.lift_tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
	}
	if (SUCCEEDED(hr)) {
		hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
		                            &s.share);
	}
	rel(r1);
	if (SUCCEEDED(hr)) {
		hr = l->svc_device1->OpenSharedResource1(s.share, __uuidof(ID3D11Texture2D), (void **)&s.svc_tex);
	}
	if (SUCCEEDED(hr)) {
		hr = s.svc_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&s.svc_km);
	}
	if (SUCCEEDED(hr)) {
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = typed_srv_format((DXGI_FORMAT)format);
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		hr = l->svc_device->CreateShaderResourceView(s.svc_tex, &sd, &s.svc_srv);
	}
	if (FAILED(hr)) {
		U_LOG_E("[lift] output slot %ux%u fmt=%u create/share failed: 0x%08lx", w, h, format,
		        (unsigned long)hr);
		out_slot_release(s);
		return false;
	}
	s.w = w;
	s.h = h;
	s.format = format;
	U_LOG_I("[lift] output slot %ux%u fmt=%u ready", w, h, format);
	return true;
}

static void
stream_release_gpu(lift_stream &st)
{
	for (auto &s : st.in) {
		in_slot_release(s);
	}
	for (auto &s : st.out) {
		out_slot_release(s);
	}
	rel(st.exact_tex);
	rel(st.imp_km);
	rel(st.imp_srv);
	rel(st.imp_tex);
	if (!st.imp_dxgi) {
		close_handle(st.imp_handle);
	}
	st.imp_handle = nullptr;
	rel(st.exp_fence);
	close_handle(st.exp_fence_handle);
	rel(st.exp_tex);
	close_handle(st.exp_handle);
	rel(st.exp_depth_tex);
	close_handle(st.exp_depth_handle);
	st.blob_latched.reset();
	for (auto &r : st.lb_rb) {
		rel(r.staging);
		r.pending = false;
	}
	rel(st.ac_staging);
	st.ac_pending = false;
}


/*
 *
 * Lift thread.
 *
 */

//! The lift device on the service's adapter (shared textures cross between them).
static HRESULT
lift_create_device(d3d11_lift *l)
{
	IDXGIDevice *dxgi_dev = nullptr;
	IDXGIAdapter *adapter = nullptr;
	HRESULT hr = S_OK;
	hr = l->svc_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev);
	if (SUCCEEDED(hr)) {
		hr = dxgi_dev->GetAdapter(&adapter);
	}
	rel(dxgi_dev);
	D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
	if (SUCCEEDED(hr)) {
		hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		                       levels, 2, D3D11_SDK_VERSION, &l->lift_device, &got, &l->lift_context);
	}
	rel(adapter);
	if (SUCCEEDED(hr)) {
		hr = l->lift_device->QueryInterface(__uuidof(ID3D11Device1), (void **)&l->lift_device1);
	}
	if (SUCCEEDED(hr)) {
		// A conversion module may flush / signal this context from its own
		// worker thread — the same protection the vendor weaver relies on.
		ID3D11Multithread *mt = nullptr;
		if (SUCCEEDED(l->lift_context->QueryInterface(__uuidof(ID3D11Multithread), (void **)&mt))) {
			mt->SetMultithreadProtected(TRUE);
			rel(mt);
		}
	}
	if (FAILED(hr)) {
		rel(l->lift_device1);
		rel(l->lift_context);
		rel(l->lift_device);
	}
	return hr;
}

//! Bring up the lift device + the vendor module's DP. Lift thread, no lock held.
static void
lift_activate(d3d11_lift *l)
{
	const char *kill = getenv("DXR_LIFT");
	if (kill != nullptr && strcmp(kill, "0") == 0) {
		std::lock_guard<std::mutex> g(l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "DXR_LIFT=0 (kill switch)");
		return;
	}
	// G5: re-resolved at EVERY attempt, from the plug-in that registered the
	// lift factory (a plug-in swap or a late install is picked up, never latched).
	void *lift_factory_p = nullptr, *fallback_factory_p = nullptr;
	if (l->resolve != nullptr) {
		(void)l->resolve(l->resolve_ud, &lift_factory_p, &fallback_factory_p);
	}
	const xrt_dp_factory_d3d11_fn_t lift_factory = (xrt_dp_factory_d3d11_fn_t)lift_factory_p;
	const xrt_dp_factory_d3d11_fn_t fallback_factory = (xrt_dp_factory_d3d11_fn_t)fallback_factory_p;
	if (lift_factory == nullptr && fallback_factory == nullptr) {
		std::lock_guard<std::mutex> g(l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "no D3D11 display-processor factory");
		return;
	}

	// Same adapter as the service device: shared textures cross between them.
	// A retry keeps the device it already made.
	if (l->lift_device == nullptr && FAILED(lift_create_device(l))) {
		std::lock_guard<std::mutex> g(l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "lift device creation failed");
		return;
	}

	// The lift DP — never weaves, carries only the module. The plug-in's
	// explicit lift-only factory when it has one (no weaver, no tracker
	// session); else its ordinary factory with a NULL window.
	const bool lift_only = lift_factory != nullptr;
	lift_vendor_call(l, "display-processor factory");
	xrt_result_t xret =
	    (lift_only ? lift_factory : fallback_factory)(l->lift_device, l->lift_context, nullptr, &l->dp);
	lift_vendor_call(l, nullptr);
	U_LOG_W("[lift] lift DP %s via the plug-in's %s factory (dedicated device, NULL window)",
	        xret == XRT_SUCCESS && l->dp != nullptr ? "created" : "REFUSED",
	        lift_only ? "lift-only" : "ordinary (no lift-only factory)");
	xrt_dp_lift_caps caps;
	bool have = false;
	if (xret == XRT_SUCCESS && l->dp != nullptr && xrt_display_processor_d3d11_has_lift(l->dp)) {
		lift_vendor_call(l, "lift_get_caps");
		have = xrt_display_processor_d3d11_lift_get_caps(l->dp, &caps);
		lift_vendor_call(l, nullptr);
	} else {
		xrt_dp_lift_caps_init(&caps);
	}
	if (!have && l->dp != nullptr) {
		// No module on this plug-in: don't keep a vendor DP instance alive for nothing.
		xrt_display_processor_d3d11_destroy(&l->dp);
	}

	std::lock_guard<std::mutex> g(l->mtx);
	l->activated = true;
	l->module_alive = have;
	l->has_depth_slot = have && XRT_DP_HAS_SLOT(l->dp, lift_get_depth) && l->dp->lift_get_depth != nullptr;
	if (!have) {
		l->caps = caps;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE,
		               xret != XRT_SUCCESS ? "lift DP factory refused (NULL window)"
		                                   : "display processor ships no conversion module");
		return;
	}
	l->caps = caps;
	if (l->caps.state > XRT_DP_LIFT_STATE_READY) {
		l->caps.state = XRT_DP_LIFT_STATE_UNAVAILABLE;
	}
	if (l->caps.modes == 0) {
		l->caps.state = XRT_DP_LIFT_STATE_UNAVAILABLE;
	}
	l->logged_state = 0xffffffffu;
	caps_set_state(l, l->caps.state, nullptr);
}

//! Poll caps while not READY (a module warming up). Lift thread, no lock held.
static void
lift_poll_caps(d3d11_lift *l)
{
	if (l->dp == nullptr) {
		return;
	}
	xrt_dp_lift_caps caps;
	lift_vendor_call(l, "lift_get_caps");
	bool have = xrt_display_processor_d3d11_lift_get_caps(l->dp, &caps);
	std::lock_guard<std::mutex> g(l->mtx);
	l->vendor_call = nullptr;
	if (!have) {
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "module stopped answering");
		return;
	}
	uint32_t st = caps.state > XRT_DP_LIFT_STATE_READY ? XRT_DP_LIFT_STATE_UNAVAILABLE : caps.state;
	l->caps = caps;
	l->caps.state = l->logged_state; // keep the logged value until caps_set_state compares
	caps_set_state(l, st, nullptr);
}

//! Destroy dead streams nobody still reads. Caller holds mtx (via @p lk).
static void
lift_reap(d3d11_lift *l, std::unique_lock<std::mutex> &lk)
{
	for (auto it = l->streams.begin(); it != l->streams.end();) {
		lift_stream &st = *it->second;
		bool pinned = false;
		for (uint32_t i = 0; i < U_LIFT_RING_SIZE; i++) {
			pinned = pinned || st.mb.out_pins[i] > 0;
		}
		if (!st.dead || st.converting || pinned) {
			++it;
			continue;
		}
		std::unique_ptr<lift_stream> victim = std::move(it->second);
		it = l->streams.erase(it);
		lk.unlock();
		if (victim->dp_created && l->dp != nullptr) {
			l->dp->lift_stream_destroy(l->dp, victim->dp_id);
		}
		stream_release_gpu(*victim);
		U_LOG_I("[lift] stream %llu reaped (submitted=%llu converted=%llu dropped=%llu failed=%llu)",
		        (unsigned long long)victim->id, (unsigned long long)victim->mb.submitted,
		        (unsigned long long)victim->mb.converted, (unsigned long long)victim->mb.dropped,
		        (unsigned long long)victim->mb.failed);
		victim.reset();
		lk.lock();
		it = l->streams.begin(); // the map may have changed while unlocked
	}
}

//! Log name of a recenter mode.
static const char *
recenter_str(uint32_t mode)
{
	return mode == U_LIFT_RECENTER_EASE_BACK ? "ease-back" : "off";
}

//! The viewpoints one conversion uses (ADR-048), and what the DP is told about them.
struct viewpoint_result
{
	uint32_t floats = 0;   //!< 3 x viewpoints in the caller's array
	float centre[3] = {};  //!< rect centre, display space
	float rect_w = 0.0f;   //!< metres; 0 = unknown
	float rect_h = 0.0f;   //!< metres; 0 = unknown
	float baseline = 0.0f; //!< first-to-last viewpoint distance
	float mid[3] = {};     //!< midpoint after the policy
	uint32_t axis_mode = XRT_DP_LIFT_AXIS_X;
	float max_offset = 0.0f;
	uint32_t recenter_mode = U_LIFT_RECENTER_OFF;
	uint32_t source = XRT_DP_LIFT_VIEWPOINTS_TRACKED; //!< XRT_DP_LIFT_VIEWPOINTS_*
};

//! Midpoint + first-to-last distance of @p n viewpoints (3 floats each).
static void
viewpoints_mid_baseline(const float *vps, uint32_t n, float mid[3], float *baseline)
{
	mid[0] = mid[1] = mid[2] = 0.0f;
	*baseline = 0.0f;
	if (n == 0) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		for (int a = 0; a < 3; a++) {
			mid[a] += vps[3 * i + a] / (float)n;
		}
	}
	if (n >= 2) {
		const float *f = &vps[0];
		const float *e = &vps[3 * (n - 1)];
		const float dx = e[0] - f[0], dy = e[1] - f[1], dz = e[2] - f[2];
		*baseline = sqrtf(dx * dx + dy * dy + dz * dz);
	}
}

/*!
 * Resolve the viewpoints of one conversion (lift thread, no lock held).
 *
 * Every viewpoint is rebased to the lifted region's centre (@p frame; the
 * panel centre when it has none). EXPLICIT viewpoints (the caller's, in
 * @p vps on entry) get only that rebase. TRACKED ones are resolved HERE, per
 * conversion, from the panel's predicted eyes (the lift DP has no tracker
 * session of its own) and then run through the stream's viewpoint policy
 * (u_lift_viewpoint.h): rig factors, axis mask, recenter filter, clamp.
 * With an app rig (XR_DXR_lift v3, ADR-048 Addendum A) the rig replaces the
 * policy's ipd / parallax factors: the policy (axis mask, recenter, clamp)
 * runs on the viewer first, then the rig maps the eyes with the xrLocateViews
 * math (u_lift_rig.h), the rect as the screen.
 * DEPTH / GAUSSIANS streams get none.
 */
static viewpoint_result
resolve_viewpoints(d3d11_lift *l,
                   lift_stream &st,
                   uint32_t view_count,
                   const xrt_lift_view_control &in_vc,
                   const xrt_lift_rig &in_rig,
                   const xrt_lift_rect_frame &frame,
                   float *vps,
                   uint32_t vp_floats)
{
	viewpoint_result r;
	if (frame.valid) {
		memcpy(r.centre, frame.center, sizeof(r.centre));
	}
	r.rect_w = frame.width_m > 0.0f ? frame.width_m : 0.0f;
	r.rect_h = frame.height_m > 0.0f ? frame.height_m : 0.0f;

	if (vp_floats >= 3) {
		// EXPLICIT: the app's own viewpoints, only rebased.
		const uint32_t n = vp_floats / 3;
		u_lift_viewpoint_rebase(vps, n, r.centre);
		r.floats = 3 * n;
		r.axis_mode = XRT_DP_LIFT_AXIS_XYZ;
		r.source = XRT_DP_LIFT_VIEWPOINTS_EXPLICIT;
		viewpoints_mid_baseline(vps, n, r.mid, &r.baseline);
		return r;
	}
	if (st.info.mode == XRT_DP_LIFT_MODE_GAUSSIANS || st.info.mode == XRT_DP_LIFT_MODE_DEPTH) {
		return r;
	}

	u_lift_view_control vc = {in_vc.ipd_factor,    in_vc.parallax_factor, in_vc.axis_mode, in_vc.max_offset_m,
	                          in_vc.recenter_mode, in_vc.hold_s,          in_vc.tau_s};
	u_lift_view_control_sanitize(&vc);
	if (l->recenter_off) {
		vc.recenter_mode = U_LIFT_RECENTER_OFF;
	}
	r.axis_mode = vc.axis_mode;
	r.max_offset = vc.max_offset_m;
	r.recenter_mode = vc.recenter_mode;
	// WARN when a stream's effective policy changes (an app sets it once, or
	// on a user toggle), never per frame.
	const xrt_lift_view_control eff = {vc.ipd_factor,    vc.parallax_factor, vc.axis_mode, vc.max_offset_m,
	                                   vc.recenter_mode, vc.hold_s,          vc.tau_s};
	if (!st.logged_vc_valid || memcmp(&eff, &st.logged_vc, sizeof(eff)) != 0) {
		st.logged_vc = eff;
		st.logged_vc_valid = true;
		U_LOG_W(
		    "[lift] stream %llu viewpoint policy: ipd=%.2f parallax=%.2f axis=%u max_offset=%.3f m "
		    "recenter=%s hold=%.2f s tau=%.2f s (rect-relative viewpoints, ADR-048)",
		    (unsigned long long)st.id, vc.ipd_factor, vc.parallax_factor, vc.axis_mode, vc.max_offset_m,
		    recenter_str(vc.recenter_mode), vc.hold_s, vc.tau_s);
	}
	// XR_DXR_lift v3 app rig (sanitized here, on the lift thread, like the policy).
	u_lift_rig rig = {in_rig.type,
	                  in_rig.ipd_factor,
	                  in_rig.parallax_factor,
	                  in_rig.perspective_factor,
	                  in_rig.inv_convergence_distance,
	                  in_rig.half_tan_vfov,
	                  in_rig.m2v};
	u_lift_rig_sanitize(&rig);
	const xrt_lift_rig eff_rig = {rig.type,
	                              rig.ipd_factor,
	                              rig.parallax_factor,
	                              rig.perspective_factor,
	                              rig.inv_convergence_distance,
	                              rig.half_tan_vfov,
	                              rig.m2v};
	if (memcmp(&eff_rig, &st.logged_rig, sizeof(eff_rig)) != 0) {
		st.logged_rig = eff_rig;
		if (rig.type == U_LIFT_RIG_DISPLAY) {
			U_LOG_W(
			    "[lift] stream %llu app display rig: ipd=%.2f parallax=%.2f perspective=%.2f "
			    "(viewpoints = the xrLocateViews eyes, rect as the screen; ADR-048 Addendum A)",
			    (unsigned long long)st.id, rig.ipd_factor, rig.parallax_factor, rig.perspective_factor);
		} else if (rig.type == U_LIFT_RIG_CAMERA) {
			U_LOG_W(
			    "[lift] stream %llu app camera rig: ipd=%.2f parallax=%.2f convergence=%.3f /unit "
			    "half_tan_vfov=%.3f m2v=%.3f (viewpoints = the equivalent physical viewer; ADR-048 "
			    "Addendum A)",
			    (unsigned long long)st.id, rig.ipd_factor, rig.parallax_factor,
			    rig.inv_convergence_distance, rig.half_tan_vfov, rig.m2v);
		} else {
			U_LOG_W("[lift] stream %llu app rig cleared: tracked viewpoints + policy",
			        (unsigned long long)st.id);
		}
	}
	if (rig.type != U_LIFT_RIG_NONE) {
		// The rig owns the eye factors (step 1a / 1b): the policy keeps its
		// axis mask, recentering and clamp, applied to the viewer.
		vc.ipd_factor = 1.0f;
		vc.parallax_factor = 1.0f;
	}

	// G7: the snapshot the service published (no render_mutex on this thread).
	struct xrt_eye_positions eyes = {};
	uint64_t eyes_ns = 0;
	{
		std::lock_guard<std::mutex> g(l->eyes_mtx);
		eyes = l->eyes;
		eyes_ns = l->eyes_ns;
	}
	const uint64_t eyes_now = os_monotonic_get_ns();
	if (eyes_ns == 0 || eyes_now - eyes_ns > 1000ull * 1000ull * 1000ull || !eyes.valid || eyes.count < 2) {
		u_lift_recenter_reset(&st.recenter); // no eyes: the filter starts over
		return r;
	}
	if (!eyes.is_tracking) {
		// Tracking lost: the vendor's fallback eyes are still used, but the
		// recenter filter forgets its reference (ADR-048: reset on loss).
		u_lift_recenter_reset(&st.recenter);
	}
	// The pair (a module spreads N views around it), or N eyes when the panel
	// reports exactly the stream's view count.
	uint32_t n = eyes.count == view_count ? eyes.count : 2;
	n = n > XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS ? XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS : n;
	for (uint32_t i = 0; i < n; i++) {
		vps[3 * i + 0] = eyes.eyes[i].x;
		vps[3 * i + 1] = eyes.eyes[i].y;
		vps[3 * i + 2] = eyes.eyes[i].z;
	}
	u_lift_viewpoint_rebase(vps, n, r.centre);
	u_lift_viewpoint_apply(&vc, &st.recenter, l->nominal_z, os_monotonic_get_ns(), vps, n, vps, &r.baseline, r.mid);
	if (rig.type != U_LIFT_RIG_NONE && u_lift_rig_apply(&rig, l->nominal_z, r.rect_w, r.rect_h, vps, n, vps)) {
		r.source = rig.type == U_LIFT_RIG_CAMERA ? XRT_DP_LIFT_VIEWPOINTS_CAMERA_RIG
		                                         : XRT_DP_LIFT_VIEWPOINTS_DISPLAY_RIG;
		viewpoints_mid_baseline(vps, n, r.mid, &r.baseline);
	}
	r.floats = 3 * n;
	return r;
}

/*!
 * XR_DXR_lift v3 (ADR-048 Addendum A): fetch the auxiliary depth of the
 * conversion lift_convert just returned for @p st, copy it into out slot
 * @p o, and derive what the app is told (xrt_lift_depth_info) — including the
 * display transform, from the viewpoints and rect the runtime already owns.
 * Lift thread, no lock held, right after the views were copied into @p o.
 */
static void
lift_copy_depth(
    d3d11_lift *l, lift_stream &st, lift_out_slot &o, const viewpoint_result &vr, const xrt_dp_lift_depth *got)
{
	if (got == nullptr) {
		if (!st.depth_missing_logged) {
			st.depth_missing_logged = true;
			U_LOG_W("[lift] stream %llu asked for auxiliary depth but the module returned none%s",
			        (unsigned long long)st.id,
			        XRT_DP_HAS_SLOT(l->dp, lift_get_depth) && l->dp->lift_get_depth != nullptr
			            ? ""
			            : " (the plug-in has no lift_get_depth slot)");
		}
		return;
	}
	const xrt_dp_lift_depth &dd = *got;
	if (dd.format != DXGI_FORMAT_R32_FLOAT && dd.format != DXGI_FORMAT_R16_FLOAT) {
		if (!st.depth_missing_logged) {
			st.depth_missing_logged = true;
			U_LOG_W("[lift] stream %llu: module depth format %u is not R32_FLOAT / R16_FLOAT — dropped",
			        (unsigned long long)st.id, dd.format);
		}
		return;
	}
	if (!depth_slot_ensure(l, o, dd.width, dd.height, dd.format)) {
		return;
	}
	HRESULT hr = o.d_lift_km->AcquireSync(0, 100);
	if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
		return;
	}
	D3D11_BOX box = {0, 0, 0, dd.width, dd.height, 1};
	l->lift_context->CopySubresourceRegion(o.d_lift_tex, 0, 0, 0, 0, (ID3D11Resource *)dd.resource, 0, &box);
	o.d_lift_km->ReleaseSync(0);
	l->lift_context->Flush();

	xrt_lift_depth_info &di = o.depth;
	di = xrt_lift_depth_info{};
	di.width = dd.width;
	di.height = dd.height;
	di.format = dd.format;
	di.units = dd.units == XRT_DP_LIFT_DEPTH_METRIC ? XRT_DP_LIFT_DEPTH_METRIC : XRT_DP_LIFT_DEPTH_RELATIVE;
	di.encoding = dd.encoding == XRT_DP_LIFT_DEPTH_ENCODING_INVERSE ? XRT_DP_LIFT_DEPTH_ENCODING_INVERSE
	                                                                : XRT_DP_LIFT_DEPTH_ENCODING_LINEAR;
	di.same_inference = dd.same_inference != 0 ? 1u : 0u;
	di.value_scale = dd.value_scale != 0.0f && std::isfinite(dd.value_scale) ? dd.value_scale : 1.0f;
	di.value_offset = std::isfinite(dd.value_offset) ? dd.value_offset : 0.0f;
	di.near_depth = dd.near_depth > 0.0f ? dd.near_depth : 0.0f;
	di.far_depth = dd.far_depth > 0.0f ? dd.far_depth : 0.0f;
	const bool metric = di.units == XRT_DP_LIFT_DEPTH_METRIC;
	// RELATIVE depth: no intrinsics, no transform — never a faked scale.
	if (metric && dd.focal_x_px > 0.0f && dd.focal_y_px > 0.0f) {
		di.intrinsics_valid = 1;
		di.focal[0] = dd.focal_x_px;
		di.focal[1] = dd.focal_y_px;
		di.principal[0] = dd.principal_x_px;
		di.principal[1] = dd.principal_y_px;
	}
	di.convergence_depth = dd.convergence_depth > 0.0f ? dd.convergence_depth : 0.0f;
	// The depth refers to the source camera = the midpoint of the synthesized
	// viewpoints (the straight-on viewer when none were known).
	if (vr.floats >= 3) {
		memcpy(di.viewpoint, vr.mid, sizeof(di.viewpoint));
	} else {
		di.viewpoint[0] = di.viewpoint[1] = 0.0f;
		di.viewpoint[2] = l->nominal_z;
	}
	di.transform_valid = metric && u_lift_depth_to_display(di.viewpoint, vr.centre, vr.rect_w, vr.rect_h,
	                                                       di.convergence_depth, di.depth_to_display)
	                         ? 1u
	                         : 0u;
	o.has_depth = true;
	o.relief_valid = dd.relief_valid != 0 && std::isfinite(dd.relief_scale) && std::isfinite(dd.relief_offset) &&
	                 dd.relief_scale != 0.0f;
	o.relief_scale = o.relief_valid ? dd.relief_scale : 0.0f;
	o.relief_offset = o.relief_valid ? dd.relief_offset : 0.0f;
	if (!st.depth_logged) {
		st.depth_logged = true;
		U_LOG_W(
		    "[lift] stream %llu auxiliary depth: %ux%u fmt=%u units=%s encoding=%s same_inference=%u "
		    "convergence=%.3f intrinsics=%s transform=%s relief=%s (ADR-048 Addendum A)",
		    (unsigned long long)st.id, dd.width, dd.height, dd.format, metric ? "metric" : "relative",
		    di.encoding == XRT_DP_LIFT_DEPTH_ENCODING_INVERSE ? "inverse" : "linear", di.same_inference,
		    di.convergence_depth, di.intrinsics_valid ? "yes" : "no", di.transform_valid ? "yes" : "no",
		    o.relief_valid ? "yes" : "no");
	}
}

/*
 *
 * Runtime auto-convergence (epic #1858, u_lift_autoconv.h). Lift thread only.
 *
 * The depth of a conversion is copied into a staging texture at most every
 * U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS, mapped (never waited on) before a later
 * conversion, measured, and fed to the stream's filter; every conversion then
 * carries the filter's current value as its convergence.
 *
 */

//! IEEE half -> float (the depth may be R16_FLOAT).
static float
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

//! The policy runs for this conversion: runtime source, a view-synthesis
//! stream, and the app left the convergence AUTO.
static bool
autoconv_active(const d3d11_lift *l, const lift_stream &st, const xrt_dp_lift_params &params)
{
	return l->autoconv == U_LIFT_AUTOCONV_SOURCE_RUNTIME &&
	       (st.info.mode == XRT_DP_LIFT_MODE_SBS || st.info.mode == XRT_DP_LIFT_MODE_NVIEW) &&
	       !(params.convergence >= 0.0f);
}

//! Map the readback in flight if the GPU is done with it, measure, feed the filter.
static void
autoconv_poll(d3d11_lift *l, lift_stream &st, uint64_t now_ns)
{
	if (!st.ac_pending || st.ac_staging == nullptr) {
		return;
	}
	D3D11_MAPPED_SUBRESOURCE m = {};
	HRESULT hr = l->lift_context->Map(st.ac_staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
	if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
		return; // next conversion
	}
	st.ac_pending = false;
	if (FAILED(hr)) {
		return;
	}
	static u_lift_autoconv_grid grid; // ~64 KiB: off the stack; the lift thread is the only user
	bool ok = false;
	if (st.ac_fmt == DXGI_FORMAT_R32_FLOAT && (m.RowPitch % sizeof(float)) == 0) {
		ok = u_lift_autoconv_grid_from_samples((const float *)m.pData, st.ac_w, st.ac_h,
		                                       m.RowPitch / (uint32_t)sizeof(float), st.ac_inverse, st.ac_scale,
		                                       st.ac_offset, U_LIFT_AUTOCONV_GRID_MAX_W, &grid);
	} else if (st.ac_fmt == DXGI_FORMAT_R16_FLOAT) {
		st.ac_scratch.resize((size_t)st.ac_w * st.ac_h);
		for (uint32_t y = 0; y < st.ac_h; y++) {
			const uint16_t *row = (const uint16_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
			for (uint32_t x = 0; x < st.ac_w; x++) {
				st.ac_scratch[(size_t)y * st.ac_w + x] = half_to_float(row[x]);
			}
		}
		ok = u_lift_autoconv_grid_from_samples(st.ac_scratch.data(), st.ac_w, st.ac_h, st.ac_w, st.ac_inverse,
		                                       st.ac_scale, st.ac_offset, U_LIFT_AUTOCONV_GRID_MAX_W, &grid);
	}
	l->lift_context->Unmap(st.ac_staging, 0);

	// The depth covers exactly what was converted, and weave-rect snapshots
	// are letterbox-cropped BEFORE conversion: no bars to exclude here.
	u_lift_autoconv_estimate e = {};
	if (ok) {
		// Prefer the clump this stream already follows (hysteresis).
		u_lift_autoconv_params ep = l->ac_params;
		ep.prefer_h = (st.ac_last.valid && st.ac_last.peak_h >= 0.0f) ? st.ac_last.peak_h : -1.0f;
		u_lift_autoconv_find_subject(&grid, nullptr, &ep, &e);
	}
	st.ac_last = e;
	u_lift_autoconv_filter_measure(&st.ac, &l->ac_fparams, &e, now_ns);
}

//! Start a readback of the depth the module just returned, if one is due.
static void
autoconv_issue(d3d11_lift *l, lift_stream &st, const xrt_dp_lift_depth &dd, uint64_t now_ns)
{
	if (st.ac_pending || (st.ac_issued_ns != 0 && now_ns - st.ac_issued_ns < U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS)) {
		return;
	}
	if (dd.resource == nullptr || dd.width == 0 || dd.height == 0 ||
	    (dd.format != DXGI_FORMAT_R32_FLOAT && dd.format != DXGI_FORMAT_R16_FLOAT)) {
		return;
	}
	if (st.ac_staging == nullptr || st.ac_w != dd.width || st.ac_h != dd.height || st.ac_fmt != dd.format) {
		rel(st.ac_staging);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = dd.width;
		td.Height = dd.height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = (DXGI_FORMAT)dd.format;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(l->lift_device->CreateTexture2D(&td, nullptr, &st.ac_staging))) {
			st.ac_staging = nullptr;
			return;
		}
		st.ac_w = dd.width;
		st.ac_h = dd.height;
		st.ac_fmt = dd.format;
	}
	D3D11_BOX box = {0, 0, 0, dd.width, dd.height, 1};
	l->lift_context->CopySubresourceRegion(st.ac_staging, 0, 0, 0, 0, (ID3D11Resource *)dd.resource, 0, &box);
	l->lift_context->Flush(); // submitted now, mapped (DO_NOT_WAIT) before a later conversion
	st.ac_inverse = dd.encoding == XRT_DP_LIFT_DEPTH_ENCODING_INVERSE;
	st.ac_scale = dd.value_scale;
	st.ac_offset = dd.value_offset;
	st.ac_pending = true;
	st.ac_issued_ns = now_ns;
}

/*!
 * Convert the pending frame of @p st. Called with @p lk HELD; drops it around
 * every GPU / module call and returns with it held.
 */
static void
lift_convert_one(d3d11_lift *l, lift_stream &st, std::unique_lock<std::mutex> &lk)
{
	int32_t in_slot = -1;
	u_lift_frame_meta meta = {};
	if (!u_lift_mailbox_take_pending(&st.mb, os_monotonic_get_ns(), &in_slot, &meta)) {
		return;
	}
	st.converting = true;
	lift_in_slot &in = st.in[in_slot];
	xrt_dp_lift_params params = in.params;
	float vps[3 * XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS];
	memcpy(vps, in.viewpoints, sizeof(vps));
	uint32_t vp_floats = in.viewpoint_floats;
	float active[4];
	memcpy(active, in.active, sizeof(active));
	const xrt_lift_view_control in_vc = in.vc;
	const xrt_lift_rect_frame frame = in.frame;
	const xrt_lift_rig in_rig = in.rig;
	const uint32_t w = meta.width, h = meta.height;
	lk.unlock();

	// ADR-048: rect-relative viewpoints + the stream's viewpoint policy (and,
	// v3, the app rig), and what the DP is told about them (appended
	// xrt_dp_lift_params fields).
	const viewpoint_result vr = resolve_viewpoints(l, st, params.view_count, in_vc, in_rig, frame, vps, vp_floats);
	vp_floats = vr.floats;
	params.struct_size = (uint32_t)sizeof(params);
	params.rect_width_m = vr.rect_w;
	params.rect_height_m = vr.rect_h;
	params.baseline_m = vr.baseline;
	params.axis_mode = vr.axis_mode;
	params.max_offset_m = vr.max_offset;
	params.viewpoint_frame = XRT_DP_LIFT_VIEWPOINT_FRAME_RECT;
	params.viewpoint_source = vr.source;
	params.nominal_z_m = l->nominal_z;
	const bool want_depth = (st.info.aux_outputs & XRT_DP_LIFT_AUX_DEPTH) != 0;

	// Runtime auto-convergence (epic #1858): measure the depth that came back
	// since the last conversion, then hand this one the follower's value as
	// an ordinary caller-set convergence. Until the first measurement — or
	// when the module returns no depth — the app's AUTO goes through and the
	// module's own auto-convergence stays in charge.
	const bool ac_on = autoconv_active(l, st, params);
	if (ac_on) {
		const uint64_t ac_now = os_monotonic_get_ns();
		autoconv_poll(l, st, ac_now);
		float c = -1.0f;
		if (u_lift_autoconv_filter_step(&st.ac, &l->ac_fparams, ac_now, &c)) {
			params.convergence = c;
			st.ac_value = c;
			st.ac_applied++;
			if (!st.ac_locked_logged) {
				st.ac_locked_logged = true;
				U_LOG_W(
				    "[lift] stream %llu: runtime auto-convergence locked at %.3f (subject h=%.3f, "
				    "confidence %.2f, %u cells)",
				    (unsigned long long)st.id, c, st.ac_last.mode_h, st.ac_last.confidence,
				    st.ac_last.samples);
			}
		}
	} else if (st.ac.have) {
		// The app set its own convergence: start over (re-lock) if it hands
		// the choice back.
		u_lift_autoconv_filter_reset(&st.ac);
		st.ac_value = -1.0f;
		st.ac_locked_logged = false;
	}

	bool ok = true;
	// 1. The module's own stream, created lazily (module contract: lift thread only).
	if (!st.dp_created && !st.dp_failed) {
		// Runtime auto-convergence measures the depth of the conversion
		// itself: ask the module for it (if it can return one) even when the
		// app did not. The app-facing depth export still follows only what
		// the APP asked for (st.info). l->caps: written by this thread only.
		xrt_dp_lift_stream_info mi = st.info;
		st.ac_depth = l->autoconv == U_LIFT_AUTOCONV_SOURCE_RUNTIME &&
		              (mi.mode == XRT_DP_LIFT_MODE_SBS || mi.mode == XRT_DP_LIFT_MODE_NVIEW) &&
		              (l->caps.aux_outputs & XRT_DP_LIFT_AUX_DEPTH) != 0 &&
		              (mi.aux_outputs & XRT_DP_LIFT_AUX_DEPTH) == 0;
		if (st.ac_depth) {
			mi.aux_outputs |= XRT_DP_LIFT_AUX_DEPTH;
		}
		if (l->dp->lift_stream_create(l->dp, &mi, &st.dp_id)) {
			st.dp_created = true;
			if (st.ac_depth) {
				U_LOG_W(
				    "[lift] stream %llu: module asked for the depth of each conversion (runtime "
				    "auto-convergence)",
				    (unsigned long long)st.id);
			}
		} else {
			st.dp_failed = true;
			U_LOG_W("[lift] module refused stream %llu (mode=%u hint=%u) — its frames will never convert",
			        (unsigned long long)st.id, st.info.mode, st.info.content_hint);
		}
	}
	ok = st.dp_created;

	// 2. Exact-size RGBA8 copy of the input on the lift device (the module
	//    contract: input is exactly w x h).
	if (ok && (st.exact_tex == nullptr || st.exact_w != w || st.exact_h != h)) {
		rel(st.exact_tex);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ok = SUCCEEDED(l->lift_device->CreateTexture2D(&td, nullptr, &st.exact_tex));
		st.exact_w = ok ? w : 0;
		st.exact_h = ok ? h : 0;
	}
	if (ok) {
		HRESULT hr = in.lift_km->AcquireSync(0, 100);
		ok = SUCCEEDED(hr) && hr != (HRESULT)WAIT_TIMEOUT;
		if (ok) {
			D3D11_BOX box = {0, 0, 0, w, h, 1};
			l->lift_context->CopySubresourceRegion(st.exact_tex, 0, 0, 0, 0, in.lift_tex, 0, &box);
			in.lift_km->ReleaseSync(0);
		}
	}

	// 3. The conversion itself — synchronous, possibly long.
	void *out_res = nullptr;
	uint32_t ow = 0, oh = 0, of = 0;
	const void *blob_bytes = nullptr;
	size_t blob_size = 0;
	uint32_t blob_format = 0;
	const bool is_blob = st.info.mode == XRT_DP_LIFT_MODE_GAUSSIANS;
	if (ok) {
		// Bracketed like the factory and lift_get_caps, so the stall WARN can
		// name a conversion the thread is stuck in (#2003).
		lift_vendor_call(l, is_blob ? "lift_convert_blob" : "lift_convert");
		if (is_blob) {
			ok = xrt_display_processor_d3d11_has_lift_blob(l->dp) &&
			     l->dp->lift_convert_blob(l->dp, st.dp_id, l->lift_context, st.exact_tex, w, h, &params,
			                              &blob_format, &blob_bytes, &blob_size) &&
			     blob_bytes != nullptr && blob_size > 0;
		} else {
			ok = l->dp->lift_convert(l->dp, st.dp_id, l->lift_context, st.exact_tex, w, h, &params,
			                         vp_floats > 0 ? vps : nullptr, vp_floats, &out_res, &ow, &oh, &of) &&
			     out_res != nullptr && ow > 0 && oh > 0;
		}
		lift_vendor_call(l, nullptr);
	}

	// 4. Copy the module's output (valid only until its next call) into the ring.
	std::shared_ptr<std::vector<uint8_t>> blob_copy;
	if (ok && is_blob) {
		blob_copy = std::make_shared<std::vector<uint8_t>>((const uint8_t *)blob_bytes,
		                                                   (const uint8_t *)blob_bytes + blob_size);
	}

	lk.lock();
	u_lift_mailbox_finish_input(&st.mb, in_slot);
	int32_t out_slot = -1;
	if (ok) {
		// Wait while a consumer still pins the only writable slot (one GPU-copy
		// issue long); give up if the stream dies or we are stopping.
		while (!u_lift_mailbox_begin_output(&st.mb, &out_slot)) {
			if (st.dead || l->stop) {
				ok = false;
				break;
			}
			l->cv.wait_for(lk, std::chrono::milliseconds(5));
		}
	}
	lk.unlock();

	if (ok && !is_blob) {
		lift_out_slot &o = st.out[out_slot];
		uint32_t views = 1;
		if (st.info.mode == XRT_DP_LIFT_MODE_SBS) {
			views = 2;
		} else if (st.info.mode == XRT_DP_LIFT_MODE_NVIEW) {
			views = params.view_count >= 1 ? params.view_count : 2;
		}
		ok = out_slot_ensure(l, o, ow, oh, of);
		if (ok) {
			HRESULT hr = o.lift_km->AcquireSync(0, 100);
			ok = SUCCEEDED(hr) && hr != (HRESULT)WAIT_TIMEOUT;
			if (ok) {
				D3D11_BOX box = {0, 0, 0, ow, oh, 1};
				l->lift_context->CopySubresourceRegion(o.lift_tex, 0, 0, 0, 0,
				                                       (ID3D11Resource *)out_res, 0, &box);
				o.lift_km->ReleaseSync(0);
				l->lift_context->Flush();
				o.view_count = views;
				memcpy(o.active, active, sizeof(o.active));
				o.vp_count = vp_floats / 3;
				memcpy(o.vps, vps, sizeof(o.vps));
				memcpy(o.rect_center, vr.centre, sizeof(o.rect_center));
				o.rect_size[0] = vr.rect_w;
				o.rect_size[1] = vr.rect_h;
			}
		}
		// XR_DXR_lift v3: the auxiliary depth of THIS conversion, copied into
		// the same ring slot (the DP's texture is valid only until its next
		// lift_convert on this stream).
		o.has_depth = false;
		o.relief_valid = false;
		const bool ac_want = ok && ac_on && (st.ac_depth || want_depth);
		if (ok && (want_depth || ac_want)) {
			xrt_dp_lift_depth dd;
			const bool got =
			    xrt_display_processor_d3d11_lift_get_depth(l->dp, st.dp_id, l->lift_context, &dd);
			if (want_depth) {
				lift_copy_depth(l, st, o, vr, got ? &dd : nullptr);
			}
			if (ac_want && got) {
				autoconv_issue(l, st, dd, os_monotonic_get_ns());
			} else if (ac_want && !st.ac_nodepth_logged) {
				st.ac_nodepth_logged = true;
				U_LOG_W(
				    "[lift] stream %llu: the module returned no depth — runtime auto-convergence "
				    "cannot measure; the module's own auto-convergence stays in charge",
				    (unsigned long long)st.id);
			}
		}
	} else if (ok && is_blob) {
		lift_out_slot &o = st.out[out_slot];
		o.blob = blob_copy;
		o.blob_format = blob_format;
	}

	lk.lock();
	st.converting = false;
	if (out_slot >= 0) {
		if (ok) {
			u_lift_mailbox_publish_output(&st.mb, out_slot, &meta, os_monotonic_get_ns());
		} else {
			u_lift_mailbox_abort_output(&st.mb, out_slot);
		}
	} else if (!ok) {
		st.mb.failed++;
	}
	// Throttled INFO (never per frame at WARN).
	uint64_t now = os_monotonic_get_ns();
	if (now - st.last_stats_log_ns > 5ull * 1000 * 1000 * 1000) {
		st.last_stats_log_ns = now;
		U_LOG_I(
		    "[lift] stream %llu: submitted=%llu converted=%llu dropped=%llu failed=%llu "
		    "latency last=%.1fms ema=%.1fms min=%.1fms max=%.1fms",
		    (unsigned long long)st.id, (unsigned long long)st.mb.submitted, (unsigned long long)st.mb.converted,
		    (unsigned long long)st.mb.dropped, (unsigned long long)st.mb.failed, st.mb.lat_last_ns / 1e6,
		    st.mb.lat_ema_ns / 1e6, st.mb.lat_min_ns / 1e6, st.mb.lat_max_ns / 1e6);
		if (ac_on) {
			U_LOG_I(
			    "[lift] stream %llu auto-convergence (runtime): convergence=%.3f target=%.3f subject "
			    "h=%.3f (peak %.3f, p%.0f %.3f) "
			    "median h=%.3f confidence=%.2f cells=%u measured=%u held=%u cuts=%u applied=%llu%s",
			    (unsigned long long)st.id, st.ac_value, st.ac.target, st.ac_last.mode_h, st.ac_last.peak_h,
			    (double)l->ac_params.percentile, st.ac_last.pct_h, st.ac_last.median_h,
			    st.ac_last.confidence, st.ac_last.samples, st.ac.measurements, st.ac.held, st.ac.cuts,
			    (unsigned long long)st.ac_applied, st.ac.fast ? " (cut ramp)" : "");
		}
		if (vp_floats >= 3) {
			U_LOG_I(
			    "[lift] stream %llu viewpoints: rect centre=(%.3f, %.3f) m size=%.3fx%.3f m mid=(%.3f, "
			    "%.3f, "
			    "%.3f) baseline=%.4f m axis=%u recenter=%s",
			    (unsigned long long)st.id, vr.centre[0], vr.centre[1], vr.rect_w, vr.rect_h, vr.mid[0],
			    vr.mid[1], vr.mid[2], vr.baseline, vr.axis_mode, recenter_str(vr.recenter_mode));
		}
	}
	l->cv.notify_all();
}

/*
 *
 * The dp-lift backend on the lift thread (#2003). The provider's exec thread
 * queues one command per [S] call (lift_cmd_run) and waits; the lift thread
 * runs it between pre-core conversions. The runtime's images arrive as
 * shared NT handles (glue-allocated on the service device, keyed mutex) and
 * are opened on the lift device once per allocation (lift_open).
 *
 */

static const char *k_blit_ps = R"(
Texture2D src : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	return src.SampleLevel(samp, uv, 0);
}
)";

//! XRT_CONV_FMT_* -> DXGI_FORMAT (UNKNOWN = not representable).
static DXGI_FORMAT
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
static uint32_t
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
static bool
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

static void
img_release(IDXGIKeyedMutex *km, const xrt_conv_image *img)
{
	if (km != nullptr) {
		km->ReleaseSync(img != nullptr && img->sync.kind == XRT_CONV_SYNC_KEYED_MUTEX ? img->sync.release_value
		                                                                              : 0);
	}
}

//! Drop lift-device views of images the glue freed. Lift thread.
static void
lift_drop_evicted(d3d11_lift *l)
{
	std::vector<uint64_t> keys;
	{
		std::lock_guard<std::mutex> g(l->mtx);
		keys.swap(l->lift_evict);
	}
	for (uint64_t k : keys) {
		auto it = l->lift_open.find(k);
		if (it != l->lift_open.end()) {
			rel(it->second.rtv);
			rel(it->second.km);
			rel(it->second.tex);
			l->lift_open.erase(it);
		}
	}
}

//! The lift device's view of a runtime image (opened once per allocation). Lift thread.
static lift_open_img *
lift_open(d3d11_lift *l, const xrt_conv_image *img, bool want_rtv)
{
	if (img == nullptr || img->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || img->handle == 0 ||
	    l->lift_device1 == nullptr) {
		return nullptr;
	}
	auto it = l->lift_open.find(img->import_key);
	if (it == l->lift_open.end()) {
		lift_open_img o;
		HRESULT hr = l->lift_device1->OpenSharedResource1((HANDLE)(uintptr_t)img->handle,
		                                                  __uuidof(ID3D11Texture2D), (void **)&o.tex);
		if (FAILED(hr)) {
			U_LOG_E("[lift] dp-lift: runtime image open on the lift device failed: 0x%08lx",
			        (unsigned long)hr);
			return nullptr;
		}
		(void)o.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&o.km);
		it = l->lift_open.emplace(img->import_key, o).first;
	}
	lift_open_img &o = it->second;
	if (want_rtv && o.rtv == nullptr) {
		D3D11_TEXTURE2D_DESC td = {};
		o.tex->GetDesc(&td);
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = typed_srv_format(td.Format);
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		(void)l->lift_device->CreateRenderTargetView(o.tex, &rd, &o.rtv);
	}
	return &o;
}

static bool
lift_blit_init(d3d11_lift *l)
{
	if (l->blit_tried) {
		return l->blit_ps != nullptr && l->blit_vs != nullptr && l->blit_samp != nullptr;
	}
	l->blit_tried = true;
	ID3DBlob *b = nullptr, *err = nullptr;
	bool ok = SUCCEEDED(D3DCompile(k_snap_vs, strlen(k_snap_vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0,
	                               &b, &err)) &&
	          SUCCEEDED(l->lift_device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr,
	                                                       &l->blit_vs));
	rel(b);
	rel(err);
	ok = ok &&
	     SUCCEEDED(D3DCompile(k_blit_ps, strlen(k_blit_ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b,
	                          &err)) &&
	     SUCCEEDED(
	         l->lift_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &l->blit_ps));
	rel(b);
	rel(err);
	if (ok) {
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		ok = SUCCEEDED(l->lift_device->CreateSamplerState(&sd, &l->blit_samp));
	}
	if (!ok) {
		U_LOG_E(
		    "[lift] dp-lift: output blit init failed — module outputs of another size / format are dropped");
	}
	return ok;
}

/*!
 * Write the module's output @p src (@p sw x @p sh, DXGI @p sfmt) into the
 * runtime target @p img: a copy when the shapes match (the usual case: the
 * provider's output desc predicted them), else a stretch / format-converting
 * draw. Lift thread.
 */
static bool
lift_write_target(
    d3d11_lift *l, const xrt_conv_image *img, ID3D11Resource *src, uint32_t sw, uint32_t sh, uint32_t sfmt)
{
	if (img == nullptr || img->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || src == nullptr) {
		return false; // the glue never hands dp-lift a CPU target
	}
	const DXGI_FORMAT tf = conv_fmt_to_dxgi(img->format);
	const bool same =
	    img->width == sw && img->height == sh && typed_srv_format((DXGI_FORMAT)sfmt) == typed_srv_format(tf);
	lift_open_img *o = lift_open(l, img, !same);
	if (o == nullptr || !img_acquire(o->km, img, 100)) {
		return false;
	}
	ID3D11DeviceContext *ctx = l->lift_context;
	bool ok = true;
	if (same) {
		D3D11_BOX box = {0, 0, 0, sw, sh, 1};
		ctx->CopySubresourceRegion(o->tex, 0, 0, 0, 0, src, 0, &box);
	} else {
		ID3D11ShaderResourceView *srv = nullptr;
		ok = o->rtv != nullptr && lift_blit_init(l);
		if (ok) {
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = typed_srv_format((DXGI_FORMAT)sfmt);
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
			ok = SUCCEEDED(l->lift_device->CreateShaderResourceView(src, &sd, &srv));
		}
		if (ok) {
			D3D11_VIEWPORT vp = {};
			vp.Width = (float)img->width;
			vp.Height = (float)img->height;
			vp.MaxDepth = 1.0f;
			ctx->RSSetViewports(1, &vp);
			ctx->RSSetState(nullptr);
			ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
			ctx->OMSetDepthStencilState(nullptr, 0);
			ctx->OMSetRenderTargets(1, &o->rtv, nullptr);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			ctx->IASetInputLayout(nullptr);
			ctx->VSSetShader(l->blit_vs, nullptr, 0);
			ctx->GSSetShader(nullptr, nullptr, 0);
			ctx->PSSetShader(l->blit_ps, nullptr, 0);
			ctx->PSSetSamplers(0, 1, &l->blit_samp);
			ctx->PSSetShaderResources(0, 1, &srv);
			ctx->Draw(4, 0);
			ID3D11ShaderResourceView *null_srv = nullptr;
			ctx->PSSetShaderResources(0, 1, &null_srv);
			ID3D11RenderTargetView *null_rtv = nullptr;
			ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
		}
		rel(srv);
	}
	img_release(o->km, img);
	return ok;
}

//! One core-routed conversion (LIFT_CMD_CONVERT). Lift thread, no lock held.
static xrt_result_t
lift_core_convert(d3d11_lift *l, uint64_t dp_id, conv_dp_lift_convert *io)
{
	if (l->dp == nullptr || io == nullptr || io->input == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const xrt_conv_image *in = io->input;
	const uint32_t w = in->width, h = in->height;
	lift_core_stream &cs = l->core_streams[dp_id];

	// The module contract: an RGBA8 input of exactly w x h on the lift device,
	// valid for the call. Copied out of the runtime's slot so its keyed mutex
	// is held for one copy only (the pre-core path's exact_tex).
	bool ok = w > 0 && h > 0;
	if (ok && (cs.exact_tex == nullptr || cs.exact_w != w || cs.exact_h != h)) {
		rel(cs.exact_tex);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ok = SUCCEEDED(l->lift_device->CreateTexture2D(&td, nullptr, &cs.exact_tex));
		cs.exact_w = ok ? w : 0;
		cs.exact_h = ok ? h : 0;
	}
	if (ok && in->memory_kind == XRT_CONV_MEM_WIN32_NT_HANDLE) {
		lift_open_img *o = lift_open(l, in, false);
		ok = o != nullptr && img_acquire(o->km, in, 100);
		if (ok) {
			D3D11_BOX box = {0, 0, 0, w, h, 1};
			l->lift_context->CopySubresourceRegion(cs.exact_tex, 0, 0, 0, 0, o->tex, 0, &box);
			img_release(o->km, in);
		}
	} else if (ok && in->memory_kind == XRT_CONV_MEM_CPU && in->cpu_ptr != nullptr) {
		l->lift_context->UpdateSubresource(cs.exact_tex, 0, nullptr, in->cpu_ptr,
		                                   in->row_stride != 0 ? in->row_stride : w * 4u, 0);
	} else {
		ok = false;
	}

	void *out_res = nullptr;
	uint32_t ow = 0, oh = 0, of = 0;
	if (ok) {
		lift_vendor_call(l, "lift_convert");
		ok = l->dp->lift_convert(l->dp, dp_id, l->lift_context, cs.exact_tex, w, h, io->params, io->viewpoints,
		                         io->viewpoint_floats, &out_res, &ow, &oh, &of) &&
		     out_res != nullptr && ow > 0 && oh > 0;
		lift_vendor_call(l, nullptr);
	}
	if (!ok) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // no output this frame: the core keeps the previous one
	}
	io->out_w = ow;
	io->out_h = oh;
	io->out_format = dxgi_to_conv_fmt(of);

	const xrt_conv_image *primary = io->primary_is_depth ? io->depth_target : io->views_target;
	const bool pw = primary != nullptr && lift_write_target(l, primary, (ID3D11Resource *)out_res, ow, oh, of);
	if (io->primary_is_depth) {
		io->depth_written = pw;
	} else {
		io->views_written = pw;
	}
	// The auxiliary depth of THIS conversion (the DP's texture is valid only
	// until its next lift_convert on this stream).
	if (pw && io->want_depth && io->depth_target != nullptr) {
		xrt_dp_lift_depth dd;
		lift_vendor_call(l, "lift_get_depth");
		const bool got = xrt_display_processor_d3d11_lift_get_depth(l->dp, dp_id, l->lift_context, &dd);
		lift_vendor_call(l, nullptr);
		if (got && dd.resource != nullptr &&
		    (dd.format == DXGI_FORMAT_R32_FLOAT || dd.format == DXGI_FORMAT_R16_FLOAT)) {
			io->depth_written = lift_write_target(l, io->depth_target, (ID3D11Resource *)dd.resource,
			                                      dd.width, dd.height, dd.format);
			io->have_depth_meta = io->depth_written;
			io->depth = dd;
			io->depth.resource = nullptr;
		} else if (!cs.depth_logged) {
			cs.depth_logged = true;
			U_LOG_W("[lift] dp-lift stream %llu: depth asked for, the module returned none%s",
			        (unsigned long long)dp_id, got ? " in R32_FLOAT / R16_FLOAT" : "");
		}
	}
	l->lift_context->Flush();
	return pw ? XRT_SUCCESS : XRT_ERROR_OUTPUT_REQUEST_FAILURE;
}

//! Run one backend command. Lift thread, no lock held.
static void
lift_run_cmd(d3d11_lift *l, lift_cmd *c)
{
	lift_drop_evicted(l);
	switch (c->op) {
	case LIFT_CMD_ACTIVATE: {
		bool run;
		{
			std::lock_guard<std::mutex> g(l->mtx);
			l->activate_requested = true;
			// G5: no latch — a retry after "no module" runs the whole attempt
			// again, re-resolving the factory pair.
			run = !l->activated || (c->retry && !l->module_alive);
			if (run) {
				l->activated = false;
			}
		}
		if (run) {
			lift_activate(l);
		}
		c->result = XRT_SUCCESS;
		break;
	}
	case LIFT_CMD_POLL:
		if (l->dp != nullptr) {
			lift_poll_caps(l);
		}
		c->result = XRT_SUCCESS;
		break;
	case LIFT_CMD_STREAM_CREATE: {
		c->result = XRT_ERROR_FEATURE_NOT_SUPPORTED;
		if (l->dp == nullptr || c->info == nullptr) {
			break;
		}
		lift_vendor_call(l, "lift_stream_create");
		const bool ok = l->dp->lift_stream_create(l->dp, c->info, &c->dp_id);
		lift_vendor_call(l, nullptr);
		if (ok) {
			l->core_streams[c->dp_id] = lift_core_stream{};
			c->result = XRT_SUCCESS;
			U_LOG_W("[lift] dp-lift: module stream %llu created (mode=%u hint=%u aux=%u)",
			        (unsigned long long)c->dp_id, c->info->mode, c->info->content_hint,
			        c->info->aux_outputs);
		} else {
			U_LOG_W("[lift] dp-lift: the module refused a stream (mode=%u hint=%u)", c->info->mode,
			        c->info->content_hint);
		}
		break;
	}
	case LIFT_CMD_STREAM_DESTROY: {
		if (l->dp != nullptr) {
			lift_vendor_call(l, "lift_stream_destroy");
			l->dp->lift_stream_destroy(l->dp, c->dp_id);
			lift_vendor_call(l, nullptr);
		}
		auto it = l->core_streams.find(c->dp_id);
		if (it != l->core_streams.end()) {
			rel(it->second.exact_tex);
			l->core_streams.erase(it);
		}
		c->result = XRT_SUCCESS;
		break;
	}
	case LIFT_CMD_CONVERT: c->result = lift_core_convert(l, c->dp_id, c->io); break;
	}
}

static bool
any_dead(d3d11_lift *l)
{
	for (auto &kv : l->streams) {
		if (kv.second->dead) {
			return true;
		}
	}
	return false;
}

static bool
any_work(d3d11_lift *l)
{
	for (auto &kv : l->streams) {
		const lift_stream &st = *kv.second;
		if (st.dead || (st.priority != U_LIFT_PRIORITY_PAUSED && u_lift_mailbox_has_pending(&st.mb))) {
			return true;
		}
	}
	return false;
}

static void
lift_thread_main(d3d11_lift *l)
{
	uint64_t last_poll_ns = 0;
	std::unique_lock<std::mutex> lk(l->mtx);
	for (;;) {
		l->cv.wait_for(lk, std::chrono::milliseconds(250), [&] {
			// Frames only count as work once the module is READY (they wait,
			// latest-wins, until then); dead streams always do (reaping).
			return l->stop || !l->cmds.empty() || (l->activate_requested && !l->activated) ||
			       (l->activated &&
			        (any_dead(l) || (l->caps.state == XRT_DP_LIFT_STATE_READY && any_work(l))));
		});
		if (l->stop) {
			break;
		}
		// The conversion core's dp-lift commands (#2003) first; the pre-core
		// path below is a branch of the same thread.
		if (!l->cmds.empty()) {
			lift_cmd *c = l->cmds.front();
			l->cmds.pop_front();
			lk.unlock();
			lift_run_cmd(l, c);
			lk.lock();
			c->done = true;
			l->cmd_cv.notify_all();
			continue;
		}
		if (l->activate_requested && !l->activated) {
			lk.unlock();
			lift_activate(l);
			lk.lock();
			continue;
		}
		if (!l->activated) {
			continue;
		}

		// A module warming up: poll ≤ 1 Hz until READY.
		uint64_t now = os_monotonic_get_ns();
		lift_check_activating_stall(l, now);
		if (l->dp != nullptr && l->caps.state != XRT_DP_LIFT_STATE_READY &&
		    now - last_poll_ns > 1000000000ull) {
			last_poll_ns = now;
			lk.unlock();
			lift_poll_caps(l);
			lk.lock();
		}

		lift_reap(l, lk);

		if (l->caps.state != XRT_DP_LIFT_STATE_READY || l->dp == nullptr) {
			continue; // frames wait (latest wins) until the module is READY
		}

		// A failed stream's frames can never convert: drain them so the wait
		// predicate does not spin on them.
		for (auto &kv : l->streams) {
			lift_stream &st = *kv.second;
			int32_t slot = -1;
			while (st.dp_failed && u_lift_mailbox_take_pending(&st.mb, now, &slot, nullptr)) {
				u_lift_mailbox_finish_input(&st.mb, slot);
				st.mb.failed++;
			}
		}

		// One scheduling round (XrLiftPriorityDXR): HIGH streams with a new
		// frame, one NORMAL round-robin, LOW every Nth round, PAUSED never.
		std::vector<u_lift_sched_entry> entries;
		entries.reserve(l->streams.size());
		for (auto &kv : l->streams) { // std::map: ascending id, as the planner wants
			const lift_stream &st = *kv.second;
			if (st.dead || st.dp_failed) {
				continue;
			}
			entries.push_back({st.id, st.priority, u_lift_mailbox_has_pending(&st.mb)});
		}
		uint64_t plan_ids[64];
		uint32_t n = u_lift_sched_plan(&l->sched, entries.data(), (uint32_t)entries.size(), plan_ids, 64);
		if (n == 0) {
			if (any_work(l)) {
				// Only not-this-round LOW / dead-but-pinned left: back off briefly.
				l->cv.wait_for(lk, std::chrono::milliseconds(5));
			}
			continue;
		}
		for (uint32_t i = 0; i < n && !l->stop; i++) {
			auto it = l->streams.find(plan_ids[i]); // re-look-up: unlocked between conversions
			if (it == l->streams.end() || it->second->dead) {
				continue;
			}
			lift_convert_one(l, *it->second, lk);
		}
	}

	// Shutdown: queued backend commands fail, every stream (pre-core and
	// core-routed) goes, then the DP, on this thread.
	while (!l->cmds.empty()) {
		lift_cmd *c = l->cmds.front();
		l->cmds.pop_front();
		c->result = XRT_ERROR_FEATURE_NOT_SUPPORTED;
		c->done = true;
	}
	l->module_alive = false;
	l->cmd_cv.notify_all();
	for (auto &kv : l->streams) {
		lift_stream &st = *kv.second;
		if (st.dp_created && l->dp != nullptr) {
			l->dp->lift_stream_destroy(l->dp, st.dp_id);
		}
		stream_release_gpu(st);
	}
	l->streams.clear();
	lk.unlock();
	for (auto &kv : l->core_streams) {
		if (l->dp != nullptr) {
			l->dp->lift_stream_destroy(l->dp, kv.first);
		}
		rel(kv.second.exact_tex);
	}
	l->core_streams.clear();
	{
		std::lock_guard<std::mutex> g(l->mtx);
		l->lift_evict.clear();
	}
	for (auto &kv : l->lift_open) {
		rel(kv.second.rtv);
		rel(kv.second.km);
		rel(kv.second.tex);
	}
	l->lift_open.clear();
	rel(l->blit_samp);
	rel(l->blit_ps);
	rel(l->blit_vs);
	if (l->dp != nullptr) {
		xrt_display_processor_d3d11_destroy(&l->dp);
	}
}


/*
 *
 * Public API.
 *
 */

static lift_stream *
find_live(d3d11_lift *l, uint64_t owner, uint64_t id)
{
	auto it = l->streams.find(id);
	if (it == l->streams.end() || it->second->dead || it->second->owner != owner) {
		return nullptr;
	}
	return it->second.get();
}

// #2003: the glue, the bridge and the core weave branch (defined at the end).
static bool
glue_enter(d3d11_lift *l);
static void
glue_leave(d3d11_lift *l);
static void
glue_close(d3d11_lift *l);
static void
glue_release_all(d3d11_lift *l);
static bool
bridge_resolve(
    d3d11_lift *l, uint64_t owner, uint64_t wire_id, conv_core **out_core, uint64_t *out_core_id, uint32_t *out_mode);
static void
eyes_refresh_if_stale(d3d11_lift *l);
static xrt_result_t
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
static bool
core_pin_latest(d3d11_lift *l, uint64_t owner, uint64_t id, d3d11_lift_pin *out);
static void
core_unpin(d3d11_lift *l, d3d11_lift_pin *pin);

struct d3d11_lift *
d3d11_lift_create(ID3D11Device *svc_device,
                  ID3D11DeviceContext *svc_context,
                  std::mutex *svc_ctx_mutex,
                  d3d11_lift_resolve_factories_fn resolve,
                  void *resolve_ud,
                  float nominal_z)
{
	if (svc_device == nullptr || svc_context == nullptr || svc_ctx_mutex == nullptr) {
		return nullptr;
	}
	auto *l = new d3d11_lift();
	l->svc_device = svc_device;
	l->svc_device->AddRef();
	l->svc_context = svc_context;
	l->svc_context->AddRef();
	l->svc_ctx_mutex = svc_ctx_mutex;
	l->resolve = resolve;
	l->resolve_ud = resolve_ud;
	{
		// The adapter the runtime's images live on (xrt_conv_image::adapter_luid).
		IDXGIDevice *dd = nullptr;
		IDXGIAdapter *ad = nullptr;
		DXGI_ADAPTER_DESC desc = {};
		if (SUCCEEDED(svc_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd)) &&
		    SUCCEEDED(dd->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc))) {
			l->adapter_luid =
			    ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
		}
		rel(ad);
		rel(dd);
	}
	l->nominal_z = nominal_z > 0.0f ? nominal_z : U_LIFT_NOMINAL_Z_DEFAULT_M;
	{
		const char *rc = getenv("DXR_LIFT_RECENTER");
		l->recenter_off = rc != nullptr && (_stricmp(rc, "off") == 0 || strcmp(rc, "0") == 0);
		if (l->recenter_off) {
			U_LOG_W("[lift] DXR_LIFT_RECENTER=%s: viewpoint recentering forced OFF for every stream", rc);
		}
	}
	{
		const char *ac = getenv("DXR_LIFT_AUTO_CONVERGENCE");
		l->autoconv = u_lift_autoconv_source_parse(ac);
		u_lift_autoconv_params_default(&l->ac_params);
		u_lift_autoconv_filter_params_default(&l->ac_fparams);
		if (l->autoconv == U_LIFT_AUTOCONV_SOURCE_RUNTIME) {
			U_LOG_W(
			    "[lift] auto-convergence: RUNTIME policy (DXR_LIFT_AUTO_CONVERGENCE=%s) — centre-weighted "
			    "foreground mode of each stream's conversion depth, sampled every %llu ms, tau %.2f s "
			    "(cut %.2f s); applies where the app leaves convergence AUTO",
			    ac != nullptr ? ac : "(unset)",
			    (unsigned long long)(U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS / 1000000ull), l->ac_fparams.tau_s,
			    l->ac_fparams.tau_cut_s);
		} else {
			U_LOG_W("[lift] auto-convergence: MODULE's own (DXR_LIFT_AUTO_CONVERGENCE=%s)", ac);
		}
	}
	(void)svc_device->QueryInterface(__uuidof(ID3D11Device1), (void **)&l->svc_device1);
	(void)svc_context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&l->svc_context4);

	// Snapshot blit resources (service device).
	ID3DBlob *b = nullptr;
	ID3DBlob *err = nullptr;
	bool ok =
	    SUCCEEDED(D3DCompile(k_snap_vs, strlen(k_snap_vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &b,
	                         &err)) &&
	    SUCCEEDED(svc_device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &l->snap_vs));
	rel(b);
	rel(err);
	ok = ok &&
	     SUCCEEDED(D3DCompile(k_snap_ps, strlen(k_snap_ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b,
	                          &err)) &&
	     SUCCEEDED(svc_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &l->snap_ps));
	rel(b);
	rel(err);
	ok = ok &&
	     SUCCEEDED(D3DCompile(k_snap_ps_scaled, strlen(k_snap_ps_scaled), nullptr, nullptr, nullptr, "main",
	                          "ps_5_0", 0, 0, &b, &err)) &&
	     SUCCEEDED(
	         svc_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &l->snap_ps_scaled));
	rel(b);
	rel(err);
	if (ok) {
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // 1:1 snapshot: exact texels
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		ok = SUCCEEDED(svc_device->CreateSamplerState(&sd, &l->snap_sampler));
		if (ok) {
			sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT; // capped snapshot: box-filter taps
			ok = SUCCEEDED(svc_device->CreateSamplerState(&sd, &l->snap_sampler_linear));
		}
	}

	// Weave-rect snapshot cap, read once in the service process (ADR-042:
	// the runtime owns the policy; the vendor's own input autoscaling only
	// affects inference, not the view synthesis that dominates at 8K).
	l->max_input_edge = u_lift_max_input_edge_parse(getenv("DXR_LIFT_MAX_INPUT_EDGE"));
	if (l->max_input_edge != U_LIFT_MAX_INPUT_EDGE_DEFAULT) {
		U_LOG_W("[lift] DXR_LIFT_MAX_INPUT_EDGE=%u%s", l->max_input_edge,
		        l->max_input_edge == 0 ? " (weave-rect snapshots uncapped)" : "");
	}
	if (ok) {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(snap_cb);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ok = SUCCEEDED(svc_device->CreateBuffer(&bd, nullptr, &l->snap_cb));
	}
	l->snap_ok = ok && l->svc_device1 != nullptr && l->svc_context4 != nullptr;
	if (!l->snap_ok) {
		U_LOG_E("[lift] snapshot pipeline init failed — lift will report UNAVAILABLE");
	}

	// Letterbox crop (optional: a failure here only disables the crop).
	l->letterbox = u_lift_letterbox_parse(getenv("DXR_LIFT_LETTERBOX"));
	if (l->snap_ok && l->letterbox) {
		bool lok = SUCCEEDED(D3DCompile(k_lb_ps, strlen(k_lb_ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0,
		                                0, &b, &err)) &&
		           SUCCEEDED(svc_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr,
		                                                   &l->lb_ps));
		if (err != nullptr) {
			U_LOG_E("[lift] letterbox shader: %s", (const char *)err->GetBufferPointer());
		}
		rel(b);
		rel(err);
		if (lok) {
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = U_LIFT_LETTERBOX_BINS_MAX;
			td.Height = 2;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R32_FLOAT;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_RENDER_TARGET;
			lok = SUCCEEDED(svc_device->CreateTexture2D(&td, nullptr, &l->lb_tex)) &&
			      SUCCEEDED(svc_device->CreateRenderTargetView(l->lb_tex, nullptr, &l->lb_rtv));
		}
		if (lok) {
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = sizeof(lb_cb);
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			lok = SUCCEEDED(svc_device->CreateBuffer(&bd, nullptr, &l->lb_cbuf));
		}
		l->lb_ok = lok;
		if (!lok) {
			U_LOG_E("[lift] letterbox profile init failed — weave-rect snapshots are not cropped");
		}
	} else if (!l->letterbox) {
		U_LOG_W("[lift] DXR_LIFT_LETTERBOX=0 (weave-rect snapshots never cropped to the active area)");
	}

	xrt_dp_lift_caps_init(&l->caps);
	l->caps.state = l->snap_ok ? XRT_DP_LIFT_STATE_ACTIVATING : XRT_DP_LIFT_STATE_UNAVAILABLE;
	if (!l->snap_ok) {
		l->activated = true;
	}
	l->thread = std::thread(lift_thread_main, l);
	return l;
}

void
d3d11_lift_destroy(struct d3d11_lift **lift_ptr)
{
	if (lift_ptr == nullptr || *lift_ptr == nullptr) {
		return;
	}
	d3d11_lift *l = *lift_ptr;
	glue_close(l);
	{
		std::lock_guard<std::mutex> g(l->mtx);
		l->stop = true;
	}
	l->cv.notify_all();
	if (l->thread.joinable()) {
		l->thread.join();
	}
	glue_release_all(l);
	rel(l->lb_cbuf);
	rel(l->lb_rtv);
	rel(l->lb_tex);
	rel(l->lb_ps);
	rel(l->snap_cb);
	rel(l->snap_sampler_linear);
	rel(l->snap_sampler);
	rel(l->snap_ps_scaled);
	rel(l->snap_ps);
	rel(l->snap_vs);
	rel(l->lift_device1);
	rel(l->lift_context);
	rel(l->lift_device);
	rel(l->svc_context4);
	rel(l->svc_device1);
	rel(l->svc_context);
	rel(l->svc_device);
	delete l;
	*lift_ptr = nullptr;
}

void
d3d11_lift_get_caps(struct d3d11_lift *l, struct xrt_dp_lift_caps *out)
{
	xrt_dp_lift_caps_init(out);
	if (l == nullptr) {
		return;
	}
	bool kick = false;
	{
		// Never waits on the lift thread or the module (#1812): the
		// last-known snapshot, whatever state activation is in.
		std::lock_guard<std::mutex> g(l->mtx);
		if (!l->activate_requested) {
			l->activate_requested = true;
			kick = true;
		}
		lift_check_activating_stall(l, os_monotonic_get_ns());
		*out = l->caps;
		out->struct_size = (uint32_t)sizeof(*out);
	}
	if (kick) {
		l->cv.notify_all();
	}
}

xrt_result_t
d3d11_lift_stream_create(struct d3d11_lift *l,
                         uint64_t owner,
                         const struct xrt_dp_lift_stream_info *info,
                         uint64_t *out_id)
{
	if (l == nullptr || info == nullptr || out_id == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const uint32_t m = info->mode;
	if (m != XRT_DP_LIFT_MODE_DEPTH && m != XRT_DP_LIFT_MODE_SBS && m != XRT_DP_LIFT_MODE_NVIEW &&
	    m != XRT_DP_LIFT_MODE_GAUSSIANS) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	std::lock_guard<std::mutex> g(l->mtx);
	l->activate_requested = true;
	if (l->caps.state == XRT_DP_LIFT_STATE_UNAVAILABLE && l->activated) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (l->caps.state == XRT_DP_LIFT_STATE_READY && (l->caps.modes & m) == 0) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	uint32_t max_streams = l->caps.max_streams > 0 ? l->caps.max_streams : 4;
	if (l->live_streams >= max_streams) {
		return XRT_ERROR_CLIENT_LIMIT_REACHED;
	}
	auto st = std::make_unique<lift_stream>();
	st->id = ++l->next_stream_id;
	st->owner = owner;
	st->info = *info;
	st->info.struct_size = (uint32_t)sizeof(st->info);
	if (!(st->info.input_scale > 0.0f) || st->info.input_scale > 1.0f) {
		st->info.input_scale = 1.0f;
	}
	// XR_DXR_lift v3: auxiliary depth rides SBS / NVIEW streams only (a
	// caller's shorter struct carries none).
	if (info->struct_size < offsetof(xrt_dp_lift_stream_info, aux_depth_format) + sizeof(uint32_t) ||
	    (m != XRT_DP_LIFT_MODE_SBS && m != XRT_DP_LIFT_MODE_NVIEW)) {
		st->info.aux_outputs = 0;
		st->info.aux_depth_format = 0;
	}
	st->info.aux_outputs &= XRT_DP_LIFT_AUX_DEPTH;
	u_lift_mailbox_init(&st->mb);
	st->last_params.struct_size = (uint32_t)sizeof(st->last_params);
	st->last_params.convergence = -1.0f;
	st->last_params.strength = 1.0f;
	st->last_params.inpaint = 1;
	st->last_params.view_count = m == XRT_DP_LIFT_MODE_NVIEW ? 4 : 2;
	{
		u_lift_view_control d;
		u_lift_view_control_default(&d);
		st->vc = {d.ipd_factor,    d.parallax_factor, d.axis_mode, d.max_offset_m,
		          d.recenter_mode, d.hold_s,          d.tau_s};
	}
	*out_id = st->id;
	U_LOG_W("[lift] stream %llu created (mode=%u hint=%u scale=%.2f aux=%u, owner=%llu)",
	        (unsigned long long)st->id, m, info->content_hint, st->info.input_scale, st->info.aux_outputs,
	        (unsigned long long)owner);
	l->streams[st->id] = std::move(st);
	l->live_streams++;
	l->cv.notify_all();
	return XRT_SUCCESS;
}

void
d3d11_lift_stream_destroy(struct d3d11_lift *l, uint64_t owner, uint64_t id)
{
	if (l == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> g(l->mtx);
	lift_stream *st = find_live(l, owner, id);
	if (st != nullptr) {
		st->dead = true;
		l->live_streams--;
		U_LOG_W("[lift] stream %llu destroyed", (unsigned long long)id);
		l->cv.notify_all();
	}
}

void
d3d11_lift_release_owner(struct d3d11_lift *l, uint64_t owner)
{
	if (l == nullptr || owner == 0) {
		return;
	}
	std::lock_guard<std::mutex> g(l->mtx);
	uint32_t n = 0;
	for (auto &kv : l->streams) {
		if (kv.second->owner == owner && !kv.second->dead) {
			kv.second->dead = true;
			l->live_streams--;
			n++;
		}
	}
	if (n > 0) {
		U_LOG_W("[lift] client gone: %u stream(s) released", n);
		l->cv.notify_all();
	}
}

uint32_t
d3d11_lift_stream_mode(struct d3d11_lift *l, uint64_t owner, uint64_t id)
{
	if (l == nullptr) {
		return 0;
	}
	if ((id & D3D11_LIFT_CORE_STREAM_TAG) != 0) {
		// #2003: a core-routed stream (weave rects resolve it, never refused).
		uint64_t core_id = 0;
		uint32_t mode = 0;
		return bridge_resolve(l, owner, id, nullptr, &core_id, &mode) ? mode : 0;
	}
	std::lock_guard<std::mutex> g(l->mtx);
	lift_stream *st = find_live(l, owner, id);
	return st != nullptr ? st->info.mode : 0;
}

/*!
 * Snapshot @p src region (@p x, @p y, @p w, @p h) into the top-left
 * @p dst_w x @p dst_h of input slot @p s's service texture — 1:1 when the dims
 * match, box-filtered down when the snapshot cap shrank them. Caller holds the
 * service context mutex; takes the slot's keyed mutex itself.
 */
//! A snapshot destination: the pre-core input slot, or a core slot image (#2003).
struct blit_dst
{
	IDXGIKeyedMutex *svc_km;
	ID3D11RenderTargetView *svc_rtv;
};

template <typename SLOT>
static bool
snapshot_blit(d3d11_lift *l,
              SLOT &s,
              ID3D11ShaderResourceView *src,
              uint32_t src_tw,
              uint32_t src_th,
              uint32_t x,
              uint32_t y,
              uint32_t w,
              uint32_t h,
              uint32_t dst_w,
              uint32_t dst_h)
{
	const bool scaled = dst_w != w || dst_h != h;
	HRESULT hr = s.svc_km->AcquireSync(0, 4);
	if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
		return false;
	}
	ID3D11DeviceContext *ctx = l->svc_context;
	snap_cb cb = {};
	cb.src_rect[0] = (float)x;
	cb.src_rect[1] = (float)y;
	cb.src_rect[2] = (float)w;
	cb.src_rect[3] = (float)h;
	cb.src_size[0] = (float)src_tw;
	cb.src_size[1] = (float)src_th;
	cb.dst_size[0] = (float)dst_w;
	cb.dst_size[1] = (float)dst_h;
	ctx->UpdateSubresource(l->snap_cb, 0, nullptr, &cb, 0, 0);

	// State this sequence completely: the context is shared (immediate_ctx_mutex
	// serializes SEQUENCES, and every sequence states its own state). The fixed-
	// function states are restored afterwards, because this runs in the MIDDLE
	// of a weave sequence whose later draws may rely on what they set earlier.
	ID3D11RasterizerState *prev_rs = nullptr;
	ID3D11BlendState *prev_bs = nullptr;
	float prev_bf[4] = {0, 0, 0, 0};
	UINT prev_mask = 0xffffffff;
	ID3D11DepthStencilState *prev_ds = nullptr;
	UINT prev_ref = 0;
	ctx->RSGetState(&prev_rs);
	ctx->OMGetBlendState(&prev_bs, prev_bf, &prev_mask);
	ctx->OMGetDepthStencilState(&prev_ds, &prev_ref);

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)dst_w;
	vp.Height = (float)dst_h;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	D3D11_RECT sc = {0, 0, (LONG)dst_w, (LONG)dst_h};
	ctx->RSSetScissorRects(1, &sc);
	ctx->RSSetState(nullptr);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(nullptr, 0);
	ctx->OMSetRenderTargets(1, &s.svc_rtv, nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetInputLayout(nullptr);
	ctx->VSSetShader(l->snap_vs, nullptr, 0);
	ctx->GSSetShader(nullptr, nullptr, 0);
	ctx->PSSetShader(scaled ? l->snap_ps_scaled : l->snap_ps, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &l->snap_cb);
	ctx->PSSetSamplers(0, 1, scaled ? &l->snap_sampler_linear : &l->snap_sampler);
	ctx->PSSetShaderResources(0, 1, &src);
	ctx->Draw(4, 0);
	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);
	ID3D11RenderTargetView *null_rtv = nullptr;
	ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
	ctx->RSSetState(prev_rs);
	ctx->OMSetBlendState(prev_bs, prev_bf, prev_mask);
	ctx->OMSetDepthStencilState(prev_ds, prev_ref);
	rel(prev_rs);
	rel(prev_bs);
	rel(prev_ds);

	s.svc_km->ReleaseSync(0);
	return true;
}

/*!
 * Letterbox: collect finished profile readbacks (never waits) into the stream's
 * crop state, then measure this frame's rect into the next free readback. The
 * crop therefore lags the picture by a few frames, which the settle hysteresis
 * dwarfs anyway. Producer thread (service context mutex held); @p st is not
 * reaped meanwhile (its input slot is WRITING).
 */
template <typename STATE>
static void
letterbox_step(d3d11_lift *l, STATE &st, ID3D11ShaderResourceView *src, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	ID3D11DeviceContext *ctx = l->svc_context;

	// 1. Oldest-first collection, stop at the first still in flight.
	for (uint32_t k = 0; k < LB_RING; k++) {
		lift_lb_readback &r = st.lb_rb[(st.lb_next + k) % LB_RING];
		if (!r.pending) {
			continue;
		}
		D3D11_MAPPED_SUBRESOURCE ms = {};
		HRESULT hr = ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
			break;
		}
		r.pending = false;
		if (FAILED(hr)) {
			continue;
		}
		const float *rows = (const float *)ms.pData;
		const float *cols = (const float *)((const uint8_t *)ms.pData + ms.RowPitch);
		const bool changed = u_lift_letterbox_update(&st.lb, r.w, r.h, rows, r.nr, cols, r.nc);
		// The row profile that decided it, top to bottom in 64 bands, each the
		// band's max non-black fraction as a digit 0-9 — so a surprising crop
		// change in the field says what was in the bars (captions, player UI).
		char prof[65] = {0};
		if (changed) {
			for (uint32_t k = 0; k < 64; k++) {
				const uint32_t b0 = k * r.nr / 64, b1 = (k + 1) * r.nr / 64 > b0 ? (k + 1) * r.nr / 64 : b0 + 1;
				float mx = 0.0f;
				for (uint32_t i = b0; i < b1 && i < r.nr; i++) {
					mx = rows[i] > mx ? rows[i] : mx;
				}
				const int d = (int)(mx * 10.0f);
				prof[k] = (char)('0' + (d > 9 ? 9 : (d < 0 ? 0 : d)));
			}
		}
		ctx->Unmap(r.staging, 0);
		if (changed) {
			const u_lift_crop &c = st.lb.committed;
			if (u_lift_crop_active(&c)) {
				U_LOG_W("[lift] stream %llu: letterbox crop %ux%u -> active %ux%u (bars top %u bottom %u left %u "
				        "right %u; bars woven flat) rows[%s]",
				        (unsigned long long)st.id, r.w, r.h, r.w - c.left - c.right, r.h - c.top - c.bottom,
				        c.top, c.bottom, c.left, c.right, prof);
			} else {
				U_LOG_W("[lift] stream %llu: letterbox crop off (%ux%u lifted whole) rows[%s]",
				        (unsigned long long)st.id, r.w, r.h, prof);
			}
		}
	}

	// 2. Measure this frame into the next ring entry, if it is free.
	lift_lb_readback &r = st.lb_rb[st.lb_next];
	if (r.pending) {
		return; // GPU behind by LB_RING readbacks: skip a measurement, never stall
	}
	if (r.staging == nullptr) {
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = U_LIFT_LETTERBOX_BINS_MAX;
		td.Height = 2;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(l->svc_device->CreateTexture2D(&td, nullptr, &r.staging))) {
			return;
		}
	}
	r.w = w;
	r.h = h;
	r.nr = h < U_LIFT_LETTERBOX_BINS_MAX ? h : U_LIFT_LETTERBOX_BINS_MAX;
	r.nc = w < U_LIFT_LETTERBOX_BINS_MAX ? w : U_LIFT_LETTERBOX_BINS_MAX;

	lb_cb cb = {};
	cb.src_rect[0] = (float)x;
	cb.src_rect[1] = (float)y;
	cb.src_rect[2] = (float)w;
	cb.src_rect[3] = (float)h;
	cb.bins[0] = (float)r.nr;
	cb.bins[1] = (float)r.nc;
	ctx->UpdateSubresource(l->lb_cbuf, 0, nullptr, &cb, 0, 0);

	// Same state discipline as snapshot_blit: this runs mid weave sequence.
	ID3D11RasterizerState *prev_rs = nullptr;
	ID3D11BlendState *prev_bs = nullptr;
	float prev_bf[4] = {0, 0, 0, 0};
	UINT prev_mask = 0xffffffff;
	ID3D11DepthStencilState *prev_ds = nullptr;
	UINT prev_ref = 0;
	ctx->RSGetState(&prev_rs);
	ctx->OMGetBlendState(&prev_bs, prev_bf, &prev_mask);
	ctx->OMGetDepthStencilState(&prev_ds, &prev_ref);

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)U_LIFT_LETTERBOX_BINS_MAX;
	vp.Height = 2.0f;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	D3D11_RECT sc = {0, 0, (LONG)U_LIFT_LETTERBOX_BINS_MAX, 2};
	ctx->RSSetScissorRects(1, &sc);
	ctx->RSSetState(nullptr);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(nullptr, 0);
	ctx->OMSetRenderTargets(1, &l->lb_rtv, nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetInputLayout(nullptr);
	ctx->VSSetShader(l->snap_vs, nullptr, 0);
	ctx->GSSetShader(nullptr, nullptr, 0);
	ctx->PSSetShader(l->lb_ps, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &l->lb_cbuf);
	ctx->PSSetShaderResources(0, 1, &src);
	ctx->Draw(4, 0);
	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);
	ID3D11RenderTargetView *null_rtv = nullptr;
	ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
	ctx->RSSetState(prev_rs);
	ctx->OMSetBlendState(prev_bs, prev_bf, prev_mask);
	ctx->OMSetDepthStencilState(prev_ds, prev_ref);
	rel(prev_rs);
	rel(prev_bs);
	rel(prev_ds);

	ctx->CopyResource(r.staging, l->lb_tex);
	r.pending = true;
	st.lb_next = (st.lb_next + 1) % LB_RING;
}

//! Producer common path. Caller holds the service context mutex.
static xrt_result_t
submit_locked(d3d11_lift *l,
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
              uint32_t max_input_edge,
              bool letterbox,
              uint64_t *out_frame_id)
{
	if (w == 0 || h == 0 || x + w > src_tw || y + h > src_th) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream / bad extent: non-fatal
	}
	letterbox = letterbox && l->lb_ok;
	// Service policy (ADR-042), in this order: letterbox crop to the active
	// area, then the long-edge cap. The module synthesizes at INPUT resolution
	// and the result is stretched back into the (active part of the) rect, so
	// neither costs much on the panel and both save a lot in the module.
	uint32_t cx = x, cy = y, cw = w, ch = h;
	uint32_t dw = w, dh = h;
	bool capped = false;
	int32_t slot = -1;
	lift_stream *st = nullptr;
	{
		std::lock_guard<std::mutex> g(l->mtx);
		st = find_live(l, owner, id);
		if (st == nullptr) {
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream / bad extent: non-fatal
		}
		if (letterbox && st->lb.w == w && st->lb.h == h && u_lift_crop_active(&st->lb.committed)) {
			const u_lift_crop &c = st->lb.committed;
			cx = x + c.left;
			cy = y + c.top;
			cw = w - c.left - c.right;
			ch = h - c.top - c.bottom;
		}
		dw = cw;
		dh = ch;
		capped = u_lift_cap_dims(cw, ch, max_input_edge, &dw, &dh);
		if (!l->activated || l->lift_device1 == nullptr) {
			// Not up yet: there is no lift device to share the slot with. The
			// frame is simply not taken (the caller submits again next frame).
			l->activate_requested = true;
			l->cv.notify_all();
			*out_frame_id = 0;
			return XRT_SUCCESS;
		}
		if (!u_lift_mailbox_begin_submit(&st->mb, &slot)) {
			return XRT_ERROR_WEAVE_REFUSED;
		}
		// Stream is not reaped while a slot is WRITING (not converting, but the
		// reaper only runs for dead streams, and a dead stream is not found above).
		if (params != nullptr) {
			st->last_params = *params;
			st->last_params.struct_size = (uint32_t)sizeof(st->last_params);
			if (st->last_params.convergence > 1.0f) {
				st->last_params.convergence = 1.0f; // [0,1]; negative = AUTO passes through
			}
			if (!(st->last_params.strength >= 0.0f)) {
				st->last_params.strength = 1.0f;
			}
			if (st->last_params.view_count == 0) {
				st->last_params.view_count = st->info.mode == XRT_DP_LIFT_MODE_NVIEW ? 4 : 2;
			}
			uint32_t n = viewpoint_floats;
			if (n > 3 * XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS) {
				n = 3 * XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS;
			}
			st->last_viewpoint_floats = viewpoints != nullptr ? n - n % 3 : 0;
			if (st->last_viewpoint_floats > 0) {
				memcpy(st->last_viewpoints, viewpoints, st->last_viewpoint_floats * sizeof(float));
			}
		}
		if (view_control != nullptr) {
			st->vc = *view_control; // sticky (sanitized on the lift thread)
		}
		if (params != nullptr || rig != nullptr) {
			// XR_DXR_lift v3: the rig rides with the options, like explicit
			// viewpoints — options without a rig clear it.
			st->rig = rig != nullptr ? *rig : xrt_lift_rig{};
		}
		lift_in_slot &in = st->in[slot];
		in.params = st->last_params;
		in.vc = st->vc;
		in.rig = st->rig;
		in.frame = frame != nullptr ? *frame : xrt_lift_rect_frame{};
		if (capped && in.params.focal_px > 0.0f) {
			in.params.focal_px *= (float)dw / (float)cw; // focal is in INPUT pixels (a crop keeps the scale)
		}
		in.viewpoint_floats = st->last_viewpoint_floats;
		memcpy(in.viewpoints, st->last_viewpoints, sizeof(in.viewpoints));
		in.active[0] = (float)(cx - x) / (float)w;
		in.active[1] = (float)(cy - y) / (float)h;
		in.active[2] = (float)(cx - x + cw) / (float)w;
		in.active[3] = (float)(cy - y + ch) / (float)h;
	}

	// Allocation + blit outside mtx: this slot is WRITING, nobody else touches it.
	lift_in_slot &in = st->in[slot];
	bool ok = in_slot_ensure(l, in, dw, dh) && snapshot_blit(l, in, src, src_tw, src_th, cx, cy, cw, ch, dw, dh);
	if (ok && letterbox) {
		letterbox_step(l, *st, src, x, y, w, h); // measures the WHOLE rect, bars included
	}

	std::lock_guard<std::mutex> g(l->mtx);
	if (!ok) {
		u_lift_mailbox_abort_submit(&st->mb, slot);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	// WARN once per stream when the cap engages or its capped size changes
	// (a rect resize), never per frame.
	if (capped && (st->cap_logged_w != dw || st->cap_logged_h != dh)) {
		st->cap_logged_w = dw;
		st->cap_logged_h = dh;
		U_LOG_W("[lift] stream %llu: snapshot %ux%u capped to %ux%u (DXR_LIFT_MAX_INPUT_EDGE=%u)",
		        (unsigned long long)id, cw, ch, dw, dh, max_input_edge);
	}
	// The mailbox carries the REAL input dims: the lift thread copies exactly
	// dw x dh out of the slot and hands the module that size.
	*out_frame_id = u_lift_mailbox_commit_submit(&st->mb, slot, source_time, os_monotonic_get_ns(), dw, dh);
	l->cv.notify_all();
	return XRT_SUCCESS;
}

xrt_result_t
d3d11_lift_submit_srv_locked(struct d3d11_lift *l,
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
                             const struct xrt_dp_lift_params *params,
                             const float *viewpoints,
                             uint32_t viewpoint_floats,
                             const struct xrt_lift_view_control *view_control,
                             const struct xrt_lift_rig *rig,
                             const struct xrt_lift_rect_frame *frame,
                             uint64_t *out_frame_id)
{
	if (l == nullptr || !l->snap_ok || src == nullptr || out_frame_id == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if ((id & D3D11_LIFT_CORE_STREAM_TAG) != 0) {
		// #2003: a core-routed stream — the same snapshot policy into a core slot.
		return core_weave_submit_locked(l, owner, id, src, src_tw, src_th, x, y, w, h, source_time, params,
		                                viewpoints, viewpoint_floats, view_control, rig, frame, out_frame_id);
	}
	// Weave-rect snapshot: the service's size cap applies. XR_DXR_lift v3: a
	// lifted rect may carry EXPLICIT viewpoints and an app rig.
	return submit_locked(l, owner, id, src, src_tw, src_th, x, y, w, h, source_time, params, viewpoints,
	                     viewpoint_floats, view_control, rig, frame, l->max_input_edge, l->letterbox, out_frame_id);
}

xrt_result_t
d3d11_lift_submit_handle(struct d3d11_lift *l,
                         uint64_t owner,
                         uint64_t id,
                         HANDLE handle,
                         bool is_dxgi,
                         uint32_t w,
                         uint32_t h,
                         int64_t source_time,
                         const struct xrt_dp_lift_params *params,
                         const float *viewpoints,
                         uint32_t viewpoint_floats,
                         const struct xrt_lift_view_control *view_control,
                         const struct xrt_lift_rig *rig,
                         const struct xrt_lift_rect_frame *frame,
                         uint64_t *out_frame_id)
{
	if (out_frame_id != nullptr) {
		*out_frame_id = 0;
	}
	if (l == nullptr || !l->snap_ok || handle == nullptr || out_frame_id == nullptr) {
		if (handle != nullptr && !is_dxgi) {
			CloseHandle(handle);
		}
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	lift_stream *st = nullptr;
	{
		std::lock_guard<std::mutex> g(l->mtx);
		st = find_live(l, owner, id);
	}
	if (st == nullptr) {
		if (!is_dxgi) {
			CloseHandle(handle);
		}
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream / bad extent: non-fatal
	}
	eyes_refresh_if_stale(l);

	// Import cache, per stream (producer thread only — one IPC thread per
	// connection). A duplicated NT handle is a new value every submit, so the
	// cache compares kernel objects; a legacy DXGI handle is a stable global.
	bool reuse = false;
	if (st->imp_tex != nullptr && st->imp_dxgi == is_dxgi) {
		reuse = is_dxgi ? (st->imp_handle == handle) : same_kernel_object(st->imp_handle, handle);
	}
	if (reuse) {
		if (!is_dxgi) {
			CloseHandle(handle);
		}
	} else {
		rel(st->imp_km);
		rel(st->imp_srv);
		rel(st->imp_tex);
		if (!st->imp_dxgi) {
			close_handle(st->imp_handle);
		}
		st->imp_handle = nullptr;
		HRESULT hr;
		if (is_dxgi) {
			hr =
			    l->svc_device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&st->imp_tex);
		} else {
			hr = l->svc_device1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D),
			                                         (void **)&st->imp_tex);
		}
		D3D11_TEXTURE2D_DESC d = {};
		if (SUCCEEDED(hr)) {
			st->imp_tex->GetDesc(&d);
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = typed_srv_format(d.Format);
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
			hr = l->svc_device->CreateShaderResourceView(st->imp_tex, &sd, &st->imp_srv);
		}
		if (FAILED(hr)) {
			U_LOG_E("[lift] input OpenSharedResource(%s) failed: 0x%08lx", is_dxgi ? "DXGI" : "NT",
			        (unsigned long)hr);
			rel(st->imp_srv);
			rel(st->imp_tex);
			if (!is_dxgi) {
				CloseHandle(handle);
			}
			return XRT_ERROR_WEAVE_REFUSED;
		}
		(void)st->imp_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&st->imp_km);
		st->imp_handle = handle; // kept open (NT) for the identity compare
		st->imp_dxgi = is_dxgi;
		st->imp_w = d.Width;
		st->imp_h = d.Height;
		U_LOG_I("[lift] stream %llu input import cached (%s, %ux%u)", (unsigned long long)id,
		        is_dxgi ? "DXGI" : "NT", d.Width, d.Height);
	}

	// The caller's keyed mutex: 4 ms, the budget every weave-side acquire uses.
	bool acquired = false;
	if (st->imp_km != nullptr) {
		HRESULT hr = st->imp_km->AcquireSync(0, 4);
		if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
			return XRT_ERROR_WEAVE_REFUSED;
		}
		acquired = true;
	}
	xrt_result_t xret;
	{
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		// An app's explicit frame: its size and content are the app's choice —
		// never capped, never letterbox-cropped.
		xret = submit_locked(l, owner, id, st->imp_srv, st->imp_w, st->imp_h, 0, 0, w, h, source_time, params,
		                     viewpoints, viewpoint_floats, view_control, rig, frame, /*max_input_edge*/ 0,
		                     /*letterbox*/ false, out_frame_id);
	}
	if (acquired) {
		st->imp_km->ReleaseSync(0);
	}
	return xret;
}

bool
d3d11_lift_pin_latest(struct d3d11_lift *l, uint64_t owner, uint64_t id, struct d3d11_lift_pin *out)
{
	*out = d3d11_lift_pin{};
	if (l == nullptr) {
		return false;
	}
	if ((id & D3D11_LIFT_CORE_STREAM_TAG) != 0) {
		return core_pin_latest(l, owner, id, out);
	}
	int32_t slot = -1;
	lift_out_slot *o = nullptr;
	{
		std::lock_guard<std::mutex> g(l->mtx);
		lift_stream *st = find_live(l, owner, id);
		if (st == nullptr || st->info.mode == XRT_DP_LIFT_MODE_GAUSSIANS ||
		    !u_lift_mailbox_pin_latest(&st->mb, false, &slot, nullptr)) {
			return false;
		}
		o = &st->out[slot];
		if (o->svc_srv == nullptr || o->svc_km == nullptr) {
			u_lift_mailbox_unpin(&st->mb, slot);
			return false;
		}
	}
	// Pinned: the lift thread will not write this slot, so its mutex is free.
	HRESULT hr = o->svc_km->AcquireSync(0, 4);
	if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
		std::lock_guard<std::mutex> g(l->mtx);
		lift_stream *st = find_live(l, owner, id);
		if (st != nullptr) {
			u_lift_mailbox_unpin(&st->mb, slot);
		}
		return false;
	}
	out->lift = l;
	out->stream_id = id;
	out->slot = slot;
	out->km = o->svc_km;
	out->srv = o->svc_srv;
	out->width = o->w;
	out->height = o->h;
	out->view_count = o->view_count;
	memcpy(out->active, o->active, sizeof(out->active));
	out->vp_count = o->vp_count <= XRT_LIFT_MAX_VIEWS ? o->vp_count : XRT_LIFT_MAX_VIEWS;
	memcpy(out->vps, o->vps, sizeof(float) * 3 * out->vp_count);
	memcpy(out->rect_center, o->rect_center, sizeof(out->rect_center));
	memcpy(out->rect_size, o->rect_size, sizeof(out->rect_size));
	if (o->has_depth && o->d_svc_tex != nullptr && o->d_svc_km != nullptr) {
		out->depth_tex = o->d_svc_tex;
		out->depth_km = o->d_svc_km;
		out->depth_w = o->d_w;
		out->depth_h = o->d_h;
		out->depth_format = o->d_format;
		out->depth_encoding = o->depth.encoding;
		out->depth_value_scale = o->depth.value_scale;
		out->depth_value_offset = o->depth.value_offset;
		out->relief_valid = o->relief_valid;
		out->relief_scale = o->relief_scale;
		out->relief_offset = o->relief_offset;
	}
	out->valid = true;
	return true;
}

bool
d3d11_lift_pin_depth(struct d3d11_lift_pin *pin)
{
	if (pin == nullptr || !pin->valid || pin->depth_km == nullptr) {
		return false;
	}
	if (pin->depth_held) {
		return true;
	}
	// The slot is pinned, so the lift thread will not write it; a busy mutex
	// means the acquire export is copying it out right now. Skip this frame.
	HRESULT hr = pin->depth_km->AcquireSync(0, 0);
	if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
		return false;
	}
	pin->depth_held = true;
	return true;
}

void
d3d11_lift_unpin(struct d3d11_lift_pin *pin)
{
	if (pin == nullptr || !pin->valid || pin->lift == nullptr) {
		return;
	}
	if (pin->depth_held) {
		pin->depth_km->ReleaseSync(0);
	}
	pin->km->ReleaseSync(0);
	d3d11_lift *l = pin->lift;
	if (pin->core) {
		core_unpin(l, pin);
		*pin = d3d11_lift_pin{};
		return;
	}
	{
		std::lock_guard<std::mutex> g(l->mtx);
		auto it = l->streams.find(pin->stream_id); // dead streams stay mapped until unpinned
		if (it != l->streams.end()) {
			u_lift_mailbox_unpin(&it->second->mb, pin->slot);
		}
	}
	l->cv.notify_all();
	*pin = d3d11_lift_pin{};
}

xrt_result_t
d3d11_lift_acquire_result(
    struct d3d11_lift *l, uint64_t owner, uint64_t id, bool *out_ready, struct d3d11_lift_result_info *out)
{
	*out_ready = false;
	*out = d3d11_lift_result_info{};
	if (l == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	int32_t slot = -1;
	u_lift_frame_meta meta = {};
	lift_stream *st = nullptr;
	lift_out_slot *o = nullptr;
	{
		std::lock_guard<std::mutex> g(l->mtx);
		st = find_live(l, owner, id);
		if (st == nullptr) {
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream / bad extent: non-fatal
		}
		if (st->info.mode == XRT_DP_LIFT_MODE_GAUSSIANS) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		if (!u_lift_mailbox_pin_latest(&st->mb, true, &slot, &meta)) {
			return XRT_SUCCESS; // nothing newer: NOT READY
		}
		o = &st->out[slot];
	}

	auto unpin = [&]() {
		std::lock_guard<std::mutex> g(l->mtx);
		auto it = l->streams.find(id);
		if (it != l->streams.end()) {
			u_lift_mailbox_unpin(&it->second->mb, slot);
		}
		l->cv.notify_all();
	};

	// Export texture: sized to the result, re-created on size / format change.
	bool realloc = false;
	if (st->exp_tex == nullptr || st->exp_w != o->w || st->exp_h != o->h || st->exp_format != o->format) {
		rel(st->exp_tex);
		close_handle(st->exp_handle);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = o->w;
		td.Height = o->h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = (DXGI_FORMAT)o->format;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
		HRESULT hr = l->svc_device->CreateTexture2D(&td, nullptr, &st->exp_tex);
		IDXGIResource1 *r1 = nullptr;
		if (SUCCEEDED(hr)) {
			hr = st->exp_tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
		}
		if (SUCCEEDED(hr)) {
			hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
			                            nullptr, &st->exp_handle);
		}
		rel(r1);
		if (SUCCEEDED(hr) && st->exp_fence == nullptr) {
			ID3D11Device5 *d5 = nullptr;
			hr = l->svc_device->QueryInterface(__uuidof(ID3D11Device5), (void **)&d5);
			if (SUCCEEDED(hr)) {
				hr = d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
				                     (void **)&st->exp_fence);
			}
			rel(d5);
			if (SUCCEEDED(hr)) {
				hr = st->exp_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
				                                       &st->exp_fence_handle);
			}
		}
		if (FAILED(hr)) {
			U_LOG_E("[lift] export texture %ux%u fmt=%u create failed: 0x%08lx", o->w, o->h, o->format,
			        (unsigned long)hr);
			rel(st->exp_tex);
			close_handle(st->exp_handle);
			unpin();
			return XRT_ERROR_WEAVE_REFUSED;
		}
		st->exp_w = o->w;
		st->exp_h = o->h;
		st->exp_format = o->format;
		realloc = true;
		U_LOG_W("[lift] stream %llu export texture %ux%u fmt=%u + fence ready", (unsigned long long)id, o->w,
		        o->h, o->format);
	}

	// XR_DXR_lift v3: the auxiliary depth export texture, the same pattern
	// (sized to the result's depth, re-created on change). A failure here only
	// costs this result its depth, never the views.
	bool depth_realloc = false;
	bool depth_ok = false;
	if (o->has_depth && o->d_svc_tex != nullptr) {
		depth_ok = true;
		if (st->exp_depth_tex == nullptr || st->exp_depth_w != o->d_w || st->exp_depth_h != o->d_h ||
		    st->exp_depth_format != o->d_format) {
			rel(st->exp_depth_tex);
			close_handle(st->exp_depth_handle);
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = o->d_w;
			td.Height = o->d_h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = (DXGI_FORMAT)o->d_format;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
			HRESULT dhr = l->svc_device->CreateTexture2D(&td, nullptr, &st->exp_depth_tex);
			IDXGIResource1 *r1 = nullptr;
			if (SUCCEEDED(dhr)) {
				dhr = st->exp_depth_tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
			}
			if (SUCCEEDED(dhr)) {
				dhr = r1->CreateSharedHandle(nullptr,
				                             DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
				                             nullptr, &st->exp_depth_handle);
			}
			rel(r1);
			if (FAILED(dhr)) {
				U_LOG_E("[lift] depth export texture %ux%u fmt=%u create failed: 0x%08lx", o->d_w,
				        o->d_h, o->d_format, (unsigned long)dhr);
				rel(st->exp_depth_tex);
				close_handle(st->exp_depth_handle);
				st->exp_depth_w = st->exp_depth_h = st->exp_depth_format = 0;
				depth_ok = false;
			} else {
				st->exp_depth_w = o->d_w;
				st->exp_depth_h = o->d_h;
				st->exp_depth_format = o->d_format;
				depth_realloc = true;
				U_LOG_W("[lift] stream %llu depth export texture %ux%u fmt=%u ready",
				        (unsigned long long)id, o->d_w, o->d_h, o->d_format);
			}
		}
	}

	HRESULT hr = o->svc_km->AcquireSync(0, 4);
	if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
		unpin();
		return XRT_ERROR_WEAVE_REFUSED;
	}
	if (depth_ok) {
		HRESULT dhr = o->d_svc_km->AcquireSync(0, 4);
		depth_ok = SUCCEEDED(dhr) && dhr != (HRESULT)WAIT_TIMEOUT;
	}
	{
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		l->svc_context->CopyResource(st->exp_tex, o->svc_tex);
		if (depth_ok) {
			l->svc_context->CopyResource(st->exp_depth_tex, o->d_svc_tex);
		}
		// One fence covers the views and the depth (XrLiftDepthResultDXR).
		st->exp_fence_value++;
		l->svc_context4->Signal(st->exp_fence, st->exp_fence_value);
		l->svc_context->Flush();
	}
	o->svc_km->ReleaseSync(0);
	if (depth_ok) {
		o->d_svc_km->ReleaseSync(0);
	}

	out->frame_id = meta.frame_id;
	out->source_time = meta.source_time;
	out->fence_value = st->exp_fence_value;
	out->latency_ns = meta.done_ns >= meta.submit_ns ? meta.done_ns - meta.submit_ns : 0;
	out->width = o->w;
	out->height = o->h;
	out->format = o->format;
	out->view_count = o->view_count;
	out->output_realloc = realloc;
	out->viewpoint_count = o->vp_count > XRT_LIFT_MAX_VIEWS ? XRT_LIFT_MAX_VIEWS : o->vp_count;
	memcpy(out->viewpoints, o->vps, sizeof(out->viewpoints));
	memcpy(out->rect_center, o->rect_center, sizeof(out->rect_center));
	memcpy(out->rect_size, o->rect_size, sizeof(out->rect_size));
	if (depth_ok) {
		out->depth = o->depth;
		out->depth.valid = 1;
		out->depth.realloc = depth_realloc ? 1u : 0u;
	}
	*out_ready = true;
	unpin();
	return XRT_SUCCESS;
}

bool
d3d11_lift_export_output(struct d3d11_lift *l,
                         uint64_t owner,
                         uint64_t id,
                         HANDLE *out_handle,
                         uint32_t *out_w,
                         uint32_t *out_h,
                         uint32_t *out_format)
{
	if (l == nullptr) {
		return false;
	}
	std::lock_guard<std::mutex> g(l->mtx);
	lift_stream *st = find_live(l, owner, id);
	if (st == nullptr || st->exp_handle == nullptr) {
		return false;
	}
	*out_handle = st->exp_handle;
	*out_w = st->exp_w;
	*out_h = st->exp_h;
	*out_format = st->exp_format;
	return true;
}

bool
d3d11_lift_export_fence(struct d3d11_lift *l, uint64_t owner, uint64_t id, HANDLE *out_handle)
{
	if (l == nullptr) {
		return false;
	}
	std::lock_guard<std::mutex> g(l->mtx);
	lift_stream *st = find_live(l, owner, id);
	if (st == nullptr || st->exp_fence_handle == nullptr) {
		return false;
	}
	*out_handle = st->exp_fence_handle;
	return true;
}

bool
d3d11_lift_export_depth(struct d3d11_lift *l,
                        uint64_t owner,
                        uint64_t id,
                        HANDLE *out_handle,
                        uint32_t *out_w,
                        uint32_t *out_h,
                        uint32_t *out_format)
{
	if (l == nullptr) {
		return false;
	}
	std::lock_guard<std::mutex> g(l->mtx);
	lift_stream *st = find_live(l, owner, id);
	if (st == nullptr || st->exp_depth_handle == nullptr) {
		return false;
	}
	*out_handle = st->exp_depth_handle;
	*out_w = st->exp_depth_w;
	*out_h = st->exp_depth_h;
	*out_format = st->exp_depth_format;
	return true;
}

xrt_result_t
d3d11_lift_acquire_blob(struct d3d11_lift *l,
                        uint64_t owner,
                        uint64_t id,
                        uint64_t capacity,
                        bool *out_ready,
                        struct d3d11_lift_blob_info *info,
                        uint8_t **out_bytes)
{
	*out_ready = false;
	*info = d3d11_lift_blob_info{};
	*out_bytes = nullptr;
	if (l == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	std::shared_ptr<std::vector<uint8_t>> blob;
	u_lift_frame_meta meta = {};
	uint32_t format = 0;
	{
		std::lock_guard<std::mutex> g(l->mtx);
		lift_stream *st = find_live(l, owner, id);
		if (st == nullptr) {
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream / bad extent: non-fatal
		}
		if (st->info.mode != XRT_DP_LIFT_MODE_GAUSSIANS) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		if (!st->blob_latched) {
			int32_t slot = -1;
			if (!u_lift_mailbox_pin_latest(&st->mb, true, &slot, &st->blob_latched_meta)) {
				return XRT_SUCCESS; // nothing newer
			}
			st->blob_latched = st->out[slot].blob;
			st->blob_latched_format = st->out[slot].blob_format;
			u_lift_mailbox_unpin(&st->mb, slot); // the shared_ptr keeps the bytes alive
			if (!st->blob_latched) {
				return XRT_SUCCESS;
			}
		}
		blob = st->blob_latched;
		meta = st->blob_latched_meta;
		format = st->blob_latched_format;
		if (capacity >= blob->size()) {
			st->blob_latched.reset(); // delivered: the latch is consumed
		}
	}
	info->frame_id = meta.frame_id;
	info->source_time = meta.source_time;
	info->format = format;
	info->byte_count = blob->size();
	*out_ready = true;
	if (capacity >= blob->size() && !blob->empty()) {
		*out_bytes = (uint8_t *)malloc(blob->size());
		if (*out_bytes == nullptr) {
			return XRT_ERROR_ALLOCATION;
		}
		memcpy(*out_bytes, blob->data(), blob->size());
	}
	return XRT_SUCCESS;
}

xrt_result_t
d3d11_lift_set_priority(struct d3d11_lift *l, uint64_t owner, uint64_t id, uint32_t priority)
{
	if (l == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (priority > U_LIFT_PRIORITY_HIGH) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	std::lock_guard<std::mutex> g(l->mtx);
	lift_stream *st = find_live(l, owner, id);
	if (st == nullptr) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream: non-fatal
	}
	if (st->priority != priority) {
		U_LOG_I("[lift] stream %llu priority %u -> %u", (unsigned long long)id, st->priority, priority);
		st->priority = priority;
		l->cv.notify_all(); // a stream un-paused may have a frame waiting
	}
	return XRT_SUCCESS;
}

xrt_result_t
d3d11_lift_get_stats(struct d3d11_lift *l, uint64_t owner, uint64_t id, struct xrt_lift_stream_stats *out)
{
	memset(out, 0, sizeof(*out));
	if (l == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	std::lock_guard<std::mutex> g(l->mtx);
	lift_stream *st = find_live(l, owner, id);
	if (st == nullptr) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	out->priority = st->priority;
	out->submitted = st->mb.submitted;
	out->converted = st->mb.converted;
	out->dropped = st->mb.dropped;
	out->failed = st->mb.failed;
	out->latency_last_ns = st->mb.lat_last_ns;
	out->latency_avg_ns = st->mb.lat_ema_ns;
	out->latency_min_ns = st->mb.lat_min_ns;
	out->latency_max_ns = st->mb.lat_max_ns;
	out->rate_hz = u_lift_mailbox_rate_hz(&st->mb);
	return XRT_SUCCESS;
}


/*
 *
 * Eyes (G7), the dp-lift backend, the bridge (#2003).
 *
 */

void
d3d11_lift_publish_eyes(struct d3d11_lift *l, const struct xrt_eye_positions *eyes)
{
	if (l == nullptr || eyes == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> g(l->eyes_mtx);
	l->eyes = *eyes;
	l->eyes_ns = os_monotonic_get_ns();
}

void
d3d11_lift_set_eyes_refresh(struct d3d11_lift *l, void (*fn)(void *ud), void *ud)
{
	if (l == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> g(l->eyes_mtx);
	l->eyes_refresh = fn;
	l->eyes_refresh_ud = ud;
}

//! Producer side: pull fresh eyes when nothing published them lately.
static void
eyes_refresh_if_stale(d3d11_lift *l)
{
	void (*fn)(void *) = nullptr;
	void *ud = nullptr;
	{
		std::lock_guard<std::mutex> g(l->eyes_mtx);
		const uint64_t now = os_monotonic_get_ns();
		if (l->eyes_ns != 0 && now - l->eyes_ns < 50ull * 1000ull * 1000ull) {
			return;
		}
		fn = l->eyes_refresh;
		ud = l->eyes_refresh_ud;
	}
	if (fn != nullptr) {
		fn(ud); // publishes through d3d11_lift_publish_eyes
	}
}

//! Hand one command to the lift thread and wait for it (the provider's exec thread).
static xrt_result_t
lift_cmd_run(d3d11_lift *l, lift_cmd *c)
{
	std::unique_lock<std::mutex> lk(l->mtx);
	if (l->stop) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	l->cmds.push_back(c);
	l->cv.notify_all();
	l->cmd_cv.wait(lk, [c] { return c->done; });
	return c->result;
}

static void
be_snapshot(void *ud, conv_dp_lift_snapshot *out)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	memset(out, 0, sizeof(*out));
	void *lf = nullptr, *ff = nullptr;
	if (l->resolve != nullptr) {
		(void)l->resolve(l->resolve_ud, &lf, &ff);
	}
	out->factory_present = lf != nullptr || ff != nullptr;
	std::lock_guard<std::mutex> g(l->mtx);
	out->attempted = l->activated;
	out->have_module = l->activated && l->module_alive;
	out->has_depth_slot = l->has_depth_slot;
	out->caps = l->caps;
	snprintf(out->detail, sizeof(out->detail), "%s", l->unavail_detail);
	out->change_count = l->change_count;
}

static xrt_result_t
be_activate(void *ud, bool retry)
{
	lift_cmd c;
	c.op = LIFT_CMD_ACTIVATE;
	c.retry = retry;
	return lift_cmd_run((d3d11_lift *)ud, &c);
}

static void
be_poll(void *ud)
{
	lift_cmd c;
	c.op = LIFT_CMD_POLL;
	(void)lift_cmd_run((d3d11_lift *)ud, &c);
}

static xrt_result_t
be_stream_create(void *ud, const xrt_dp_lift_stream_info *info, uint64_t *out_dp_id)
{
	lift_cmd c;
	c.op = LIFT_CMD_STREAM_CREATE;
	c.info = info;
	const xrt_result_t r = lift_cmd_run((d3d11_lift *)ud, &c);
	*out_dp_id = c.dp_id;
	return r;
}

static void
be_stream_destroy(void *ud, uint64_t dp_id)
{
	lift_cmd c;
	c.op = LIFT_CMD_STREAM_DESTROY;
	c.dp_id = dp_id;
	(void)lift_cmd_run((d3d11_lift *)ud, &c);
}

static xrt_result_t
be_convert(void *ud, uint64_t dp_id, conv_dp_lift_convert *io)
{
	lift_cmd c;
	c.op = LIFT_CMD_CONVERT;
	c.dp_id = dp_id;
	c.io = io;
	return lift_cmd_run((d3d11_lift *)ud, &c);
}

void
d3d11_lift_get_dp_backend(struct d3d11_lift *l, struct conv_dp_lift_backend *out)
{
	memset(out, 0, sizeof(*out));
	if (l == nullptr) {
		return;
	}
	out->snapshot = be_snapshot;
	out->activate = be_activate;
	out->poll = be_poll;
	out->stream_create = be_stream_create;
	out->stream_destroy = be_stream_destroy;
	out->convert = be_convert;
	out->ud = l;
}

void
d3d11_lift_set_core_bridge(struct d3d11_lift *l, const struct d3d11_lift_core_bridge *bridge)
{
	if (l == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> g(l->bridge_mtx);
	if (bridge != nullptr) {
		l->bridge = *bridge;
	} else {
		l->bridge = d3d11_lift_core_bridge{};
	}
}

static bool
bridge_resolve(
    d3d11_lift *l, uint64_t owner, uint64_t wire_id, conv_core **out_core, uint64_t *out_core_id, uint32_t *out_mode)
{
	d3d11_lift_core_bridge b;
	{
		std::lock_guard<std::mutex> g(l->bridge_mtx);
		b = l->bridge;
	}
	if (b.core == nullptr || b.resolve == nullptr || !b.resolve(b.ud, owner, wire_id, out_core_id, out_mode)) {
		return false;
	}
	if (out_core != nullptr) {
		*out_core = b.core;
	}
	return true;
}


/*
 *
 * The conversion core's D3D11 glue on the service device (#2003).
 *
 * Every entry point is gated (glue_enter / glue_leave): d3d11_lift_destroy
 * closes the gate and waits for in-flight calls before anything goes away.
 * glue_mtx guards the tables only and is never held across a GPU wait or the
 * service context mutex (order: svc_ctx_mutex -> glue_mtx).
 *
 */

static bool
glue_enter(d3d11_lift *l)
{
	std::lock_guard<std::mutex> g(l->glue_mtx);
	if (l->glue_closed) {
		return false;
	}
	l->glue_calls++;
	return true;
}

static void
glue_leave(d3d11_lift *l)
{
	std::lock_guard<std::mutex> g(l->glue_mtx);
	l->glue_calls--;
	l->glue_cv.notify_all();
}

static void
glue_close(d3d11_lift *l)
{
	std::unique_lock<std::mutex> lk(l->glue_mtx);
	l->glue_closed = true;
	l->glue_cv.wait(lk, [l] { return l->glue_calls == 0; });
}

static void
glue_img_release(glue_img &gi)
{
	rel(gi.rtv);
	rel(gi.srv);
	rel(gi.km);
	rel(gi.tex);
	close_handle(gi.share);
}

static void
glue_stream_release(glue_stream &gs)
{
	rel(gs.imp_km);
	rel(gs.imp_srv);
	rel(gs.imp_tex);
	if (!gs.imp_dxgi) {
		close_handle(gs.imp_handle);
	}
	gs.imp_handle = nullptr;
	rel(gs.exp_fence);
	close_handle(gs.exp_fence_handle);
	rel(gs.exp_tex);
	close_handle(gs.exp_handle);
	rel(gs.exp_depth_tex);
	close_handle(gs.exp_depth_handle);
}

static void
glue_release_all(d3d11_lift *l)
{
	std::lock_guard<std::mutex> g(l->glue_mtx);
	for (auto &kv : l->glue_imgs) {
		glue_img_release(kv.second);
	}
	l->glue_imgs.clear();
	for (auto &kv : l->glue_streams) {
		glue_stream_release(kv.second);
	}
	l->glue_streams.clear();
	for (auto &kv : l->glue_rbs) {
		rel(kv.second.staging);
	}
	l->glue_rbs.clear();
	for (auto &kv : l->core_weave) {
		for (auto &r : kv.second.lb_rb) {
			rel(r.staging);
		}
	}
	l->core_weave.clear();
	d3d11_conv_viewsyn_destroy(&l->viewsyn);
}

//! A glue image by its import key (copied; its COM pointers stay valid while
//! the caller owns the slot / pin it belongs to). False = not ours (CPU).
static bool
glue_find(d3d11_lift *l, uint64_t key, glue_img *out)
{
	std::lock_guard<std::mutex> g(l->glue_mtx);
	auto it = l->glue_imgs.find(key);
	if (it == l->glue_imgs.end()) {
		return false;
	}
	*out = it->second;
	return true;
}

static bool
format_renderable(DXGI_FORMAT f)
{
	return f != DXGI_FORMAT_UNKNOWN;
}

static bool
glue_alloc(void *ud, const struct conv_image_request *req, struct xrt_conv_image *out)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	// The bound capability must take a shared NT handle with a keyed mutex
	// (or say nothing); otherwise the service's CPU allocator serves it.
	if (req->import_memory_kinds != 0 && (req->import_memory_kinds & XRT_CONV_MEM_WIN32_NT_HANDLE) == 0) {
		return false;
	}
	if (req->import_sync_kinds != 0 && (req->import_sync_kinds & (1u << XRT_CONV_SYNC_KEYED_MUTEX)) == 0) {
		return false;
	}
	const DXGI_FORMAT f = conv_fmt_to_dxgi(req->format);
	if (f == DXGI_FORMAT_UNKNOWN || req->width == 0 || req->height == 0 || !glue_enter(l)) {
		return false;
	}
	glue_img gi;
	gi.w = req->width;
	gi.h = req->height;
	gi.layers = req->array_layers > 0 ? req->array_layers : 1;
	gi.dxgi = f;
	gi.stream_id = req->stream_id;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = gi.w;
	td.Height = gi.h;
	td.MipLevels = 1;
	td.ArraySize = gi.layers;
	td.Format = f;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (format_renderable(f) ? D3D11_BIND_RENDER_TARGET : 0u);
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	HRESULT hr = l->svc_device->CreateTexture2D(&td, nullptr, &gi.tex);
	if (SUCCEEDED(hr)) {
		hr = gi.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&gi.km);
	}
	IDXGIResource1 *r1 = nullptr;
	if (SUCCEEDED(hr)) {
		hr = gi.tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
	}
	if (SUCCEEDED(hr)) {
		hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
		                            &gi.share);
	}
	rel(r1);
	if (SUCCEEDED(hr)) {
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = typed_srv_format(f);
		if (gi.layers > 1) {
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			sd.Texture2DArray.MipLevels = 1;
			sd.Texture2DArray.ArraySize = gi.layers;
		} else {
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
		}
		hr = l->svc_device->CreateShaderResourceView(gi.tex, &sd, &gi.srv);
	}
	if (SUCCEEDED(hr) && format_renderable(f)) {
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = typed_srv_format(f);
		rd.ViewDimension =
		    D3D11_RTV_DIMENSION_TEXTURE2D; // slice 0 (ROW layouts; ARRAY views render per slice in S5)
		if (gi.layers > 1) {
			rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rd.Texture2DArray.ArraySize = 1;
		}
		hr = l->svc_device->CreateRenderTargetView(gi.tex, &rd, &gi.rtv);
	}
	if (FAILED(hr)) {
		U_LOG_E("[conv] D3D11 glue: image %ux%u fmt=%u create/share failed: 0x%08lx", gi.w, gi.h, req->format,
		        (unsigned long)hr);
		glue_img_release(gi);
		glue_leave(l);
		return false;
	}
	uint64_t key;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		key = ++l->glue_next_key; // never collides with the service CPU allocator's (bit 63 set)
		l->glue_imgs[key] = gi;
	}
	memset(out, 0, sizeof(*out));
	out->memory_kind = XRT_CONV_MEM_WIN32_NT_HANDLE;
	out->handle = (uint64_t)(uintptr_t)gi.share;
	out->import_key = key;
	out->width = gi.w;
	out->height = gi.h;
	out->format = req->format;
	out->color_space = (req->role == 0 || req->role == XRT_CONV_OUT_VIEWS) ? XRT_CONV_CS_SRGB : XRT_CONV_CS_DATA;
	out->adapter_luid = l->adapter_luid;
	out->sync.kind = XRT_CONV_SYNC_KEYED_MUTEX;
	out->sync.import_key = key;
	out->sync.acquire_value = 0;
	out->sync.release_value = 0;
	glue_leave(l);
	return true;
}

static void
glue_free(void *ud, struct xrt_conv_image *img)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	if (img == nullptr || img->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || !glue_enter(l)) {
		return;
	}
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		auto it = l->glue_imgs.find(img->import_key);
		if (it != l->glue_imgs.end()) {
			const uint64_t sid = it->second.stream_id;
			glue_img_release(it->second);
			l->glue_imgs.erase(it);
			bool any = false;
			for (auto &kv : l->glue_imgs) {
				any = any || kv.second.stream_id == sid;
			}
			if (!any) {
				auto rb = l->glue_rbs.find(sid);
				if (rb != l->glue_rbs.end()) {
					rel(rb->second.staging);
					l->glue_rbs.erase(rb);
				}
			}
		}
	}
	{
		// The lift thread drops its view of it on its next command.
		std::lock_guard<std::mutex> g(l->mtx);
		l->lift_evict.push_back(img->import_key);
	}
	glue_leave(l);
}

void
d3d11_lift_glue_alloc_ops(struct d3d11_lift *l, struct conv_image_alloc_ops *out)
{
	memset(out, 0, sizeof(*out));
	if (l == nullptr) {
		return;
	}
	out->alloc = glue_alloc;
	out->free = glue_free;
	out->ud = l;
}

/*
 * S6 / S7: depth readback. Never stalls: one staging copy in flight per
 * stream, issued at most every U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS (the lift
 * thread's old cadence) and mapped DO_NOT_WAIT on a later conversion. The
 * samples returned are therefore of an earlier conversion of the same stream
 * — the lag the pre-core readback had too.
 */
static bool
glue_rb_read(void *ud,
             const struct xrt_conv_image *depth,
             const float **out_samples,
             uint32_t *out_w,
             uint32_t *out_h,
             uint32_t *out_stride_floats)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	if (depth == nullptr || depth->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || !glue_enter(l)) {
		return false;
	}
	glue_img gi;
	glue_rb *rb = nullptr;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		auto it = l->glue_imgs.find(depth->import_key);
		if (it != l->glue_imgs.end()) {
			gi = it->second;
			rb = &l->glue_rbs[gi.stream_id]; // std::map: stable; one dispatcher per stream
		}
	}
	if (rb == nullptr || (gi.dxgi != DXGI_FORMAT_R32_FLOAT && gi.dxgi != DXGI_FORMAT_R16_FLOAT)) {
		glue_leave(l);
		return false;
	}
	bool fresh = false;
	const uint64_t now = os_monotonic_get_ns();
	if (rb->pending && rb->staging != nullptr) {
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		D3D11_MAPPED_SUBRESOURCE m = {};
		HRESULT hr = l->svc_context->Map(rb->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
		if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
			rb->pending = false;
			if (SUCCEEDED(hr)) {
				rb->samples.resize((size_t)rb->w * rb->h);
				for (uint32_t y = 0; y < rb->h; y++) {
					const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
					float *dst = &rb->samples[(size_t)y * rb->w];
					if (rb->fmt == DXGI_FORMAT_R32_FLOAT) {
						memcpy(dst, row, (size_t)rb->w * sizeof(float));
					} else {
						for (uint32_t x = 0; x < rb->w; x++) {
							dst[x] = half_to_float(((const uint16_t *)row)[x]);
						}
					}
				}
				l->svc_context->Unmap(rb->staging, 0);
				fresh = true;
			}
		}
	}
	if (!rb->pending && (rb->issued_ns == 0 || now - rb->issued_ns >= U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS)) {
		if (rb->staging == nullptr || rb->w != gi.w || rb->h != gi.h || rb->fmt != gi.dxgi) {
			rel(rb->staging);
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = gi.w;
			td.Height = gi.h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = gi.dxgi;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_STAGING;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(l->svc_device->CreateTexture2D(&td, nullptr, &rb->staging))) {
				rb->staging = nullptr;
			}
			rb->w = gi.w;
			rb->h = gi.h;
			rb->fmt = gi.dxgi;
		}
		if (rb->staging != nullptr && img_acquire(gi.km, depth, 4)) {
			{
				std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
				l->svc_context->CopyResource(rb->staging, gi.tex);
				l->svc_context->Flush();
			}
			img_release(gi.km, depth);
			rb->pending = true;
			rb->issued_ns = now;
		}
	}
	if (fresh) {
		*out_samples = rb->samples.data();
		*out_w = rb->w;
		*out_h = rb->h;
		*out_stride_floats = rb->w;
	}
	glue_leave(l);
	return fresh;
}

static void
glue_rb_release(void *ud, const float *samples)
{
	(void)ud;
	(void)samples; // owned by the stream's readback state until its next read
}

//! S5: the reference view synthesis on the service device.
static bool
glue_viewsyn_render(void *ud, const struct conv_viewsyn_request *req)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	if (req == nullptr || req->color == nullptr || req->depth == nullptr || req->target == nullptr ||
	    !glue_enter(l)) {
		return false;
	}
	glue_img c, d, t;
	bool ok = glue_find(l, req->color->import_key, &c) && glue_find(l, req->depth->import_key, &d) &&
	          glue_find(l, req->target->import_key, &t);
	std::lock_guard<std::mutex> vg(l->viewsyn_mtx);
	if (ok && l->viewsyn == nullptr && !l->viewsyn_tried) {
		l->viewsyn_tried = true;
		l->viewsyn = d3d11_conv_viewsyn_create(l->svc_device);
	}
	ok = ok && l->viewsyn != nullptr;
	bool held_c = false, held_d = false, held_t = false;
	ok = ok && (held_c = img_acquire(c.km, req->color, 4));
	ok = ok && (held_d = img_acquire(d.km, req->depth, 4));
	ok = ok && (held_t = img_acquire(t.km, req->target, 4));
	if (ok) {
		u_conv_viewsyn_params p;
		u_conv_viewsyn_params_defaults(&p);
		p.view_count = req->view_count;
		for (uint32_t i = 0; i < req->view_count && i < U_CONV_VIEWSYN_MAX_VIEWS; i++) {
			p.viewpoints_m[i][0] = req->viewpoints_m[i][0];
			p.viewpoints_m[i][1] = req->viewpoints_m[i][1];
			p.viewpoints_m[i][2] = req->viewpoints_m[i][2];
		}
		p.rect_width_m = req->rect_width_m;
		p.rect_height_m = req->rect_height_m;
		if (req->depth_info != nullptr) {
			p.depth = *req->depth_info;
		}
		p.range_near = req->range_near;
		p.range_far = req->range_far;
		p.convergence = req->convergence;
		p.budget = req->budget;
		p.rear_cap_m = req->rear_cap_m;
		d3d11_conv_viewsyn_inputs in = {};
		in.color = c.srv;
		in.color_width = c.w;
		in.color_height = c.h;
		in.depth = d.srv;
		in.depth_width = d.w;
		in.depth_height = d.h;
		in.new_content = req->new_content;
		d3d11_conv_viewsyn_target tg = {};
		tg.texture = t.tex;
		tg.rtv_format = DXGI_FORMAT_UNKNOWN;
		tg.layout = req->layout;
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		ok = d3d11_conv_viewsyn_render(l->viewsyn, l->svc_context, &in, &p, &tg);
		l->svc_context->Flush();
	}
	if (held_t) {
		img_release(t.km, req->target);
	}
	if (held_d) {
		img_release(d.km, req->depth);
	}
	if (held_c) {
		img_release(c.km, req->color);
	}
	glue_leave(l);
	return ok;
}

void
d3d11_lift_glue_gpu_ops(struct d3d11_lift *l,
                        struct conv_viewsyn_ops *viewsyn,
                        struct conv_depth_readback_ops *readback)
{
	if (viewsyn != nullptr) {
		memset(viewsyn, 0, sizeof(*viewsyn));
		if (l != nullptr) {
			viewsyn->render = glue_viewsyn_render;
			viewsyn->ud = l;
		}
	}
	if (readback != nullptr) {
		memset(readback, 0, sizeof(*readback));
		if (l != nullptr) {
			readback->read = glue_rb_read;
			readback->release = glue_rb_release;
			readback->ud = l;
		}
	}
}

/*!
 * Open / reuse the client's shared input for v3 stream @p wire (producer
 * thread). Takes ownership of @p handle. Returns the cached import.
 */
static bool
glue_import_client(d3d11_lift *l, uint64_t wire, HANDLE handle, bool is_dxgi, glue_stream **out_gs)
{
	glue_stream *gs;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		gs = &l->glue_streams[wire]; // std::map: stable; one producer thread per stream
	}
	*out_gs = gs;
	bool reuse = false;
	if (gs->imp_tex != nullptr && gs->imp_dxgi == is_dxgi) {
		reuse = is_dxgi ? (gs->imp_handle == handle) : same_kernel_object(gs->imp_handle, handle);
	}
	if (reuse) {
		if (!is_dxgi) {
			CloseHandle(handle);
		}
		return true;
	}
	rel(gs->imp_km);
	rel(gs->imp_srv);
	rel(gs->imp_tex);
	if (!gs->imp_dxgi) {
		close_handle(gs->imp_handle);
	}
	gs->imp_handle = nullptr;
	HRESULT hr;
	if (is_dxgi) {
		hr = l->svc_device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&gs->imp_tex);
	} else {
		hr = l->svc_device1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **)&gs->imp_tex);
	}
	D3D11_TEXTURE2D_DESC d = {};
	if (SUCCEEDED(hr)) {
		gs->imp_tex->GetDesc(&d);
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = typed_srv_format(d.Format);
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		hr = l->svc_device->CreateShaderResourceView(gs->imp_tex, &sd, &gs->imp_srv);
	}
	if (FAILED(hr)) {
		U_LOG_E("[conv] v3 input OpenSharedResource(%s) failed: 0x%08lx", is_dxgi ? "DXGI" : "NT",
		        (unsigned long)hr);
		rel(gs->imp_srv);
		rel(gs->imp_tex);
		if (!is_dxgi) {
			CloseHandle(handle);
		}
		return false;
	}
	(void)gs->imp_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&gs->imp_km);
	gs->imp_handle = handle; // kept open (NT) for the identity compare
	gs->imp_dxgi = is_dxgi;
	gs->imp_w = d.Width;
	gs->imp_h = d.Height;
	U_LOG_I("[conv] v3 stream 0x%llx input import cached (%s, %ux%u)", (unsigned long long)wire,
	        is_dxgi ? "DXGI" : "NT", d.Width, d.Height);
	return true;
}

xrt_result_t
d3d11_lift_glue_import_input(struct d3d11_lift *l,
                             uint64_t wire,
                             HANDLE handle,
                             bool is_dxgi,
                             uint32_t w,
                             uint32_t h,
                             const struct conv_submit_slot *slot)
{
	if (l == nullptr || !l->snap_ok || slot == nullptr || slot->image_count < 1 || !glue_enter(l)) {
		if (handle != nullptr && !is_dxgi) {
			CloseHandle(handle);
		}
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	eyes_refresh_if_stale(l);
	glue_stream *gs = nullptr;
	if (!glue_import_client(l, wire, handle, is_dxgi, &gs)) {
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	const xrt_conv_image *img = &slot->images[0];
	w = w > gs->imp_w ? gs->imp_w : w;
	h = h > gs->imp_h ? gs->imp_h : h;
	glue_img gi;
	const bool gpu = glue_find(l, img->import_key, &gi);
	if (!gpu &&
	    (img->memory_kind != XRT_CONV_MEM_CPU || img->cpu_ptr == nullptr || img->width != w || img->height != h)) {
		glue_leave(l);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	// The caller's keyed mutex: 4 ms, the budget every weave-side acquire uses.
	bool acquired = false;
	if (gs->imp_km != nullptr) {
		HRESULT hr = gs->imp_km->AcquireSync(0, 4);
		if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
			glue_leave(l);
			return XRT_ERROR_WEAVE_REFUSED;
		}
		acquired = true;
	}
	bool ok;
	if (gpu) {
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		// The pre-core snapshot, into the core's slot: 1:1 (point) at the
		// submitted size, box-filtered when inputScale shrank the slot.
		blit_dst dst = {gi.km, gi.rtv};
		ok = gi.rtv != nullptr &&
		     snapshot_blit(l, dst, gs->imp_srv, gs->imp_w, gs->imp_h, 0, 0, w, h, gi.w, gi.h);
		l->svc_context->Flush();
	} else {
		// A CPU provider's slot: read the region back (slow path, sim providers only).
		D3D11_TEXTURE2D_DESC td = {};
		gs->imp_tex->GetDesc(&td);
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.MiscFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *st = nullptr;
		ok = (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
		      td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
		     SUCCEEDED(l->svc_device->CreateTexture2D(&td, nullptr, &st));
		if (ok) {
			std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
			D3D11_BOX box = {0, 0, 0, w, h, 1};
			l->svc_context->CopySubresourceRegion(st, 0, 0, 0, 0, gs->imp_tex, 0, &box);
			D3D11_MAPPED_SUBRESOURCE m = {};
			ok = SUCCEEDED(l->svc_context->Map(st, 0, D3D11_MAP_READ, 0, &m));
			if (ok) {
				const uint32_t stride = img->row_stride != 0 ? img->row_stride : w * 4u;
				for (uint32_t y = 0; y < h; y++) {
					memcpy((uint8_t *)img->cpu_ptr + (size_t)y * stride,
					       (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4u);
				}
				l->svc_context->Unmap(st, 0);
			}
		}
		rel(st);
	}
	if (acquired) {
		gs->imp_km->ReleaseSync(0);
	}
	glue_leave(l);
	return ok ? XRT_SUCCESS : XRT_ERROR_WEAVE_REFUSED;
}

xrt_result_t
d3d11_lift_glue_import_input_cpu(
    struct d3d11_lift *l, uint64_t wire, HANDLE handle, bool is_dxgi, uint32_t w, uint32_t h, uint8_t **out_pixels)
{
	*out_pixels = nullptr;
	if (l == nullptr || !l->snap_ok || !glue_enter(l)) {
		if (handle != nullptr && !is_dxgi) {
			CloseHandle(handle);
		}
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	glue_stream *gs = nullptr;
	if (!glue_import_client(l, wire, handle, is_dxgi, &gs)) {
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	w = w > gs->imp_w ? gs->imp_w : w;
	h = h > gs->imp_h ? gs->imp_h : h;
	// Snapshot into a plain RGBA8 target (any client format), then read it back.
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	ID3D11Texture2D *rt = nullptr, *st = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
	IDXGIKeyedMutex *rkm = nullptr;
	bool ok = w > 0 && h > 0 && SUCCEEDED(l->svc_device->CreateTexture2D(&td, nullptr, &rt)) &&
	          SUCCEEDED(l->svc_device->CreateRenderTargetView(rt, nullptr, &rtv)) &&
	          SUCCEEDED(rt->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&rkm));
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.MiscFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ok = ok && SUCCEEDED(l->svc_device->CreateTexture2D(&td, nullptr, &st));
	bool acquired = false;
	if (ok && gs->imp_km != nullptr) {
		HRESULT hr = gs->imp_km->AcquireSync(0, 4);
		ok = SUCCEEDED(hr) && hr != (HRESULT)WAIT_TIMEOUT;
		acquired = ok;
	}
	uint8_t *px = nullptr;
	if (ok) {
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		blit_dst dst = {rkm, rtv};
		ok = snapshot_blit(l, dst, gs->imp_srv, gs->imp_w, gs->imp_h, 0, 0, w, h, w, h);
		if (ok) {
			l->svc_context->CopyResource(st, rt);
			D3D11_MAPPED_SUBRESOURCE m = {};
			ok = SUCCEEDED(l->svc_context->Map(st, 0, D3D11_MAP_READ, 0, &m));
			if (ok) {
				px = (uint8_t *)malloc((size_t)w * h * 4u);
				ok = px != nullptr;
				for (uint32_t y = 0; ok && y < h; y++) {
					memcpy(px + (size_t)y * w * 4u,
					       (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4u);
				}
				l->svc_context->Unmap(st, 0);
			}
		}
	}
	if (acquired) {
		gs->imp_km->ReleaseSync(0);
	}
	rel(st);
	rel(rkm);
	rel(rtv);
	rel(rt);
	glue_leave(l);
	if (!ok) {
		free(px);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	*out_pixels = px;
	return XRT_SUCCESS;
}

void
d3d11_lift_glue_release_input(HANDLE handle, bool is_dxgi)
{
	if (handle != nullptr && !is_dxgi) {
		CloseHandle(handle);
	}
}

//! (Re)create a shared export texture @p w x @p h @p fmt (+ its NT handle).
static bool
glue_export_ensure(d3d11_lift *l, ID3D11Texture2D **tex, HANDLE *handle, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	rel(*tex);
	close_handle(*handle);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	HRESULT hr = l->svc_device->CreateTexture2D(&td, nullptr, tex);
	IDXGIResource1 *r1 = nullptr;
	if (SUCCEEDED(hr)) {
		hr = (*tex)->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
	}
	if (SUCCEEDED(hr)) {
		hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
		                            handle);
	}
	rel(r1);
	if (FAILED(hr)) {
		U_LOG_E("[conv] export texture %ux%u fmt=%u create failed: 0x%08lx", w, h, (unsigned)fmt,
		        (unsigned long)hr);
		rel(*tex);
		close_handle(*handle);
		return false;
	}
	return true;
}

//! Copy runtime image @p img into export @p dst (caller holds the context mutex).
static bool
glue_copy_out(d3d11_lift *l, const xrt_conv_image *img, const glue_img *gi, ID3D11Texture2D *dst)
{
	if (gi != nullptr) {
		l->svc_context->CopySubresourceRegion(dst, 0, 0, 0, 0, gi->tex, 0, nullptr);
		return true;
	}
	if (img->memory_kind == XRT_CONV_MEM_CPU && img->cpu_ptr != nullptr) {
		const uint32_t bpp = img->format == XRT_CONV_FMT_R16_FLOAT ? 2u : 4u;
		l->svc_context->UpdateSubresource(dst, 0, nullptr, img->cpu_ptr,
		                                  img->row_stride != 0 ? img->row_stride : img->width * bpp, 0);
		return true;
	}
	return false;
}

xrt_result_t
d3d11_lift_glue_publish(struct d3d11_lift *l,
                        uint64_t wire,
                        const struct conv_result *res,
                        struct d3d11_lift_published *out)
{
	memset(out, 0, sizeof(*out));
	if (l == nullptr || res == nullptr || l->svc_context4 == nullptr || !glue_enter(l)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// The main export: the views, or a DEPTH-mode stream's depth.
	const bool views = (res->written & XRT_CONV_OUT_VIEWS) != 0;
	const xrt_conv_image *main =
	    views ? &res->views : ((res->written & XRT_CONV_OUT_DEPTH) != 0 ? &res->depth : nullptr);
	const xrt_conv_image *aux = views && (res->written & XRT_CONV_OUT_DEPTH) != 0 ? &res->depth : nullptr;
	if (main == nullptr) {
		glue_leave(l);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	glue_img mg, ag;
	const bool m_gpu = glue_find(l, main->import_key, &mg);
	const bool a_gpu = aux != nullptr && glue_find(l, aux->import_key, &ag);
	const DXGI_FORMAT mf = m_gpu ? mg.dxgi : conv_fmt_to_dxgi(main->format);
	const DXGI_FORMAT af = aux == nullptr ? DXGI_FORMAT_UNKNOWN : (a_gpu ? ag.dxgi : conv_fmt_to_dxgi(aux->format));
	glue_stream *gs;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		gs = &l->glue_streams[wire];
	}
	// Export textures: sized to the result, re-created on size / format change.
	if (gs->exp_tex == nullptr || gs->exp_w != main->width || gs->exp_h != main->height ||
	    gs->exp_format != (uint32_t)mf) {
		if (!glue_export_ensure(l, &gs->exp_tex, &gs->exp_handle, main->width, main->height, mf)) {
			glue_leave(l);
			return XRT_ERROR_WEAVE_REFUSED;
		}
		gs->exp_w = main->width;
		gs->exp_h = main->height;
		gs->exp_format = (uint32_t)mf;
		out->realloc = true;
		U_LOG_W("[conv] v3 stream 0x%llx export texture %ux%u fmt=%u ready", (unsigned long long)wire,
		        main->width, main->height, (unsigned)mf);
	}
	if (gs->exp_fence == nullptr) {
		ID3D11Device5 *d5 = nullptr;
		HRESULT hr = l->svc_device->QueryInterface(__uuidof(ID3D11Device5), (void **)&d5);
		if (SUCCEEDED(hr)) {
			hr =
			    d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&gs->exp_fence);
		}
		rel(d5);
		if (SUCCEEDED(hr)) {
			hr = gs->exp_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &gs->exp_fence_handle);
		}
		if (FAILED(hr)) {
			rel(gs->exp_fence);
			glue_leave(l);
			return XRT_ERROR_WEAVE_REFUSED;
		}
	}
	bool depth_ok = aux != nullptr && af != DXGI_FORMAT_UNKNOWN;
	if (depth_ok && (gs->exp_depth_tex == nullptr || gs->exp_depth_w != aux->width ||
	                 gs->exp_depth_h != aux->height || gs->exp_depth_format != (uint32_t)af)) {
		depth_ok =
		    glue_export_ensure(l, &gs->exp_depth_tex, &gs->exp_depth_handle, aux->width, aux->height, af);
		gs->exp_depth_w = depth_ok ? aux->width : 0;
		gs->exp_depth_h = depth_ok ? aux->height : 0;
		gs->exp_depth_format = depth_ok ? (uint32_t)af : 0;
		out->depth_realloc = depth_ok;
	}

	const bool held_m = m_gpu ? img_acquire(mg.km, main, 4) : true;
	if (!held_m) {
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	const bool held_a = depth_ok && a_gpu ? img_acquire(ag.km, aux, 4) : depth_ok;
	depth_ok = depth_ok && held_a;
	bool ok;
	{
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		ok = glue_copy_out(l, main, m_gpu ? &mg : nullptr, gs->exp_tex);
		if (depth_ok) {
			depth_ok = glue_copy_out(l, aux, a_gpu ? &ag : nullptr, gs->exp_depth_tex);
		}
		// One fence covers the views and the depth (XrLiftDepthResultDXR).
		gs->exp_fence_value++;
		l->svc_context4->Signal(gs->exp_fence, gs->exp_fence_value);
		l->svc_context->Flush();
	}
	if (m_gpu) {
		img_release(mg.km, main);
	}
	if (held_a && a_gpu) {
		img_release(ag.km, aux);
	}
	out->fence_value = gs->exp_fence_value;
	out->width = gs->exp_w;
	out->height = gs->exp_h;
	out->format = gs->exp_format;
	if (depth_ok) {
		out->depth_width = gs->exp_depth_w;
		out->depth_height = gs->exp_depth_h;
		out->depth_format = gs->exp_depth_format;
	} else {
		out->depth_realloc = false;
	}
	glue_leave(l);
	return ok ? XRT_SUCCESS : XRT_ERROR_WEAVE_REFUSED;
}

bool
d3d11_lift_glue_export_output(struct d3d11_lift *l,
                              uint64_t wire,
                              bool depth,
                              HANDLE *out_handle,
                              uint32_t *out_w,
                              uint32_t *out_h,
                              uint32_t *out_format)
{
	if (l == nullptr) {
		return false;
	}
	std::lock_guard<std::mutex> g(l->glue_mtx);
	auto it = l->glue_streams.find(wire);
	if (it == l->glue_streams.end()) {
		return false;
	}
	const glue_stream &gs = it->second;
	HANDLE h = depth ? gs.exp_depth_handle : gs.exp_handle;
	if (h == nullptr) {
		return false;
	}
	*out_handle = h;
	*out_w = depth ? gs.exp_depth_w : gs.exp_w;
	*out_h = depth ? gs.exp_depth_h : gs.exp_h;
	*out_format = depth ? gs.exp_depth_format : gs.exp_format;
	return true;
}

bool
d3d11_lift_glue_export_fence(struct d3d11_lift *l, uint64_t wire, HANDLE *out_handle)
{
	if (l == nullptr) {
		return false;
	}
	std::lock_guard<std::mutex> g(l->glue_mtx);
	auto it = l->glue_streams.find(wire);
	if (it == l->glue_streams.end() || it->second.exp_fence_handle == nullptr) {
		return false;
	}
	*out_handle = it->second.exp_fence_handle;
	return true;
}

void
d3d11_lift_glue_stream_gone(struct d3d11_lift *l, uint64_t wire)
{
	if (l == nullptr || !glue_enter(l)) {
		return;
	}
	{
		// The weave producer uses core_weave under the context mutex.
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		std::lock_guard<std::mutex> g(l->glue_mtx);
		auto it = l->glue_streams.find(wire);
		if (it != l->glue_streams.end()) {
			glue_stream_release(it->second);
			l->glue_streams.erase(it);
		}
		auto cw = l->core_weave.find(wire);
		if (cw != l->core_weave.end()) {
			for (auto &r : cw->second.lb_rb) {
				rel(r.staging);
			}
			l->core_weave.erase(cw);
		}
	}
	glue_leave(l);
}


/*
 *
 * The weave path on core-routed streams (#2003): the same snapshot policy
 * (letterbox crop, DXR_LIFT_MAX_INPUT_EDGE cap) into a core slot, and pins of
 * the core's latest result as d3d11_lift_pin.
 *
 */

static xrt_result_t
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
                         uint64_t *out_frame_id)
{
	*out_frame_id = 0;
	conv_core *core = nullptr;
	uint64_t core_id = 0;
	uint32_t mode = 0;
	if (w == 0 || h == 0 || x + w > src_tw || y + h > src_th ||
	    !bridge_resolve(l, owner, id, &core, &core_id, &mode) || !glue_enter(l)) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream / bad extent: non-fatal
	}
	core_weave_state *ws;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		ws = &l->core_weave[id];
		ws->id = id;
	}
	const bool letterbox = l->letterbox && l->lb_ok;
	uint32_t cx = x, cy = y, cw = w, ch = h;
	if (letterbox && ws->lb.w == w && ws->lb.h == h && u_lift_crop_active(&ws->lb.committed)) {
		const u_lift_crop &c = ws->lb.committed;
		cx = x + c.left;
		cy = y + c.top;
		cw = w - c.left - c.right;
		ch = h - c.top - c.bottom;
	}
	uint32_t dw = cw, dh = ch;
	const bool capped = u_lift_cap_dims(cw, ch, l->max_input_edge, &dw, &dh);

	conv_submit_layout lay = {};
	lay.image_count = 1;
	lay.width[0] = dw;
	lay.height[0] = dh;
	lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
	lay.color_space[0] = XRT_CONV_CS_SRGB;
	lay.role[0] = XRT_CONV_IMG_COLOR;
	conv_submit_slot slot = {};
	if (conv_core_stream_begin_submit(core, core_id, &lay, &slot) != XRT_SUCCESS) {
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	glue_img gi;
	bool ok = glue_find(l, slot.images[0].import_key, &gi) && gi.rtv != nullptr;
	if (ok) {
		blit_dst dst = {gi.km, gi.rtv};
		ok = snapshot_blit(l, dst, src, src_tw, src_th, cx, cy, cw, ch, dw, dh);
	}
	if (ok && letterbox) {
		letterbox_step(l, *ws, src, x, y, w, h); // measures the WHOLE rect, bars included
	}
	if (!ok) {
		conv_core_stream_abort_submit(core, core_id, slot.slot);
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	if (capped && (ws->cap_logged_w != dw || ws->cap_logged_h != dh)) {
		ws->cap_logged_w = dw;
		ws->cap_logged_h = dh;
		U_LOG_W("[conv] v3 stream 0x%llx: snapshot %ux%u capped to %ux%u (DXR_LIFT_MAX_INPUT_EDGE=%u)",
		        (unsigned long long)id, cw, ch, dw, dh, l->max_input_edge);
	}

	// The rect's options are sticky like the pre-core stream's last params.
	if (params != nullptr) {
		ws->last_params = *params;
		ws->has_last_params = true;
	}
	xrt_dp_lift_params p = {};
	p.convergence = -1.0f;
	p.strength = 1.0f;
	p.inpaint = 1;
	if (ws->has_last_params) {
		p = ws->last_params;
	}
	conv_frame_submit fr = {};
	fr.source_time = source_time;
	fr.frame_flags = p.inpaint != 0 ? XRT_CONV_FRAME_INPAINT : 0u;
	uint32_t n = viewpoint_floats / 3;
	n = n > XRT_CONV_MAX_VIEWS ? XRT_CONV_MAX_VIEWS : n;
	if (viewpoints != nullptr && n > 0) {
		fr.explicit_count = n;
		memcpy(fr.explicit_xyz, viewpoints, sizeof(float) * 3 * n);
	}
	if (view_control != nullptr) {
		fr.has_view_control = true;
		u_lift_view_control_default(&fr.vc);
		fr.vc.ipd_factor = view_control->ipd_factor;
		fr.vc.parallax_factor = view_control->parallax_factor;
		fr.vc.axis_mode = view_control->axis_mode;
		fr.vc.max_offset_m = view_control->max_offset_m;
		fr.vc.recenter_mode = view_control->recenter_mode;
		if (view_control->hold_s >= 0.0f) {
			fr.vc.hold_s = view_control->hold_s;
		}
		if (view_control->tau_s > 0.0f) {
			fr.vc.tau_s = view_control->tau_s;
		}
	}
	if (rig != nullptr && rig->type != XRT_LIFT_RIG_NONE) {
		fr.rig.type = rig->type;
		fr.rig.ipd_factor = rig->ipd_factor;
		fr.rig.parallax_factor = rig->parallax_factor;
		fr.rig.perspective_factor = rig->perspective_factor;
		fr.rig.inv_convergence_distance = rig->inv_convergence_distance;
		fr.rig.half_tan_vfov = rig->half_tan_vfov;
		fr.rig.m2v = rig->m2v;
		u_lift_rig_sanitize(&fr.rig);
	}
	if (frame != nullptr && frame->valid) {
		fr.rect_valid = true;
		memcpy(fr.rect_center, frame->center, sizeof(fr.rect_center));
		fr.rect_w_m = frame->width_m;
		fr.rect_h_m = frame->height_m;
	}
	fr.active[0] = (float)(cx - x) / (float)w;
	fr.active[1] = (float)(cy - y) / (float)h;
	fr.active[2] = (float)(cx - x + cw) / (float)w;
	fr.active[3] = (float)(cy - y + ch) / (float)h;
	if (p.convergence >= 0.0f) {
		fr.convergence_set = true;
		fr.convergence_rel01 = p.convergence > 1.0f ? 1.0f : p.convergence;
	}
	fr.strength = p.strength;
	fr.focal_px = p.focal_px;
	if (capped && fr.focal_px > 0.0f) {
		fr.focal_px *= (float)dw / (float)cw; // focal is in INPUT pixels (a crop keeps the scale)
	}
	const xrt_result_t r = conv_core_stream_commit_submit(core, core_id, slot.slot, &fr, out_frame_id);
	glue_leave(l);
	return r;
}

static bool
core_pin_latest(d3d11_lift *l, uint64_t owner, uint64_t id, d3d11_lift_pin *out)
{
	conv_core *core = nullptr;
	uint64_t core_id = 0;
	uint32_t mode = 0;
	if (!bridge_resolve(l, owner, id, &core, &core_id, &mode) || !glue_enter(l)) {
		return false;
	}
	conv_result r = {};
	if (conv_core_stream_acquire(core, core_id, false, &r) != XRT_SUCCESS) {
		glue_leave(l);
		return false;
	}
	glue_img vg, dg;
	const bool have_views = (r.written & XRT_CONV_OUT_VIEWS) != 0 && glue_find(l, r.views.import_key, &vg) &&
	                        vg.srv != nullptr && vg.km != nullptr;
	if (!have_views || !img_acquire(vg.km, &r.views, 4)) {
		// No GPU views (a CPU provider's result, or a busy slot): weave it flat.
		conv_core_stream_release(core, core_id, r.ring_slot);
		glue_leave(l);
		return false;
	}
	out->lift = l;
	out->stream_id = id;
	out->slot = -1;
	out->km = vg.km;
	out->srv = vg.srv;
	out->width = vg.w;
	out->height = vg.h;
	out->view_count = r.views_info.view_count != 0 ? r.views_info.view_count : 1;
	const bool whole = r.active[0] == 0.0f && r.active[1] == 0.0f && r.active[2] == 0.0f && r.active[3] == 0.0f;
	out->active[0] = whole ? 0.0f : r.active[0];
	out->active[1] = whole ? 0.0f : r.active[1];
	out->active[2] = whole ? 1.0f : r.active[2];
	out->active[3] = whole ? 1.0f : r.active[3];
	out->vp_count = r.views_info.view_count <= XRT_LIFT_MAX_VIEWS ? r.views_info.view_count : XRT_LIFT_MAX_VIEWS;
	for (uint32_t i = 0; i < out->vp_count; i++) {
		out->vps[3 * i + 0] = r.views_info.eye_xyz[i][0];
		out->vps[3 * i + 1] = r.views_info.eye_xyz[i][1];
		out->vps[3 * i + 2] = r.views_info.eye_xyz[i][2];
	}
	memcpy(out->rect_center, r.rect_center, sizeof(out->rect_center));
	memcpy(out->rect_size, r.rect_size, sizeof(out->rect_size));
	// The pin is the service's own consumer (the DP weave + the lifted-rect
	// depth cursor, ADR-046 A1): like the pre-core path, it sees the module's
	// depth whenever one was produced, including the observer-only depth
	// (#2003) the app never asked for. The app-facing export (core_export)
	// still follows r.written.
	if ((r.provider_written & XRT_CONV_OUT_DEPTH) != 0 && glue_find(l, r.depth.import_key, &dg) &&
	    dg.km != nullptr) {
		const uint32_t k = r.depth_info.kind;
		out->depth_tex = dg.tex;
		out->depth_km = dg.km;
		out->depth_w = dg.w;
		out->depth_h = dg.h;
		out->depth_format = (uint32_t)dg.dxgi;
		out->depth_encoding = (k == XRT_CONV_DEPTH_METRIC_INVERSE || k == XRT_CONV_DEPTH_AFFINE_INVERSE ||
		                       k == XRT_CONV_DEPTH_AFFINE_DISPARITY || k == XRT_CONV_DEPTH_STEREO_DISPARITY_PX)
		                          ? XRT_DP_LIFT_DEPTH_ENCODING_INVERSE
		                          : XRT_DP_LIFT_DEPTH_ENCODING_LINEAR;
		out->depth_value_scale = r.depth_info.value_scale != 0.0f ? r.depth_info.value_scale : 1.0f;
		out->depth_value_offset = r.depth_info.value_shift;
		out->relief_valid = r.depth_info.relief_valid != 0;
		out->relief_scale = r.depth_info.relief_scale;
		out->relief_offset = r.depth_info.relief_offset;
	}
	out->core = true;
	out->core_id = core_id;
	out->core_ring_slot = r.ring_slot;
	out->valid = true;
	glue_leave(l);
	return true;
}

static void
core_unpin(d3d11_lift *l, d3d11_lift_pin *pin)
{
	d3d11_lift_core_bridge b;
	{
		std::lock_guard<std::mutex> g(l->bridge_mtx);
		b = l->bridge;
	}
	if (b.core != nullptr) {
		conv_core_stream_release(b.core, pin->core_id, pin->core_ring_slot);
	}
}
