// Copyright 2024-2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 DXGI swapchain target implementation.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_target.h"
#include "comp_d3d11_compositor.h"

#include "util/u_logging.h"
#include "util/u_debug.h"

#include "os/os_time.h"

#include <atomic>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dcomp.h>

// Decoupled presentation (#833): with DXR_PRESENT_OPAQUE=1 a transparent
// session presents through the opaque HWND flip-model path while the DP
// keeps compose-under-bg - the baked desktop makes the output opaque by
// construction, and the HWND flip swapchain is eligible for Hardware
// Independent Flip (DComp visuals stay at full Composed: Flip cost
// regardless of their alpha mode). See the use_transparent gate below.
DEBUG_GET_ONCE_BOOL_OPTION(present_opaque, "DXR_PRESENT_OPAQUE", false)

#include "util/comp_weave_latency_win.h"

// One in-process D3D11 target per compositor per app process — file-scope
// harness + late-weave state (mirrors comp_d3d12_target.cpp; the target
// struct is memset-adjacent zero-init so C++ members live at file scope).
static weave_latency_log g_weave_latency_d3d11;

static bool
dxr_late_weave_enabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		// Default ON: late-weave is the product behavior on every path
		// (measured 96->17 ms VK, 62->17 D3D12, 32->17 D3D11, 29->17
		// workspace). DXR_LATE_WEAVE=0 opts out for A/B or triage.
		const char *e = getenv("DXR_LATE_WEAVE");
		enabled = (e != nullptr && e[0] == '0') ? 0 : 1;
	}
	return enabled == 1;
}

// Late-weave pacing state (single target per process on this path).
static HANDLE g_frame_latency_waitable = nullptr;
static UINT g_last_present_count = 0;

// Live-path pacing state (#833): the transparent/composed chain paces with the
// DComp compositor clock (its frame statistics are unreliable), the opaque
// flip chain with GetFrameStatistics — behaviorally gated: consecutive
// full-bound timeouts on a stats-succeeding chain declare the stats unusable.
static bool g_target_is_composed = false;
static int g_scanout_stats_strikes = 0; //!< consecutive TIMED_OUT; <0 = disabled
// #868 repaint presents release waitable tokens no one consumed; the app's
// next waits would return instantly for that many frames (pacing loss after
// every idle stretch). weave_mark drains the excess when this is nonzero.
static std::atomic<uint32_t> g_repaint_presents_since_app{0};

// Latency governor (#850): DXR_LATE_WEAVE_MAX_LATENCY knob + saturation
// auto-backoff (mirrors comp_d3d12_target.cpp).
static late_weave_governor g_lw_gov_d3d11;


/*!
 * D3D11 target structure.
 */
struct comp_d3d11_target
{
	//! Parent compositor.
	struct comp_d3d11_compositor *c;

	//! DXGI swapchain.
	IDXGISwapChain1 *swapchain;

	//! Current render target view (for current back buffer).
	ID3D11RenderTargetView *rtv;

	//! Current back buffer texture.
	ID3D11Texture2D *back_buffer;

	//! Window handle.
	HWND hwnd;

	//! Current dimensions.
	uint32_t width;
	uint32_t height;

	//! Swapchain creation flags — DXGI requires ResizeBuffers to pass the
	//! exact same flags or it fails with E_INVALIDARG (#848; hit whenever
	//! the late-weave FRAME_LATENCY_WAITABLE_OBJECT chain is resized).
	UINT swapchain_flags;

	//! Current back buffer index.
	uint32_t current_index;

	//! DirectComposition resources (transparent path only — null on default path).
	//! On the transparent path the swapchain is created via
	//! CreateSwapChainForComposition (HWND-less) and bound to the app's HWND
	//! through DComp instead of via DXGI, so DWM can blend per-pixel alpha.
	IDCompositionDevice *dcomp_device;
	IDCompositionTarget *dcomp_target;
	IDCompositionVisual *dcomp_visual;
};

// Access compositor internals
extern "C" {
struct comp_d3d11_compositor_internals
{
	struct xrt_compositor_native base;
	struct xrt_device *xdev;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	IDXGIFactory4 *dxgi_factory;
};
}

static inline struct comp_d3d11_compositor_internals *
get_internals(struct comp_d3d11_compositor *c)
{
	return reinterpret_cast<struct comp_d3d11_compositor_internals *>(c);
}

static xrt_result_t
create_rtv(struct comp_d3d11_target *target)
{
	auto internals = get_internals(target->c);

	// Release existing RTV and back buffer
	if (target->rtv != nullptr) {
		target->rtv->Release();
		target->rtv = nullptr;
	}
	if (target->back_buffer != nullptr) {
		target->back_buffer->Release();
		target->back_buffer = nullptr;
	}

	// Get back buffer
	HRESULT hr = target->swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
	                                           reinterpret_cast<void **>(&target->back_buffer));
	if (FAILED(hr)) {
		U_LOG_E("Failed to get back buffer: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create RTV
	hr = internals->device->CreateRenderTargetView(target->back_buffer, nullptr, &target->rtv);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create RTV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_target_create(struct comp_d3d11_compositor *c,
                         void *hwnd,
                         uint32_t width,
                         uint32_t height,
                         bool transparent,
                         struct comp_d3d11_target **out_target)
{
	auto internals = get_internals(c);

	comp_d3d11_target *target = new comp_d3d11_target();
	target->c = c;
	target->hwnd = static_cast<HWND>(hwnd);
	target->width = width;
	target->height = height;
	target->current_index = 0;
	target->dcomp_device = nullptr;
	target->dcomp_target = nullptr;
	target->dcomp_visual = nullptr;

	// Create swapchain.
	//
	// Default: flip-model + ALPHA_MODE_IGNORE (#163) — opaque present, no DWM bleed-through.
	// Transparent opt-in (only when an HWND was provided): flip-model +
	// ALPHA_MODE_PREMULTIPLIED via CreateSwapChainForComposition, bound to the app's
	// HWND through DirectComposition (IDCompositionTarget::SetRoot(visual)).
	// DComp gives us per-pixel alpha — no chroma-key, no disocclusion fringes, no
	// LWA_COLORKEY on the plugin side.
	// Decoupled presentation (#833): DXR_PRESENT_OPAQUE=1 keeps the DP's
	// compose-under-bg (the compositor still passes transparent=true to the
	// DP) but presents through the opaque HWND flip-model path — the baked
	// desktop makes the output opaque by construction, and an HWND flip
	// swapchain on the app's WS_EX_NOREDIRECTIONBITMAP window is eligible for
	// Hardware Independent Flip (dwm ~1 % instead of ~32 % measured on Intel
	// iGPUs; DComp visuals stay at full Composed: Flip cost regardless of
	// their alpha mode).
	const bool present_opaque = debug_get_bool_option_present_opaque();
	const bool use_transparent = transparent && hwnd != nullptr && !present_opaque;
	g_target_is_composed = use_transparent;
	g_scanout_stats_strikes = 0;
	if (transparent && hwnd != nullptr && present_opaque) {
		U_LOG_W("DXR_PRESENT_OPAQUE: HWND flip-model opaque present; DP compose-under-bg keeps "
		        "the transparent look (#833)");
	}

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	if (use_transparent) {
		desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	} else {
		// IGNORE so DWM doesn't composite the desktop through the bound HWND (#163).
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	}
	desc.Flags = 0;
	// Live-path late-weave: the waitable now goes on the COMPOSITION swapchain
	// too — live+shaped is the product default and it must be paceable. On the
	// composed chain the waitable releases at DWM *pickup* (≈1 interval before
	// scanout); the scanout-proximate second stage there is the DComp
	// compositor clock (see weave_mark), since GetFrameStatistics is
	// unreliable on composition swapchains.
	if (dxr_late_weave_enabled()) {
		desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
		if (use_transparent) {
			// bc=2 cannot realize governor depths 3–4: Present would block
			// on buffer availability, the interval never improves, and the
			// governor escalates and sticks at max. Match the opaque path.
			desc.BufferCount = 3;
		}
	}
	target->swapchain_flags = desc.Flags;

	HRESULT hr;
	if (use_transparent) {
		hr = internals->dxgi_factory->CreateSwapChainForComposition(
		    internals->device, &desc, nullptr, &target->swapchain);
		U_LOG_W("Transparent HWND opt-in: DComp + flip-model swapchain "
		        "(FLIP_DISCARD + PREMULTIPLIED, bc=%u)",
		        desc.BufferCount);
	} else {
		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
		fsDesc.Windowed = TRUE;
		hr = internals->dxgi_factory->CreateSwapChainForHwnd(
		    internals->device, target->hwnd, &desc, &fsDesc, nullptr, &target->swapchain);
	}
	if (FAILED(hr)) {
		U_LOG_E("Failed to create swapchain: 0x%08x", hr);
		delete target;
		return XRT_ERROR_D3D;
	}

	if ((desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
		IDXGISwapChain2 *sc2 = nullptr;
		if (SUCCEEDED(target->swapchain->QueryInterface(__uuidof(IDXGISwapChain2),
		                                                reinterpret_cast<void **>(&sc2)))) {
			const int lat = g_lw_gov_d3d11.base_latency();
			sc2->SetMaximumFrameLatency(lat);
			g_frame_latency_waitable = sc2->GetFrameLatencyWaitableObject();
			sc2->Release();
			U_LOG_W("Late-weave: D3D11 in-process swapchain waitable, max latency %d%s (waitable=%p)",
			        lat,
			        (lat == 1 && g_lw_gov_d3d11.auto_backoff == 1) ? " + saturation auto-backoff"
			                                                       : "",
			        g_frame_latency_waitable);
		}
	}

	// Disable Alt-Enter fullscreen toggle (HWND-bound only — composition swapchains
	// have no HWND association).
	if (!use_transparent) {
		internals->dxgi_factory->MakeWindowAssociation(target->hwnd, DXGI_MWA_NO_ALT_ENTER);
	}

	// Transparent path: bind the composition swapchain to the HWND through DComp.
	if (use_transparent) {
		hr = DCompositionCreateDevice2(
		    /*renderingDevice*/ nullptr,
		    __uuidof(IDCompositionDevice),
		    reinterpret_cast<void **>(&target->dcomp_device));
		if (FAILED(hr) || target->dcomp_device == nullptr) {
			U_LOG_E("DCompositionCreateDevice2 failed: 0x%08x", hr);
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_device->CreateTargetForHwnd(
		    target->hwnd, /*topmost*/ TRUE, &target->dcomp_target);
		if (FAILED(hr) || target->dcomp_target == nullptr) {
			U_LOG_E("IDCompositionDevice::CreateTargetForHwnd failed: 0x%08x", hr);
			target->dcomp_device->Release();
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_device->CreateVisual(&target->dcomp_visual);
		if (FAILED(hr) || target->dcomp_visual == nullptr) {
			U_LOG_E("IDCompositionDevice::CreateVisual failed: 0x%08x", hr);
			target->dcomp_target->Release();
			target->dcomp_device->Release();
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_visual->SetContent(target->swapchain);
		if (SUCCEEDED(hr)) {
			hr = target->dcomp_target->SetRoot(target->dcomp_visual);
		}
		if (SUCCEEDED(hr)) {
			hr = target->dcomp_device->Commit();
		}
		if (FAILED(hr)) {
			U_LOG_E("DComp visual setup failed: 0x%08x", hr);
			target->dcomp_visual->Release();
			target->dcomp_target->Release();
			target->dcomp_device->Release();
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}
	}

	// Create initial RTV
	xrt_result_t xret = create_rtv(target);
	if (xret != XRT_SUCCESS) {
		if (target->dcomp_visual != nullptr) target->dcomp_visual->Release();
		if (target->dcomp_target != nullptr) target->dcomp_target->Release();
		if (target->dcomp_device != nullptr) target->dcomp_device->Release();
		target->swapchain->Release();
		delete target;
		return xret;
	}

	*out_target = target;

	U_LOG_I("Created D3D11 target: %ux%u", width, height);

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_target_destroy(struct comp_d3d11_target **target_ptr)
{
	if (target_ptr == nullptr || *target_ptr == nullptr) {
		return;
	}

	comp_d3d11_target *target = *target_ptr;

	if (g_frame_latency_waitable != nullptr) {
		CloseHandle(g_frame_latency_waitable);
		g_frame_latency_waitable = nullptr;
		g_last_present_count = 0;
	}

	if (target->rtv != nullptr) {
		target->rtv->Release();
	}
	if (target->back_buffer != nullptr) {
		target->back_buffer->Release();
	}

	// Release DComp resources before the swapchain (visual holds a swapchain reference;
	// target holds the visual). DWM tears down the on-screen content when target releases.
	if (target->dcomp_visual != nullptr) {
		target->dcomp_visual->Release();
	}
	if (target->dcomp_target != nullptr) {
		target->dcomp_target->Release();
	}
	if (target->dcomp_device != nullptr) {
		target->dcomp_device->Release();
	}

	if (target->swapchain != nullptr) {
		target->swapchain->Release();
	}

	delete target;
	*target_ptr = nullptr;
}

extern "C" xrt_result_t
comp_d3d11_target_acquire(struct comp_d3d11_target *target, uint32_t *out_index)
{
	auto internals = get_internals(target->c);

	// For FLIP_DISCARD swapchain, we always render to buffer 0
	// The swapchain handles double-buffering internally
	*out_index = 0;
	target->current_index = 0;

	// Bind the render target
	internals->context->OMSetRenderTargets(1, &target->rtv, nullptr);

	// Set viewport
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(target->width);
	viewport.Height = static_cast<float>(target->height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	internals->context->RSSetViewports(1, &viewport);

	// Clear to black
	float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	internals->context->ClearRenderTargetView(target->rtv, clear_color);

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_target_bind(struct comp_d3d11_target *target)
{
	auto internals = get_internals(target->c);

	// Re-bind the render target and viewport (no clear)
	internals->context->OMSetRenderTargets(1, &target->rtv, nullptr);

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(target->width);
	viewport.Height = static_cast<float>(target->height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	internals->context->RSSetViewports(1, &viewport);
}

/*!
 * #868: pace a repaint to the panel — the wait half of weave_mark, split out so
 * it can run WITHOUT the compositor lock held.
 *
 * Deliberately does NOT wait on the frame-latency waitable: that is a SEMAPHORE,
 * and a non-presenting thread waiting on it consumes tokens the app's own frame
 * loop needs (measured on the D3D12 leg: 31.9 -> 16.9 fps with essentially zero
 * repaints issued). Scanout pacing alone is passive and costs nothing.
 */
extern "C" void
comp_d3d11_target_repaint_pace(struct comp_d3d11_target *target)
{
	// Composed chains: statistics are unusable and measured_r is 0, so BOTH
	// pacers below no-op — leaving Present(1) as the only throttle, blocking
	// inside c->mutex (which this function exists to avoid). Stats-free QPC
	// deadline instead: at most one repaint per period, paced out here, then
	// a compositor-clock alignment so the repaint weave gets the same
	// constant phase the app path gets.
	if (g_target_is_composed) {
		static uint64_t s_last_ns = 0;
		const uint64_t now_ns = os_monotonic_get_ns();
		const double period_ns = (g_lw_gov_d3d11.period_ns > 0.0) ? g_lw_gov_d3d11.period_ns : 16.7e6;
		if (s_last_ns != 0 && now_ns > s_last_ns) {
			const int64_t remain = (int64_t)(s_last_ns + (uint64_t)period_ns) - (int64_t)now_ns;
			if (remain > 0 && remain < (int64_t)(period_ns * 2.0)) {
				os_nanosleep(remain);
			}
		}
		s_last_ns = os_monotonic_get_ns();
		late_weave_wait_compositor_clock(period_ns);
		return;
	}

	if (g_frame_latency_waitable != nullptr && g_scanout_stats_strikes >= 0) {
		const UINT relax = (UINT)(g_lw_gov_d3d11.effective - 1);
		if (g_last_present_count > relax) {
			late_weave_wait_scanout(target->swapchain, g_last_present_count - relax,
			                        g_lw_gov_d3d11.period_ns, g_weave_latency_d3d11.freq());
		}
	}

	/*
	 * #884: LATE-weave, not early-weave (ported from comp_vk_native_target).
	 *
	 * The scanout wait above returns just after the PREVIOUS frame hit glass
	 * — the START of a refresh period. Weaving there samples the eyes a whole
	 * period before the pixels are scanned out, which is exactly the staleness
	 * the repaint exists to remove. The next scanout is one period away and
	 * measured_r_ns is how long a weave takes to reach glass, so the last safe
	 * moment to begin is period - R.
	 *
	 * Clamped hard: a quarter-period floor is always kept in hand, and with no
	 * measurement yet (R = 0) we weave immediately rather than guess. A repaint
	 * that arrives late is a dropped repaint; an early one is merely stale —
	 * never gamble the frame.
	 */
	const double period_ns = g_lw_gov_d3d11.period_ns;
	const double residual_ns = (double)g_weave_latency_d3d11.measured_r_ns;
	if (period_ns <= 0.0 || residual_ns <= 0.0) {
		return; // unmeasured — weave now rather than guess
	}
	double slack_ns = period_ns - residual_ns;
	const double floor_ns = period_ns * 0.25; // never cut it finer than this
	if (slack_ns > period_ns - floor_ns) {
		slack_ns = period_ns - floor_ns;
	}
	if (slack_ns <= 0.0) {
		return; // the weave already fills the period; go now
	}
	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W("#884: repaint late-weave pacing engaged (period %.1f ms, weave residual %.1f ms, "
		        "sleeping %.1f ms)",
		        period_ns / 1e6, residual_ns / 1e6, slack_ns / 1e6);
	}
	os_nanosleep((int64_t)slack_ns);
}

/*!
 * #868: stamp T_weave for a repaint and nothing else — no governor EMA (repaints
 * land a panel period apart by construction and would collapse the saturation
 * signal) and no predicted display time (xrWaitFrame promised no photon time for
 * this present, so there is nothing to score it against).
 */
extern "C" void
comp_d3d11_target_weave_mark_repaint(struct comp_d3d11_target *target)
{
	(void)target;
	g_weave_latency_d3d11.mark_weave("d3d11", 0, true);
	// Each repaint's Present releases a waitable token nobody waits for; the
	// app's weave_mark drains the excess on its next frame (#868 interplay).
	g_repaint_presents_since_app.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void
comp_d3d11_target_weave_mark(struct comp_d3d11_target *target, uint64_t predicted_display_time_ns)
{
	// Late-weave pacing, two stages. Stage 1: the waitable caps the queue.
	// Stage 2 is chain-dependent — opaque flip chains wait on frame
	// statistics until the previous present actually reaches glass (DWM
	// pickup releases the waitable 2-3 frames early); composed chains have
	// unreliable statistics, so they align to the DComp compositor clock
	// instead. Bounded everywhere so occluded windows never wedge.
	bool wait_timed_out = false;
	if (g_frame_latency_waitable != nullptr) {
		// #868 interplay: repaint presents released tokens no one consumed.
		// Left alone, the wait below returns instantly for that many frames
		// — pacing loss after every idle stretch. Drain the surplus first;
		// the blocking wait then re-syncs to the real present cadence.
		const uint32_t rp = g_repaint_presents_since_app.exchange(0, std::memory_order_relaxed);
		if (rp > 0) {
			uint32_t drained = 0;
			while (drained < rp + LATE_WEAVE_MAX_DEPTH &&
			       WaitForSingleObjectEx(g_frame_latency_waitable, 0, FALSE) == WAIT_OBJECT_0) {
				drained++;
			}
			static bool logged_drain = false;
			if (!logged_drain && drained > 2) {
				logged_drain = true;
				U_LOG_W("Late-weave: drained %u surplus waitable tokens after repaints "
				        "(#868 interplay; logged once)",
				        drained);
			}
		}
		const uint64_t wait_t0 = os_monotonic_get_ns();
		wait_timed_out = WaitForSingleObject(g_frame_latency_waitable, 100) == WAIT_TIMEOUT;
		const uint64_t wait_t1 = os_monotonic_get_ns();
		const bool wait_blocked = (wait_t1 - wait_t0) > 2000000; // >2 ms
		// Catch-up guard: if the LAST frame overran its period the pipeline
		// is already a vsync behind — tick-aligning now waits for yet
		// another tick and turns one late pickup into a dropped frame
		// (measured on the VK bridge: align-always put 20% of frames at
		// 2 vsyncs, 60 -> 51 fps, identical per-frame GPU cost). Skip the
		// align once; the queue slack absorbs the late frame and the next
		// on-time frame re-aligns.
		static uint64_t s_last_pace_done_ns;
		const double cu_period_ns =
		    (g_lw_gov_d3d11.period_ns > 0.0) ? g_lw_gov_d3d11.period_ns : 16.7e6;
		const bool running_late = s_last_pace_done_ns != 0 &&
		                          (double)(wait_t1 - s_last_pace_done_ns) > cu_period_ns * 1.15;

		if (g_target_is_composed) {
			// Composed chain: the weave should start at a constant, small
			// offset after a compositor tick. A BLOCKING waitable release
			// already lands at a tick (DWM pickup is tick-aligned) — a
			// clock wait on top would add a whole period (measured: frame
			// interval 2x period, spurious governor backoff). Only when
			// the wait returned instantly (banked token, unknown phase)
			// does the clock alignment buy the constant phase.
			static bool logged_src = false;
			bool clocked = false;
			if (!wait_blocked && !wait_timed_out && !running_late && g_lw_gov_d3d11.align_ok()) {
				clocked = late_weave_wait_compositor_clock(g_lw_gov_d3d11.period_ns);
			}
			if (!logged_src) {
				logged_src = true;
				U_LOG_W("Late-weave: live-path stage-2 source = %s",
				        (clocked || wait_blocked) ? "DComp compositor clock / tick-aligned pickup"
				                                  : "none (waitable only)");
			}
		} else if (g_scanout_stats_strikes >= 0) {
			// At effective depth L>1 (#850) the pacer targets the present
			// L-1 back instead, restoring L-1 frames of CPU/GPU overlap.
			const UINT relax = (UINT)(g_lw_gov_d3d11.effective - 1);
			if (g_last_present_count > relax) {
				const enum late_weave_scanout_result r = late_weave_wait_scanout(
				    target->swapchain, g_last_present_count - relax,
				    g_lw_gov_d3d11.period_ns, g_weave_latency_d3d11.freq());
				// Behavioral trust gate: stats that succeed but never
				// advance burn the full 3-period bound every frame —
				// declare them unusable after a few strikes.
				if (r == LATE_WEAVE_SCANOUT_TIMED_OUT) {
					if (++g_scanout_stats_strikes >= 8) {
						g_scanout_stats_strikes = -1;
						U_LOG_W("Late-weave: frame statistics stale on this "
						        "chain — stage-2 scanout wait disabled");
					}
				} else if (r == LATE_WEAVE_SCANOUT_REACHED) {
					g_scanout_stats_strikes = 0;
				}
			}
		}
		s_last_pace_done_ns = os_monotonic_get_ns();
	}
	g_weave_latency_d3d11.mark_weave("d3d11", predicted_display_time_ns);
	if (wait_timed_out) {
		// DWM stopped pickup (occlusion/minimize) — not saturation. Without
		// this the ~100 ms timeout is below the governor's 250 ms pause
		// threshold and reads as a saturated pipeline, pinning max depth.
		g_lw_gov_d3d11.on_wait_timeout();
	}
	if (g_frame_latency_waitable != nullptr) {
		const int tr = g_lw_gov_d3d11.on_mark(g_weave_latency_d3d11.freq());
		if (tr != 0) {
			IDXGISwapChain2 *sc2 = nullptr;
			if (SUCCEEDED(target->swapchain->QueryInterface(
			        __uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&sc2)))) {
				sc2->SetMaximumFrameLatency((UINT)g_lw_gov_d3d11.effective);
				sc2->Release();
			}
			static bool logged_backoff = false, logged_return = false;
			bool &logged = (tr > 0) ? logged_backoff : logged_return;
			if (!logged) {
				logged = true;
				U_LOG_W("Late-weave: saturation %s -> max latency %d "
				        "(frame interval %.1f ms vs display period %.1f ms)",
				        tr > 0 ? "backoff engaged" : "cleared, probing return",
				        g_lw_gov_d3d11.effective, g_lw_gov_d3d11.interval_ema_ns / 1e6,
				        g_lw_gov_d3d11.period_ns / 1e6);
			}
		}
	}
}

extern "C" void
comp_d3d11_target_mark_wait_frame(struct comp_d3d11_target *target)
{
	(void)target;
	g_lw_gov_d3d11.on_wait_frame();
}

extern "C" uint64_t
comp_d3d11_target_get_predicted_lookahead_ns(struct comp_d3d11_target *target)
{
	(void)target;
	return g_lw_gov_d3d11.predicted_lookahead_ns(g_weave_latency_d3d11.measured_r_ns);
}

extern "C" void
comp_d3d11_target_set_display_period(struct comp_d3d11_target *target, uint64_t period_ns)
{
	(void)target;
	// Seed the governor from the compositor's queried refresh rate so the
	// first frames judge saturation correctly; DXGI frame statistics refine
	// it from there.
	if (period_ns > 0 && g_lw_gov_d3d11.period_ns == 0.0) {
		g_lw_gov_d3d11.period_ns = (double)period_ns;
	}
}

extern "C" uint64_t
comp_d3d11_target_get_measured_weave_ns(struct comp_d3d11_target *target)
{
	(void)target;
	return g_weave_latency_d3d11.measured_r_ns;
}

extern "C" xrt_result_t
comp_d3d11_target_present(struct comp_d3d11_target *target, uint32_t sync_interval)
{
	HRESULT hr = target->swapchain->Present(sync_interval, 0);
	g_weave_latency_d3d11.after_present("d3d11", target->swapchain, &g_lw_gov_d3d11);
	if (SUCCEEDED(hr) && g_frame_latency_waitable != nullptr) {
		target->swapchain->GetLastPresentCount(&g_last_present_count);
	}
	if (FAILED(hr)) {
		U_LOG_E("Present failed: 0x%08x", hr);

		// Check for device removed
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
			return XRT_ERROR_D3D;
		}
		return XRT_ERROR_D3D;
	}

	// DComp path: publish the new frame to dwm.exe. Cheap — IPC of delta state, no GPU work.
	if (target->dcomp_device != nullptr) {
		target->dcomp_device->Commit();
	}

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_target_get_dimensions(struct comp_d3d11_target *target,
                                 uint32_t *out_width,
                                 uint32_t *out_height)
{
	*out_width = target->width;
	*out_height = target->height;
}

extern "C" void *
comp_d3d11_target_get_back_buffer(struct comp_d3d11_target *target)
{
	if (target == nullptr) {
		return nullptr;
	}
	return target->back_buffer;
}

extern "C" xrt_result_t
comp_d3d11_target_resize(struct comp_d3d11_target *target,
                         uint32_t width,
                         uint32_t height)
{
	if (width == target->width && height == target->height) {
		return XRT_SUCCESS;
	}

	auto internals = get_internals(target->c);

	// Release current back buffer and RTV
	internals->context->OMSetRenderTargets(0, nullptr, nullptr);
	if (target->rtv != nullptr) {
		target->rtv->Release();
		target->rtv = nullptr;
	}
	if (target->back_buffer != nullptr) {
		target->back_buffer->Release();
		target->back_buffer = nullptr;
	}

	// Resize swapchain. Must pass the creation flags — DXGI returns
	// E_INVALIDARG if they differ (#848, waitable late-weave chains).
	HRESULT hr = target->swapchain->ResizeBuffers(0, width, height,
	                                               DXGI_FORMAT_UNKNOWN,
	                                               target->swapchain_flags);
	if (FAILED(hr)) {
		U_LOG_E("Failed to resize swapchain: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	target->width = width;
	target->height = height;

	// Recreate RTV
	return create_rtv(target);
}
