// Copyright 2018-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds D3D11 specific session functions.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup oxr_main
 * @ingroup comp_client
 */

#include <stdlib.h>

#include "util/u_misc.h"
#include "util/u_logging.h"

#include "xrt/xrt_instance.h"
#include "xrt/xrt_gfx_d3d11.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_two_call.h"
#include "oxr_handle.h"


XrResult
oxr_session_populate_d3d11(struct oxr_logger *log,
                           struct oxr_system *sys,
                           XrGraphicsBindingD3D11KHR const *next,
                           struct oxr_session *sess)
{
	struct xrt_compositor_native *xcn = sess->xcn;

	// Diagnostic logging for IPC debugging
	U_LOG_W("oxr_session_populate_d3d11: xcn=%p, device=%p", (void*)xcn, (void*)next->device);

	if (xcn == NULL) {
		U_LOG_E("oxr_session_populate_d3d11: xcn is NULL! IPC compositor not created.");
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "IPC compositor (xcn) is NULL - session not properly initialized");
	}

	// Log compositor info to verify IPC is working
	U_LOG_W("oxr_session_populate_d3d11: xcn->base.info.format_count=%u", xcn->base.info.format_count);

	struct xrt_compositor_d3d11 *xcd3d = xrt_gfx_d3d11_provider_create( //
	    xcn,                                                            //
	    next->device);                                                  //

	if (xcd3d == NULL) {
		U_LOG_E("oxr_session_populate_d3d11: xrt_gfx_d3d11_provider_create returned NULL!");
		U_LOG_E("  This usually means D3D11 device doesn't support ID3D11Device5 or");
		U_LOG_E("  the IPC compositor has incompatible formats. Check D3D_COMPOSITOR_LOG for details.");
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create D3D11 client compositor - check logs for details");
	}

	U_LOG_W("oxr_session_populate_d3d11: D3D11 client compositor created successfully");
	U_LOG_W("  xcd3d->base.info.format_count=%u", xcd3d->base.info.format_count);

	sess->compositor = &xcd3d->base;
	sess->create_swapchain = oxr_swapchain_d3d11_create;

	return XR_SUCCESS;
}
