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
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_config_os.h"


#include "os/os_time.h"

#include "util/u_debug.h"
#include "util/u_system.h"
#include "util/u_trace_marker.h"
#include "util/u_system_helpers.h"

// Only include main compositor header when we actually need it
// (not in hybrid mode native apps which use null compositor during instance creation)
#if defined(XRT_MODULE_COMPOSITOR_MAIN) && !defined(XRT_HYBRID_USE_NULL_COMPOSITOR)
#include "main/comp_main_interface.h"
#endif

// D3D11 service compositor is used when available (both service and hybrid client)
#ifdef XRT_USE_D3D11_SERVICE_COMPOSITOR
#include "d3d11_service/comp_d3d11_service.h"
#endif

// SR display dimension query for proper swapchain dimensions
#ifdef XRT_HAVE_LEIA_SR
#include "leiasr/leiasr_d3d11.h"
#endif

#include "target_instance_parts.h"

#include <assert.h>

#ifdef XRT_OS_ANDROID
#include "android/android_instance_base.h"
#endif

// For hybrid mode native apps, always use null compositor during instance creation
// (D3D11 native compositor is created during session creation)
#ifdef XRT_HYBRID_USE_NULL_COMPOSITOR
#define USE_NULL_DEFAULT (true)
#elif defined(XRT_MODULE_COMPOSITOR_MAIN)
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
	u_system_fill_properties(usys, head->str);

	bool use_null = debug_get_bool_option_use_null();

#ifdef XRT_MODULE_COMPOSITOR_NULL
	if (use_null) {
		uint32_t sr_rec_width = 0;
		uint32_t sr_rec_height = 0;
		float sr_refresh_rate_hz = 0.0f;

#ifdef XRT_HAVE_LEIA_SR
		// Query SR display for recommended view dimensions and refresh rate
		// This ensures apps create properly-sized swapchains and correct frame pacing
		if (leiasr_query_recommended_view_dimensions(5.0, &sr_rec_width, &sr_rec_height,
		                                             &sr_refresh_rate_hz)) {
			U_LOG_I("Using SR recommended view dimensions: %ux%u per eye, %.0f Hz", sr_rec_width,
			        sr_rec_height, sr_refresh_rate_hz);
		} else {
			U_LOG_W("Could not query SR display dimensions, using defaults");
		}
#endif

		xret = null_compositor_create_system_with_dims(head, sr_rec_width, sr_rec_height,
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
		U_LOG_I("Using D3D11 service compositor");
		xret = comp_d3d11_service_create_system(head, &xsysc);
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

// Vulkan compositor fallback (not available in D3D11-only service mode or hybrid native apps)
#if defined(XRT_MODULE_COMPOSITOR_MAIN) && !defined(XRT_D3D11_SERVICE_ONLY) && !defined(XRT_HYBRID_USE_NULL_COMPOSITOR)
	if (xret == XRT_SUCCESS && xsysc == NULL) {
		xret = comp_main_create_system_compositor(head, NULL, NULL, &xsysc);
	}
#endif

#if !defined(XRT_MODULE_COMPOSITOR_MAIN) || defined(XRT_HYBRID_USE_NULL_COMPOSITOR)
	if (!use_null && xsysc == NULL) {
		U_LOG_E("Explicitly didn't request the null compositor, but no compositor is available!");
		xret = XRT_ERROR_VULKAN;
	}
#endif

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
