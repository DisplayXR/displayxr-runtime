// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The thing that binds all of the OpenXR driver together.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author David Fattal
 */

#include "xrt/xrt_config_build.h"

#include "util/u_trace_marker.h"


#ifdef XRT_FEATURE_HYBRID_MODE

/*
 * Hybrid mode: auto-select between in-process native compositor and IPC
 * based on runtime environment detection (AppContainer sandbox, etc.)
 */

// Insert the on load constructor to setup trace marker.
U_TRACE_TARGET_SETUP(U_TRACE_WHICH_SERVICE)

#include "xrt/xrt_instance.h"
#include "util/u_sandbox.h"
#include "util/u_logging.h"
#include "client/ipc_client_interface.h"

// Forward declaration of native instance creation from target_instance_hybrid
xrt_result_t
native_instance_create(struct xrt_instance_info *ii, struct xrt_instance **out_xinst);


xrt_result_t
xrt_instance_create(struct xrt_instance_info *ii, struct xrt_instance **out_xinst)
{
	u_trace_marker_init();

	XRT_TRACE_MARKER();

	// Workspace controllers (sessions with XR_DXR_spatial_workspace enabled)
	// always go IPC — the controller talks to the service over IPC by
	// design, and the in-process native compositor doesn't host the
	// workspace state. This auto-detection lets the shell drop the
	// runtime-specific SetEnvironmentVariableA("XRT_FORCE_MODE", "ipc")
	// hack and stay genuinely runtime-agnostic.
	if (ii != NULL && ii->app_info.ext_spatial_workspace_enabled) {
		U_LOG_I("Hybrid mode: workspace controller session — forcing IPC");
		return ipc_instance_create(ii, out_xinst);
	}

	// Check if we should use IPC mode
	if (u_sandbox_should_use_ipc()) {
		U_LOG_I("Hybrid mode: using IPC/service compositor (sandboxed environment)");
		return ipc_instance_create(ii, out_xinst);
	} else {
		U_LOG_I("Hybrid mode: using in-process native compositor");
		return native_instance_create(ii, out_xinst);
	}
}

#elif defined(XRT_FEATURE_IPC_CLIENT)

// Insert the on load constructor to setup trace marker.
U_TRACE_TARGET_SETUP(U_TRACE_WHICH_OPENXR)

#include "xrt/xrt_instance.h"
#include "client/ipc_client_interface.h"


xrt_result_t
xrt_instance_create(struct xrt_instance_info *ii, struct xrt_instance **out_xinst)
{
	u_trace_marker_init();

	XRT_TRACE_MARKER();

	return ipc_instance_create(ii, out_xinst);
}

#else

// Insert the on load constructor to setup trace marker.
U_TRACE_TARGET_SETUP(U_TRACE_WHICH_SERVICE)

/*
 * For a non-service runtime, xrt_instance_create is defined in target_instance
 * helper lib, so we just have a placeholder symbol below to silence warnings about
 * empty translation units.
 */
#include <xrt/xrt_compiler.h>
XRT_MAYBE_UNUSED static const int PLACEHOLDER = 42;

#endif
