// Copyright 2026, The DisplayXR Project
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
#include "sim_display_stereo_camera.h"
#if defined(XRT_HAVE_VULKAN) || !defined(_WIN32)
#include "vk/vk_helpers.h" // #1243: sizeof(struct vk_bundle) fingerprint
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>


/*!
 * ADR-051: bumped on every probe_displays so the per-screen status
 * change_counter moves when the monitor set is re-probed.
 */
static xrt_atomic_s32_t g_sim_display_probe_count = 0;


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
	 * XrEventDataEyeTrackingStateChangedDXR are testable without
	 * hardware. Opt-in webcam tracking (#1855, SIM_DISPLAY_WEBCAM_TRACKING
	 * with a face estimator built in) adds MANAGED_BIT and makes MANAGED the
	 * default. Neither: 0 / 0. */
	sim_display_eye_tracking_caps(&out_info->supported_eye_tracking_modes, &out_info->default_eye_tracking_mode);

	return true;
}


/*!
 * Multi-screen M1: describe ONE monitor sim_display won (FALLBACK claim) —
 * the per-monitor twin of @ref sim_display_plugin_get_display_info. Physical
 * size from the monitor's EDID mm (the env/default size when EDID has none),
 * pixels from the connector's device mode (else the desktop mode), sim's
 * usual nominal viewer, and the same eye-tracking advertisement as the head
 * (none, or MANUAL under SIM_DISPLAY_FAKE_TRACKING=1).
 */
static bool
sim_display_plugin_get_display_info_for_monitor(struct xrt_plugin_instance *inst,
                                                const struct xrt_display_descriptor *display,
                                                const struct xrt_display_physical *physical,
                                                struct xrt_plugin_display_info *out_info)
{
	(void)inst;
	if (display == NULL || out_info == NULL ||
	    out_info->struct_size <
	        offsetof(struct xrt_plugin_display_info, supported_eye_tracking_modes) + 2 * sizeof(uint32_t)) {
		return false;
	}

	float w_m = 0.0f, h_m = 0.0f, ny = 0.0f, nz = 0.0f;
	sim_display_get_default_viewer(&w_m, &h_m, &ny, &nz);

	uint32_t mm_w = 0, mm_h = 0, px_w = 0, px_h = 0;
	if (physical != NULL &&
	    physical->struct_size >= offsetof(struct xrt_display_physical, native_pixel_height) + sizeof(uint32_t)) {
		mm_w = physical->physical_width_mm;
		mm_h = physical->physical_height_mm;
		px_w = physical->native_pixel_width;
		px_h = physical->native_pixel_height;
	}
	if (mm_w > 0 && mm_h > 0) {
		w_m = (float)mm_w / 1000.0f;
		h_m = (float)mm_h / 1000.0f;
	}
	if (px_w == 0 || px_h == 0) {
		px_w = display->pixel_width;
		px_h = display->pixel_height;
	}

	out_info->display_width_m = w_m;
	out_info->display_height_m = h_m;
	out_info->nominal_viewer_x_m = 0.0f;
	out_info->nominal_viewer_y_m = ny;
	out_info->nominal_viewer_z_m = nz;
	out_info->display_pixel_width = px_w;
	out_info->display_pixel_height = px_h;
	out_info->recommended_view_scale_x = 0.0f; // runtime derives
	out_info->recommended_view_scale_y = 0.0f;
	out_info->display_screen_left = display->screen_left;
	out_info->display_screen_top = display->screen_top;
	if (sim_display_fake_tracking_enabled()) {
		out_info->supported_eye_tracking_modes = 2u; /* MANUAL_BIT */
		out_info->default_eye_tracking_mode = 1u;    /* MANUAL */
	} else {
		out_info->supported_eye_tracking_modes = 0u;
		out_info->default_eye_tracking_mode = 0u;
	}
	if (out_info->struct_size >= offsetof(struct xrt_plugin_display_info, refresh_mhz) + sizeof(uint32_t)) {
		out_info->refresh_mhz = display->refresh_mhz;
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

	xrt_atomic_s32_inc_return(&g_sim_display_probe_count);

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
#if !defined(XRT_OS_ANDROID) && defined(XRT_HAVE_OPENGL)
		// Desktop-GL DP only; Android is VK-only and a headless Linux build
		// without GL dev libs compiles no GL processor (XRT_HAVE_OPENGL off).
		c->supported_apis |= XRT_DP_API_BIT_GL;
#endif
#if defined(__APPLE__)
		c->supported_apis |= XRT_DP_API_BIT_METAL;
#endif
		c->serial[0] = '\0';
	}
	return n;
}

/*!
 * ADR-051 D2 per-screen status: synthetic values, so every dashboard row has
 * one on a box with no hardware. Pure C, reads only process-local state —
 * passive by construction.
 */
static xrt_result_t
sim_display_plugin_get_screen_status(struct xrt_plugin_instance *inst,
                                     uint64_t monitor_id,
                                     struct xrt_plugin_screen_status *out)
{
	(void)inst;
	if (out == NULL || out->struct_size < sizeof(struct xrt_plugin_screen_status)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED; // caller too old for v1: "no vendor status"
	}

	uint64_t edges = 0;
	bool tracking = sim_display_fake_tracking_sample(&edges);
	// #1855: a running webcam tracker is the real state (MANAGED wins over the fake).
	bool webcam_tracking = false;
	uint64_t webcam_edges = 0;
	const bool webcam = sim_display_webcam_tracking_status(&webcam_tracking, &webcam_edges);
	if (webcam) {
		tracking = webcam_tracking;
		edges = webcam_edges;
	}

	out->version = XRT_PLUGIN_SCREEN_STATUS_VERSION;
	// Both terms are monotonic, so the sum moves on every tracker edge and
	// every re-probe, and nowhere else. cmpxchg(0, 0) is an atomic load.
	int32_t probes = xrt_atomic_s32_cmpxchg(&g_sim_display_probe_count, 0, 0);
	out->change_counter = edges + (uint64_t)(uint32_t)probes;
	out->ready = true;
	out->verified = true;
	out->calibrated = true;
	if (!webcam && !sim_display_fake_tracking_enabled()) {
		out->tracker = XRT_PLUGIN_TRACKER_STATE_NONE;
	} else {
		out->tracker = tracking ? XRT_PLUGIN_TRACKER_STATE_RUNNING : XRT_PLUGIN_TRACKER_STATE_DOWN;
	}
	// The output mode is one process-singleton atomic, shared by every screen
	// sim drives: it is the mode of THIS process (in a diagnostic process that
	// is the SIM_DISPLAY_OUTPUT start-up mode, not a client's live one).
	out->lens = sim_display_get_output_mode() == SIM_DISPLAY_OUTPUT_PASSTHROUGH ? XRT_PLUGIN_LENS_STATE_2D
	                                                                            : XRT_PLUGIN_LENS_STATE_3D;
	snprintf(out->model, sizeof(out->model), "Sim");
	snprintf(out->serial, sizeof(out->serial), "SIM-%016llx", (unsigned long long)monitor_id);
	out->warning_count = 1;
	memset(out->warnings, 0, sizeof(out->warnings));
	snprintf(out->warnings[0].code, sizeof(out->warnings[0].code), "SIMULATED");
	out->warnings[0].level = XRT_PLUGIN_SCREEN_WARNING_LEVEL_INFO;
	// UTF-8 em dash spelled out: MSVC reads this file in the ANSI code page.
	snprintf(out->warnings[0].text, sizeof(out->warnings[0].text),
	         "Simulated display processor \xE2\x80\x94 no hardware");
	out->dashboard_command[0] = '\0';
	return XRT_SUCCESS;
}


/*
 *
 * Vtable.
 *
 */

/*!
 * ADR-045 platform state. sim_display has no platform to be missing: it is
 * always READY, and it is the FALLBACK plug-in — the runtime adopts a vendor
 * plug-in on re-probe only while this one is active.
 */
static bool
sim_display_plugin_get_platform_state(struct xrt_plugin_platform_status *out_status)
{
	if (out_status == NULL || out_status->struct_size < offsetof(struct xrt_plugin_platform_status, hint)) {
		return false;
	}
	out_status->state = XRT_PLUGIN_PLATFORM_STATE_READY;
	out_status->flags = XRT_PLUGIN_PLATFORM_FLAG_FALLBACK;
	return true;
}

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
    /* #1243: vk_bundle ABI fingerprint (size + table offset) — see xrt_plugin_iface. */
    .vk_bundle_abi_size = (uint32_t)sizeof(struct vk_bundle),
    .vk_bundle_fn_table_offset = (uint32_t)offsetof(struct vk_bundle, vkGetInstanceProcAddr),
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

#if !defined(XRT_OS_ANDROID) && defined(XRT_HAVE_OPENGL)
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

    /*
     * ADR-042 lift-only D3D11 DP. sim_display builds no weaver or tracker for
     * any DP, so its ordinary factory is already "lift-only"-cheap; the lift
     * slots are filled only under SIM_DISPLAY_FAKE_LIFT=1.
     */
#if defined(_WIN32)
    .create_dp_d3d11_lift = sim_display_dp_factory_d3d11,
#else
    .create_dp_d3d11_lift = NULL,
#endif

    .get_platform_state = sim_display_plugin_get_platform_state,

    /*
     * ADR-043 stereo camera source. The slots are always filled; the FAKE
     * camera itself only exists under SIM_DISPLAY_FAKE_STEREO_CAMERA=1 (the
     * enumerate slot reports zero cameras otherwise).
     */
    .stereo_camera_enumerate = sim_display_stereo_camera_enumerate,
    .stereo_camera_get_calibration = sim_display_stereo_camera_get_calibration,
    .stereo_camera_open = sim_display_stereo_camera_open,
    .stereo_camera_wait_frame = sim_display_stereo_camera_wait_frame,
    .stereo_camera_release_frame = sim_display_stereo_camera_release_frame,
    .stereo_camera_close = sim_display_stereo_camera_close,

    /* Multi-screen M1: per-monitor display info for every monitor sim wins. */
    .get_display_info_for_monitor = sim_display_plugin_get_display_info_for_monitor,

    /* Multi-screen M2: one VK DP per screen a spanning window covers. */
#if defined(XRT_HAVE_VULKAN) || !defined(_WIN32)
    .create_dp_vk_for_screen = sim_display_dp_factory_vk_for_screen,
#else
    .create_dp_vk_for_screen = NULL,
#endif

    /* Multi-screen M6: one D3D11 DP per screen a spanning window covers. */
#if defined(_WIN32)
    .create_dp_d3d11_for_screen = sim_display_dp_factory_d3d11_for_screen,
#else
    .create_dp_d3d11_for_screen = NULL,
#endif

    /* Multi-screen on macOS: one Metal DP per screen a spanning window covers. */
#if defined(__APPLE__)
    .create_dp_metal_for_screen = sim_display_dp_factory_metal_for_screen,
#else
    .create_dp_metal_for_screen = NULL,
#endif

/* Multi-screen M6 (D3D12): one D3D12 DP per screen a spanning window covers. */
#if defined(_WIN32)
    .create_dp_d3d12_for_screen = sim_display_dp_factory_d3d12_for_screen,
#else
    .create_dp_d3d12_for_screen = NULL,
#endif

    /* ADR-051 D2: per-screen status for the dashboard (pure C, every platform). */
    .get_screen_status = sim_display_plugin_get_screen_status,
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
