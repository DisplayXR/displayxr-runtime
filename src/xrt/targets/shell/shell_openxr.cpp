// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shell-side OpenXR scaffolding (Phase 2.I + Phase 2.C) implementation.
 *
 * Phase 2.I shipped the IPC-only session for workspace + launcher dispatch.
 * Phase 2.C adds a D3D11 graphics binding so the shell can mint chrome
 * swapchains via xrCreateWorkspaceClientChromeSwapchainEXT — the runtime
 * detects XR_EXT_spatial_workspace and flags the session as a controller, so
 * the per-client compositor is created without slot registration (no phantom
 * tile in the workspace).
 */

#include "shell_openxr.h"

#define WIN32_LEAN_AND_MEAN
#include <Unknwn.h>
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

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
	    XR_EXT_DISPLAY_INFO_EXTENSION_NAME,
	    XR_KHR_D3D11_ENABLE_EXTENSION_NAME, // Phase 2.C: chrome rendering
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

	// Phase 2.G: query physical display dimensions via XR_EXT_display_info.
	// Layout-preset math needs the real dims; LP-3D for instance is
	// 0.344 × 0.194 m, very different from the 0.700 × 0.394 fallback the
	// runtime uses when no display is detected.
	XrDisplayInfoEXT dinfo = {XR_TYPE_DISPLAY_INFO_EXT};
	XrSystemProperties sysprops = {XR_TYPE_SYSTEM_PROPERTIES};
	sysprops.next = &dinfo;
	XrResult dr = xrGetSystemProperties(s->instance, s->system_id, &sysprops);
	if (XR_SUCCEEDED(dr) && dinfo.displaySizeMeters.width > 0.0f &&
	    dinfo.displaySizeMeters.height > 0.0f) {
		s->display_width_m = dinfo.displaySizeMeters.width;
		s->display_height_m = dinfo.displaySizeMeters.height;
		P("shell_openxr: display dims = %.3f × %.3f m (XR_EXT_display_info)\n",
		  s->display_width_m, s->display_height_m);
	} else {
		s->display_width_m = 0.700f;
		s->display_height_m = 0.394f;
		PE("shell_openxr: XR_EXT_display_info unavailable; using LP-3D fallback (%.3f × %.3f m)\n",
		   s->display_width_m, s->display_height_m);
	}

	// Phase 2.C: query the adapter the runtime expects for D3D11 work, then
	// create an ID3D11Device on it. Chrome SHARED_NTHANDLE textures the
	// runtime mints come back here; OpenSharedResource1 only succeeds against
	// a device on the same adapter (LUID match).
	PFN_xrGetD3D11GraphicsRequirementsKHR get_d3d11_reqs = nullptr;
	r = xrGetInstanceProcAddr(s->instance, "xrGetD3D11GraphicsRequirementsKHR",
	                          reinterpret_cast<PFN_xrVoidFunction *>(&get_d3d11_reqs));
	if (XR_FAILED(r) || get_d3d11_reqs == nullptr) {
		PE("shell_openxr: xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR) failed: %s\n",
		   xr_result_str(r));
		shell_openxr_shutdown(s);
		return nullptr;
	}
	XrGraphicsRequirementsD3D11KHR d3d11_reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
	r = get_d3d11_reqs(s->instance, s->system_id, &d3d11_reqs);
	if (XR_FAILED(r)) {
		PE("shell_openxr: xrGetD3D11GraphicsRequirementsKHR failed: %s\n", xr_result_str(r));
		shell_openxr_shutdown(s);
		return nullptr;
	}

	using Microsoft::WRL::ComPtr;
	ComPtr<IDXGIFactory4> dxgi_factory;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(dxgi_factory.GetAddressOf()));
	if (FAILED(hr)) {
		PE("shell_openxr: CreateDXGIFactory1 failed: 0x%08lx\n", hr);
		shell_openxr_shutdown(s);
		return nullptr;
	}
	ComPtr<IDXGIAdapter> adapter;
	hr = dxgi_factory->EnumAdapterByLuid(d3d11_reqs.adapterLuid, IID_PPV_ARGS(adapter.GetAddressOf()));
	if (FAILED(hr)) {
		PE("shell_openxr: EnumAdapterByLuid(%08lx-%08lx) failed: 0x%08lx\n",
		   d3d11_reqs.adapterLuid.HighPart, d3d11_reqs.adapterLuid.LowPart, hr);
		shell_openxr_shutdown(s);
		return nullptr;
	}
	D3D_FEATURE_LEVEL feature_levels[] = {
	    D3D_FEATURE_LEVEL_11_1,
	    D3D_FEATURE_LEVEL_11_0,
	};
	D3D_FEATURE_LEVEL chosen_level = D3D_FEATURE_LEVEL_11_0;
	ComPtr<ID3D11Device> d3d11_device;
	ComPtr<ID3D11DeviceContext> d3d11_context;
	hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
	                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels,
	                       (UINT)(sizeof(feature_levels) / sizeof(feature_levels[0])),
	                       D3D11_SDK_VERSION, d3d11_device.GetAddressOf(), &chosen_level,
	                       d3d11_context.GetAddressOf());
	if (FAILED(hr)) {
		PE("shell_openxr: D3D11CreateDevice failed: 0x%08lx\n", hr);
		shell_openxr_shutdown(s);
		return nullptr;
	}
	// Hand ownership to the state struct (raw pointers — header-visible to C).
	s->d3d11_device = d3d11_device.Detach();
	s->d3d11_context = d3d11_context.Detach();

	// Workspace-controller session with D3D11 binding. The runtime sees
	// XR_EXT_spatial_workspace enabled and sets is_workspace_controller on the
	// xrt_session_info, so the d3d11_service compositor allocates the per-
	// client compositor (needed for swapchain create RPCs) but skips slot
	// registration — no phantom tile inside the workspace this shell controls.
	XrGraphicsBindingD3D11KHR d3d11_binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	d3d11_binding.device = static_cast<ID3D11Device *>(s->d3d11_device);

	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &d3d11_binding;
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
	    {"xrSetWorkspaceClientWindowPoseEXT",       reinterpret_cast<PFN_xrVoidFunction *>(&s->set_pose)},
	    {"xrGetWorkspaceClientWindowPoseEXT",       reinterpret_cast<PFN_xrVoidFunction *>(&s->get_pose)},
	    {"xrCaptureWorkspaceFrameEXT",              reinterpret_cast<PFN_xrVoidFunction *>(&s->capture_frame)},
	    {"xrClearLauncherAppsEXT",                  reinterpret_cast<PFN_xrVoidFunction *>(&s->clear_launcher)},
	    {"xrAddLauncherAppEXT",                     reinterpret_cast<PFN_xrVoidFunction *>(&s->add_launcher_app)},
	    {"xrSetLauncherVisibleEXT",                 reinterpret_cast<PFN_xrVoidFunction *>(&s->set_launcher_visible)},
	    {"xrPollLauncherClickEXT",                  reinterpret_cast<PFN_xrVoidFunction *>(&s->poll_launcher_click)},
	    {"xrSetLauncherRunningTileMaskEXT",         reinterpret_cast<PFN_xrVoidFunction *>(&s->set_running_tile_mask)},
	    {"xrSetWorkspaceFocusedClientEXT",          reinterpret_cast<PFN_xrVoidFunction *>(&s->set_focused)},
	    {"xrEnumerateWorkspaceClientsEXT",          reinterpret_cast<PFN_xrVoidFunction *>(&s->enumerate_clients)},
	    {"xrGetWorkspaceClientInfoEXT",             reinterpret_cast<PFN_xrVoidFunction *>(&s->get_client_info)},
	    {"xrEnumerateWorkspaceInputEventsEXT",      reinterpret_cast<PFN_xrVoidFunction *>(&s->enumerate_input_events)},
	    // Phase 2.K additions
	    {"xrEnableWorkspacePointerCaptureEXT",      reinterpret_cast<PFN_xrVoidFunction *>(&s->enable_pointer_capture)},
	    {"xrDisableWorkspacePointerCaptureEXT",     reinterpret_cast<PFN_xrVoidFunction *>(&s->disable_pointer_capture)},
	    {"xrRequestWorkspaceClientExitEXT",         reinterpret_cast<PFN_xrVoidFunction *>(&s->request_client_exit)},
	    {"xrRequestWorkspaceClientFullscreenEXT",   reinterpret_cast<PFN_xrVoidFunction *>(&s->request_client_fullscreen)},
	    // Phase 2.C additions
	    {"xrCreateWorkspaceClientChromeSwapchainEXT",  reinterpret_cast<PFN_xrVoidFunction *>(&s->create_chrome_swapchain)},
	    {"xrDestroyWorkspaceClientChromeSwapchainEXT", reinterpret_cast<PFN_xrVoidFunction *>(&s->destroy_chrome_swapchain)},
	    {"xrSetWorkspaceClientChromeLayoutEXT",        reinterpret_cast<PFN_xrVoidFunction *>(&s->set_chrome_layout)},
	    {"xrAcquireWorkspaceWakeupEventEXT",           reinterpret_cast<PFN_xrVoidFunction *>(&s->acquire_wakeup_event)},
	};
	for (const auto &e : entries) {
		if (!resolve_pfn(s->instance, e.name, e.out)) {
			shell_openxr_shutdown(s);
			return nullptr;
		}
	}

	// spec_version 8: acquire the wakeup event handle once. Stored on the
	// state struct for main.c to add to its MsgWaitForMultipleObjects
	// wait set. If the runtime is older / unsupported we just leave
	// wakeup_event_handle = NULL and main.c falls back to its old polling
	// cadence — no functional regression, just no idle-CPU optimization.
	uint64_t handle_value = 0;
	XrResult wr = s->acquire_wakeup_event != nullptr
	                  ? s->acquire_wakeup_event(s->session, &handle_value)
	                  : XR_ERROR_FUNCTION_UNSUPPORTED;
	if (wr == XR_SUCCESS && handle_value != 0) {
		s->wakeup_event_handle = (void *)(uintptr_t)handle_value;
		P("shell_openxr: workspace wakeup event acquired (handle=%p)\n", s->wakeup_event_handle);
	} else {
		s->wakeup_event_handle = nullptr;
		P("shell_openxr: workspace wakeup event unsupported (XrResult=%d) — falling back to polling\n", (int)wr);
	}

	P("shell_openxr: instance=%p session=%p (25 PFNs resolved, no graphics binding)\n",
	  (void *)s->instance, (void *)s->session);
	return s;
}

extern "C" void shell_openxr_shutdown(struct shell_openxr_state *s)
{
	if (s == nullptr) {
		return;
	}
	// spec_version 8: close our DuplicateHandle of the wakeup event. The
	// runtime keeps its source HANDLE (closed on service teardown).
	if (s->wakeup_event_handle != nullptr) {
		CloseHandle((HANDLE)s->wakeup_event_handle);
		s->wakeup_event_handle = nullptr;
	}
	if (s->session != XR_NULL_HANDLE) {
		xrDestroySession(s->session);
		s->session = XR_NULL_HANDLE;
	}
	if (s->instance != XR_NULL_HANDLE) {
		xrDestroyInstance(s->instance);
		s->instance = XR_NULL_HANDLE;
	}
	if (s->d3d11_context != nullptr) {
		static_cast<ID3D11DeviceContext *>(s->d3d11_context)->Release();
		s->d3d11_context = nullptr;
	}
	if (s->d3d11_device != nullptr) {
		static_cast<ID3D11Device *>(s->d3d11_device)->Release();
		s->d3d11_device = nullptr;
	}
	delete s;
}
