// Copyright 2020-2024, Collabora, Ltd.
// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared default implementation of the instance with compositor.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author David Fattal
 */

#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_config_os.h"


#include "os/os_time.h"

#include "util/u_debug.h"
#include "util/u_system.h"
#include "util/u_tiling.h"
#include "util/u_trace_marker.h"
#include "util/u_system_helpers.h"

// comp_main (Vulkan server compositor) has been removed — lightweight runtime
// uses null compositor + native compositors (D3D11/Metal) only.

// D3D11 service compositor is used when available (both service and hybrid client)
#ifdef XRT_USE_D3D11_SERVICE_COMPOSITOR
#include "d3d11_service/comp_d3d11_service.h"
#endif

// SR display dimension query for proper swapchain dimensions.
// Only compiled in the in-proc fallback build (issue #256 / ADR-019);
// production builds query through the plug-in iface and have no
// drv_leia symbols available at link time.
#if defined(XRT_HAVE_LEIA_SR) && defined(XRT_PLUGIN_BUILD_INPROC_FALLBACK)
#include "xrt/xrt_compositor.h"
#include "leia/leia_interface.h"
#include "leia/leia_sr_d3d11.h"
#include "leia/leia_display_processor.h"
#include "leia/leia_display_processor_d3d11.h"
#ifdef XRT_HAVE_LEIA_SR_D3D12
#include "leia/leia_display_processor_d3d12.h"
#endif
#ifdef XRT_HAVE_LEIA_SR_GL
#include "leia/leia_display_processor_gl.h"
#endif
#endif

// sim_display display info for XR_EXT_display_info fallback — only
// compiled in the developer in-proc fallback build (ADR-019 / issue
// #256). Production builds route everything through the plug-in iface.
#ifdef XRT_PLUGIN_BUILD_INPROC_FALLBACK
#include "sim_display/sim_display_interface.h"
#endif

#include "target_plugin_loader.h"

#include "target_instance_parts.h"

#include <assert.h>
#include <string.h>

#ifdef XRT_OS_ANDROID
#include "android/android_instance_base.h"
#endif

// In D3D11-only service mode, prefer D3D11 service compositor over null.
// Otherwise, use null compositor (no Vulkan server compositor in lightweight runtime).
#ifdef XRT_D3D11_SERVICE_ONLY
#define USE_NULL_DEFAULT (false)
#else
#define USE_NULL_DEFAULT (true)
#endif

DEBUG_GET_ONCE_BOOL_OPTION(use_null, "XRT_COMPOSITOR_NULL", USE_NULL_DEFAULT)

// When D3D11 service compositor is available, prefer it to avoid Vulkan-D3D11 interop issues
// This is enabled for both the service (target_instance) and client (target_instance_hybrid)
#ifdef XRT_USE_D3D11_SERVICE_COMPOSITOR
DEBUG_GET_ONCE_BOOL_OPTION(use_d3d11_service, "XRT_SERVICE_USE_D3D11", true)
#endif

xrt_result_t
null_compositor_create_system(struct xrt_device *xdev, struct xrt_system_compositor **out_xsysc);

xrt_result_t
null_compositor_create_system_with_dims(struct xrt_device *xdev,
                                         uint32_t recommended_width,
                                         uint32_t recommended_height,
                                         float refresh_rate_hz,
                                         struct xrt_system_compositor **out_xsysc);



/*
 *
 * Internal functions.
 *
 */

static xrt_result_t
t_instance_create_system(struct xrt_instance *xinst,
                         struct xrt_system **out_xsys,
                         struct xrt_system_devices **out_xsysd,
                         struct xrt_space_overseer **out_xso,
                         struct xrt_system_compositor **out_xsysc)
{
	XRT_TRACE_MARKER();

	assert(out_xsys != NULL);
	assert(*out_xsys == NULL);
	assert(out_xsysd != NULL);
	assert(*out_xsysd == NULL);
	assert(out_xso != NULL);
	assert(*out_xso == NULL);
	assert(out_xsysc == NULL || *out_xsysc == NULL);

	struct u_system *usys = NULL;
	struct xrt_system_compositor *xsysc = NULL;
	struct xrt_space_overseer *xso = NULL;
	struct xrt_system_devices *xsysd = NULL;
	xrt_result_t xret = XRT_SUCCESS;

	usys = u_system_create();
	assert(usys != NULL); // Should never fail.

	xret = u_system_devices_create_from_prober( //
	    xinst,                                  // xinst
	    &usys->broadcast,                       // broadcast
	    &xsysd,                                 // out_xsysd
	    &xso);                                  // out_xso
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	// Early out if we only want devices.
	if (out_xsysc == NULL) {
		goto out;
	}

	struct xrt_device *head = xsysd->static_roles.head;
	U_LOG_W("System head device: '%s'", head->str);
	u_system_fill_properties(usys, head->str);

	bool use_null = debug_get_bool_option_use_null();

#ifdef XRT_MODULE_COMPOSITOR_NULL
	if (use_null) {
		float sr_refresh_rate_hz = 0.0f;

#if defined(XRT_HAVE_LEIA_SR) && defined(XRT_PLUGIN_BUILD_INPROC_FALLBACK)
		// Query SR display for refresh rate only; dims come from device
		// native resolution. Only available in the in-proc fallback
		// build — production builds get the SR refresh rate through the
		// plug-in iface (TODO: add to xrt_plugin_display_info if it
		// becomes load-bearing for the null compositor path).
		//
		// Gate on the cached EDID probe rather than on a device-name
		// substring: leia_edid_get_cached_result() reports hw_found=true
		// only when a known Leia/Dimenco panel is connected, so the slow
		// 5s leiasr_query_* call is skipped cleanly on sim_display and
		// any non-Leia HMD without depending on driver-name strings.
		struct leia_display_probe_result probe = {0};
		if (leia_edid_get_cached_result(&probe) && probe.hw_found) {
			uint32_t sr_rec_width = 0, sr_rec_height = 0;
			uint32_t sr_native_width = 0, sr_native_height = 0;
			if (leiasr_query_recommended_view_dimensions(5.0, &sr_rec_width, &sr_rec_height,
			                                             &sr_refresh_rate_hz, &sr_native_width,
			                                             &sr_native_height)) {
				U_LOG_I("Using SR display refresh rate: %.0f Hz", sr_refresh_rate_hz);
			} else {
				U_LOG_W("Could not query SR display, using default refresh rate");
			}
		}
#endif

		xret = null_compositor_create_system_with_dims(head, 0, 0,
		                                               sr_refresh_rate_hz, &xsysc);
	}
#else
	if (use_null) {
		U_LOG_E("The null compositor is not compiled in!");
		xret = XRT_ERROR_VULKAN;
	}
#endif

#ifdef XRT_USE_D3D11_SERVICE_COMPOSITOR
	// Try D3D11 service compositor first (preferred for Windows service mode)
	if (xret == XRT_SUCCESS && xsysc == NULL && debug_get_bool_option_use_d3d11_service()) {
		U_LOG_W("Using D3D11 service compositor");
		xret = comp_d3d11_service_create_system(head, xsysd, usys, &xsysc);
		if (xret == XRT_SUCCESS && xsysc != NULL) {
			U_LOG_W("Service compositor ready: D3D11 service compositor (pure D3D11, no Vulkan)");
		}
		if (xret != XRT_SUCCESS) {
#ifdef XRT_D3D11_SERVICE_ONLY
			// D3D11-only mode (service): no Vulkan fallback available
			U_LOG_E("D3D11 service compositor creation failed (no Vulkan fallback in service mode)");
			// Don't reset xret - let the error propagate
#else
			// Hybrid client: can fall back to Vulkan
			U_LOG_W("D3D11 service compositor creation failed, falling back to Vulkan");
			xret = XRT_SUCCESS; // Reset to allow fallback
#endif
		}
	}
#ifdef XRT_D3D11_SERVICE_ONLY
	// D3D11-only mode: if D3D11 was disabled, error out
	if (xret == XRT_SUCCESS && xsysc == NULL && !debug_get_bool_option_use_d3d11_service()) {
		U_LOG_E("D3D11 service compositor disabled via XRT_SERVICE_USE_D3D11=0, but Vulkan is not available in service mode");
		xret = XRT_ERROR_VULKAN;
	}
#endif
#endif

	if (!use_null && xsysc == NULL) {
		U_LOG_E("Explicitly didn't request the null compositor, but no compositor is available!");
		xret = XRT_ERROR_VULKAN;
	}

	if (xret != XRT_SUCCESS) {
		goto err_destroy;
	}

out:
	*out_xsys = &usys->base;
	*out_xsysd = xsysd;
	*out_xso = xso;

	if (xsysc != NULL) {
		// Tell the system about the system compositor.
		u_system_set_system_compositor(usys, xsysc);

		// Vendor-neutral display-info population through the plug-in
		// iface (issue #256 / ADR-019). Runs FIRST regardless of whether
		// the active plug-in is leia-sr or sim-display — the
		// iface->get_display_info call returns the same shape of struct
		// for either. After this, dp_factory_* + xsysc->info are
		// populated and the legacy in-proc branches below see the
		// "already filled in" guards and skip themselves.
		bool plugin_filled_display_info = false;
		const struct xrt_plugin_iface *plugin = target_plugin_get_active();
		if (plugin != NULL && plugin->struct_size >
		                          offsetof(struct xrt_plugin_iface, get_display_info) &&
		    plugin->get_display_info != NULL) {
			struct xrt_plugin_display_info pdi = {0};
			pdi.struct_size = (uint32_t)sizeof(pdi);
			if (plugin->get_display_info(target_plugin_get_active_instance(), head, &pdi)) {
				xsysc->info.display_width_m = pdi.display_width_m;
				xsysc->info.display_height_m = pdi.display_height_m;
				xsysc->info.nominal_viewer_x_m = pdi.nominal_viewer_x_m;
				xsysc->info.nominal_viewer_y_m = pdi.nominal_viewer_y_m;
				xsysc->info.nominal_viewer_z_m = pdi.nominal_viewer_z_m;
				xsysc->info.display_pixel_width = pdi.display_pixel_width;
				xsysc->info.display_pixel_height = pdi.display_pixel_height;
				xsysc->info.display_screen_left = pdi.display_screen_left;
				xsysc->info.display_screen_top = pdi.display_screen_top;
				xsysc->info.supported_eye_tracking_modes = pdi.supported_eye_tracking_modes;
				xsysc->info.default_eye_tracking_mode = pdi.default_eye_tracking_mode;

				// Compute tiling once we have native pixel dims.
				if (pdi.display_pixel_width > 0 && pdi.display_pixel_height > 0) {
					for (uint32_t mi = 0; mi < head->rendering_mode_count; mi++) {
						u_tiling_compute_mode(&head->rendering_modes[mi],
						                      pdi.display_pixel_width,
						                      pdi.display_pixel_height);
					}
					u_tiling_compute_system_atlas(
					    head->rendering_modes, head->rendering_mode_count,
					    &xsysc->info.atlas_width_pixels,
					    &xsysc->info.atlas_height_pixels);
				}

				// Recommended view scale: prefer iface-supplied (Leia SR
				// gives this from its weaver); otherwise derive worst-case
				// from rendering modes (the sim_display path).
				if (pdi.recommended_view_scale_x > 0.0f && pdi.recommended_view_scale_y > 0.0f) {
					xsysc->info.recommended_view_scale_x = pdi.recommended_view_scale_x;
					xsysc->info.recommended_view_scale_y = pdi.recommended_view_scale_y;
				} else {
					float min_scale_x = 1.0f, min_scale_y = 1.0f;
					for (uint32_t mi = 0; mi < head->rendering_mode_count; mi++) {
						if (head->rendering_modes[mi].view_scale_x > 0.0f &&
						    head->rendering_modes[mi].view_scale_x < min_scale_x)
							min_scale_x = head->rendering_modes[mi].view_scale_x;
						if (head->rendering_modes[mi].view_scale_y > 0.0f &&
						    head->rendering_modes[mi].view_scale_y < min_scale_y)
							min_scale_y = head->rendering_modes[mi].view_scale_y;
					}
					xsysc->info.recommended_view_scale_x = min_scale_x;
					xsysc->info.recommended_view_scale_y = min_scale_y;
				}

				// Per-API DP factories from the iface.
				if (plugin->create_dp_vk != NULL) {
					xsysc->info.dp_factory_vk = (void *)plugin->create_dp_vk;
				}
#ifdef XRT_OS_WINDOWS
				if (plugin->create_dp_d3d11 != NULL) {
					xsysc->info.dp_factory_d3d11 = (void *)plugin->create_dp_d3d11;
				}
				if (plugin->create_dp_d3d12 != NULL) {
					xsysc->info.dp_factory_d3d12 = (void *)plugin->create_dp_d3d12;
				}
#endif
				if (plugin->create_dp_gl != NULL) {
					xsysc->info.dp_factory_gl = (void *)plugin->create_dp_gl;
				}
#ifdef __APPLE__
				if (plugin->create_dp_metal != NULL) {
					xsysc->info.dp_factory_metal = (void *)plugin->create_dp_metal;
				}
#endif

				U_LOG_W("XR_EXT_display_info (iface=%s): display=%.4f x %.4f m, "
				        "nominal=(%.4f, %.4f, %.4f) m, scale=%.4f x %.4f, "
				        "pixels=%ux%u, atlas=%ux%u",
				        plugin->id ? plugin->id : "?",
				        pdi.display_width_m, pdi.display_height_m,
				        pdi.nominal_viewer_x_m, pdi.nominal_viewer_y_m, pdi.nominal_viewer_z_m,
				        xsysc->info.recommended_view_scale_x,
				        xsysc->info.recommended_view_scale_y, pdi.display_pixel_width,
				        pdi.display_pixel_height, xsysc->info.atlas_width_pixels,
				        xsysc->info.atlas_height_pixels);

				plugin_filled_display_info = true;
			}
		}

#if defined(XRT_HAVE_LEIA_SR) && defined(XRT_PLUGIN_BUILD_INPROC_FALLBACK)
		// Legacy in-proc Leia path — only compiled when
		// XRT_PLUGIN_BUILD_INPROC_FALLBACK is on (developer fallback for
		// debugging without the plug-in DLL). Production runtime uses
		// the iface-populated values above and never touches these
		// vendor symbols.
		//
		// Gate on the cached EDID probe (non-blocking) rather than on a
		// device-name substring: hw_found is true iff a known
		// Leia/Dimenco panel is connected, so non-Leia setups skip the
		// whole block cleanly without depending on driver-name strings.
		struct leia_display_probe_result probe_edid = {0};
		if (!plugin_filled_display_info &&
		    leia_edid_get_cached_result(&probe_edid) && probe_edid.hw_found)
		{
			uint32_t di_sr_w = 0, di_sr_h = 0, di_nat_w = 0, di_nat_h = 0;
			float di_refresh = 0.0f;
			if (leiasr_query_recommended_view_dimensions(5.0, &di_sr_w, &di_sr_h, &di_refresh,
			                                             &di_nat_w, &di_nat_h) &&
			    di_nat_w > 0 && di_nat_h > 0) {
				xsysc->info.recommended_view_scale_x = (float)di_sr_w / (float)di_nat_w;
				xsysc->info.recommended_view_scale_y = (float)di_sr_h / (float)di_nat_h;
				xsysc->info.display_pixel_width = di_nat_w;
				xsysc->info.display_pixel_height = di_nat_h;
				U_LOG_W("XR_EXT_display_info (in-proc): scale=%.4f x %.4f (sr=%ux%u, native=%ux%u)",
				        xsysc->info.recommended_view_scale_x,
				        xsysc->info.recommended_view_scale_y, di_sr_w, di_sr_h, di_nat_w, di_nat_h);
			}

			struct leiasr_display_dimensions dims = {0};
			if (leiasr_static_get_display_dimensions(&dims) && dims.valid) {
				xsysc->info.display_width_m = dims.width_m;
				xsysc->info.display_height_m = dims.height_m;
				xsysc->info.nominal_viewer_x_m = dims.nominal_x_m;
				xsysc->info.nominal_viewer_y_m = dims.nominal_y_m;
				xsysc->info.nominal_viewer_z_m = dims.nominal_z_m;
				xsysc->info.supported_eye_tracking_modes = 1; /* MANAGED_BIT */
				xsysc->info.default_eye_tracking_mode = 0;    /* MANAGED */
			}

			// Reuse the gating probe — hw_found guarantees screen
			// coords are valid.
			xsysc->info.display_screen_left = probe_edid.screen_left;
			xsysc->info.display_screen_top = probe_edid.screen_top;

			if (xsysc->info.display_pixel_width > 0 && xsysc->info.display_pixel_height > 0) {
				for (uint32_t mi = 0; mi < head->rendering_mode_count; mi++) {
					u_tiling_compute_mode(&head->rendering_modes[mi],
					                      xsysc->info.display_pixel_width,
					                      xsysc->info.display_pixel_height);
				}
				u_tiling_compute_system_atlas(head->rendering_modes,
				                              head->rendering_mode_count,
				                              &xsysc->info.atlas_width_pixels,
				                              &xsysc->info.atlas_height_pixels);
			}

#ifdef XRT_HAVE_LEIA_SR_VULKAN
			if (xsysc->info.dp_factory_vk == NULL) {
				xsysc->info.dp_factory_vk = (void *)leia_dp_factory_vk;
			}
#endif
			if (xsysc->info.dp_factory_d3d11 == NULL) {
				xsysc->info.dp_factory_d3d11 = (void *)leia_dp_factory_d3d11;
			}
#ifdef XRT_HAVE_LEIA_SR_D3D12
			if (xsysc->info.dp_factory_d3d12 == NULL) {
				xsysc->info.dp_factory_d3d12 = (void *)leia_dp_factory_d3d12;
			}
#endif
#ifdef XRT_HAVE_LEIA_SR_GL
			if (xsysc->info.dp_factory_gl == NULL) {
				xsysc->info.dp_factory_gl = (void *)leia_dp_factory_gl;
			}
#endif
		}
#endif /* XRT_HAVE_LEIA_SR && XRT_PLUGIN_BUILD_INPROC_FALLBACK */

		/* Consumed by the gated fallback blocks (Leia in-proc + sim_display
		 * in-proc). When neither fallback compiles in (production builds),
		 * silence -Wunused-but-set-variable. */
		(void)plugin_filled_display_info;

		// sim_display fallback. Production builds (ADR-019/issue #256)
		// populate everything through the plug-in iface above and skip
		// this block entirely — the runtime DLL no longer link-includes
		// drv_sim_display, so its symbols aren't available.
		//
		// XRT_PLUGIN_BUILD_INPROC_FALLBACK keeps the static path
		// compiled in for developer iteration without a registered
		// plug-in dylib. Even in the fallback build, the iface path
		// runs first; this block only fires when no plug-in loaded
		// AND the static archive is on the link line.
#ifdef XRT_PLUGIN_BUILD_INPROC_FALLBACK
		{
			struct sim_display_info sd_info;
			if (!plugin_filled_display_info && xsysc->info.display_width_m == 0.0f &&
			    sim_display_get_display_info(head, &sd_info)) {
				xsysc->info.display_width_m = sd_info.display_width_m;
				xsysc->info.display_height_m = sd_info.display_height_m;
				xsysc->info.nominal_viewer_x_m = 0.0f;
				xsysc->info.nominal_viewer_y_m = sd_info.nominal_y_m;
				xsysc->info.nominal_viewer_z_m = sd_info.nominal_z_m;
				xsysc->info.display_pixel_width = sd_info.display_pixel_width;
				xsysc->info.display_pixel_height = sd_info.display_pixel_height;

				// Compute tiling for all modes and derive system atlas
				for (uint32_t mi = 0; mi < head->rendering_mode_count; mi++) {
					u_tiling_compute_mode(&head->rendering_modes[mi],
					                      sd_info.display_pixel_width,
					                      sd_info.display_pixel_height);
				}
				u_tiling_compute_system_atlas(head->rendering_modes,
				                              head->rendering_mode_count,
				                              &xsysc->info.atlas_width_pixels,
				                              &xsysc->info.atlas_height_pixels);

				// Backward compat: recommended_view_scale from worst-case mode
				float min_scale_x = 1.0f, min_scale_y = 1.0f;
				for (uint32_t mi = 0; mi < head->rendering_mode_count; mi++) {
					if (head->rendering_modes[mi].view_scale_x > 0.0f &&
					    head->rendering_modes[mi].view_scale_x < min_scale_x)
						min_scale_x = head->rendering_modes[mi].view_scale_x;
					if (head->rendering_modes[mi].view_scale_y > 0.0f &&
					    head->rendering_modes[mi].view_scale_y < min_scale_y)
						min_scale_y = head->rendering_modes[mi].view_scale_y;
				}
				xsysc->info.recommended_view_scale_x = min_scale_x;
				xsysc->info.recommended_view_scale_y = min_scale_y;

				// Sim display: manual eye tracking only (simulated device, always "tracking")
				xsysc->info.supported_eye_tracking_modes = 2; // MANUAL_BIT
				xsysc->info.default_eye_tracking_mode = 1;    // MANUAL
				U_LOG_W("XR_EXT_display_info (sim_display in-proc fallback): display=%.3fx%.3f m, "
				        "nominal=(0, %.3f, %.3f) m, scale=%.2fx%.2f, atlas=%ux%u, pixels=%ux%u",
				        sd_info.display_width_m, sd_info.display_height_m,
				        sd_info.nominal_y_m, sd_info.nominal_z_m,
				        xsysc->info.recommended_view_scale_x,
				        xsysc->info.recommended_view_scale_y,
				        xsysc->info.atlas_width_pixels,
				        xsysc->info.atlas_height_pixels,
				        sd_info.display_pixel_width, sd_info.display_pixel_height);

				if (xsysc->info.dp_factory_vk == NULL) {
					xsysc->info.dp_factory_vk = (void *)sim_display_dp_factory_vk;
				}
#ifdef XRT_OS_WINDOWS
				if (xsysc->info.dp_factory_d3d11 == NULL) {
					xsysc->info.dp_factory_d3d11 = (void *)sim_display_dp_factory_d3d11;
				}
#endif
#if defined(XRT_OS_WINDOWS) && defined(XRT_HAVE_D3D12)
				if (xsysc->info.dp_factory_d3d12 == NULL) {
					xsysc->info.dp_factory_d3d12 = (void *)sim_display_dp_factory_d3d12;
				}
#endif
#ifdef __APPLE__
				if (xsysc->info.dp_factory_metal == NULL) {
					xsysc->info.dp_factory_metal = (void *)sim_display_dp_factory_metal;
				}
#endif
				if (xsysc->info.dp_factory_gl == NULL) {
					xsysc->info.dp_factory_gl = (void *)sim_display_dp_factory_gl;
				}
			}
		}
#endif /* XRT_PLUGIN_BUILD_INPROC_FALLBACK */

		assert(out_xsysc != NULL);
		*out_xsysc = xsysc;
	}

	return xret;


err_destroy:
	xrt_space_overseer_destroy(&xso);
	xrt_system_devices_destroy(&xsysd);
	u_system_destroy(&usys);

	return xret;
}


/*
 *
 * Exported function(s).
 *
 */

#ifdef XRT_FEATURE_HYBRID_MODE
// In hybrid mode, export as native_instance_create to avoid symbol conflict
// with ipc_instance_create from the IPC client library
xrt_result_t
native_instance_create(struct xrt_instance_info *ii, struct xrt_instance **out_xinst)
#else
xrt_result_t
xrt_instance_create(struct xrt_instance_info *ii, struct xrt_instance **out_xinst)
#endif
{
	struct xrt_prober *xp = NULL;

	u_trace_marker_init();

	XRT_TRACE_MARKER();

	int ret = xrt_prober_create_with_lists(&xp, &target_lists);
	if (ret < 0) {
		return XRT_ERROR_PROBER_CREATION_FAILED;
	}

	struct t_instance *tinst = U_TYPED_CALLOC(struct t_instance);
	tinst->base.create_system = t_instance_create_system;
	tinst->base.get_prober = t_instance_get_prober;
	tinst->base.destroy = t_instance_destroy;
	tinst->xp = xp;

	tinst->base.startup_timestamp = os_monotonic_get_ns();

#ifdef XRT_OS_ANDROID
	if (ii != NULL) {
		ret = android_instance_base_init(&tinst->android, &tinst->base, ii);
		if (ret < 0) {
			xrt_prober_destroy(&xp);
			free(tinst);
			return ret;
		}
	}
#endif // XRT_OS_ANDROID

	*out_xinst = &tinst->base;

	return XRT_SUCCESS;
}
