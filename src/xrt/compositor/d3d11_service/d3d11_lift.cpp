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

#include "xrt/xrt_display_processor_d3d11.h"

#include "util/u_lift_mailbox.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <d3d11_1.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
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
 * Snapshot blit (service device): sample a source sub-rect 1:1 into a slot.
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

struct snap_cb
{
	float src_rect[4];
	float src_size[4];
};


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

	uint32_t priority = U_LIFT_PRIORITY_NORMAL; //!< XrLiftPriorityDXR
	bool dead = false;       //!< destroy requested; the lift thread reaps it
	bool converting = false; //!< the lift thread is inside a conversion for it

	// Lift thread only.
	uint64_t dp_id = 0;
	bool dp_created = false;
	bool dp_failed = false;
	ID3D11Texture2D *exact_tex = nullptr;
	uint32_t exact_w = 0, exact_h = 0;

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

	// Blob latch (xrAcquireLiftBlobDXR two-call idiom).
	std::shared_ptr<std::vector<uint8_t>> blob_latched;
	u_lift_frame_meta blob_latched_meta = {};
	uint32_t blob_latched_format = 0;

	// Throttled INFO.
	uint64_t last_stats_log_ns = 0;
};

struct d3d11_lift
{
	// Service side (borrowed).
	ID3D11Device *svc_device = nullptr;
	ID3D11Device1 *svc_device1 = nullptr;
	ID3D11DeviceContext *svc_context = nullptr;
	ID3D11DeviceContext4 *svc_context4 = nullptr;
	std::mutex *svc_ctx_mutex = nullptr;
	xrt_dp_factory_d3d11_fn_t lift_factory = nullptr;
	xrt_dp_factory_d3d11_fn_t fallback_factory = nullptr;
	bool (*eyes_fn)(void *ud, struct xrt_eye_positions *out) = nullptr;
	void *eyes_ud = nullptr;

	// Snapshot blit (service device).
	ID3D11VertexShader *snap_vs = nullptr;
	ID3D11PixelShader *snap_ps = nullptr;
	ID3D11SamplerState *snap_sampler = nullptr;
	ID3D11Buffer *snap_cb = nullptr;
	bool snap_ok = false;

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
	uint64_t next_stream_id = 0;
	uint32_t live_streams = 0;
	std::map<uint64_t, std::unique_ptr<lift_stream>> streams;
	u_lift_sched sched = {}; //!< cross-stream priority scheduling (u_lift_mailbox.h)

	std::thread thread;
};

static void
caps_set_state(d3d11_lift *l, uint32_t state, const char *why)
{
	// Caller holds mtx. WARN once per state CHANGE, never per poll.
	l->caps.state = state;
	if (l->logged_state != state) {
		l->logged_state = state;
		U_LOG_W("[lift] module state -> %s (modes=0x%x backend='%s' max_streams=%u max_views=%u)%s%s",
		        state_str(state), l->caps.modes, l->caps.backend, l->caps.max_streams, l->caps.max_views,
		        why != nullptr ? " — " : "", why != nullptr ? why : "");
	}
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
	st.blob_latched.reset();
}


/*
 *
 * Lift thread.
 *
 */

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
	if (l->lift_factory == nullptr && l->fallback_factory == nullptr) {
		std::lock_guard<std::mutex> g(l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "no D3D11 display-processor factory");
		return;
	}

	// Same adapter as the service device: shared textures cross between them.
	IDXGIDevice *dxgi_dev = nullptr;
	IDXGIAdapter *adapter = nullptr;
	HRESULT hr = l->svc_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev);
	if (SUCCEEDED(hr)) {
		hr = dxgi_dev->GetAdapter(&adapter);
	}
	rel(dxgi_dev);
	D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
	if (SUCCEEDED(hr)) {
		hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
		                       2, D3D11_SDK_VERSION, &l->lift_device, &got, &l->lift_context);
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
		std::lock_guard<std::mutex> g(l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "lift device creation failed");
		return;
	}

	// The lift DP — never weaves, carries only the module. The plug-in's
	// explicit lift-only factory when it has one (no weaver, no tracker
	// session); else its ordinary factory with a NULL window.
	const bool lift_only = l->lift_factory != nullptr;
	xrt_result_t xret = (lift_only ? l->lift_factory : l->fallback_factory)(l->lift_device, l->lift_context,
	                                                                        nullptr, &l->dp);
	U_LOG_W("[lift] lift DP %s via the plug-in's %s factory (dedicated device, NULL window)",
	        xret == XRT_SUCCESS && l->dp != nullptr ? "created" : "REFUSED",
	        lift_only ? "lift-only" : "ordinary (no lift-only factory)");
	xrt_dp_lift_caps caps;
	bool have = false;
	if (xret == XRT_SUCCESS && l->dp != nullptr && xrt_display_processor_d3d11_has_lift(l->dp)) {
		have = xrt_display_processor_d3d11_lift_get_caps(l->dp, &caps);
	} else {
		xrt_dp_lift_caps_init(&caps);
	}
	if (!have && l->dp != nullptr) {
		// No module on this plug-in: don't keep a vendor DP instance alive for nothing.
		xrt_display_processor_d3d11_destroy(&l->dp);
	}

	std::lock_guard<std::mutex> g(l->mtx);
	l->activated = true;
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
	bool have = xrt_display_processor_d3d11_lift_get_caps(l->dp, &caps);
	std::lock_guard<std::mutex> g(l->mtx);
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
	const xrt_dp_lift_params params = in.params;
	float vps[3 * XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS];
	memcpy(vps, in.viewpoints, sizeof(vps));
	uint32_t vp_floats = in.viewpoint_floats;
	const uint32_t w = meta.width, h = meta.height;
	lk.unlock();

	// TRACKED viewpoints are resolved HERE, per conversion, from the panel's
	// predicted eyes, and always handed to the module explicitly: the lift DP
	// has no tracker session of its own. The pair (a module spreads N views
	// around it); nothing when no eyes are known yet.
	if (vp_floats == 0 && l->eyes_fn != nullptr && st.info.mode != XRT_DP_LIFT_MODE_GAUSSIANS &&
	    st.info.mode != XRT_DP_LIFT_MODE_DEPTH) {
		struct xrt_eye_positions eyes = {};
		if (l->eyes_fn(l->eyes_ud, &eyes) && eyes.valid && eyes.count >= 2) {
			uint32_t n = eyes.count == params.view_count ? eyes.count : 2;
			n = n > XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS ? XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS : n;
			for (uint32_t i = 0; i < n; i++) {
				vps[3 * i + 0] = eyes.eyes[i].x;
				vps[3 * i + 1] = eyes.eyes[i].y;
				vps[3 * i + 2] = eyes.eyes[i].z;
			}
			vp_floats = 3 * n;
		}
	}

	bool ok = true;
	// 1. The module's own stream, created lazily (module contract: lift thread only).
	if (!st.dp_created && !st.dp_failed) {
		if (l->dp->lift_stream_create(l->dp, &st.info, &st.dp_id)) {
			st.dp_created = true;
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
		U_LOG_I("[lift] stream %llu: submitted=%llu converted=%llu dropped=%llu failed=%llu "
		        "latency last=%.1fms ema=%.1fms min=%.1fms max=%.1fms",
		        (unsigned long long)st.id, (unsigned long long)st.mb.submitted,
		        (unsigned long long)st.mb.converted, (unsigned long long)st.mb.dropped,
		        (unsigned long long)st.mb.failed, st.mb.lat_last_ns / 1e6, st.mb.lat_ema_ns / 1e6,
		        st.mb.lat_min_ns / 1e6, st.mb.lat_max_ns / 1e6);
	}
	l->cv.notify_all();
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
			return l->stop || (l->activate_requested && !l->activated) ||
			       (l->activated && (any_dead(l) || (l->caps.state == XRT_DP_LIFT_STATE_READY && any_work(l))));
		});
		if (l->stop) {
			break;
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
		if (l->dp != nullptr && l->caps.state != XRT_DP_LIFT_STATE_READY && now - last_poll_ns > 1000000000ull) {
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

	// Shutdown: every stream, then the DP, on this thread.
	for (auto &kv : l->streams) {
		lift_stream &st = *kv.second;
		if (st.dp_created && l->dp != nullptr) {
			l->dp->lift_stream_destroy(l->dp, st.dp_id);
		}
		stream_release_gpu(st);
	}
	l->streams.clear();
	lk.unlock();
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

struct d3d11_lift *
d3d11_lift_create(ID3D11Device *svc_device,
                  ID3D11DeviceContext *svc_context,
                  std::mutex *svc_ctx_mutex,
                  void *lift_factory,
                  void *fallback_factory,
                  bool (*eyes_fn)(void *ud, struct xrt_eye_positions *out),
                  void *eyes_ud)
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
	l->lift_factory = (xrt_dp_factory_d3d11_fn_t)lift_factory;
	l->fallback_factory = (xrt_dp_factory_d3d11_fn_t)fallback_factory;
	l->eyes_fn = eyes_fn;
	l->eyes_ud = eyes_ud;
	(void)svc_device->QueryInterface(__uuidof(ID3D11Device1), (void **)&l->svc_device1);
	(void)svc_context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&l->svc_context4);

	// Snapshot blit resources (service device).
	ID3DBlob *b = nullptr;
	ID3DBlob *err = nullptr;
	bool ok = SUCCEEDED(D3DCompile(k_snap_vs, strlen(k_snap_vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0,
	                               &b, &err)) &&
	          SUCCEEDED(svc_device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &l->snap_vs));
	rel(b);
	rel(err);
	ok = ok &&
	     SUCCEEDED(D3DCompile(k_snap_ps, strlen(k_snap_ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b,
	                          &err)) &&
	     SUCCEEDED(svc_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &l->snap_ps));
	rel(b);
	rel(err);
	if (ok) {
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // 1:1 snapshot: exact texels
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		ok = SUCCEEDED(svc_device->CreateSamplerState(&sd, &l->snap_sampler));
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
	{
		std::lock_guard<std::mutex> g(l->mtx);
		l->stop = true;
	}
	l->cv.notify_all();
	if (l->thread.joinable()) {
		l->thread.join();
	}
	rel(l->snap_cb);
	rel(l->snap_sampler);
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
		std::lock_guard<std::mutex> g(l->mtx);
		*out = l->caps;
		out->struct_size = (uint32_t)sizeof(*out);
		if (!l->activate_requested) {
			l->activate_requested = true;
			kick = true;
		}
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
	u_lift_mailbox_init(&st->mb);
	st->last_params.struct_size = (uint32_t)sizeof(st->last_params);
	st->last_params.convergence = -1.0f;
	st->last_params.strength = 1.0f;
	st->last_params.inpaint = 1;
	st->last_params.view_count = m == XRT_DP_LIFT_MODE_NVIEW ? 4 : 2;
	*out_id = st->id;
	U_LOG_W("[lift] stream %llu created (mode=%u hint=%u scale=%.2f, owner=%llu)", (unsigned long long)st->id, m,
	        info->content_hint, st->info.input_scale, (unsigned long long)owner);
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
	std::lock_guard<std::mutex> g(l->mtx);
	lift_stream *st = find_live(l, owner, id);
	return st != nullptr ? st->info.mode : 0;
}

/*!
 * Snapshot @p src region into input slot @p slot's service texture. Caller
 * holds the service context mutex; takes the slot's keyed mutex itself.
 */
static bool
snapshot_blit(d3d11_lift *l,
              lift_in_slot &s,
              ID3D11ShaderResourceView *src,
              uint32_t src_tw,
              uint32_t src_th,
              uint32_t x,
              uint32_t y,
              uint32_t w,
              uint32_t h)
{
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
	vp.Width = (float)w;
	vp.Height = (float)h;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	D3D11_RECT sc = {0, 0, (LONG)w, (LONG)h};
	ctx->RSSetScissorRects(1, &sc);
	ctx->RSSetState(nullptr);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(nullptr, 0);
	ctx->OMSetRenderTargets(1, &s.svc_rtv, nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetInputLayout(nullptr);
	ctx->VSSetShader(l->snap_vs, nullptr, 0);
	ctx->GSSetShader(nullptr, nullptr, 0);
	ctx->PSSetShader(l->snap_ps, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &l->snap_cb);
	ctx->PSSetSamplers(0, 1, &l->snap_sampler);
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
              uint64_t *out_frame_id)
{
	if (w == 0 || h == 0 || x + w > src_tw || y + h > src_th) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream / bad extent: non-fatal
	}
	int32_t slot = -1;
	lift_stream *st = nullptr;
	{
		std::lock_guard<std::mutex> g(l->mtx);
		st = find_live(l, owner, id);
		if (st == nullptr) {
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream / bad extent: non-fatal
		}
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
		lift_in_slot &in = st->in[slot];
		in.params = st->last_params;
		in.viewpoint_floats = st->last_viewpoint_floats;
		memcpy(in.viewpoints, st->last_viewpoints, sizeof(in.viewpoints));
	}

	// Allocation + blit outside mtx: this slot is WRITING, nobody else touches it.
	lift_in_slot &in = st->in[slot];
	bool ok = in_slot_ensure(l, in, w, h) && snapshot_blit(l, in, src, src_tw, src_th, x, y, w, h);

	std::lock_guard<std::mutex> g(l->mtx);
	if (!ok) {
		u_lift_mailbox_abort_submit(&st->mb, slot);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	*out_frame_id = u_lift_mailbox_commit_submit(&st->mb, slot, source_time, os_monotonic_get_ns(), w, h);
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
                             uint64_t *out_frame_id)
{
	if (l == nullptr || !l->snap_ok || src == nullptr || out_frame_id == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	return submit_locked(l, owner, id, src, src_tw, src_th, x, y, w, h, source_time, params, nullptr, 0,
	                     out_frame_id);
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
			hr = l->svc_device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&st->imp_tex);
		} else {
			hr = l->svc_device1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **)&st->imp_tex);
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
		xret = submit_locked(l, owner, id, st->imp_srv, st->imp_w, st->imp_h, 0, 0, w, h, source_time, params,
		                     viewpoints, viewpoint_floats, out_frame_id);
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
	out->valid = true;
	return true;
}

void
d3d11_lift_unpin(struct d3d11_lift_pin *pin)
{
	if (pin == nullptr || !pin->valid || pin->lift == nullptr) {
		return;
	}
	pin->km->ReleaseSync(0);
	d3d11_lift *l = pin->lift;
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
d3d11_lift_acquire_result(struct d3d11_lift *l,
                          uint64_t owner,
                          uint64_t id,
                          bool *out_ready,
                          struct d3d11_lift_result_info *out)
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
				hr = d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&st->exp_fence);
			}
			rel(d5);
			if (SUCCEEDED(hr)) {
				hr = st->exp_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &st->exp_fence_handle);
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

	HRESULT hr = o->svc_km->AcquireSync(0, 4);
	if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
		unpin();
		return XRT_ERROR_WEAVE_REFUSED;
	}
	{
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		l->svc_context->CopyResource(st->exp_tex, o->svc_tex);
		st->exp_fence_value++;
		l->svc_context4->Signal(st->exp_fence, st->exp_fence_value);
		l->svc_context->Flush();
	}
	o->svc_km->ReleaseSync(0);

	out->frame_id = meta.frame_id;
	out->source_time = meta.source_time;
	out->fence_value = st->exp_fence_value;
	out->latency_ns = meta.done_ns >= meta.submit_ns ? meta.done_ns - meta.submit_ns : 0;
	out->width = o->w;
	out->height = o->h;
	out->format = o->format;
	out->view_count = o->view_count;
	out->output_realloc = realloc;
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
