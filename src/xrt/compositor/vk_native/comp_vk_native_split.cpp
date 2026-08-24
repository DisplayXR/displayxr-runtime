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
#include "xrt/xrt_limits.h"

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
#include "d3d11/comp_d3d11_outcomp.h"
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
	//! #1178 — the format the crop scratch was created in, which must track the
	//! EGRESS's rather than be assumed: the copy that fills it is dropped in
	//! silence if the two are different DXGI typeless families.
	DXGI_FORMAT dp_input_fmt;

	/*!
	 * VK-1b — a REGION-SIZED output-device view of a panel-sized bridge plane,
	 * for the display-processor sideband entry points that take dims but no uv
	 * scale. See @ref split_plane_dp_view.
	 */
	struct split_plane_view
	{
		ID3D11Texture2D *tex;
		ID3D11ShaderResourceView *srv;
		uint32_t w, h;
		uint64_t seq;
		bool warned;
		//! #1178 — one WARN, ever, for a caller whose requested format is not the
		//! plane's own. This runs per frame; the repo forbids a per-frame WARN.
		bool fmt_warned;
	};
	struct split_plane_view dp_bd_view;

	//! @name VK-1b — this frame's BACKDROP plane, staged before the submit.
	//! @{
	bool bd_plane_live;
	uint32_t bd_w, bd_h;
	uint64_t sideband_copies, sideband_skips, sideband_bytes;
	//! @}

	/*!
	 * VK-1b-2 — one out-device R8 mask raster. RTV texture plus a staged SRV
	 * sibling, because an RT is not an SRV; the same decouple the D3D11 leg's
	 * four rasterisers use.
	 */
	struct split_mask_raster
	{
		ID3D11Texture2D *tex;
		ID3D11Texture2D *staged;
		ID3D11RenderTargetView *rtv;
		ID3D11ShaderResourceView *srv;
		uint32_t w, h;
		//! Dirty-check cache — re-rasters only on a real change.
		uint32_t kind;
		uint32_t rect_count;
		struct xrt_rect rects[XRT_MAX_LAYERS];
		float feather[XRT_MAX_LAYERS];
		bool warned;
	};
	struct split_mask_raster mask;

	/*!
	 * The raster the LAST APP FRAME produced. #868: a repaint replays
	 * RENDERING, never state transitions, so it composites from this instead of
	 * re-rastering — re-rastering drives a once-per-app-frame machine (including
	 * the wish publish) at panel rate.
	 */
	ID3D11ShaderResourceView *repaint_mask_srv;

	//! The masked composite, on the OUT device. Created lazily on the first
	//! frame that needs it — see @ref split_ensure_outcomp.
	struct comp_d3d11_outcomp *outcomp;
	bool outcomp_failed;

	//! @name VK-1b-2 — the composite recipe this frame stages.
	//! @{
	bool l2d_plane_live;
	uint32_t comp_region_w, comp_region_h;
	int32_t comp_cx, comp_cy;
	uint32_t comp_cw, comp_ch;
	uint32_t comp_mode;
	uint32_t comp_mask_kind;
	bool comp_opaque;
	//! @}

	//! @name VK-1b-2 — the CPU-rastered diagnostic HUD, on the OUT device.
	//! @{
	ID3D11Texture2D *hud_tex;
	uint32_t hud_w, hud_h;
	bool hud_live;
	//! @}

	//! @name VK-1b-3 — the app-authored (Tier-3) mask plane.
	//! @{
	bool mask_plane_live;
	uint64_t mask_plane_gen;
	//! The region-sized out-device view of the mask plane, for the wish publish.
	struct split_plane_view dp_mask_view;
	//! @}

	//! ADR-027 P4 — the scanout weaver's zone capability, probed once.
	int zone_dp_state;
	bool zone_published;

	uint64_t composites, composite_bails;

	//! @name Counters for the `[RENDER]` line and the F4/R1 tripwires.
	//! @{
	uint64_t no_slot;
	uint64_t out_crop;
	uint64_t stale_refusals;
	uint64_t inflight_weaves;
	uint64_t diag_window_ns;
	//! @}

	//! @name #1178 — the swapchain-follows-the-window tripwire.
	//! @{
	//! When the swapchain and the window client rect FIRST disagreed, or 0
	//! while they agree. A live drag legitimately disagrees for a frame or two,
	//! so the complaint waits out @ref SPLIT_DRIFT_GRACE_NS of continuous drift.
	uint64_t tgt_drift_since_ns;
	uint64_t tgt_drift_log_ns;
	uint64_t tgt_drift_frames;
	//! @}
};


/*
 *
 * Helpers.
 *
 */

//! #1178 — how long the swapchain may lag the window before the tripwire says so.
#define SPLIT_DRIFT_GRACE_NS (500 * 1000 * 1000ULL)
//! ...and how often it may repeat itself. Never per frame; the repo forbids that.
#define SPLIT_DRIFT_LOG_NS (5 * 1000 * 1000 * 1000ULL)

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

/*!
 * VK-1b — a region-sized OUTPUT-DEVICE view of a panel-sized bridge plane.
 *
 * The 2D planes are allocated at the panel and the composite writes only the
 * region's top-left sub-rect (#464), but the display processor's
 * `set_background_2d` takes width/height and no uv scale — hand it the panel-sized
 * egress and it stretches a mostly-empty texture over the region. So the sub-rect
 * is copied into a right-sized texture on the device that samples it.
 *
 * Three properties, each of which the D3D11 leg learned in review:
 *   - **Passthrough** when the plane already IS the requested extent (no texture,
 *     no copy, no bytes).
 *   - **Seq-gated** (#918 review F8): the copy re-runs only when the slot's plane
 *     generation differs from the one this view already holds. Without it a
 *     forced repaint re-copies a full region on every panel-rate tick.
 *   - **Clamped and metered** (#918 review F6): the box is min(src, dst), because
 *     D3D11 DROPS an oversized `CopySubresourceRegion` silently and would leave
 *     the vendor sampling an uninitialised texture.
 *
 * TODO(#1178 VK-1b): a near-duplicate of `d3d11_plane_dp_view`
 * (src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp:4975), which is `static`
 * and wired to `struct comp_d3d11_compositor`. Extracting the two into one unit
 * is tracked separately — it touches the shipped D3D11 leg, and this epic's rule
 * is land-before-restructure.
 *
 * Returns NULL on any failure, which the caller treats as "no backdrop" — a
 * degraded feature, never a broken frame.
 */
static ID3D11ShaderResourceView *
split_plane_dp_view(struct comp_vk_split *s,
                    struct comp_vk_split::split_plane_view *v,
                    void *plane_srv,
                    uint64_t content_seq,
                    uint32_t w,
                    uint32_t h,
                    DXGI_FORMAT fmt,
                    const char *what)
{
	auto *psrv = static_cast<ID3D11ShaderResourceView *>(plane_srv);
	if (psrv == nullptr || w == 0 || h == 0) {
		return nullptr;
	}
	ID3D11Resource *pres = nullptr;
	psrv->GetResource(&pres);
	if (pres == nullptr) {
		return nullptr;
	}
	ID3D11Texture2D *ptex = nullptr;
	D3D11_TEXTURE2D_DESC pd = {};
	if (FAILED(pres->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&ptex)) || ptex == nullptr) {
		pres->Release();
		return nullptr;
	}
	ptex->GetDesc(&pd);
	ptex->Release();

	// Passthrough: the plane is exactly what the display processor asked for.
	if (pd.Width == w && pd.Height == h) {
		pres->Release();
		return psrv;
	}

	/*
	 * #1178 — the view is filled by a CopySubresourceRegion out of `ptex`, so its
	 * format is the SOURCE's and not the caller's wish. The two agree today (the
	 * planes are bound BGRA and the mask R8, and the bridge now refuses a bind
	 * whose source disagrees), and taking it from the source anyway is what stops
	 * them silently disagreeing tomorrow: a cross-family copy here would be
	 * dropped, not failed, and the display processor would sample a never-written
	 * texture with every counter healthy.
	 */
	if (fmt != pd.Format && !v->fmt_warned) {
		v->fmt_warned = true;
		U_LOG_W(
		    "%s: DP-view format 0x%x requested but the plane is 0x%x — using the plane's, because a "
		    "cross-family CopySubresourceRegion is silently dropped (#1178)",
		    what, (unsigned)fmt, (unsigned)pd.Format);
	}
	fmt = pd.Format;

	const bool need_alloc = (v->tex == nullptr) || (v->srv == nullptr) || v->w != w || v->h != h;
	if (need_alloc) {
		if (v->srv != nullptr) {
			v->srv->Release();
			v->srv = nullptr;
		}
		if (v->tex != nullptr) {
			v->tex->Release();
			v->tex = nullptr;
		}
		v->w = 0;
		v->h = 0;
		v->seq = 0;

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = s->out_dev->CreateTexture2D(&td, nullptr, &v->tex);
		if (SUCCEEDED(hr) && v->tex != nullptr) {
			hr = s->out_dev->CreateShaderResourceView(v->tex, nullptr, &v->srv);
		}
		if (FAILED(hr) || v->srv == nullptr) {
			// Latched: this runs on the frame path and on every repaint tick,
			// and the repo forbids a per-frame WARN (#918 review D7).
			if (!v->warned) {
				v->warned = true;
				U_LOG_W(
				    "%s: DP-view alloc (%ux%u) failed 0x%08lx — that sideband degrades for "
				    "this session (#918)",
				    what, w, h, (unsigned long)hr);
			}
			if (v->tex != nullptr) {
				v->tex->Release();
				v->tex = nullptr;
			}
			pres->Release();
			return nullptr;
		}
		v->w = w;
		v->h = h;
	} else if (content_seq != 0 && v->seq == content_seq) {
		// Same pixels, already copied — the steady state under a repaint.
		s->sideband_skips++;
		pres->Release();
		return v->srv;
	}

	const uint32_t cw = (w < pd.Width) ? w : pd.Width;
	const uint32_t ch = (h < pd.Height) ? h : pd.Height;
	if (cw == 0 || ch == 0) {
		pres->Release();
		return nullptr;
	}
	D3D11_BOX box = {0, 0, 0, cw, ch, 1};
	s->out_ctx->CopySubresourceRegion(v->tex, 0, 0, 0, 0, pres, 0, &box);
	pres->Release();
	v->seq = content_seq;
	s->sideband_copies++;
	s->sideband_bytes += (uint64_t)cw * ch * 4u;
	return v->srv;
}

/*
 *
 * VK-1b-2 — the OUT-DEVICE mask raster.
 *
 */

//! Free the raster's four D3D objects. Idempotent.
static void
split_release_mask(struct comp_vk_split::split_mask_raster *m)
{
	if (m->srv != nullptr) {
		m->srv->Release();
		m->srv = nullptr;
	}
	if (m->staged != nullptr) {
		m->staged->Release();
		m->staged = nullptr;
	}
	if (m->rtv != nullptr) {
		m->rtv->Release();
		m->rtv = nullptr;
	}
	if (m->tex != nullptr) {
		m->tex->Release();
		m->tex = nullptr;
	}
	m->w = 0;
	m->h = 0;
	m->kind = COMP_VK_SPLIT_MASK_NONE;
	m->rect_count = 0;
}

//! (Re)allocate the R8 RTV + staged SRV pair at @p w x @p h on the OUT device.
static bool
split_mask_ensure(struct comp_vk_split *s, uint32_t w, uint32_t h)
{
	struct comp_vk_split::split_mask_raster *m = &s->mask;
	if (m->tex != nullptr && m->w == w && m->h == h) {
		return true;
	}
	split_release_mask(m);

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	HRESULT hr = s->out_dev->CreateTexture2D(&td, nullptr, &m->tex);
	if (SUCCEEDED(hr) && m->tex != nullptr) {
		hr = s->out_dev->CreateRenderTargetView(m->tex, nullptr, &m->rtv);
	}
	if (SUCCEEDED(hr) && m->rtv != nullptr) {
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		hr = s->out_dev->CreateTexture2D(&td, nullptr, &m->staged);
	}
	if (SUCCEEDED(hr) && m->staged != nullptr) {
		hr = s->out_dev->CreateShaderResourceView(m->staged, nullptr, &m->srv);
	}
	if (FAILED(hr) || m->srv == nullptr) {
		// Latched: this is on the frame path and the repo forbids a per-frame WARN.
		if (!m->warned) {
			m->warned = true;
			U_LOG_W(
			    "#918 VK-1b: the out-device mask raster (%ux%u) could not be allocated 0x%08lx — 2D "
			    "content does not composite under the split for this session; the 3D weave is "
			    "unaffected",
			    w, h, (unsigned long)hr);
		}
		split_release_mask(m);
		return false;
	}
	m->w = w;
	m->h = h;
	return true;
}

//! One `ClearView` of @p value over @p r, clamped into the raster.
static void
split_mask_fill(
    struct comp_vk_split *s, ID3D11DeviceContext1 *ctx1, const struct xrt_rect *r, int32_t inset, float value)
{
	struct comp_vk_split::split_mask_raster *m = &s->mask;
	int32_t left = r->offset.w + inset;
	int32_t top = r->offset.h + inset;
	int32_t right = r->offset.w + r->extent.w - inset;
	int32_t bottom = r->offset.h + r->extent.h - inset;
	if (left < 0) {
		left = 0;
	}
	if (top < 0) {
		top = 0;
	}
	if (right > (int32_t)m->w) {
		right = (int32_t)m->w;
	}
	if (bottom > (int32_t)m->h) {
		bottom = (int32_t)m->h;
	}
	if (right <= left || bottom <= top) {
		return;
	}
	const float val[4] = {value, 0.0f, 0.0f, 0.0f};
	D3D11_RECT dr = {left, top, right, bottom};
	ctx1->ClearView(m->rtv, val, &dr, 1);
}

static void
split_release_plane_view(struct comp_vk_split::split_plane_view *v)
{
	if (v->srv != nullptr) {
		v->srv->Release();
		v->srv = nullptr;
	}
	if (v->tex != nullptr) {
		v->tex->Release();
		v->tex = nullptr;
	}
	v->w = 0;
	v->h = 0;
	v->seq = 0;
}

//! Release the output half. Idempotent; leaves the borrowed app end alone.
static void
split_release_out(struct comp_vk_split *s)
{
	if (s->xbridge != NULL) {
		comp_xbridge_quiesce(s->xbridge);
		comp_xbridge_destroy(&s->xbridge);
	}
	/*
	 * VK-1b-2 — the zone contribution is withdrawn BEFORE the weaver goes away
	 * (#224 P4's clear edge). A session that just disappears leaves the vendor
	 * unioning a mask nobody owns.
	 */
	comp_vk_split_clear_zone_wish(s);
	comp_d3d11_outcomp_destroy(&s->outcomp);
	split_release_mask(&s->mask);
	s->repaint_mask_srv = NULL;
	if (s->hud_tex != NULL) {
		s->hud_tex->Release();
		s->hud_tex = NULL;
	}
	s->hud_live = false;
	split_release_plane_view(&s->dp_bd_view);
	split_release_plane_view(&s->dp_mask_view);
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
	/*
	 * #1178 — THE ATLAS FORMAT, and the whole of this bug.
	 *
	 * The deposit's D3D11 texture takes its format from the VULKAN atlas, which on
	 * this stack is `VK_FORMAT_B8G8R8A8_UNORM` -> `DXGI_FORMAT_B8G8R8A8_UNORM`.
	 * The bridge used to build its rings in a hardcoded `R8G8B8A8`, and
	 * `CopySubresourceRegion` between two different DXGI typeless families is not
	 * an error Windows reports — it is DROPPED. Every counter stayed green, the
	 * egress kept its zero-fill, and the panel was black.
	 *
	 * Declaring the format here (rather than coercing the Vulkan atlas to RGBA) is
	 * the right direction: the atlas format is chosen upstream by the swapchain,
	 * and forcing it would either swizzle the channels or break the VK render
	 * path. Everything downstream of this — the cross-adapter heap, the egress
	 * ring, the display processor's SRV — is already format-agnostic.
	 */
	xbi.atlas_format = handoff->dxgi_format;

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

	U_LOG_W(
	    "VK output-device split: %llu frames bridged, %llu weaves with no usable slot, %llu stale-slot "
	    "refusals, %llu in-flight (transition) weaves, %llu output-device crops, %llu composites, %llu "
	    "composite bails (#918)",
	    (unsigned long long)s->seq, (unsigned long long)s->no_slot, (unsigned long long)s->stale_refusals,
	    (unsigned long long)s->inflight_weaves, (unsigned long long)s->out_crop, (unsigned long long)s->composites,
	    (unsigned long long)s->composite_bails);

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
comp_vk_split_stage_backdrop(struct comp_vk_split *s,
                             void *nt_handle,
                             uint64_t generation,
                             uint32_t alloc_w,
                             uint32_t alloc_h,
                             uint64_t content_seq,
                             int32_t dirty_x,
                             int32_t dirty_y,
                             uint32_t dirty_w,
                             uint32_t dirty_h,
                             uint32_t bd_w,
                             uint32_t bd_h)
{
	if (s == nullptr || s->xbridge == nullptr) {
		return;
	}

	/*
	 * 0 is the bridge's "this frame does not use the plane", and it is what the
	 * recipe then stamps invalid — which is the honest answer rather than
	 * lending the weave an older frame's backdrop.
	 */
	if (nt_handle == nullptr || content_seq == 0 || bd_w == 0 || bd_h == 0) {
		comp_xbridge_stage_plane(s->xbridge, COMP_XBRIDGE_PLANE_BACKDROP, 0, 0, 0, 0, 0);
		s->bd_plane_live = false;
		s->bd_w = 0;
		s->bd_h = 0;
		return;
	}

	/*
	 * Panel-sized, always. The steady-state call is the handle+generation
	 * early-out inside the bridge; a generation change re-opens the handle and
	 * drains the producer first, which is why the surface is allocated once at
	 * the panel and never at the region.
	 */
	if (!comp_xbridge_bind_plane(s->xbridge, COMP_XBRIDGE_PLANE_BACKDROP, nt_handle, generation,
	                             (uint32_t)DXGI_FORMAT_B8G8R8A8_UNORM, alloc_w, alloc_h)) {
		/*
		 * The backdrop degrades on its OWN — a session without one simply has
		 * no 2D-under band, and the 3D weave is untouched. Never a reason to
		 * give the scanout adapter back.
		 */
		comp_xbridge_stage_plane(s->xbridge, COMP_XBRIDGE_PLANE_BACKDROP, 0, 0, 0, 0, 0);
		s->bd_plane_live = false;
		s->bd_w = 0;
		s->bd_h = 0;
		return;
	}

	comp_xbridge_stage_plane(s->xbridge, COMP_XBRIDGE_PLANE_BACKDROP, content_seq, dirty_x, dirty_y, dirty_w,
	                         dirty_h);
	s->bd_plane_live = true;
	s->bd_w = bd_w;
	s->bd_h = bd_h;
}

extern "C" void
comp_vk_split_invalidate_plane(struct comp_vk_split *s, uint32_t plane)
{
	if (s == nullptr || s->xbridge == nullptr) {
		return;
	}
	comp_xbridge_invalidate_plane(s->xbridge, plane);
}

extern "C" bool
comp_vk_split_raster_mask(struct comp_vk_split *s,
                          uint32_t kind,
                          const struct xrt_rect *rects,
                          const float *feather_px,
                          uint32_t rect_count,
                          uint32_t region_w,
                          uint32_t region_h)
{
	if (s == nullptr || s->out_dev == nullptr || region_w == 0 || region_h == 0) {
		return false;
	}
	if (kind == COMP_VK_SPLIT_MASK_NONE || rect_count == 0 || rects == nullptr) {
		s->repaint_mask_srv = nullptr;
		return false;
	}
	if (rect_count > XRT_MAX_LAYERS) {
		rect_count = XRT_MAX_LAYERS;
	}

	struct comp_vk_split::split_mask_raster *m = &s->mask;

	// Dirty-check on everything the raster is a function of. The steady-state
	// frame reuses the staged SRV and touches the GPU not at all.
	bool dirty =
	    m->srv == nullptr || m->w != region_w || m->h != region_h || m->kind != kind || m->rect_count != rect_count;
	for (uint32_t i = 0; !dirty && i < rect_count; i++) {
		if (memcmp(&m->rects[i], &rects[i], sizeof(rects[i])) != 0) {
			dirty = true;
		}
		if (kind == COMP_VK_SPLIT_MASK_ZONE_FEATHER && feather_px != nullptr &&
		    m->feather[i] != feather_px[i]) {
			dirty = true;
		}
	}
	if (!dirty) {
		s->repaint_mask_srv = m->srv;
		return true;
	}

	if (!split_mask_ensure(s, region_w, region_h)) {
		s->repaint_mask_srv = nullptr;
		return false;
	}

	ID3D11DeviceContext1 *ctx1 = nullptr;
	if (FAILED(s->out_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1)) || ctx1 == nullptr) {
		if (!m->warned) {
			m->warned = true;
			U_LOG_W(
			    "#918 VK-1b: the out device has no ID3D11DeviceContext1, so the mask raster has no "
			    "ClearView — 2D content does not composite under the split for this session");
		}
		s->repaint_mask_srv = nullptr;
		return false;
	}

	/*
	 * The three rasters, and all three are pure CLEARS — no shader, no draw.
	 * That is what makes "rebuild the mask where it is consumed" cheap enough to
	 * be the rule rather than a transport.
	 *
	 * TODO(#1178 VK-1b): duplicates of `d3d11_update_implicit_mask`
	 * (src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp:5502),
	 * `d3d11_update_zone_wish_mask` (:5652) and `d3d11_update_zone_feather_mask`
	 * (:5820). Those are `static` and wired to `struct comp_d3d11_compositor`;
	 * extracting all four into one unit touches the shipped D3D11 leg, so it is
	 * tracked as its own change rather than folded into new Vulkan transport.
	 */
	const float base = (kind == COMP_VK_SPLIT_MASK_IMPLICIT) ? 1.0f : 0.0f;
	const float all[4] = {base, 0.0f, 0.0f, 0.0f};
	s->out_ctx->ClearRenderTargetView(m->rtv, all);

	for (uint32_t i = 0; i < rect_count; i++) {
		if (kind == COMP_VK_SPLIT_MASK_IMPLICIT) {
			// Inverse of the zone raster: M=0 (show the flattened 2D) inside
			// each Local2D rect, M=1 (keep the weave) everywhere else.
			split_mask_fill(s, ctx1, &rects[i], 0, 0.0f);
			continue;
		}
		if (kind == COMP_VK_SPLIT_MASK_ZONE_BINARY) {
			split_mask_fill(s, ctx1, &rects[i], 0, 1.0f);
			continue;
		}

		/*
		 * #803 — the per-zone inward ramp, by the rings idiom: ascending value
		 * WITH ascending inset, so later, deeper, higher-value clears overwrite
		 * the inner part of earlier ones. The edge keeps the low values and the
		 * core reaches 1. 2px steps, capped at a 64px ramp (beyond which the
		 * step widens instead), and the inset is clamped so a small zone's
		 * centre still reaches 1.
		 */
		const float radius = (feather_px != nullptr) ? feather_px[i] : 0.0f;
		const bool feathered = radius > 0.0f;
		int32_t steps = 1;
		int32_t step_px = 0;
		if (feathered) {
			step_px = 2;
			steps = (int32_t)(radius / (float)step_px + 0.5f);
			if (steps < 1) {
				steps = 1;
			}
			if (steps > 32) {
				step_px = (int32_t)(radius / 32.0f + 0.5f);
				steps = 32;
			}
		}
		const int32_t min_ext = rects[i].extent.w < rects[i].extent.h ? rects[i].extent.w : rects[i].extent.h;
		int32_t max_inset = (min_ext - 1) / 2;
		if (max_inset < 0) {
			max_inset = 0;
		}
		for (int32_t st = 1; st <= steps; st++) {
			const float v = (float)st / (float)steps; // 1.0 for the hard single step
			int32_t inset = feathered ? st * step_px : 0;
			if (inset > max_inset) {
				inset = max_inset;
			}
			split_mask_fill(s, ctx1, &rects[i], inset, v);
		}
	}
	ctx1->Release();

	// Stage the snapshot the composite samples (RT is not an SRV).
	s->out_ctx->CopyResource(m->staged, m->tex);

	m->kind = kind;
	m->rect_count = rect_count;
	memcpy(m->rects, rects, sizeof(rects[0]) * rect_count);
	if (feather_px != nullptr) {
		memcpy(m->feather, feather_px, sizeof(feather_px[0]) * rect_count);
	} else {
		memset(m->feather, 0, sizeof(m->feather));
	}

	// On a rect/dims change only, never per frame.
	U_LOG_W("#918 VK-1b: out-device mask raster kind=%u %ux%u, %u rect(s)", kind, region_w, region_h, rect_count);

	s->repaint_mask_srv = m->srv;
	return true;
}

/*!
 * The masked composite unit, on the OUT device.
 *
 * LAZY, following the D3D12 leg rather than the D3D11 one: the create compiles
 * shaders, and paying that inside Stage A would add it to the session-warmup
 * critical path for every split session, including the projection-only ones that
 * never composite anything. A failure here is feature-local — one latched WARN,
 * `composite=false` on the recipe, projection-only frames — and never a retire.
 */
static bool
split_ensure_outcomp(struct comp_vk_split *s)
{
	if (s->outcomp != nullptr) {
		return true;
	}
	if (s->outcomp_failed) {
		return false;
	}
	if (comp_d3d11_outcomp_create(s->out_dev, s->out_ctx, &s->outcomp) != XRT_SUCCESS || s->outcomp == nullptr) {
		s->outcomp_failed = true;
		s->outcomp = nullptr;
		U_LOG_W(
		    "#918 VK-1b: the output composite unit could not be created on the scanout device — 2D "
		    "content does not composite under the split for this session; the 3D weave is unaffected");
		return false;
	}
	U_LOG_W("#918 VK-1b: output composite unit up on the scanout device");
	return true;
}

extern "C" void
comp_vk_split_stage_no_composite(struct comp_vk_split *s)
{
	if (s == nullptr || s->xbridge == nullptr) {
		return;
	}
	comp_xbridge_stage_plane(s->xbridge, COMP_XBRIDGE_PLANE_LOCAL2D, 0, 0, 0, 0, 0);
	s->l2d_plane_live = false;
}

extern "C" void
comp_vk_split_stage_local2d(struct comp_vk_split *s,
                            void *nt_handle,
                            uint64_t generation,
                            uint32_t alloc_w,
                            uint32_t alloc_h,
                            uint64_t content_seq,
                            int32_t dirty_x,
                            int32_t dirty_y,
                            uint32_t dirty_w,
                            uint32_t dirty_h,
                            uint32_t region_w,
                            uint32_t region_h,
                            int32_t cx,
                            int32_t cy,
                            uint32_t cw,
                            uint32_t ch,
                            uint32_t composite_mode,
                            bool opaque_present,
                            bool mask_is_plane)
{
	if (s == nullptr || s->xbridge == nullptr) {
		return;
	}

	/*
	 * The unit the CONSUME half will need is created HERE rather than there,
	 * because this half stamps the recipe: a create failure stamps
	 * `composite = false` and the frame ships projection-only, where discovering
	 * it in the consume half would leave a slot marked compositable that nothing
	 * can composite.
	 */
	if (nt_handle == nullptr || content_seq == 0 || region_w == 0 || region_h == 0 || !split_ensure_outcomp(s)) {
		comp_vk_split_stage_no_composite(s);
		return;
	}

	if (!comp_xbridge_bind_plane(s->xbridge, COMP_XBRIDGE_PLANE_LOCAL2D, nt_handle, generation,
	                             (uint32_t)DXGI_FORMAT_B8G8R8A8_UNORM, alloc_w, alloc_h)) {
		/*
		 * #918 review D4 — the Local2D plane IS the composite's `twod` under the
		 * split, so a frame that could not bind it has no composite to stamp.
		 * Claiming otherwise stamps `composite=true` on a slot whose plane the
		 * submit then marks invalid, and the consume half bails on every frame
		 * afterwards with no log.
		 */
		static bool warned = false;
		if (!warned) {
			warned = true;
			U_LOG_W(
			    "#918 VK-1b: the Local2D plane could not be bound — 2D content does not composite "
			    "under the split for this session; the 3D weave is unaffected");
		}
		comp_vk_split_stage_no_composite(s);
		return;
	}

	comp_xbridge_stage_plane(s->xbridge, COMP_XBRIDGE_PLANE_LOCAL2D, content_seq, dirty_x, dirty_y, dirty_w,
	                         dirty_h);
	s->l2d_plane_live = true;
	s->comp_region_w = region_w;
	s->comp_region_h = region_h;
	s->comp_cx = cx;
	s->comp_cy = cy;
	s->comp_cw = cw;
	s->comp_ch = ch;
	s->comp_mode = composite_mode;
	s->comp_opaque = opaque_present;
	/*
	 * VK-1b-3 — which mask the consume half will actually SAMPLE, which is the
	 * only thing this stamp may describe. A bridged Tier-3 mask reads the plane
	 * that landed with this slot; every other kind is the out-device raster.
	 */
	s->comp_mask_kind = mask_is_plane ? COMP_XBRIDGE_MASK_PLANE : COMP_XBRIDGE_MASK_OUT_RASTER;
}

extern "C" bool
comp_vk_split_bind_mask_plane(struct comp_vk_split *s, void *nt_handle, uint64_t generation, uint32_t w, uint32_t h)
{
	if (s == nullptr || s->xbridge == nullptr) {
		return true; // nothing to bind against, and nothing broken
	}
	if (nt_handle == nullptr || w == 0 || h == 0) {
		s->mask_plane_live = false;
		s->mask_plane_gen = 0;
		return true; // no authored mask this frame — not a failure
	}

	// Sized at the MASK (#918 review F5), so a dims change rebuilds the chain.
	// On-change only: the steady-state call is the bridge's own early-out.
	if (!comp_xbridge_bind_plane(s->xbridge, COMP_XBRIDGE_PLANE_MASK, nt_handle, generation,
	                             (uint32_t)DXGI_FORMAT_R8_UNORM, w, h)) {
		s->mask_plane_live = false;
		s->mask_plane_gen = 0;
		return false;
	}
	s->mask_plane_live = true;
	s->mask_plane_gen = generation;
	return true;
}

extern "C" void
comp_vk_split_stage_mask_plane(struct comp_vk_split *s, uint64_t content_seq, uint32_t w, uint32_t h)
{
	if (s == nullptr || s->xbridge == nullptr) {
		return;
	}
	if (!s->mask_plane_live || content_seq == 0) {
		// 0 is the bridge's "this frame does not use the plane".
		comp_xbridge_stage_plane(s->xbridge, COMP_XBRIDGE_PLANE_MASK, 0, 0, 0, 0, 0);
		return;
	}
	comp_xbridge_stage_plane(s->xbridge, COMP_XBRIDGE_PLANE_MASK, content_seq, 0, 0, w, h);
}

extern "C" bool
comp_vk_split_zone_dp_supported(struct comp_vk_split *s)
{
	if (s == nullptr || s->dp == nullptr) {
		return false;
	}
	if (s->zone_dp_state == 0) {
		struct xrt_dp_local_zone_caps caps = {};
		caps.struct_size = sizeof(caps);
		const bool ok = xrt_display_processor_d3d11_get_local_zone_caps(s->dp, &caps) && caps.supported != 0;
		s->zone_dp_state = ok ? 1 : 2;
		if (ok) {
			U_LOG_W("#918 VK-1b: scanout weaver supports local zones — grid %ux%u max_mask %ux%u",
			        caps.zone_grid_width, caps.zone_grid_height, caps.max_mask_width, caps.max_mask_height);
		}
	}
	return s->zone_dp_state == 1;
}

extern "C" void
comp_vk_split_publish_zone_wish(struct comp_vk_split *s, uint64_t seq)
{
	if (s == nullptr || s->dp == nullptr || s->hwnd == nullptr || !comp_vk_split_zone_dp_supported(s)) {
		return;
	}
	/*
	 * The BINARY raster and nothing else. A feather ramp is a cosmetic composite
	 * opt-in (#803) and must never reach the vendor as geometry, so a frame whose
	 * composite sampled the feather mask still publishes the binary one — which
	 * is why the publish reads the raster's KIND rather than just its SRV.
	 */
	ID3D11ShaderResourceView *srv = nullptr;
	uint32_t mw = 0, mh = 0;
	if (s->mask_plane_live) {
		/*
		 * VK-1b-3 - an APP-AUTHORED wish publishes VERBATIM, off the plane it
		 * crossed on. That is the whole point of a Tier-3 wish: the app chose the
		 * geometry and the runtime must not substitute its own. Falling back to
		 * the auto raster when the plane has not landed would publish DIFFERENT
		 * geometry for one frame, which is a flicker rather than a degradation -
		 * so a not-yet-landed authored wish publishes nothing and the vendor keeps
		 * the one it already has.
		 */
		const int32_t wslot = comp_xbridge_get_weave_slot(s->xbridge);
		struct comp_xbridge_recipe rec = {};
		if (wslot < 0 || !comp_xbridge_slot_recipe(s->xbridge, wslot, &rec) ||
		    (rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_MASK)) == 0) {
			return;
		}
		const uint64_t want = rec.plane_seq[COMP_XBRIDGE_PLANE_MASK];
		void *plane_srv = comp_xbridge_get_plane_srv(s->xbridge, wslot, COMP_XBRIDGE_PLANE_MASK, want);
		uint32_t pw = 0, ph = 0;
		if (plane_srv == nullptr || !comp_xbridge_plane_extent(s->xbridge, COMP_XBRIDGE_PLANE_MASK, &pw, &ph) ||
		    pw == 0 || ph == 0) {
			return;
		}
		// Normally a passthrough - the mask plane is sized AT the mask (#918
		// review F5) - but the helper still handles a publish that asks for dims
		// the plane does not have, seq-gated and clamped.
		srv = split_plane_dp_view(s, &s->dp_mask_view, plane_srv, want, pw, ph, DXGI_FORMAT_R8_UNORM,
		                          "zone-mask publish");
		mw = pw;
		mh = ph;
	} else {
		/*
		 * The BINARY raster and nothing else. A feather ramp is a cosmetic
		 * composite opt-in (#803) and must never reach the vendor as geometry, so
		 * a frame whose composite sampled the feather mask still publishes the
		 * binary one - which is why this reads the raster's KIND, not just its SRV.
		 */
		if (s->mask.srv == nullptr || s->mask.kind != COMP_VK_SPLIT_MASK_ZONE_BINARY) {
			return;
		}
		srv = s->mask.srv;
		mw = s->mask.w;
		mh = s->mask.h;
	}
	if (srv == nullptr || mw == 0 || mh == 0) {
		return;
	}

	RECT r;
	POINT origin = {0, 0};
	if (!GetClientRect(s->hwnd, &r) || r.right <= 0 || r.bottom <= 0 || !ClientToScreen(s->hwnd, &origin)) {
		return;
	}
	if (xrt_display_processor_d3d11_publish_local_zone_mask(s->dp, s->out_ctx, srv, mw, mh, (int32_t)origin.x,
	                                                        (int32_t)origin.y, (uint32_t)r.right,
	                                                        (uint32_t)r.bottom, seq)) {
		s->zone_published = true;
	}
}

extern "C" void
comp_vk_split_clear_zone_wish(struct comp_vk_split *s)
{
	if (s == nullptr || s->dp == nullptr || !s->zone_published) {
		return;
	}
	xrt_display_processor_d3d11_clear_local_zone_mask(s->dp);
	s->zone_published = false;
}

extern "C" void
comp_vk_split_set_hud(struct comp_vk_split *s, const void *pixels, uint32_t w, uint32_t h, bool dirty)
{
	if (s == nullptr || s->out_dev == nullptr) {
		return;
	}
	if (pixels == nullptr || w == 0 || h == 0) {
		s->hud_live = false;
		return;
	}

	if (s->hud_tex == nullptr || s->hud_w != w || s->hud_h != h) {
		if (s->hud_tex != nullptr) {
			s->hud_tex->Release();
			s->hud_tex = nullptr;
		}
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		if (FAILED(s->out_dev->CreateTexture2D(&td, nullptr, &s->hud_tex)) || s->hud_tex == nullptr) {
			s->hud_tex = nullptr;
			s->hud_live = false;
			return;
		}
		s->hud_w = w;
		s->hud_h = h;
		dirty = true; // a fresh texture holds nothing
	}
	if (dirty) {
		s->out_ctx->UpdateSubresource(s->hud_tex, 0, nullptr, pixels, w * 4u, 0);
	}
	s->hud_live = true;
}

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
	 * #1140 — the recipe travels with the pixels. `composite` stays false until
	 * VK-1b-2 puts the masked composite on the output device; what this rung
	 * adds is the BACKDROP's own extent, which the recipe carries separately
	 * from the composite for exactly this case: a frame can flatten a backdrop
	 * and run no composite at all, and `set_background_2d` still needs its real
	 * width and height. A consume half can never read a slot claiming pixels it
	 * does not have — every `plane_valid` test fails closed.
	 */
	struct comp_xbridge_recipe r = {};
	r.composite = s->l2d_plane_live;
	r.region_w = s->l2d_plane_live ? s->comp_region_w : content_w;
	r.region_h = s->l2d_plane_live ? s->comp_region_h : content_h;
	r.bd_w = s->bd_w;
	r.bd_h = s->bd_h;
	if (s->l2d_plane_live) {
		r.composite_mode = s->comp_mode;
		r.mask_kind = s->comp_mask_kind;
		r.opaque_present = s->comp_opaque;
		r.cx = s->comp_cx;
		r.cy = s->comp_cy;
		r.cw = s->comp_cw;
		r.ch = s->comp_ch;
	}
	comp_xbridge_stage_recipe(s->xbridge, &r);

	s->seq++;
	comp_xbridge_submit(s->xbridge, s->seq, s->layout_gen, handoff->texture, content_w, content_h);

	/*
	 * VK-1b — THE PLANE BACK-FENCE, and the one place this leg diverges from
	 * the two D3D ones.
	 *
	 * A plane's ingress is Option I: the producer's copy queue opened the
	 * app-device texture above and reads it in place. `pre_plane_write` makes
	 * the app IMMEDIATE CONTEXT wait for that read — which is all the D3D legs
	 * need, because the immediate context is also what WRITES their scratch.
	 * Here the plane is written by the Vulkan queue, so this wait on its own
	 * orders nothing; it is the first half of a pair. The second half is the
	 * caller's `comp_vk_deposit_note_planes_consumed`, which signals the shared
	 * fence on this same context — behind this wait, one ordered stream — and
	 * which Vulkan's next flatten waits for on the timeline.
	 *
	 * Without the pair, Vulkan laps the bridge and tears the 2D band: the same
	 * defect VK-1a's atlas release edge closed, one level down. It costs one
	 * queued wait and one queued signal — no deeper ring, and no change to the
	 * staged ingress the atlas depends on.
	 */
	if (s->bd_plane_live) {
		comp_xbridge_pre_plane_write(s->xbridge, COMP_XBRIDGE_PLANE_BACKDROP);
	}
	if (s->l2d_plane_live) {
		comp_xbridge_pre_plane_write(s->xbridge, COMP_XBRIDGE_PLANE_LOCAL2D);
	}
	if (s->mask_plane_live) {
		comp_xbridge_pre_plane_write(s->xbridge, COMP_XBRIDGE_PLANE_MASK);
	}
}

/*!
 * VK-1b-2 — the masked 2D-over-3D composite, on the OUT device, over the frame
 * the display processor has just woven into @p dst.
 *
 * Every parameter comes FROM THE SLOT (#1140). The slot was filled by an earlier
 * frame, so reading the composite's recipe from live CPU state would pair one
 * frame's pixels with another frame's blend — exactly what `eg_gen` already
 * forbids for the atlas, one level down.
 *
 * Returns false on every "no composite this frame" path, which is a normal
 * outcome and never a broken frame: the weave has already gone into @p dst and
 * is presented regardless.
 */
static bool
split_composite(struct comp_vk_split *s, int32_t slot, bool is_repaint, ID3D11Texture2D *dst)
{
	if (s->outcomp == nullptr || dst == nullptr) {
		return false;
	}

	struct comp_xbridge_recipe rec = {};
	if (!comp_xbridge_slot_recipe(s->xbridge, slot, &rec) || !rec.composite) {
		// A projection-only frame filled this slot. The correct answer, not a bail.
		return false;
	}

	D3D11_TEXTURE2D_DESC dd = {};
	dst->GetDesc(&dd);
	uint32_t region_w = rec.region_w;
	uint32_t region_h = rec.region_h;
	if (region_w > dd.Width) {
		region_w = dd.Width;
	}
	if (region_h > dd.Height) {
		region_h = dd.Height;
	}
	if (region_w == 0 || region_h == 0) {
		s->composite_bails++;
		return false;
	}

	/*
	 * The `twod` source is the LOCAL2D PLANE that landed with THIS slot, proved
	 * against the recipe's own generation (#918 review F3) — a later submit that
	 * rewrote the plane under this weave comes back NULL rather than as
	 * mismatched pixels.
	 */
	if ((rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_LOCAL2D)) == 0) {
		s->composite_bails++;
		return false;
	}
	auto *twod_srv = static_cast<ID3D11ShaderResourceView *>(comp_xbridge_get_plane_srv(
	    s->xbridge, slot, COMP_XBRIDGE_PLANE_LOCAL2D, rec.plane_seq[COMP_XBRIDGE_PLANE_LOCAL2D]));
	if (twod_srv == nullptr) {
		s->composite_bails++;
		return false;
	}

	/*
	 * The mask is the OUT-DEVICE raster the last app frame produced. A repaint
	 * reads the same pointer without re-rastering (#868), which is the whole
	 * reason `is_repaint` is an explicit parameter of the weave rather than
	 * inferred: the raster is not just a draw, it feeds the once-per-app-frame
	 * wish publish, and driving that at panel rate desynchronises the sideband.
	 */
	(void)is_repaint;
	ID3D11ShaderResourceView *mask_srv = nullptr;
	if (rec.mask_kind == COMP_XBRIDGE_MASK_PLANE) {
		/*
		 * VK-1b-3 — the app drew these pixels, so they crossed as a plane and
		 * are proved against the slot's own generation like every other plane.
		 * Resolved FROM THE SLOT on a repaint too, rather than cached: a repaint
		 * re-weaves the slot the last app frame wove, so the same call returns
		 * the same resource under the same seq test — and a cached pointer would
		 * be the one thing that could outlive its seq.
		 */
		if ((rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_MASK)) == 0) {
			s->composite_bails++;
			return false;
		}
		mask_srv = static_cast<ID3D11ShaderResourceView *>(comp_xbridge_get_plane_srv(
		    s->xbridge, slot, COMP_XBRIDGE_PLANE_MASK, rec.plane_seq[COMP_XBRIDGE_PLANE_MASK]));
	} else {
		mask_srv = s->repaint_mask_srv;
	}
	if (mask_srv == nullptr) {
		s->composite_bails++;
		return false;
	}

	const DXGI_FORMAT unorm_fmt = (dd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)   ? DXGI_FORMAT_B8G8R8A8_UNORM
	                              : (dd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM
	                                                                               : dd.Format;
	if (!comp_d3d11_outcomp_ensure_weave_scratch(s->outcomp, region_w, region_h, (uint32_t)unorm_fmt)) {
		s->composite_bails++;
		return false;
	}
	// The display processor wrote dst and an RT is not an SRV, so the lerp reads
	// a snapshot. Unit-owned: source and destination are both its device's.
	void *weave_srv = comp_d3d11_outcomp_snapshot_weave(s->outcomp, dst, region_w, region_h);
	if (weave_srv == nullptr) {
		s->composite_bails++;
		return false;
	}

	uint32_t cx_u = (rec.cx < 0) ? 0u : (uint32_t)rec.cx;
	uint32_t cy_u = (rec.cy < 0) ? 0u : (uint32_t)rec.cy;
	if (cx_u > region_w) {
		cx_u = region_w;
	}
	if (cy_u > region_h) {
		cy_u = region_h;
	}
	const uint32_t cright = (cx_u + rec.cw > region_w) ? region_w : cx_u + rec.cw;
	const uint32_t cbottom = (cy_u + rec.ch > region_h) ? region_h : cy_u + rec.ch;

	const xrt_result_t xret = comp_d3d11_outcomp_composite_2d_masked(
	    s->outcomp, dst, twod_srv, mask_srv, weave_srv, region_w, region_h, (int32_t)cx_u, (int32_t)cy_u,
	    cright - cx_u, cbottom - cy_u, rec.composite_mode, rec.opaque_present);
	if (xret != XRT_SUCCESS) {
		s->composite_bails++;
		return false;
	}

	s->composites++;
	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W("#918 VK-1b: Local2D composite on the SCANOUT device — %ux%u region, mode=%u mask_kind=%u",
		        region_w, region_h, rec.composite_mode, rec.mask_kind);
	}
	return true;
}

/*!
 * VK-1b-2 — the diagnostic HUD, copied onto the back buffer after the composite.
 *
 * Not a bridge plane and not transported: `u_hud` rasterises to a CPU buffer, so
 * the bitmap was uploaded straight to an out-device texture by
 * @ref comp_vk_split_set_hud. Same shape as `d3d11_render_hud_overlay`'s.
 */
static void
split_draw_hud(struct comp_vk_split *s, ID3D11Texture2D *dst, uint32_t tgt_w, uint32_t tgt_h)
{
	if (!s->hud_live || s->hud_tex == nullptr || dst == nullptr) {
		return;
	}
	if (tgt_w < s->hud_w + 10u || tgt_h < s->hud_h + 10u) {
		return;
	}
	const uint32_t dst_x = 10u;
	const uint32_t dst_y = tgt_h - s->hud_h - 10u;
	D3D11_BOX src = {0, 0, 0, s->hud_w, s->hud_h, 1};
	s->out_ctx->CopySubresourceRegion(dst, 0, dst_x, dst_y, 0, s->hud_tex, 0, &src);
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

	/*
	 * #1178 — the crop texture's format is the EGRESS's, read off the source,
	 * never a constant. This was the second live instance of the defect: the
	 * egress ring is BGRA on the Vulkan leg, this scratch was hardcoded
	 * `R8G8B8A8_UNORM`, and the CopySubresourceRegion below across two DXGI
	 * typeless families is silently dropped rather than failed. It only fires when
	 * the R2 resize hysteresis is holding a worst-case ring, which is exactly the
	 * kind of conditional path a byte counter can never speak for.
	 */
	DXGI_FORMAT src_fmt = DXGI_FORMAT_UNKNOWN;
	{
		ID3D11Resource *probe_res = nullptr;
		src_srv->GetResource(&probe_res);
		if (probe_res == nullptr) {
			return src_srv;
		}
		ID3D11Texture2D *probe_tex = nullptr;
		if (SUCCEEDED(probe_res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&probe_tex)) &&
		    probe_tex != nullptr) {
			D3D11_TEXTURE2D_DESC pd = {};
			probe_tex->GetDesc(&pd);
			src_fmt = pd.Format;
			probe_tex->Release();
		}
		probe_res->Release();
	}
	if (src_fmt == DXGI_FORMAT_UNKNOWN) {
		return src_srv;
	}

	if (s->dp_input_tex == nullptr || s->dp_input_w != content_w || s->dp_input_h != content_h ||
	    s->dp_input_fmt != src_fmt) {
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
		desc.Format = src_fmt;
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
		s->dp_input_fmt = src_fmt;
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
comp_vk_split_resize_target(struct comp_vk_split *s, uint32_t width, uint32_t height)
{
	if (s == nullptr || s->target == nullptr || width == 0 || height == 0) {
		return false;
	}

	uint32_t cur_w = 0, cur_h = 0;
	comp_d3d11_target_get_dimensions(s->target, &cur_w, &cur_h);
	if (cur_w == width && cur_h == height) {
		return true;
	}

	/*
	 * No ID3D10Multithread bracket here, and that is the split's whole point:
	 * this swapchain lives on the runtime's OWN output device, which the app's
	 * render thread never touches. The non-split D3D11 leg takes one at its
	 * equivalent site precisely because there the chain shares the app's
	 * immediate context (`resize_needs_lock` in d3d11_compositor_begin_frame),
	 * and it too drops the bracket once the split is active.
	 *
	 * The caller holds the compositor mutex, which is what keeps the #868
	 * repaint thread — the other user of this target — out for the duration.
	 */
	const xrt_result_t xret = comp_d3d11_target_resize(s->target, width, height);
	if (xret != XRT_SUCCESS) {
		U_LOG_E(
		    "VK output-device split: the scanout swapchain REFUSED %ux%u (%d) and stays at %ux%u — "
		    "the present will keep the OLD geometry (#1178)",
		    width, height, (int)xret, cur_w, cur_h);
		return false;
	}

	// A window resize, not a frame event.
	U_LOG_W("VK output-device split: scanout swapchain follows the window, %ux%u -> %ux%u (#1178)", cur_w, cur_h,
	        width, height);
	s->tgt_drift_since_ns = 0;
	return true;
}

/*!
 * #1178 — THE TRIPWIRE, at the point of use.
 *
 * The fix one level up makes the swapchain follow the window; this makes a
 * failure to do so DIAGNOSABLE, which is the part that outlives the instance.
 * Nothing in the pipeline can see this defect: the atlas, the content box, the
 * egress ring and every `[RENDER]` counter follow the window correctly, so the
 * only witness is the geometry of the surface being presented against the
 * geometry of the window presenting it. #1178 shipped with `split=1 no_slot=0
 * ing_leak=0 out_crop=0` and a visibly wrong picture.
 *
 * Grace before complaint, because a real drag DOES disagree briefly: a repaint
 * tick can weave between the window changing and the next `begin_frame`
 * observing it. Only drift that persists past @ref SPLIT_DRIFT_GRACE_NS is a
 * defect rather than a frame of latency.
 */
static void
split_check_target_follows_window(struct comp_vk_split *s, uint32_t tgt_w, uint32_t tgt_h)
{
	if (s->hwnd == nullptr) {
		return;
	}
	RECT rc = {};
	if (!GetClientRect(s->hwnd, &rc)) {
		return;
	}
	const uint32_t win_w = (uint32_t)(rc.right - rc.left);
	const uint32_t win_h = (uint32_t)(rc.bottom - rc.top);
	if (win_w == 0 || win_h == 0 || (win_w == tgt_w && win_h == tgt_h)) {
		s->tgt_drift_since_ns = 0;
		return;
	}

	const uint64_t now = os_monotonic_get_ns();
	if (s->tgt_drift_since_ns == 0) {
		s->tgt_drift_since_ns = now;
		return;
	}
	s->tgt_drift_frames++;
	if (now - s->tgt_drift_since_ns < SPLIT_DRIFT_GRACE_NS) {
		return;
	}
	if (s->tgt_drift_log_ns != 0 && now - s->tgt_drift_log_ns < SPLIT_DRIFT_LOG_NS) {
		return;
	}
	s->tgt_drift_log_ns = now;
	U_LOG_E(
	    "VK output-device split: THE SCANOUT SWAPCHAIN IS NOT FOLLOWING THE WINDOW — the window's client "
	    "rect is %ux%u but the chain is %ux%u, and has been for %llu ms / %llu frames. Every weave is "
	    "composed for the CHAIN's geometry and DXGI then scales or clips it into the window, so the picture "
	    "keeps the old size while the atlas, the content box, the egress ring and every [RENDER] counter "
	    "track the window correctly and read healthy. Whatever moved the window did not reach "
	    "comp_vk_split_resize_target (#1178).",
	    win_w, win_h, tgt_w, tgt_h, (unsigned long long)((now - s->tgt_drift_since_ns) / (1000 * 1000)),
	    (unsigned long long)s->tgt_drift_frames);
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
	 * #1178 — the CONTENT probe, off unless DXR_SPLIT_CONTENT_PROBE=1. This is the
	 * weave thread, which is where it has to run: it drives the output immediate
	 * context. It reads the EGRESS slot rather than the (possibly cropped) SRV
	 * above, because the destination of the bridge's own copy is the thing whose
	 * emptiness no other diagnostic can see.
	 */
	comp_xbridge_content_probe(s->xbridge, slot);

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
	split_check_target_follows_window(s, tgt_w, tgt_h);

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

	/*
	 * VK-1b — hand the display processor THIS SLOT's backdrop, read from the
	 * slot's own recipe rather than from live CPU state (#1140). The slot the
	 * weave is consuming was filled by an earlier frame, so a backdrop resolved
	 * from the current frame would pair one frame's pixels with another's
	 * extent. Must precede process_atlas.
	 *
	 * A NULL SRV here is the correct clear, not a failure: it means the frame
	 * that filled this slot produced no 2D-under layers, and the display
	 * processor falls back to the captured desktop alone.
	 */
	{
		struct comp_xbridge_recipe rec = {};
		ID3D11ShaderResourceView *bd_srv = nullptr;
		uint32_t bd_w = 0, bd_h = 0;
		if (comp_xbridge_slot_recipe(s->xbridge, slot, &rec) && rec.bd_w > 0 && rec.bd_h > 0 &&
		    (rec.plane_valid & (1u << COMP_XBRIDGE_PLANE_BACKDROP)) != 0) {
			const uint64_t bd_seq = rec.plane_seq[COMP_XBRIDGE_PLANE_BACKDROP];
			bd_srv = split_plane_dp_view(
			    s, &s->dp_bd_view,
			    comp_xbridge_get_plane_srv(s->xbridge, slot, COMP_XBRIDGE_PLANE_BACKDROP, bd_seq), bd_seq,
			    rec.bd_w, rec.bd_h, DXGI_FORMAT_B8G8R8A8_UNORM, "backdrop publish");
			if (bd_srv != nullptr) {
				bd_w = rec.bd_w;
				bd_h = rec.bd_h;
			}
		}
		xrt_display_processor_d3d11_set_background_2d(s->dp, bd_srv, bd_w, bd_h);
	}

	struct xrt_rect cv = {};
	if (canvas != nullptr) {
		cv = *canvas;
	}
	xrt_display_processor_d3d11_process_atlas(s->dp, s->out_ctx, atlas_srv, weave_view_w, weave_view_h, weave_cols,
	                                          weave_rows, (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM, tgt_w, tgt_h,
	                                          cv.offset.w, cv.offset.h, (uint32_t)cv.extent.w,
	                                          (uint32_t)cv.extent.h);

	/*
	 * VK-1b-2 — the masked 2D-over-3D composite, then the HUD, both on the OUT
	 * device and both over the frame the weave has just written. This is the tail
	 * the split moved to the scanout adapter along with the weave itself; every
	 * input belongs to that device, which is the single rule
	 * comp_d3d11_outcomp states and the plane transports exist to satisfy.
	 */
	{
		auto *dst = static_cast<ID3D11Texture2D *>(comp_d3d11_target_get_back_buffer(s->target));
		(void)split_composite(s, slot, is_repaint, dst);
		split_draw_hud(s, dst, tgt_w, tgt_h);
	}

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

	U_LOG_W(
	    "[RENDER] split=1 xb_kb=%llu xb_degraded=%d no_slot=%llu out_crop=%llu ingress=%s "
	    "ing_direct=%llu ing_staged=%llu ing_rebind=%llu ing_churn=%llu ing_leak=%llu window_s=10",
	    (unsigned long long)(xb_bytes / 1024u), (int)comp_xbridge_is_degraded(s->xbridge),
	    (unsigned long long)s->no_slot, (unsigned long long)s->out_crop, ing_name, (unsigned long long)ing.direct,
	    (unsigned long long)ing.staged, (unsigned long long)ing.rebind, (unsigned long long)ing.churn,
	    (unsigned long long)ing.leak);

	/*
	 * VK-1b — one line per live PLANE, in the same shape and with the same field
	 * names as the two D3D legs', so one parser reads all three. Emitted only for
	 * a plane that has actually transported something, so a projection-only
	 * session's log is byte-identical to what it was before the planes existed.
	 */
	for (uint32_t p = 0; p < COMP_XBRIDGE_PLANE_COUNT; p++) {
		uint64_t pb = 0, pc = 0, ps = 0;
		bool half = false;
		comp_xbridge_take_plane_stats(s->xbridge, p, &pb, &pc, &ps, &half);
		if (pc == 0 && ps == 0) {
			continue;
		}
		U_LOG_W("[RENDER] plane=%s kb=%llu copies=%llu skips=%llu half_rate=%d window_s=10",
		        comp_xbridge_plane_label(p), (unsigned long long)(pb / 1024u), (unsigned long long)pc,
		        (unsigned long long)ps, (int)half);
	}
	if (s->sideband_copies != 0 || s->sideband_skips != 0) {
		U_LOG_W("[RENDER] sideband copies=%llu skips=%llu kb=%llu window_s=10",
		        (unsigned long long)s->sideband_copies, (unsigned long long)s->sideband_skips,
		        (unsigned long long)(s->sideband_bytes / 1024u));
		s->sideband_copies = 0;
		s->sideband_skips = 0;
		s->sideband_bytes = 0;
	}
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

extern "C" void
comp_vk_split_stage_backdrop(struct comp_vk_split *split,
                             void *nt_handle,
                             uint64_t generation,
                             uint32_t alloc_w,
                             uint32_t alloc_h,
                             uint64_t content_seq,
                             int32_t dirty_x,
                             int32_t dirty_y,
                             uint32_t dirty_w,
                             uint32_t dirty_h,
                             uint32_t bd_w,
                             uint32_t bd_h)
{
	(void)split;
	(void)nt_handle;
	(void)generation;
	(void)alloc_w;
	(void)alloc_h;
	(void)content_seq;
	(void)dirty_x;
	(void)dirty_y;
	(void)dirty_w;
	(void)dirty_h;
	(void)bd_w;
	(void)bd_h;
}

extern "C" void
comp_vk_split_invalidate_plane(struct comp_vk_split *split, uint32_t plane)
{
	(void)split;
	(void)plane;
}

extern "C" bool
comp_vk_split_raster_mask(struct comp_vk_split *split,
                          uint32_t kind,
                          const struct xrt_rect *rects,
                          const float *feather_px,
                          uint32_t rect_count,
                          uint32_t region_w,
                          uint32_t region_h)
{
	(void)split;
	(void)kind;
	(void)rects;
	(void)feather_px;
	(void)rect_count;
	(void)region_w;
	(void)region_h;
	return false;
}

extern "C" void
comp_vk_split_stage_local2d(struct comp_vk_split *split,
                            void *nt_handle,
                            uint64_t generation,
                            uint32_t alloc_w,
                            uint32_t alloc_h,
                            uint64_t content_seq,
                            int32_t dirty_x,
                            int32_t dirty_y,
                            uint32_t dirty_w,
                            uint32_t dirty_h,
                            uint32_t region_w,
                            uint32_t region_h,
                            int32_t cx,
                            int32_t cy,
                            uint32_t cw,
                            uint32_t ch,
                            uint32_t composite_mode,
                            bool opaque_present,
                            bool mask_is_plane)
{
	(void)split;
	(void)nt_handle;
	(void)generation;
	(void)alloc_w;
	(void)alloc_h;
	(void)content_seq;
	(void)dirty_x;
	(void)dirty_y;
	(void)dirty_w;
	(void)dirty_h;
	(void)region_w;
	(void)region_h;
	(void)cx;
	(void)cy;
	(void)cw;
	(void)ch;
	(void)composite_mode;
	(void)opaque_present;
	(void)mask_is_plane;
}

extern "C" void
comp_vk_split_stage_no_composite(struct comp_vk_split *split)
{
	(void)split;
}

extern "C" bool
comp_vk_split_bind_mask_plane(struct comp_vk_split *split, void *nt_handle, uint64_t generation, uint32_t w, uint32_t h)
{
	(void)split;
	(void)nt_handle;
	(void)generation;
	(void)w;
	(void)h;
	return true;
}

extern "C" void
comp_vk_split_stage_mask_plane(struct comp_vk_split *split, uint64_t content_seq, uint32_t w, uint32_t h)
{
	(void)split;
	(void)content_seq;
	(void)w;
	(void)h;
}

extern "C" bool
comp_vk_split_zone_dp_supported(struct comp_vk_split *split)
{
	(void)split;
	return false;
}

extern "C" void
comp_vk_split_publish_zone_wish(struct comp_vk_split *split, uint64_t seq)
{
	(void)split;
	(void)seq;
}

extern "C" void
comp_vk_split_clear_zone_wish(struct comp_vk_split *split)
{
	(void)split;
}

extern "C" void
comp_vk_split_set_hud(struct comp_vk_split *split, const void *pixels, uint32_t w, uint32_t h, bool dirty)
{
	(void)split;
	(void)pixels;
	(void)w;
	(void)h;
	(void)dirty;
}

extern "C" bool
comp_vk_split_resize_target(struct comp_vk_split *split, uint32_t width, uint32_t height)
{
	(void)split;
	(void)width;
	(void)height;
	return false;
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
