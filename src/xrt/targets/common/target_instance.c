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
#include "os/os_display_edid.h"

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

// Vendor display-info (Leia SR, etc.) is sourced from the plug-in DLL's
// xrt_plugin_iface — see target_plugin_loader. The runtime DLL no
// longer link-includes any vendor drv_* code (ADR-019 / #256 / #263).

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

/*!
 * Copy the per-graphics-API DP factories from a plug-in iface into the system
 * compositor info. Used both at instance create (after `get_display_info`
 * succeeded) and from the @ref refresh_display_processors_cb callback at
 * per-client compositor create. NULL plug-in is a no-op so the callback path
 * survives an empty-discovery-root start.
 */
static void
fill_dp_factories_from_plugin(struct xrt_system_compositor_info *info, const struct xrt_plugin_iface *plugin)
{
	if (info == NULL || plugin == NULL) {
		return;
	}
	if (plugin->create_dp_vk != NULL) {
		info->dp_factory_vk = (void *)plugin->create_dp_vk;
	}
#ifdef XRT_OS_WINDOWS
	if (plugin->create_dp_d3d11 != NULL) {
		info->dp_factory_d3d11 = (void *)plugin->create_dp_d3d11;
	}
	if (plugin->create_dp_d3d12 != NULL) {
		info->dp_factory_d3d12 = (void *)plugin->create_dp_d3d12;
	}
#endif
	if (plugin->create_dp_gl != NULL) {
		info->dp_factory_gl = (void *)plugin->create_dp_gl;
	}
#ifdef __APPLE__
	if (plugin->create_dp_metal != NULL) {
		info->dp_factory_metal = (void *)plugin->create_dp_metal;
	}
#endif
}

/*!
 * Enumerate connected monitors (vendor-neutral EDID), ask the registered
 * plug-ins which they claim, and build the per-monitor DP factory registry
 * (issue #69 / ADR-015). The scalar `dp_factory_*` fields are left as the
 * authoritative compositor input in Phase 1 — the registry's primary-monitor
 * winner is the same plug-in as the active one, so they stay consistent; the
 * registry is built in parallel for the CLI and the future Phase 3 compositor
 * migration. No-op (empty registry) off-Windows, where the EDID enumerator
 * returns no monitors.
 */
static void
build_dp_registry(struct xrt_system_compositor_info *info)
{
	if (info == NULL) {
		return;
	}
	struct os_display_edid_list edid = {0};
	os_display_edid_enumerate(&edid);

	struct xrt_display_descriptor descs[XRT_DP_REGISTRY_MAX_ENTRIES];
	uint32_t dn = target_plugin_build_descriptors(&edid, descs, XRT_DP_REGISTRY_MAX_ENTRIES);
	target_plugin_resolve_displays(descs, dn, &info->dp_registry);
}

/*!
 * Installed on @ref xrt_system_compositor_info::refresh_display_processors so a
 * long-lived service-mode compositor can pick up a vendor plug-in registered
 * AFTER the service started (issue #342). Each per-client compositor-create
 * path invokes it once, before any DP-factory read; the call is cheap when no
 * better plug-in has appeared. See ADR-020 / `target_plugin_refresh_active`.
 */
static void
refresh_display_processors_cb(struct xrt_system_compositor_info *info)
{
	const struct xrt_plugin_iface *plugin = target_plugin_refresh_active();
	fill_dp_factories_from_plugin(info, plugin);
	// Rebuild the per-monitor registry too — refresh_active invalidates the
	// loader's source cache on a swap, so this re-resolves against the new
	// winner (#69 / ADR-015).
	build_dp_registry(info);
}

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
		// Refresh rate sourcing: previously queried via leia_edid + SR SDK
		// when the in-proc Leia fallback was linked. Post-#263 the SR
		// path lives entirely in the plug-in DLL; the null compositor
		// uses the default refresh rate. Add an entry to
		// xrt_plugin_display_info if this becomes load-bearing.
		float sr_refresh_rate_hz = 0.0f;
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

		// Install the durable-DP-refresh callback (#342, ADR-020): long-
		// lived service compositors invoke this at per-client compositor
		// create so a plug-in registered AFTER the service started is
		// picked up on the first app launch without a service restart.
		// In-process / handle apps never call it — they create a fresh
		// instance per launch, so their initial discovery is already
		// post-install. Install unconditionally so the path works even
		// when the initial `get_display_info` below failed / had no plug-in.
		xsysc->info.refresh_display_processors = refresh_display_processors_cb;

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

				// Consistency rule (#441): supported_eye_tracking_modes != 0
				// ⇔ at least one rendering mode claims HAS_TRACKING. A
				// mismatch means the plug-in's capability advertisement and
				// its per-mode flags disagree — apps would see impossible
				// combinations (e.g. MANAGED offered but isTracking pinned
				// FALSE). One-shot init WARN, not fatal.
				{
					bool any_tracked = false;
					for (uint32_t mi = 0; mi < head->rendering_mode_count; mi++) {
						if (head->rendering_modes[mi].mode_flags &
						    XRT_RENDERING_MODE_FLAG_HAS_TRACKING) {
							any_tracked = true;
							break;
						}
					}
					if ((pdi.supported_eye_tracking_modes != 0) != any_tracked) {
						U_LOG_W("Plug-in '%s' tracking advertisement inconsistent: "
						        "supported_eye_tracking_modes=0x%x but %s rendering "
						        "mode sets XRT_RENDERING_MODE_FLAG_HAS_TRACKING "
						        "(see #441 consistency rule)",
						        plugin->id ? plugin->id : "?",
						        pdi.supported_eye_tracking_modes,
						        any_tracked ? "at least one" : "no");
					}
				}

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

				// Per-API DP factories from the iface — factored into
				// a helper so the refresh callback below uses the same
				// per-platform mask. See @ref fill_dp_factories_from_plugin.
				fill_dp_factories_from_plugin(&xsysc->info, plugin);

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

		// Build the per-monitor DP factory registry (#69 / ADR-015) from the
		// vendor-neutral EDID enumeration + per-plug-in probe_displays claims.
		// Independent of the active plug-in's get_display_info above; the
		// scalar dp_factory_* set there stays the Phase-1 compositor input.
		build_dp_registry(&xsysc->info);

		// All display-info + DP factories are sourced from the plug-in
		// iface above (ADR-019 / #256 / #263). The runtime DLL no longer
		// link-includes any drv_sim_display symbols.
		(void)plugin_filled_display_info;

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
