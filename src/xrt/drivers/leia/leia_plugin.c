// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Plug-in entry point for the Leia SR display driver.
 *
 * Implements the @ref xrt_plugin_negotiate_fn_t signature defined in
 * `xrt/xrt_plugin.h`. The runtime DLL loads this plug-in via
 * `LoadLibraryExW` + `GetProcAddress("xrtPluginNegotiate")` once the
 * registry sort lands on this entry (intended `ProbeOrder=50` per
 * ADR-019; lower than sim_display's 200, so this wins on machines
 * with SR hardware).
 *
 * The probe is intentionally fast — EDID + SR-SDK/service presence
 * check only; no SR context creation. The slower
 * `leiasr_probe_display(timeout)` path stays as the in-tree builder's
 * fallback and is not on the hot path here.
 *
 * Issue #256 — vendor plug-in re-architecture.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"

#include "util/u_logging.h"

#include "leia_interface.h"
#include "leia_display_processor.h"
#ifdef XRT_HAVE_LEIA_SR_D3D11
#include "leia_display_processor_d3d11.h"
#include "leia_sr_d3d11.h" /* leiasr_query_recommended_view_dimensions + leiasr_static_get_display_dimensions */
#endif
#ifdef XRT_HAVE_LEIA_SR_D3D12
#include "leia_display_processor_d3d12.h"
#endif
#ifdef XRT_HAVE_LEIA_SR_GL
#include "leia_display_processor_gl.h"
#endif

#include <stddef.h>


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
leia_plugin_probe(struct xrt_plugin_instance **out_inst)
{
	/*
	 * Fast EDID-based detection. We require all three layers:
	 *   1. EDID panel match (hw_found),
	 *   2. SR SDK installed on this machine (sdk_installed),
	 *   3. SRService running (service_running).
	 * Any one missing → decline cleanly so the next plug-in (or the
	 * sim_display fallback) gets a turn. Skipping the slower
	 * leiasr_probe_display(3.0) path keeps probe sub-millisecond on
	 * the xrCreateInstance hot path; the in-tree builder retains the
	 * full-context probe for diagnostic logging.
	 */
	struct leia_display_probe_result edid = {0};
	bool found = leia_edid_probe_display(&edid);
	if (!found || !edid.hw_found || !edid.sdk_installed || !edid.service_running) {
		U_LOG_I("leia_plugin: probe declined — hw=%d sdk=%d service=%d", edid.hw_found,
		        edid.sdk_installed, edid.service_running);
		*out_inst = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	/*
	 * No per-instance state: drv_leia stores hardware state in
	 * file-scope statics inside the plug-in DLL (cached probe result,
	 * SR context, etc.). Mirrors the sim_display plug-in shape.
	 */
	*out_inst = NULL;
	return XRT_SUCCESS;
}

static xrt_result_t
leia_plugin_create_device(struct xrt_plugin_instance *inst, struct xrt_device **out_dev)
{
	(void)inst;
	struct xrt_device *xdev = leia_hmd_create();
	if (xdev == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	*out_dev = xdev;
	return XRT_SUCCESS;
}

static void
leia_plugin_destroy(struct xrt_plugin_instance *inst)
{
	(void)inst;
	/* No instance state — nothing to free. */
}

static bool
leia_plugin_get_display_info(struct xrt_plugin_instance *inst,
                             struct xrt_device *xdev,
                             struct xrt_plugin_display_info *out_info)
{
	(void)inst;
	(void)xdev;

	(void)out_info->struct_size; /* v1: see sim_display plug-in's note. */

	bool any_populated = false;

	/*
	 * SR-recommended view dimensions + native panel resolution. Both
	 * are needed by the compositor for atlas sizing + the per-view
	 * scale factor stored in xrt_system_compositor_info.
	 */
	uint32_t sr_w = 0, sr_h = 0, nat_w = 0, nat_h = 0;
	float refresh = 0.0f;
	if (leiasr_query_recommended_view_dimensions(5.0, &sr_w, &sr_h, &refresh, &nat_w, &nat_h) && nat_w > 0 &&
	    nat_h > 0) {
		out_info->display_pixel_width = nat_w;
		out_info->display_pixel_height = nat_h;
		out_info->recommended_view_scale_x = (float)sr_w / (float)nat_w;
		out_info->recommended_view_scale_y = (float)sr_h / (float)nat_h;
		any_populated = true;
	}

	/* Physical dimensions + nominal viewer position from SR SDK. */
	struct leiasr_display_dimensions dims = {0};
	if (leiasr_static_get_display_dimensions(&dims) && dims.valid) {
		out_info->display_width_m = dims.width_m;
		out_info->display_height_m = dims.height_m;
		out_info->nominal_viewer_x_m = dims.nominal_x_m;
		out_info->nominal_viewer_y_m = dims.nominal_y_m;
		out_info->nominal_viewer_z_m = dims.nominal_z_m;
		any_populated = true;
	}

	/* EDID screen position — cached by probe(), zero if not available. */
	struct leia_display_probe_result edid;
	if (leia_edid_get_cached_result(&edid) && edid.hw_found) {
		out_info->display_screen_left = edid.screen_left;
		out_info->display_screen_top = edid.screen_top;
	}

	/* Leia: MANAGED eye tracking only — the SR SDK owns the grace
	 * period + transition handling. */
	out_info->supported_eye_tracking_modes = 1u; /* MANAGED_BIT */
	out_info->default_eye_tracking_mode = 0u;    /* MANAGED */

	return any_populated;
}


/*
 *
 * Vtable.
 *
 */

static struct xrt_plugin_iface g_leia_iface = {
    .struct_size = sizeof(struct xrt_plugin_iface),
    .reserved_0 = 0,

    .id = "leia-sr",
    .display_name = "DisplayXR Leia SR",
    .vendor = "Leia Inc.",
    .version = NULL, /* runtime release tag; filled in at install time when needed */

    .probe = leia_plugin_probe,
    .create_device = leia_plugin_create_device,

    /*
     * Per-graphics-API DP factories. Each compile-time-gated to the
     * weaver libraries available in the SR SDK at build time, so a
     * plug-in built without the D3D12 weaver (etc.) cleanly surfaces
     * NULL — the runtime then falls back to the sim_display DP for
     * that API path on the same probe-winning Leia device. The
     * factory signatures already match the xrt_dp_factory_*_fn_t
     * typedefs.
     */
#ifdef XRT_HAVE_LEIA_SR_VULKAN
    .create_dp_vk = leia_dp_factory_vk,
#else
    .create_dp_vk = NULL,
#endif

#ifdef XRT_HAVE_LEIA_SR_D3D11
    .create_dp_d3d11 = leia_dp_factory_d3d11,
#else
    .create_dp_d3d11 = NULL,
#endif

#ifdef XRT_HAVE_LEIA_SR_D3D12
    .create_dp_d3d12 = leia_dp_factory_d3d12,
#else
    .create_dp_d3d12 = NULL,
#endif

#ifdef XRT_HAVE_LEIA_SR_GL
    .create_dp_gl = leia_dp_factory_gl,
#else
    .create_dp_gl = NULL,
#endif

    /* drv_leia has no Metal weaver — that's the macOS sim_display path. */
    .create_dp_metal = NULL,

    .destroy = leia_plugin_destroy,

    .get_display_info = leia_plugin_get_display_info,
};


/*
 *
 * Entry point.
 *
 */

XRT_PLUGIN_EXPORT xrt_result_t
xrtPluginNegotiate(uint32_t runtime_api_version,
                   const struct xrt_plugin_host_iface *host,
                   struct xrt_plugin_iface **out_iface,
                   uint32_t *out_plugin_api_version)
{
	(void)host;

	*out_plugin_api_version = XRT_PLUGIN_API_VERSION_CURRENT;

	if (runtime_api_version != XRT_PLUGIN_API_VERSION_CURRENT) {
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	*out_iface = &g_leia_iface;
	return XRT_SUCCESS;
}
