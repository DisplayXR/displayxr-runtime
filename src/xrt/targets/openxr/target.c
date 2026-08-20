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
#include "xrt/xrt_config_os.h"
#include "util/u_sandbox.h"
#include "util/u_logging.h"
#include "client/ipc_client_interface.h"

#ifdef XRT_OS_ANDROID
#include "android/android_globals.h"
#include "client/ipc_client_connection.h"
#include <stdlib.h>
#endif

// Forward declaration of native instance creation from target_instance_hybrid
xrt_result_t
native_instance_create(struct xrt_instance_info *ii, struct xrt_instance **out_xinst);


xrt_result_t
xrt_instance_create(struct xrt_instance_info *ii, struct xrt_instance **out_xinst)
{
	u_trace_marker_init();

	XRT_TRACE_MARKER();

#ifdef XRT_OS_ANDROID
	/*
	 * Android per-app IPC opt-ins (#1031 flavor merge).
	 *
	 * Android has no launcher process that can export XRT_FORCE_MODE for an
	 * app it starts, so the per-app switch has to be something the app itself
	 * carries or the OS can be told out of band. Three, checked before the
	 * generic rules below because they are the more specific signal:
	 *
	 *  (a) the app's own manifest declares
	 *      <meta-data android:name="com.displayxr.force_ipc" android:value="true"/>
	 *      — a build-time, per-app property, and the one an app that WANTS a
	 *      satellite (it also pins com.displayxr.satellite_slot) should use;
	 *  (b) the session enables XR_DXR_weave, i.e. it is a present-owner. Weave
	 *      on Android lives in the service compositor (comp_multi_weave_android.c,
	 *      #1036) and has no in-process implementation, so a weave client is
	 *      an IPC client by capability, no configuration required;
	 *  (c) somebody handed this process an already-connected service socket
	 *      (ipc_client_connection_adopt_fd / DXR_IPC_FD, #1056) — adopting one
	 *      is only meaningful on the IPC path.
	 *
	 * The env/sysprop overrides (XRT_FORCE_MODE, debug.dxr.force_ipc) are
	 * handled generically in u_sandbox_should_use_ipc() below and can still
	 * force a process either way, including back to native.
	 */
	const char *android_force_mode = getenv("XRT_FORCE_MODE");
	const bool android_env_forced = android_force_mode != NULL && android_force_mode[0] != '\0';
	if (!android_env_forced) {
		if (ii != NULL && ii->app_info.ext_weave_enabled) {
			U_LOG_I("Hybrid mode: XR_DXR_weave present-owner — forcing IPC (Android)");
			return ipc_instance_create(ii, out_xinst);
		}
		if (ipc_client_connection_has_adopted_fd()) {
			U_LOG_I("Hybrid mode: an adopted service socket is waiting — forcing IPC (Android, #1056)");
			return ipc_instance_create(ii, out_xinst);
		}
		if (ii != NULL && android_globals_self_declares_force_ipc(ii->platform_info.vm,
		                                                         ii->platform_info.context)) {
			U_LOG_I("Hybrid mode: app manifest declares com.displayxr.force_ipc — forcing IPC (Android)");
			return ipc_instance_create(ii, out_xinst);
		}
	}
#endif

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
