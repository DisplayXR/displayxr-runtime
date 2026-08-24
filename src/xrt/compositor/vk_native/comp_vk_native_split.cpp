// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  VK-1 — the in-process Vulkan output-device split (#918 / #1178).
 * @ingroup comp_vk_native
 *
 * Topology, the ordering contract and what this rung deliberately leaves to VK-2
 * are all stated in comp_vk_native_split.h — read that first.
 */

#include "comp_vk_native_split.h"
#include "comp_vk_native_deposit.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_display_processor_d3d11.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_tiling.h"

#include "os/os_time.h"
#include "util/u_time.h"

#include <string.h>

/*
 * The split is a Windows mechanism end to end (a D3D11 device on the scanout
 * adapter, a DXGI swapchain, the D3D12 cross-adapter transport behind
 * comp_xbridge). Everything below compiles to nothing elsewhere; the header keeps
 * its signatures so the one caller needs no per-platform spelling beyond the
 * guard it already has.
 *
 * XRT_HAVE_D3D11 as well as the OS: comp_xbridge and comp_d3d11 are themselves
 * built only when it is on, so without it there is nothing to link against and
 * the honest answer is the same one every non-Windows build gives.
 */
#include "xrt/xrt_config_have.h"

#if defined(XRT_OS_WINDOWS) && defined(XRT_HAVE_D3D11)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>

#include "comp_split_gate.h"
#include "comp_xbridge.h"
#include "d3d11/comp_d3d11_target.h"
#include "d3d/d3d_scanout_helpers.hpp"


/*
 *
 * Struct.
 *
 */

struct comp_vk_split
{
	struct xrt_device *xdev;
	HWND hwnd;
	void *dp_factory_d3d11;
	bool transparent_background;

	//! @name The SCANOUT adapter — everything the weave and the present touch.
	//! @{
	ID3D11Device *out_dev;
	ID3D11DeviceContext *out_ctx;
	IDXGIFactory4 *out_factory;
	struct comp_d3d11_target *target;
	struct xrt_display_processor_d3d11 *dp;
	LUID out_luid;
	WCHAR out_desc[128];
	//! @}

	//! @name The RENDER adapter — the deposit's own D3D11 device, borrowed.
	//! @{
	ID3D11Device *app_dev;
	ID3D11DeviceContext *app_ctx;
	//! `Wait` / `Signal` live here; NULL on a stack without the interface.
	ID3D11DeviceContext4 *app_ctx4;
	IDXGIAdapter *app_adapter;
	LUID render_luid;
	//! @}

	struct comp_xbridge *xbridge;

	//! Panel extent, for the placement line and the bridge's plane sizing.
	uint32_t panel_w, panel_h;

	//! Monotonic app-frame counter — the one sequence every bridge fence uses.
	uint64_t seq;

	/*!
	 * #918 R1 — the layout generation stamped on every slot, and the geometry
	 * snapshot minted WITH it (#1140). The weave reads its tile grid from the
	 * snapshot, never from live CPU state, because the slot it is weaving was
	 * filled by an earlier frame.
	 */
	uint64_t layout_gen;
	uint64_t layout_sig;
	uint32_t gen_cols, gen_rows, gen_view_w, gen_view_h;

	//! The crop the OUTPUT device owes when R2 hysteresis holds a worst-case ring.
	ID3D11Texture2D *dp_input_tex;
	ID3D11ShaderResourceView *dp_input_srv;
	uint32_t dp_input_w, dp_input_h;

	//! @name Counters for the `[RENDER]` line and the F4/R1 tripwires.
	//! @{
	uint64_t no_slot;
	uint64_t out_crop;
	uint64_t stale_refusals;
	uint64_t inflight_weaves;
	uint64_t diag_window_ns;
	//! @}
};


/*
 *
 * Helpers.
 *
 */

//! windows.h-free mirror, for @ref comp_split_gate_inputs.
static struct comp_split_luid
split_luid(LUID l)
{
	struct comp_split_luid out = {};
	out.low = (uint32_t)l.LowPart;
	out.high = (int32_t)l.HighPart;
	return out;
}

static LUID
split_luid_from_packed(uint64_t packed)
{
	LUID l = {};
	memcpy(&l, &packed, sizeof(l));
	return l;
}

/*!
 * `DXR_TEST_FAKE_DP_REFUSE=1` — the VENDOR-REFUSAL arm of the fallback matrix.
 *
 * Same shape and same name as the two D3D in-process legs deliberately: it fires
 * ONLY on the scanout-adapter create, so the app-device fallback is allowed to
 * succeed and what gets walked is the RECOVERY. Latched on first call.
 */
static bool
split_test_fake_dp_refuse(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *e = getenv("DXR_TEST_FAKE_DP_REFUSE");
		cached = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return cached == 1;
}

/*!
 * ADR-037 §3a — ask the plug-in for a weaver on the scanout adapter.
 *
 * The single creation point, so no weaver can ever be stranded on the HWND: it
 * either succeeds and becomes the session's weaver, or nothing was created at all
 * and the caller falls through to the Vulkan factory on the app device.
 */
static bool
split_make_dp(struct comp_vk_split *s)
{
	if (s->dp_factory_d3d11 == NULL) {
		U_LOG_W("VK output-device split: no D3D11 display-processor factory — the scanout adapter "
		        "has nothing to weave with (#1178)");
		return false;
	}

	if (split_test_fake_dp_refuse()) {
		U_LOG_W("DXR_TEST_FAKE_DP_REFUSE=1: refusing the display processor on the SCANOUT device (test)");
		s->dp = NULL;
		return false;
	}

	auto factory = (xrt_dp_factory_d3d11_fn_t)s->dp_factory_d3d11;
	xrt_result_t dp_ret = factory(s->out_dev, s->out_ctx, s->hwnd, &s->dp);
	if (dp_ret != XRT_SUCCESS || s->dp == NULL) {
		U_LOG_W("VK output-device split: D3D11 display processor factory failed (error %d) on the "
		        "SCANOUT device",
		        (int)dp_ret);
		s->dp = NULL;
		return false;
	}

	U_LOG_W("VK output-device split: D3D11 display processor created on the SCANOUT device (hwnd %p)",
	        (void *)s->hwnd);

	// Session settings that must follow the weaver to whichever device it
	// landed on. `client_presents` is false: under the split the RUNTIME
	// presents, on the scanout adapter.
	xrt_display_processor_d3d11_set_transparent_background(s->dp, s->transparent_background,
	                                                       /*client_presents=*/false);
	// A shared-texture session is refused by the gate before we get here, so
	// this is always false — stated rather than assumed.
	xrt_display_processor_d3d11_set_shared_texture_present(s->dp, false);
	return true;
}

//! Release the output half. Idempotent; leaves the borrowed app end alone.
static void
split_release_out(struct comp_vk_split *s)
{
	if (s->xbridge != NULL) {
		comp_xbridge_quiesce(s->xbridge);
		comp_xbridge_destroy(&s->xbridge);
	}
	if (s->dp_input_srv != NULL) {
		s->dp_input_srv->Release();
		s->dp_input_srv = NULL;
	}
	if (s->dp_input_tex != NULL) {
		s->dp_input_tex->Release();
		s->dp_input_tex = NULL;
	}
	s->dp_input_w = 0;
	s->dp_input_h = 0;
	/*
	 * The weaver dies BEFORE its device, and before the target it was writing
	 * into is gone — the (hwnd, device) bind key means a weaver left alive here
	 * would silently refuse the app-device create that follows a retire.
	 */
	xrt_display_processor_d3d11_destroy(&s->dp);
	comp_d3d11_target_destroy(&s->target);
	if (s->out_factory != NULL) {
		s->out_factory->Release();
		s->out_factory = NULL;
	}
	if (s->out_ctx != NULL) {
		s->out_ctx->Release();
		s->out_ctx = NULL;
	}
	if (s->out_dev != NULL) {
		s->out_dev->Release();
		s->out_dev = NULL;
	}
	if (s->app_ctx4 != NULL) {
		s->app_ctx4->Release();
		s->app_ctx4 = NULL;
	}
	// app_dev / app_ctx / app_adapter are the DEPOSIT's and are borrowed.
	s->app_dev = NULL;
	s->app_ctx = NULL;
	s->app_adapter = NULL;
}


/*
 *
 * Stage A.
 *
 */

extern "C" xrt_result_t
comp_vk_split_stage_a(const struct comp_vk_split_info *info,
                      struct comp_vk_split **out_split,
                      const char **out_short_reason)
{
	*out_split = NULL;
	*out_short_reason = COMP_SPLIT_REASON_KILLED_BY_ENV;

	if (!comp_split_gate_env_requested()) {
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	const uint64_t stage_a_start_ns = os_monotonic_get_ns();

	/*
	 * The gate's inputs are gathered in the ORIGINAL order and nothing is
	 * resolved past the first refusal — an ineligible session must not go near
	 * getScanoutAdapter, and a session whose own adapter would not resolve has
	 * nothing to compare a scanout adapter against.
	 */
	struct comp_split_gate_inputs gin = {};
	gin.requested = true;

	const uint32_t panel_w = (info->xdev != NULL && info->xdev->hmd != NULL)
	                             ? info->xdev->hmd->screens[0].w_pixels
	                             : 0;
	const uint32_t panel_h = (info->xdev != NULL && info->xdev->hmd != NULL)
	                             ? info->xdev->hmd->screens[0].h_pixels
	                             : 0;

	if (!info->app_timeline_semaphores) {
		/*
		 * Checked FIRST and before any adapter work: without
		 * VK_KHR_timeline_semaphore the deposit cannot exist, and the runtime
		 * cannot turn the feature on after the app created its device. See the
		 * token's own doc comment for why this is not `api_unsupported`.
		 */
		gin.ineligible_reason = COMP_SPLIT_REASON_NO_TIMELINE_SEMAPHORE;
	} else if (info->hwnd == NULL) {
		gin.ineligible_reason = COMP_SPLIT_REASON_NO_HWND;
	} else if (info->has_shared_texture) {
		gin.ineligible_reason = COMP_SPLIT_REASON_SHARED_TEXTURE;
	} else if (panel_w == 0 || panel_h == 0) {
		gin.ineligible_reason = COMP_SPLIT_REASON_NO_PANEL_DIMS;
	} else if (info->render_packed_luid == 0) {
		// The driver would not report a device LUID, so there is nothing to
		// compare the scanout adapter against — and the deposit could not
		// LUID-match a D3D11 device either.
		gin.ineligible_reason = COMP_SPLIT_REASON_RENDER_UNRESOLVABLE;
	}

	const LUID render_luid = split_luid_from_packed(info->render_packed_luid);

	wil::com_ptr<IDXGIAdapter> scanout;
	DXGI_ADAPTER_DESC sdesc{};
	if (gin.ineligible_reason == nullptr) {
		scanout = xrt::auxiliary::d3d::getScanoutAdapter(info->display_screen_left, info->display_screen_top,
		                                                 panel_w, panel_h, U_LOGGING_INFO);
		gin.scanout_resolved = scanout && SUCCEEDED(scanout->GetDesc(&sdesc));
		gin.render_luid = split_luid(render_luid);
		gin.scanout_luid = split_luid(sdesc.AdapterLuid);
	}

	struct comp_split_gate_result gate = {};
	comp_split_gate_evaluate(&gin, &gate);
	*out_short_reason = gate.short_reason;

	if (gate.same_adapter) {
		// Not a failure: on a MUX'd / single-GPU box the weave is already
		// local, so the split has nothing to do. One line, no WARN storm.
		U_LOG_W("VK output-device split: scanout adapter '%ls' LUID=%08lx:%08lx IS the app's adapter — "
		        "split is a no-op (#918)",
		        sdesc.Description, (unsigned long)sdesc.AdapterLuid.HighPart,
		        (unsigned long)sdesc.AdapterLuid.LowPart);
	}
	if (!gate.split_active) {
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	struct comp_vk_split *s = U_TYPED_CALLOC(struct comp_vk_split);
	if (s == NULL) {
		*out_short_reason = COMP_SPLIT_REASON_STAGE_A_FAILED;
		return XRT_ERROR_ALLOCATION;
	}
	s->xdev = info->xdev;
	s->hwnd = (HWND)info->hwnd;
	s->dp_factory_d3d11 = info->dp_factory_d3d11;
	s->transparent_background = info->transparent_background;
	s->render_luid = render_luid;
	s->out_luid = sdesc.AdapterLuid;
	s->panel_w = panel_w;
	s->panel_h = panel_h;
	wcsncpy_s(s->out_desc, sdesc.Description, _TRUNCATE);

	const char *reason = nullptr;
	const char *stage_a_token = nullptr;

	// Runtime-owned D3D11 device on the scanout adapter.
	{
		UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		D3D_FEATURE_LEVEL got = {};
		static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
		HRESULT hr = D3D11CreateDevice(scanout.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
		                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &s->out_dev, &got, &s->out_ctx);
		if (FAILED(hr) || s->out_dev == nullptr || s->out_ctx == nullptr) {
			U_LOG_W("VK output-device split: D3D11CreateDevice failed 0x%08lx", (unsigned long)hr);
			reason = "D3D11CreateDevice failed";
		}
	}
	if (reason == nullptr) {
		// The output context has one caller at a time (layer_commit and the
		// repaint loop, both under the compositor mutex), but the vendor DP may
		// touch it from its own threads.
		ID3D10Multithread *omt = nullptr;
		if (SUCCEEDED(s->out_dev->QueryInterface(__uuidof(ID3D10Multithread), (void **)&omt)) &&
		    omt != nullptr) {
			omt->SetMultithreadProtected(TRUE);
			omt->Release();
		}
		// Same one-deep DXGI queue the app device gets: the present happens
		// HERE now, so this is where the pacing lever lives.
		IDXGIDevice1 *od1 = nullptr;
		if (SUCCEEDED(s->out_dev->QueryInterface(__uuidof(IDXGIDevice1), (void **)&od1)) && od1 != nullptr) {
			od1->SetMaximumFrameLatency(1);
			od1->Release();
		}
		HRESULT hr = scanout->GetParent(__uuidof(IDXGIFactory4), (void **)&s->out_factory);
		if (FAILED(hr) || s->out_factory == nullptr) {
			U_LOG_W("VK output-device split: scanout DXGI factory failed 0x%08lx", (unsigned long)hr);
			reason = "scanout DXGI factory unavailable";
		}
	}

	/*
	 * ADR-037 §3a — ASK THE PLUG-IN BEFORE THE SPLIT COMMITS (the D3D11 leg's
	 * #1169 design, taken over D3D12's after-the-fact retire).
	 *
	 * The runtime picks the adapters; it does not get to assume the vendor
	 * plug-in can weave on the one it picked. Asking here — before the target
	 * exists and before the caller has skipped its Vulkan weaver — means a
	 * refusal is just another Stage-A failure, and the teardown below is the
	 * whole recovery. No retire path, no half-built state to unwind.
	 *
	 * Cost is zero: this is the display-processor create the session was going
	 * to pay for regardless. It is MOVED, not added.
	 */
	if (reason == nullptr && !split_make_dp(s)) {
		reason = "the display processor declined a weaver on the scanout adapter";
		stage_a_token = COMP_SPLIT_REASON_DP_REFUSED_SCANOUT;
	}

	// The DXGI swapchain, on the scanout adapter. This is what removes the
	// cross-adapter present the whole epic is about.
	if (reason == nullptr) {
		xrt_result_t xret = comp_d3d11_target_create(/*c=*/nullptr, s->hwnd, s->out_dev, s->out_ctx,
		                                             s->out_factory, info->preferred_width,
		                                             info->preferred_height, info->transparent_background,
		                                             &s->target);
		if (xret != XRT_SUCCESS || s->target == nullptr) {
			U_LOG_W("VK output-device split: could not create the scanout-adapter swapchain (%d)",
			        (int)xret);
			reason = "scanout swapchain creation failed";
		}
	}

	if (reason != nullptr) {
		split_release_out(s);
		free(s);
		*out_short_reason = (stage_a_token != nullptr) ? stage_a_token : COMP_SPLIT_REASON_STAGE_A_FAILED;
		U_LOG_W("VK output-device split DISABLED (%s) — falling back to the stock single-device Vulkan "
		        "path (#918)",
		        reason);
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	s->diag_window_ns = os_monotonic_get_ns();

	*out_split = s;
	*out_short_reason = nullptr;
	U_LOG_W("VK output-device split: stage A1 took %.1f ms (device+DP+swapchain on the scanout adapter)",
	        (double)(os_monotonic_get_ns() - stage_a_start_ns) / 1.0e6);
	return XRT_SUCCESS;
}

extern "C" bool
comp_vk_split_wire_bridge(struct comp_vk_split *s, const struct comp_vk_deposit_handoff *handoff)
{
	if (s == nullptr || handoff == nullptr || handoff->d3d11_device == nullptr) {
		U_LOG_W("VK output-device split: no deposit to bridge from (#1178)");
		return false;
	}

	const uint64_t t0 = os_monotonic_get_ns();

	s->app_dev = (ID3D11Device *)handoff->d3d11_device;
	s->app_ctx = (ID3D11DeviceContext *)handoff->d3d11_context;
	s->app_adapter = (IDXGIAdapter *)handoff->dxgi_adapter;

	/*
	 * The GPU-side ordering handle. Without it there is no way to make the
	 * bridge's read wait for Vulkan's write except a CPU wait, which this rung
	 * may not take — so its absence fails the wiring rather than degrading it.
	 */
	if (FAILED(s->app_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&s->app_ctx4)) ||
	    s->app_ctx4 == nullptr) {
		U_LOG_W("VK output-device split: the deposit's device has no ID3D11DeviceContext4, so the "
		        "bridge cannot GPU-wait on the atlas fence (#1178)");
		s->app_ctx4 = nullptr;
		return false;
	}

	// Worst-case system atlas — the cross-adapter ring is sized for this once,
	// so a mode switch never reallocates the heap mid-flight.
	uint32_t sys_w = 0, sys_h = 0;
	if (s->xdev != nullptr && s->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(s->xdev->rendering_modes, s->xdev->rendering_mode_count, &sys_w,
		                              &sys_h);
	}
	if (sys_w == 0 || sys_h == 0) {
		sys_w = handoff->width;
		sys_h = handoff->height;
	}

	struct comp_xbridge_info xbi = {};
	/*
	 * D3D11 ENDS — and this is the whole reason VK-0 exists. The deposit made
	 * the Vulkan atlas an NT-shared ID3D11Texture2D with its own device,
	 * context and adapter, which is exactly what this flavour consumes. No
	 * third ends flavour, no VK<->D3D12 external memory.
	 */
	xbi.d3d12_ends = false;
	xbi.app_device = s->app_dev;
	xbi.app_context = s->app_ctx;
	xbi.app_adapter = s->app_adapter;
	xbi.out_device = s->out_dev;
	xbi.out_context = s->out_ctx;
	xbi.out_adapter = nullptr;
	xbi.max_width = sys_w;
	xbi.max_height = sys_h;
	xbi.panel_width = s->panel_w;
	xbi.panel_height = s->panel_h;

	// The bridge's out_adapter is used for its own consumer D3D12 device, so it
	// must be the scanout adapter the out device lives on.
	IDXGIDevice *od = nullptr;
	IDXGIAdapter *out_adapter = nullptr;
	if (SUCCEEDED(s->out_dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&od)) && od != nullptr) {
		od->GetAdapter(&out_adapter);
		od->Release();
	}
	xbi.out_adapter = out_adapter;

	const char *xb_reason = nullptr;
	bool ok = (comp_xbridge_create(&xbi, &s->xbridge, &xb_reason) == XRT_SUCCESS);
	if (!ok) {
		s->xbridge = nullptr;
		U_LOG_W("VK output-device split: bridge creation failed (%s)",
		        xb_reason != nullptr ? xb_reason : "cross-adapter heap unsupported");
	}

	if (ok) {
		/*
		 * INGRESS OPTION II, chosen up front and not as a fallback.
		 *
		 * Two independent reasons, either sufficient. First, the deposit is a
		 * RING: the source texture's identity alternates every app frame, which
		 * is precisely the caller shape `force_staged_ingress` documents (an
		 * Option-I caller would re-open a shared handle per frame and drain the
		 * producer each time).
		 *
		 * Second, and the one that is not negotiable: Option II's staging copy
		 * runs on the APP IMMEDIATE CONTEXT inside comp_xbridge_submit. That is
		 * the only context this leg can order against Vulkan's timeline signal,
		 * and it is what lets the deposit slot be RELEASED back to Vulkan
		 * immediately after the submit returns. Under Option I the producer's
		 * own copy queue reads the slot at a time no app-context signal can
		 * bound, so the ring's back-pressure would have nothing to hang off.
		 */
		if (!comp_xbridge_force_staged_ingress(s->xbridge)) {
			U_LOG_W("VK output-device split: the ingress staging ring could not be allocated");
			ok = false;
		}
	}

	if (ok) {
		/*
		 * The egress ring is allocated HERE, not on the frame path: the split
		 * must not be able to reach its first weave and only then discover it
		 * has nothing to weave into. Sized at the ACTIVE mode's nominal content
		 * box rather than the worst-case atlas, so the session warmup does not
		 * commit (and immediately free) a large scanout-adapter allocation.
		 */
		uint32_t eg_w = 0, eg_h = 0;
		if (s->xdev != nullptr && s->xdev->hmd != nullptr) {
			uint32_t mi = s->xdev->hmd->active_rendering_mode_index;
			if (mi < s->xdev->rendering_mode_count) {
				const struct xrt_rendering_mode *m = &s->xdev->rendering_modes[mi];
				eg_w = m->tile_columns * m->view_width_pixels;
				eg_h = m->tile_rows * m->view_height_pixels;
			}
		}
		if (eg_w > 0 && eg_h > 0) {
			// Falls back to a worst-case ring internally if this fails.
			comp_xbridge_set_content_size(s->xbridge, eg_w, eg_h, 0);
			uint32_t gw = 0, gh = 0;
			comp_xbridge_get_egress_dims(s->xbridge, &gw, &gh);
			ok = (gw > 0 && gh > 0);
		} else {
			ok = comp_xbridge_alloc_worstcase_egress(s->xbridge);
		}
		if (!ok) {
			U_LOG_W("VK output-device split: egress share failed");
		}
	}

	if (out_adapter != nullptr) {
		out_adapter->Release();
	}

	if (!ok) {
		return false;
	}

	U_LOG_W("VK output-device split ACTIVE: weave/repaint/present move to '%ls' LUID=%08lx:%08lx (app "
	        "device on LUID=%08lx:%08lx); the VK-0 D3D11 deposit crosses via a D3D12 cross-adapter heap "
	        "once per frame (#918 VK-1)",
	        s->out_desc, (unsigned long)s->out_luid.HighPart, (unsigned long)s->out_luid.LowPart,
	        (unsigned long)s->render_luid.HighPart, (unsigned long)s->render_luid.LowPart);
	U_LOG_W("VK output-device split: stage A2 (bridge) took %.1f ms",
	        (double)(os_monotonic_get_ns() - t0) / 1.0e6);
	return true;
}

extern "C" void
comp_vk_split_destroy(struct comp_vk_split **split_ptr)
{
	struct comp_vk_split *s = *split_ptr;
	if (s == nullptr) {
		return;
	}
	*split_ptr = nullptr;

	U_LOG_W("VK output-device split: %llu frames bridged, %llu weaves with no usable slot, %llu stale-slot "
	        "refusals, %llu in-flight (transition) weaves, %llu output-device crops (#918)",
	        (unsigned long long)s->seq, (unsigned long long)s->no_slot,
	        (unsigned long long)s->stale_refusals, (unsigned long long)s->inflight_weaves,
	        (unsigned long long)s->out_crop);

	split_release_out(s);
	free(s);
}

extern "C" void
comp_vk_split_retire(struct comp_vk_split **split_ptr, const char *why, const char *short_reason)
{
	struct comp_vk_split *s = *split_ptr;
	if (s == nullptr) {
		return;
	}

	U_LOG_W("#918 output-device split RETIRED (%s) — moving the weave back to the app adapter for the rest "
	        "of the session (#918 VK-1)",
	        why);

	/*
	 * The placement CORRECTION, emitted before the teardown so it cannot be
	 * lost to a failure below. The contract the D3D12 leg set and this keeps:
	 * the LAST `weave placement:` line in a log is always the truth.
	 *
	 * Deliberately does NOT re-resolve the adapters — that would be a
	 * QueryDisplayConfig on the frame path with no new information to gain.
	 */
	U_LOG_W("weave placement: CHANGED — weave/present move back to the RENDER adapter for the rest of this "
	        "session (split=0 reason=%s) (#918)",
	        short_reason != nullptr ? short_reason : COMP_SPLIT_REASON_STAGE_A_FAILED);

	comp_vk_split_destroy(split_ptr);
}


/*
 *
 * Frame path.
 *
 */

extern "C" void
comp_vk_split_submit_atlas(struct comp_vk_split *s,
                           const struct comp_vk_deposit_handoff *handoff,
                           uint32_t cols,
                           uint32_t rows,
                           uint32_t view_w,
                           uint32_t view_h)
{
	if (s == nullptr || s->xbridge == nullptr || handoff == nullptr || handoff->texture == nullptr) {
		return;
	}

	/*
	 * THE ordering edge of this rung. Vulkan's atlas submit signalled the
	 * shared fence at `fence_value`; the bridge's staging copy is about to read
	 * that texture on this very context. A GPU-side wait — this thread does not
	 * block, and neither does the app's render thread.
	 */
	if (s->app_ctx4 != nullptr && handoff->fence != nullptr && handoff->fence_value != 0) {
		s->app_ctx4->Wait((ID3D11Fence *)handoff->fence, handoff->fence_value);
	}

	const uint32_t content_w = cols * view_w;
	const uint32_t content_h = rows * view_h;
	if (content_w == 0 || content_h == 0) {
		return;
	}

	/*
	 * #918 R1 — the layout GENERATION.
	 *
	 * Bumped only when the tile GRID changes, deliberately not when the tile
	 * dimensions do: those move continuously through an interactive resize, and
	 * treating every step as a mode switch would refuse every slot. A grid
	 * change is a real recipe change and a slot composited under the old one
	 * must never be woven. The geometry snapshot is minted WITH the generation
	 * (#1140) so the weave reads its grid from the slot's own era.
	 */
	const uint64_t sig = ((uint64_t)cols << 20) | (uint64_t)rows;
	if (sig != s->layout_sig) {
		s->layout_sig = sig;
		s->layout_gen++;
		s->gen_cols = cols;
		s->gen_rows = rows;
		s->gen_view_w = view_w;
		s->gen_view_h = view_h;
		U_LOG_W("#918: layout generation %llu — %ux%u tiles of %ux%u (#918 R1)",
		        (unsigned long long)s->layout_gen, cols, rows, view_w, view_h);
	} else {
		// Same recipe, possibly a new scale. The snapshot tracks the tile size
		// so a resize weaves at the size the slot was actually painted at.
		s->gen_view_w = view_w;
		s->gen_view_h = view_h;
	}

	/*
	 * CROP-BEFORE-DP (ADR-030), done by the transport itself: right-sizing the
	 * egress ring to the content box makes the consumer's copy land
	 * content-sized, so the bridge delivers CROPPED pixels and no scratch copy
	 * happens anywhere. Only the R2 hysteresis (an interactive resize whose size
	 * will not hold still) leaves a worst-case ring, and that residual crop runs
	 * on the OUTPUT device in the weave below.
	 */
	comp_xbridge_set_content_size(s->xbridge, content_w, content_h, s->layout_gen);

	/*
	 * #1140 — the recipe travels with the pixels. This rung composites nothing
	 * on the output device, so the stamp is a projection-only one; a consume
	 * half can then never read a slot claiming a composite it has no pixels
	 * for, and every `plane_valid` test fails closed.
	 */
	struct comp_xbridge_recipe r = {};
	r.composite = false;
	r.region_w = content_w;
	r.region_h = content_h;
	comp_xbridge_stage_recipe(s->xbridge, &r);

	s->seq++;
	comp_xbridge_submit(s->xbridge, s->seq, s->layout_gen, handoff->texture, content_w, content_h);
}

/*!
 * The residual crop, on the OUTPUT device — only ever reached when the R2 resize
 * hysteresis is holding a worst-case egress ring.
 */
static ID3D11ShaderResourceView *
split_crop_for_dp(struct comp_vk_split *s, ID3D11ShaderResourceView *src_srv, uint32_t content_w, uint32_t content_h)
{
	if (src_srv == nullptr) {
		return nullptr;
	}

	if (s->dp_input_tex == nullptr || s->dp_input_w != content_w || s->dp_input_h != content_h) {
		if (s->dp_input_srv != nullptr) {
			s->dp_input_srv->Release();
			s->dp_input_srv = nullptr;
		}
		if (s->dp_input_tex != nullptr) {
			s->dp_input_tex->Release();
			s->dp_input_tex = nullptr;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = content_w;
		desc.Height = content_h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(s->out_dev->CreateTexture2D(&desc, nullptr, &s->dp_input_tex))) {
			return src_srv;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = desc.Format;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;
		if (FAILED(s->out_dev->CreateShaderResourceView(s->dp_input_tex, &srv_desc, &s->dp_input_srv))) {
			s->dp_input_tex->Release();
			s->dp_input_tex = nullptr;
			return src_srv;
		}
		s->dp_input_w = content_w;
		s->dp_input_h = content_h;
	}

	ID3D11Resource *src_res = nullptr;
	src_srv->GetResource(&src_res);
	if (src_res == nullptr) {
		return src_srv;
	}
	D3D11_BOX box = {0, 0, 0, content_w, content_h, 1};
	s->out_ctx->CopySubresourceRegion(s->dp_input_tex, 0, 0, 0, 0, src_res, 0, &box);
	src_res->Release();
	s->out_crop++;
	return s->dp_input_srv;
}

extern "C" bool
comp_vk_split_weave_and_present(struct comp_vk_split *s, bool is_repaint, const struct xrt_rect *canvas)
{
	if (s == nullptr || s->xbridge == nullptr || s->target == nullptr || s->dp == nullptr) {
		return false;
	}

	/*
	 * THE SLOT IS PICKED BEFORE A SINGLE COMMAND IS RECORDED. Both #918 F4 and
	 * R1 depend on that ordering: deciding after the back buffer had been
	 * acquired and cleared would force a choice between presenting black and
	 * abandoning a half-recorded frame.
	 */
	const uint64_t want_gen = s->layout_gen;
	int32_t slot = -1;
	if (is_repaint) {
		slot = comp_xbridge_get_weave_slot(s->xbridge);
	} else {
		slot = comp_xbridge_pick_slot(s->xbridge, want_gen);
		if (slot < 0) {
			/*
			 * #918 R1, the TRANSITION pick. Presenting nothing here is not
			 * neutral: with FLIP_DISCARD the panel keeps showing the frame it
			 * was last given, which was woven for the mode the display has
			 * just left. Weaving the in-flight slot costs a GPU-side wait and
			 * the frame goes out correct.
			 */
			slot = comp_xbridge_pick_inflight_slot(s->xbridge, want_gen);
			if (slot >= 0) {
				s->inflight_weaves++;
			}
		}
	}

	uint64_t slot_gen = 0;
	uint32_t slot_cw = 0, slot_ch = 0;
	if (slot >= 0) {
		if (!comp_xbridge_slot_layout(s->xbridge, slot, &slot_gen, &slot_cw, &slot_ch) || slot_cw == 0 ||
		    slot_ch == 0) {
			slot = -1;
		} else if (slot_gen != want_gen) {
			// A slot composited under a layout the display processor has
			// since switched away from must never be woven.
			s->stale_refusals++;
			slot = -1;
		}
	}

	// #918 F7 — a repaint re-weaves a slot it did not pick itself, so nothing
	// else orders it against a consumer copy rewriting that same slot.
	if (slot >= 0 && is_repaint && !comp_xbridge_slot_ready(s->xbridge, slot)) {
		slot = -1;
	}

	if (slot < 0) {
		// #918 F4 — present NOTHING. The panel holds the last woven frame.
		s->no_slot++;
		return false;
	}

	comp_xbridge_gpu_wait_slot(s->xbridge, slot);
	ID3D11ShaderResourceView *atlas_srv = (ID3D11ShaderResourceView *)comp_xbridge_get_srv(s->xbridge, slot);
	if (atlas_srv == nullptr) {
		s->no_slot++;
		return false;
	}

	/*
	 * #1140 — the weave's geometry comes from the snapshot minted with the
	 * generation just proved, and its content box from the SLOT's own stamp,
	 * never from live CPU state. During a resize the slot is a frame behind the
	 * window, and cropping it to the current frame's box would slice the tiles
	 * at the wrong stride.
	 */
	const uint32_t weave_cols = s->gen_cols != 0 ? s->gen_cols : 1;
	const uint32_t weave_rows = s->gen_rows != 0 ? s->gen_rows : 1;
	const uint32_t weave_view_w = slot_cw / weave_cols;
	const uint32_t weave_view_h = slot_ch / weave_rows;
	if (weave_view_w == 0 || weave_view_h == 0) {
		s->no_slot++;
		return false;
	}

	// The residual crop, only when the ring is oversized (R2 hysteresis).
	uint32_t eg_w = 0, eg_h = 0;
	comp_xbridge_get_egress_dims(s->xbridge, &eg_w, &eg_h);
	if (slot_cw != eg_w || slot_ch != eg_h) {
		atlas_srv = split_crop_for_dp(s, atlas_srv, slot_cw, slot_ch);
	}

	uint32_t target_index = 0;
	if (comp_d3d11_target_acquire(s->target, &target_index) != XRT_SUCCESS) {
		s->no_slot++;
		return false;
	}

	uint32_t tgt_w = 0, tgt_h = 0;
	comp_d3d11_target_get_dimensions(s->target, &tgt_w, &tgt_h);

	// Late-weave pacing + the weave-latency harness mark, on the scanout
	// adapter where the present now happens.
	if (is_repaint) {
		comp_d3d11_target_weave_mark_repaint(s->target, /*mode_3d=*/true);
	} else {
		comp_d3d11_target_weave_mark(s->target, /*predicted_display_time_ns=*/0, /*mode_3d=*/true);
	}

	// Hand the vendor eye predictor last frame's MEASURED weave->scanout
	// residual, so it runs with an exact horizon (0 = unknown, DP heuristic).
	xrt_display_processor_d3d11_set_frame_timing(s->dp, comp_d3d11_target_get_measured_weave_ns(s->target), 0);

	comp_d3d11_target_bind(s->target);

	struct xrt_rect cv = {};
	if (canvas != nullptr) {
		cv = *canvas;
	}
	xrt_display_processor_d3d11_process_atlas(s->dp, s->out_ctx, atlas_srv, weave_view_w, weave_view_h,
	                                          weave_cols, weave_rows, (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM,
	                                          tgt_w, tgt_h, cv.offset.w, cv.offset.h, (uint32_t)cv.extent.w,
	                                          (uint32_t)cv.extent.h);

	if (!is_repaint) {
		// Publish the slot the app frame wove, so a repaint replays exactly
		// that one with zero bridge traffic.
		comp_xbridge_set_weave_slot(s->xbridge, slot);
	}

	return comp_d3d11_target_present(s->target, 1) == XRT_SUCCESS;
}

extern "C" bool
comp_vk_split_has_weave_slot(struct comp_vk_split *s)
{
	return s != nullptr && s->xbridge != nullptr && comp_xbridge_get_weave_slot(s->xbridge) >= 0;
}

extern "C" void
comp_vk_split_render_diag(struct comp_vk_split *s)
{
	if (s == nullptr || s->xbridge == nullptr) {
		return;
	}

	const uint64_t now = os_monotonic_get_ns();
	if (now - s->diag_window_ns < 10 * U_TIME_1S_IN_NS) {
		return;
	}
	s->diag_window_ns = now;

	const uint64_t xb_bytes = comp_xbridge_take_atlas_bytes(s->xbridge);
	struct comp_xbridge_ingress_stats ing = {};
	comp_xbridge_take_ingress_stats(s->xbridge, &ing);
	const char *ing_name = ing.mode == COMP_XBRIDGE_INGRESS_ADAPTIVE ? "adaptive"
	                       : ing.mode == COMP_XBRIDGE_INGRESS_DIRECT ? "direct"
	                       : ing.mode == COMP_XBRIDGE_INGRESS_STAGED ? "staged"
	                                                                 : "none";

	U_LOG_W("[RENDER] split=1 xb_kb=%llu xb_degraded=%d no_slot=%llu out_crop=%llu ingress=%s "
	        "ing_direct=%llu ing_staged=%llu ing_rebind=%llu ing_churn=%llu ing_leak=%llu window_s=10",
	        (unsigned long long)(xb_bytes / 1024u), (int)comp_xbridge_is_degraded(s->xbridge),
	        (unsigned long long)s->no_slot, (unsigned long long)s->out_crop, ing_name,
	        (unsigned long long)ing.direct, (unsigned long long)ing.staged, (unsigned long long)ing.rebind,
	        (unsigned long long)ing.churn, (unsigned long long)ing.leak);
}


/*
 *
 * Display-processor forwarding.
 *
 */

extern "C" bool
comp_vk_split_get_predicted_eye_positions(struct comp_vk_split *s, struct xrt_eye_positions *out_eye_pos)
{
	if (s == nullptr || s->dp == nullptr) {
		return false;
	}
	return xrt_display_processor_d3d11_get_predicted_eye_positions(s->dp, out_eye_pos);
}

extern "C" bool
comp_vk_split_get_display_dimensions(struct comp_vk_split *s, float *out_width_m, float *out_height_m)
{
	if (s == nullptr || s->dp == nullptr) {
		return false;
	}
	return xrt_display_processor_d3d11_get_display_dimensions(s->dp, out_width_m, out_height_m);
}

extern "C" bool
comp_vk_split_get_display_pixel_info(
    struct comp_vk_split *s, uint32_t *out_width_px, uint32_t *out_height_px, int32_t *out_left, int32_t *out_top)
{
	if (s == nullptr || s->dp == nullptr) {
		return false;
	}
	return xrt_display_processor_d3d11_get_display_pixel_info(s->dp, out_width_px, out_height_px, out_left,
	                                                          out_top);
}

extern "C" bool
comp_vk_split_request_display_mode(struct comp_vk_split *s, bool enable_3d)
{
	if (s == nullptr || s->dp == nullptr) {
		return false;
	}
	return xrt_display_processor_d3d11_request_display_mode(s->dp, enable_3d);
}

extern "C" void
comp_vk_split_set_eye_tracking_mode(struct comp_vk_split *s, uint32_t mode)
{
	if (s == nullptr || s->dp == nullptr) {
		return;
	}
	xrt_display_processor_d3d11_set_eye_tracking_mode(s->dp, mode);
}

#else /* !(XRT_OS_WINDOWS && XRT_HAVE_D3D11) */

extern "C" xrt_result_t
comp_vk_split_stage_a(const struct comp_vk_split_info *info,
                      struct comp_vk_split **out_split,
                      const char **out_short_reason)
{
	(void)info;
	*out_split = NULL;
	/*
	 * COMP_SPLIT_REASON_API_UNSUPPORTED, spelled out: comp_split_gate.h and the
	 * transport behind it are linked on Windows only, so the token cannot be
	 * referenced by name here. It is a fixed parse target and is never
	 * reformatted — see comp_split_gate.h for the catalogue.
	 */
	*out_short_reason = "api_unsupported";
	return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
}

extern "C" bool
comp_vk_split_wire_bridge(struct comp_vk_split *split, const struct comp_vk_deposit_handoff *handoff)
{
	(void)split;
	(void)handoff;
	return false;
}

extern "C" void
comp_vk_split_destroy(struct comp_vk_split **split_ptr)
{
	(void)split_ptr;
}

extern "C" void
comp_vk_split_retire(struct comp_vk_split **split_ptr, const char *why, const char *short_reason)
{
	(void)split_ptr;
	(void)why;
	(void)short_reason;
}

extern "C" void
comp_vk_split_submit_atlas(struct comp_vk_split *split,
                           const struct comp_vk_deposit_handoff *handoff,
                           uint32_t cols,
                           uint32_t rows,
                           uint32_t view_w,
                           uint32_t view_h)
{
	(void)split;
	(void)handoff;
	(void)cols;
	(void)rows;
	(void)view_w;
	(void)view_h;
}

extern "C" bool
comp_vk_split_weave_and_present(struct comp_vk_split *split, bool is_repaint, const struct xrt_rect *canvas)
{
	(void)split;
	(void)is_repaint;
	(void)canvas;
	return false;
}

extern "C" bool
comp_vk_split_has_weave_slot(struct comp_vk_split *split)
{
	(void)split;
	return false;
}

extern "C" void
comp_vk_split_render_diag(struct comp_vk_split *split)
{
	(void)split;
}

extern "C" bool
comp_vk_split_get_predicted_eye_positions(struct comp_vk_split *split, struct xrt_eye_positions *out_eye_pos)
{
	(void)split;
	(void)out_eye_pos;
	return false;
}

extern "C" bool
comp_vk_split_get_display_dimensions(struct comp_vk_split *split, float *out_width_m, float *out_height_m)
{
	(void)split;
	(void)out_width_m;
	(void)out_height_m;
	return false;
}

extern "C" bool
comp_vk_split_get_display_pixel_info(struct comp_vk_split *split,
                                     uint32_t *out_width_px,
                                     uint32_t *out_height_px,
                                     int32_t *out_left,
                                     int32_t *out_top)
{
	(void)split;
	(void)out_width_px;
	(void)out_height_px;
	(void)out_left;
	(void)out_top;
	return false;
}

extern "C" bool
comp_vk_split_request_display_mode(struct comp_vk_split *split, bool enable_3d)
{
	(void)split;
	(void)enable_3d;
	return false;
}

extern "C" void
comp_vk_split_set_eye_tracking_mode(struct comp_vk_split *split, uint32_t mode)
{
	(void)split;
	(void)mode;
}

#endif /* XRT_OS_WINDOWS && XRT_HAVE_D3D11 */
