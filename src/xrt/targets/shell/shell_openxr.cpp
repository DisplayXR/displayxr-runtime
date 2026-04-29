// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shell-side OpenXR scaffolding (Phase 2.I) implementation.
 *
 * The shell does not render swapchains; it dispatches workspace + launcher
 * extension functions only. xrCreateSession is called with no graphics
 * binding — the runtime's workspace-controller path (in oxr_session.c)
 * allocates an IPC client compositor for transport but skips the
 * graphics-API wrapper, so no redundant client window is created.
 */

#include "shell_openxr.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <openxr/openxr.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

#define P(...)  std::printf(__VA_ARGS__)
#define PE(...) std::fprintf(stderr, __VA_ARGS__)

namespace {

const char *xr_result_str(XrResult r)
{
	switch (r) {
	case XR_SUCCESS: return "XR_SUCCESS";
	case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
	case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
	case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
	case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
	case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
	default: return "(other)";
	}
}

bool resolve_pfn(XrInstance instance, const char *name, PFN_xrVoidFunction *out)
{
	XrResult r = xrGetInstanceProcAddr(instance, name, out);
	if (XR_FAILED(r) || *out == nullptr) {
		PE("shell_openxr: xrGetInstanceProcAddr(%s) failed: %s\n", name, xr_result_str(r));
		return false;
	}
	return true;
}

} // namespace

extern "C" struct shell_openxr_state *shell_openxr_init(void)
{
	auto *s = new (std::nothrow) shell_openxr_state{};
	if (s == nullptr) {
		return nullptr;
	}

	// The hybrid runtime auto-selects between in-process native compositor
	// and IPC mode based on env vars (see u_sandbox.c). The workspace
	// extension requires IPC mode — the controller talks to the service
	// over IPC. XRT_FORCE_MODE is read via both getenv() and
	// GetEnvironmentVariableA(), so SetEnvironmentVariableA reliably
	// reaches the runtime DLL even with separate static CRTs.
	SetEnvironmentVariableA("XRT_FORCE_MODE", "ipc");

	const char *exts[] = {
	    XR_EXT_SPATIAL_WORKSPACE_EXTENSION_NAME,
	    XR_EXT_APP_LAUNCHER_EXTENSION_NAME,
	};
	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	std::snprintf(ci.applicationInfo.applicationName, sizeof(ci.applicationInfo.applicationName),
	              "displayxr-shell");
	ci.applicationInfo.applicationVersion = 1;
	std::snprintf(ci.applicationInfo.engineName, sizeof(ci.applicationInfo.engineName), "displayxr-shell");
	ci.applicationInfo.engineVersion = 1;
	ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ci.enabledExtensionCount = (uint32_t)(sizeof(exts) / sizeof(exts[0]));
	ci.enabledExtensionNames = exts;

	XrResult r = xrCreateInstance(&ci, &s->instance);
	if (XR_FAILED(r)) {
		PE("shell_openxr: xrCreateInstance failed: %s\n", xr_result_str(r));
		shell_openxr_shutdown(s);
		return nullptr;
	}

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	r = xrGetSystem(s->instance, &sgi, &s->system_id);
	if (XR_FAILED(r)) {
		PE("shell_openxr: xrGetSystem failed: %s\n", xr_result_str(r));
		shell_openxr_shutdown(s);
		return nullptr;
	}

	// Workspace-controller session: no graphics binding chained on next.
	// The runtime detects this case (XR_EXT_spatial_workspace enabled +
	// no binding) and creates an IPC-only session.
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = nullptr;
	sci.systemId = s->system_id;
	r = xrCreateSession(s->instance, &sci, &s->session);
	if (XR_FAILED(r)) {
		PE("shell_openxr: xrCreateSession failed: %s\n", xr_result_str(r));
		shell_openxr_shutdown(s);
		return nullptr;
	}

	struct PfnEntry {
		const char *name;
		PFN_xrVoidFunction *out;
	};
	PfnEntry entries[] = {
	    {"xrActivateSpatialWorkspaceEXT",      reinterpret_cast<PFN_xrVoidFunction *>(&s->activate)},
	    {"xrDeactivateSpatialWorkspaceEXT",    reinterpret_cast<PFN_xrVoidFunction *>(&s->deactivate)},
	    {"xrGetSpatialWorkspaceStateEXT",      reinterpret_cast<PFN_xrVoidFunction *>(&s->get_state)},
	    {"xrAddWorkspaceCaptureClientEXT",     reinterpret_cast<PFN_xrVoidFunction *>(&s->add_capture)},
	    {"xrRemoveWorkspaceCaptureClientEXT",  reinterpret_cast<PFN_xrVoidFunction *>(&s->remove_capture)},
	    {"xrSetWorkspaceClientWindowPoseEXT",  reinterpret_cast<PFN_xrVoidFunction *>(&s->set_pose)},
	    {"xrCaptureWorkspaceFrameEXT",         reinterpret_cast<PFN_xrVoidFunction *>(&s->capture_frame)},
	    {"xrClearLauncherAppsEXT",             reinterpret_cast<PFN_xrVoidFunction *>(&s->clear_launcher)},
	    {"xrAddLauncherAppEXT",                reinterpret_cast<PFN_xrVoidFunction *>(&s->add_launcher_app)},
	    {"xrSetLauncherVisibleEXT",            reinterpret_cast<PFN_xrVoidFunction *>(&s->set_launcher_visible)},
	    {"xrPollLauncherClickEXT",             reinterpret_cast<PFN_xrVoidFunction *>(&s->poll_launcher_click)},
	    {"xrSetLauncherRunningTileMaskEXT",    reinterpret_cast<PFN_xrVoidFunction *>(&s->set_running_tile_mask)},
	    {"xrSetWorkspaceFocusedClientEXT",     reinterpret_cast<PFN_xrVoidFunction *>(&s->set_focused)},
	    {"xrEnumerateWorkspaceClientsEXT",     reinterpret_cast<PFN_xrVoidFunction *>(&s->enumerate_clients)},
	    {"xrGetWorkspaceClientInfoEXT",        reinterpret_cast<PFN_xrVoidFunction *>(&s->get_client_info)},
	};
	for (const auto &e : entries) {
		if (!resolve_pfn(s->instance, e.name, e.out)) {
			shell_openxr_shutdown(s);
			return nullptr;
		}
	}

	P("shell_openxr: instance=%p session=%p (15 PFNs resolved, no graphics binding)\n",
	  (void *)s->instance, (void *)s->session);
	return s;
}

extern "C" void shell_openxr_shutdown(struct shell_openxr_state *s)
{
	if (s == nullptr) {
		return;
	}
	if (s->session != XR_NULL_HANDLE) {
		xrDestroySession(s->session);
		s->session = XR_NULL_HANDLE;
	}
	if (s->instance != XR_NULL_HANDLE) {
		xrDestroyInstance(s->instance);
		s->instance = XR_NULL_HANDLE;
	}
	delete s;
}
