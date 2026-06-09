// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Plug-in entry point for the simulation 3D display driver.
 *
 * Implements the @ref xrt_plugin_negotiate_fn_t signature defined in
 * `xrt/xrt_plugin.h`. The runtime DLL loads this plug-in via
 * `LoadLibraryExW` + `GetProcAddress("xrtPluginNegotiate")` and
 * dispatches through the returned @ref xrt_plugin_iface vtable. See
 * `docs/roadmap/vendor-plugin-architecture.md` §4.5 (the sim_display
 * plug-in section) and `docs/adr/ADR-019-vendor-plugin-aux-boundary.md`.
 *
 * sim_display has no hardware to probe — it is the vendor-neutral
 * fallback. The probe vtable entry therefore always returns
 * `XRT_SUCCESS` with a NULL instance handle; no per-instance state is
 * needed (the device, output mode, and view count atomics in
 * `sim_display_device.c` are process-singleton and used directly).
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

// xrt_config_have.h supplies the XRT_HAVE_* feature macros used by the
// vtable gating below. Without it, `#if defined(XRT_HAVE_VULKAN)` was
// silently false in this TU, so the Windows plug-in never wired
// create_dp_vk (or the VK bit in probe_displays claims) even though the
// VK display processor was compiled in — every VK app under sim-display
// on Windows ran DP-less in permanent 2D (#456).
#include "xrt/xrt_config_have.h"
#include "xrt/xrt_config_os.h" // XRT_OS_ANDROID — gates the desktop-GL DP factory off

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"

#include "sim_display_interface.h"

#include <stddef.h>


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
sim_display_plugin_probe(struct xrt_plugin_instance **out_inst)
{
	/*
	 * sim_display is the vendor-neutral fallback — it always claims the
	 * system. Per ADR-019, ProbeOrder=200 (set at registration time) ranks
	 * it after every real-hardware plug-in.
	 *
	 * There is no per-instance state: the device, output-mode atomic, and
	 * view-count atomic live in sim_display_device.c as process-singletons.
	 */
	*out_inst = NULL;
	return XRT_SUCCESS;
}

static xrt_result_t
sim_display_plugin_create_device(struct xrt_plugin_instance *inst, struct xrt_device **out_dev)
{
	(void)inst;
	struct xrt_device *xdev = sim_display_hmd_create();
	if (xdev == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	*out_dev = xdev;
	return XRT_SUCCESS;
}

static void
sim_display_plugin_destroy(struct xrt_plugin_instance *inst)
{
	(void)inst;
	/* No instance state — nothing to free. */
}

static void
sim_display_plugin_set_pose_source(struct xrt_plugin_instance *inst,
                                   struct xrt_device *xdev,
                                   struct xrt_device *source)
{
	(void)inst;
	sim_display_hmd_set_pose_source(xdev, source);
}

static bool
sim_display_plugin_get_display_info(struct xrt_plugin_instance *inst,
                                    struct xrt_device *xdev,
                                    struct xrt_plugin_display_info *out_info)
{
	(void)inst;

	struct sim_display_info sd_info;
	if (!sim_display_get_display_info(xdev, &sd_info)) {
		return false;
	}

	/*
	 * v1: runtime + plug-in are built against the same xrt_plugin.h,
	 * so out_info->struct_size always equals
	 * sizeof(struct xrt_plugin_display_info). A future struct
	 * extension will gate per-field writes on the reported
	 * struct_size for forward compat.
	 */
	(void)out_info->struct_size;

	out_info->display_width_m = sd_info.display_width_m;
	out_info->display_height_m = sd_info.display_height_m;
	out_info->nominal_viewer_x_m = 0.0f;
	out_info->nominal_viewer_y_m = sd_info.nominal_y_m;
	out_info->nominal_viewer_z_m = sd_info.nominal_z_m;
	out_info->display_pixel_width = sd_info.display_pixel_width;
	out_info->display_pixel_height = sd_info.display_pixel_height;
	/* sim_display has no SR-style recommended scale; runtime derives
	 * one from the worst-case rendering mode when these stay zero. */
	out_info->recommended_view_scale_x = 0.0f;
	out_info->recommended_view_scale_y = 0.0f;
	/* No EDID screen position for the simulated display. */
	out_info->display_screen_left = 0;
	out_info->display_screen_top = 0;
	/* Honest by default (#441): sim_display has no eye tracker — its
	 * positions are nominal, not tracked — so it advertises NO
	 * eye-tracking capability. The dev-only SIM_DISPLAY_FAKE_TRACKING
	 * toggle re-enables MANUAL_BIT (paired with HAS_TRACKING on the 3D
	 * rendering modes in sim_display_device.c) so the MANUAL path and
	 * XrEventDataEyeTrackingStateChangedEXT are testable without
	 * hardware. */
	if (sim_display_fake_tracking_enabled()) {
		out_info->supported_eye_tracking_modes = 2u; /* MANUAL_BIT */
		out_info->default_eye_tracking_mode = 1u;    /* MANUAL */
	} else {
		out_info->supported_eye_tracking_modes = 0u; /* no tracking */
		out_info->default_eye_tracking_mode = 0u;    /* undefined; keep 0 */
	}

	return true;
}


static uint32_t
sim_display_plugin_probe_displays(struct xrt_plugin_instance *inst,
                                  const struct xrt_display_descriptor *displays,
                                  uint32_t display_count,
                                  struct xrt_display_claim *out_claims,
                                  uint32_t max_claims)
{
	(void)inst;

	/*
	 * sim_display is the vendor-neutral fallback (#69 / ADR-015): claim
	 * EVERY descriptor at FALLBACK confidence so it backstops any monitor no
	 * vendor plug-in recognized. A real vendor's EDID(50)/VERIFIED(100) claim
	 * always outranks these, and a (future) per-display override can still
	 * force sim onto a specific monitor.
	 */
	uint32_t n = 0;
	for (uint32_t i = 0; i < display_count && n < max_claims; i++) {
		struct xrt_display_claim *c = &out_claims[n++];
		c->monitor_id = displays[i].monitor_id;
		c->confidence = (uint32_t)XRT_DISPLAY_CLAIM_FALLBACK;

		/* Mirror the #ifdef gating of the DP factory fields below — sim
		 * ships a factory for every API the platform supports. */
		c->supported_apis = 0;
#if defined(XRT_HAVE_VULKAN) || !defined(_WIN32)
		c->supported_apis |= XRT_DP_API_BIT_VK;
#endif
#if defined(_WIN32)
		c->supported_apis |= XRT_DP_API_BIT_D3D11 | XRT_DP_API_BIT_D3D12;
#endif
#if !defined(XRT_OS_ANDROID)
		// Desktop-GL DP only; Android is VK-only (no GL processor built).
		c->supported_apis |= XRT_DP_API_BIT_GL;
#endif
#if defined(__APPLE__)
		c->supported_apis |= XRT_DP_API_BIT_METAL;
#endif
		c->serial[0] = '\0';
	}
	return n;
}


/*
 *
 * Vtable.
 *
 */

static struct xrt_plugin_iface g_sim_display_iface = {
    .struct_size = sizeof(struct xrt_plugin_iface),
    .reserved_0 = 0,

    .id = "sim-display",
    .display_name = "DisplayXR Sim Display",
    .vendor = "DisplayXR",
    .version = NULL, /* matches the runtime's release tag at install time */

    .probe = sim_display_plugin_probe,
    .create_device = sim_display_plugin_create_device,

    /*
     * Per-graphics-API DP factories. sim_display ships factories for
     * every API the platform supports; the runtime picks one at session
     * creation based on the app's graphics binding. Each function
     * pointer is the existing factory from sim_display_interface.h —
     * the signatures already match the xrt_dp_factory_*_fn_t typedefs.
     */
#if defined(XRT_HAVE_VULKAN) || !defined(_WIN32)
    .create_dp_vk = sim_display_dp_factory_vk,
#else
    .create_dp_vk = NULL,
#endif

#if defined(_WIN32)
    .create_dp_d3d11 = sim_display_dp_factory_d3d11,
#else
    .create_dp_d3d11 = NULL,
#endif

#if defined(_WIN32)
    .create_dp_d3d12 = sim_display_dp_factory_d3d12,
#else
    .create_dp_d3d12 = NULL,
#endif

#if !defined(XRT_OS_ANDROID)
    .create_dp_gl = sim_display_dp_factory_gl,
#else
    .create_dp_gl = NULL,
#endif

#if defined(__APPLE__)
    .create_dp_metal = sim_display_dp_factory_metal,
#else
    .create_dp_metal = NULL,
#endif

    .destroy = sim_display_plugin_destroy,

    .get_display_info = sim_display_plugin_get_display_info,

    .set_pose_source = sim_display_plugin_set_pose_source,

    .probe_displays = sim_display_plugin_probe_displays,
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
		/*
		 * The runtime is from a different ABI generation. Decline
		 * cleanly; the runtime will log the mismatch and skip us.
		 */
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	*out_iface = &g_sim_display_iface;
	return XRT_SUCCESS;
}
