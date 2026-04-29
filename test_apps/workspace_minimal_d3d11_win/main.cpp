// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// workspace_minimal_d3d11_win
//
// Minimal validation client for XR_EXT_spatial_workspace (Phase 2.A + 2.C
// + 2.D + 2.F, v4) and XR_EXT_app_launcher (Phase 2.B). Creates an instance +
// session, resolves all 19 extension PFNs, and walks through:
//   activate -> get-state ->
//     add capture client ->
//     set/get/set client window pose + visibility ->
//     hit-test (center + off-screen) ->
//     set/get focused client (Phase 2.D) ->
//     enumerate input events (count-query) (Phase 2.D) ->
//     enable/disable pointer capture (Phase 2.D) ->
//     remove capture client ->
//     clear / add / set-visible / set-running-mask / poll / hide launcher ->
//   deactivate.
//
// Two valid run paths:
//   1. Launched under an authorized workspace orchestrator -> activate
//      returns XR_SUCCESS and the full sequence runs.
//   2. Launched standalone -> activate returns XR_ERROR_FEATURE_UNSUPPORTED
//      (Phase 2.0 PID-auth denies the call). The test reports the deny and
//      exits cleanly.
//
// Any other outcome is a test failure.
//
// XR_USE_GRAPHICS_API_D3D11 and XR_USE_PLATFORM_WIN32 are set by CMake's
// target_compile_definitions; do not redefine here.

#include <Unknwn.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_spatial_workspace.h>
#include <openxr/XR_EXT_app_launcher.h>

#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

const char *
xr_result_str(XrResult r)
{
	switch (r) {
	case XR_SUCCESS: return "XR_SUCCESS";
	case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
	case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
	case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
	case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
	case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_HANDLE_INVALID: return "XR_ERROR_HANDLE_INVALID";
	default: return "<other>";
	}
}

#define CHECK_XR(stmt, label)                                                                                          \
	do {                                                                                                           \
		XrResult _r = (stmt);                                                                                  \
		std::printf("[%-44s] %s (%d)\n", label, xr_result_str(_r), _r);                                        \
		if (XR_FAILED(_r)) {                                                                                   \
			return _r;                                                                                     \
		}                                                                                                      \
	} while (0)

XrResult
run_workspace_test()
{
	// 1. Enumerate available instance extensions and confirm our extension is listed.
	uint32_t propCount = 0;
	XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &propCount, nullptr);
	if (XR_FAILED(r)) {
		std::printf("[xrEnumerateInstanceExtensionProperties (count)] %s\n", xr_result_str(r));
		return r;
	}
	std::vector<XrExtensionProperties> props(propCount, {XR_TYPE_EXTENSION_PROPERTIES});
	r = xrEnumerateInstanceExtensionProperties(nullptr, propCount, &propCount, props.data());
	if (XR_FAILED(r)) {
		std::printf("[xrEnumerateInstanceExtensionProperties] %s\n", xr_result_str(r));
		return r;
	}

	bool workspace_listed = false;
	bool launcher_listed = false;
	for (const auto &p : props) {
		if (std::strcmp(p.extensionName, XR_EXT_SPATIAL_WORKSPACE_EXTENSION_NAME) == 0) {
			workspace_listed = true;
			std::printf("[enumerate                                ] found %s v%u\n", p.extensionName,
			            p.extensionVersion);
		} else if (std::strcmp(p.extensionName, XR_EXT_APP_LAUNCHER_EXTENSION_NAME) == 0) {
			launcher_listed = true;
			std::printf("[enumerate                                ] found %s v%u\n", p.extensionName,
			            p.extensionVersion);
		}
	}
	if (!workspace_listed) {
		std::printf("FAIL: runtime did not advertise %s\n", XR_EXT_SPATIAL_WORKSPACE_EXTENSION_NAME);
		return XR_ERROR_RUNTIME_FAILURE;
	}
	if (!launcher_listed) {
		std::printf("FAIL: runtime did not advertise %s\n", XR_EXT_APP_LAUNCHER_EXTENSION_NAME);
		return XR_ERROR_RUNTIME_FAILURE;
	}

	// 2. Create instance with workspace + launcher extensions and D3D11 binding.
	const char *enabled_exts[] = {
	    XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
	    XR_EXT_SPATIAL_WORKSPACE_EXTENSION_NAME,
	    XR_EXT_APP_LAUNCHER_EXTENSION_NAME,
	};
	XrInstanceCreateInfo instInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	instInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	std::strncpy(instInfo.applicationInfo.applicationName, "workspace_minimal_d3d11_win",
	             sizeof(instInfo.applicationInfo.applicationName) - 1);
	instInfo.applicationInfo.applicationVersion = 1;
	instInfo.enabledExtensionCount = (uint32_t)(sizeof(enabled_exts) / sizeof(enabled_exts[0]));
	instInfo.enabledExtensionNames = enabled_exts;

	XrInstance instance = XR_NULL_HANDLE;
	CHECK_XR(xrCreateInstance(&instInfo, &instance), "xrCreateInstance");

	// 3. Resolve all 19 extension PFNs (must be non-null when the extension is enabled).
	PFN_xrActivateSpatialWorkspaceEXT pfnActivate = nullptr;
	PFN_xrDeactivateSpatialWorkspaceEXT pfnDeactivate = nullptr;
	PFN_xrGetSpatialWorkspaceStateEXT pfnGetState = nullptr;
	PFN_xrAddWorkspaceCaptureClientEXT pfnAddCapture = nullptr;
	PFN_xrRemoveWorkspaceCaptureClientEXT pfnRemoveCapture = nullptr;
	PFN_xrClearLauncherAppsEXT pfnClearLauncher = nullptr;
	PFN_xrAddLauncherAppEXT pfnAddLauncherApp = nullptr;
	PFN_xrSetLauncherVisibleEXT pfnSetLauncherVisible = nullptr;
	PFN_xrPollLauncherClickEXT pfnPollLauncherClick = nullptr;
	PFN_xrSetLauncherRunningTileMaskEXT pfnSetRunningTileMask = nullptr;
	PFN_xrSetWorkspaceClientWindowPoseEXT pfnSetClientPose = nullptr;
	PFN_xrGetWorkspaceClientWindowPoseEXT pfnGetClientPose = nullptr;
	PFN_xrSetWorkspaceClientVisibilityEXT pfnSetClientVisibility = nullptr;
	PFN_xrWorkspaceHitTestEXT pfnHitTest = nullptr;
	PFN_xrSetWorkspaceFocusedClientEXT pfnSetFocused = nullptr;
	PFN_xrGetWorkspaceFocusedClientEXT pfnGetFocused = nullptr;
	PFN_xrEnumerateWorkspaceInputEventsEXT pfnEnumInputEvents = nullptr;
	PFN_xrEnableWorkspacePointerCaptureEXT pfnEnableCapture = nullptr;
	PFN_xrDisableWorkspacePointerCaptureEXT pfnDisableCapture = nullptr;

	struct PfnLookup {
		const char *name;
		PFN_xrVoidFunction *out;
	};
	PfnLookup lookups[] = {
	    {"xrActivateSpatialWorkspaceEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnActivate)},
	    {"xrDeactivateSpatialWorkspaceEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnDeactivate)},
	    {"xrGetSpatialWorkspaceStateEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetState)},
	    {"xrAddWorkspaceCaptureClientEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnAddCapture)},
	    {"xrRemoveWorkspaceCaptureClientEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnRemoveCapture)},
	    {"xrClearLauncherAppsEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnClearLauncher)},
	    {"xrAddLauncherAppEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnAddLauncherApp)},
	    {"xrSetLauncherVisibleEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnSetLauncherVisible)},
	    {"xrPollLauncherClickEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnPollLauncherClick)},
	    {"xrSetLauncherRunningTileMaskEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnSetRunningTileMask)},
	    {"xrSetWorkspaceClientWindowPoseEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnSetClientPose)},
	    {"xrGetWorkspaceClientWindowPoseEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetClientPose)},
	    {"xrSetWorkspaceClientVisibilityEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnSetClientVisibility)},
	    {"xrWorkspaceHitTestEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnHitTest)},
	    {"xrSetWorkspaceFocusedClientEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnSetFocused)},
	    {"xrGetWorkspaceFocusedClientEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetFocused)},
	    {"xrEnumerateWorkspaceInputEventsEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnEnumInputEvents)},
	    {"xrEnableWorkspacePointerCaptureEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnEnableCapture)},
	    {"xrDisableWorkspacePointerCaptureEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnDisableCapture)},
	};
	for (const auto &l : lookups) {
		PFN_xrVoidFunction fn = nullptr;
		XrResult lr = xrGetInstanceProcAddr(instance, l.name, &fn);
		std::printf("[xrGetInstanceProcAddr(%-32s)] %s\n", l.name, xr_result_str(lr));
		if (XR_FAILED(lr) || fn == nullptr) {
			xrDestroyInstance(instance);
			return XR_ERROR_FUNCTION_UNSUPPORTED;
		}
		*l.out = fn;
	}

	// 4. Get system + create a D3D11 device + session.
	XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
	sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	CHECK_XR(xrGetSystem(instance, &sysInfo, &systemId), "xrGetSystem");

	PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetGfxReq = nullptr;
	CHECK_XR(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
	                               reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetGfxReq)),
	         "xrGetInstanceProcAddr(D3D11GraphicsReq)");

	XrGraphicsRequirementsD3D11KHR gfxReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
	CHECK_XR(pfnGetGfxReq(instance, systemId, &gfxReq), "xrGetD3D11GraphicsRequirementsKHR");

	ComPtr<IDXGIFactory1> dxgiFactory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.GetAddressOf())))) {
		std::printf("FAIL: CreateDXGIFactory1\n");
		xrDestroyInstance(instance);
		return XR_ERROR_RUNTIME_FAILURE;
	}
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; dxgiFactory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) == S_OK; ++i) {
		DXGI_ADAPTER_DESC1 desc = {};
		adapter->GetDesc1(&desc);
		if (std::memcmp(&desc.AdapterLuid, &gfxReq.adapterLuid, sizeof(LUID)) == 0) {
			break;
		}
		adapter.Reset();
	}
	if (!adapter) {
		// Fall back to the default adapter; the runtime may accept it.
		dxgiFactory->EnumAdapters1(0, adapter.ReleaseAndGetAddressOf());
	}

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
	HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, featureLevels,
	                               1, D3D11_SDK_VERSION, device.GetAddressOf(), &chosen,
	                               context.GetAddressOf());
	if (FAILED(hr)) {
		std::printf("FAIL: D3D11CreateDevice hr=0x%08lx\n", (unsigned long)hr);
		xrDestroyInstance(instance);
		return XR_ERROR_RUNTIME_FAILURE;
	}

	XrGraphicsBindingD3D11KHR d3dBinding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	d3dBinding.device = device.Get();

	XrSessionCreateInfo sessInfo = {XR_TYPE_SESSION_CREATE_INFO};
	sessInfo.next = &d3dBinding;
	sessInfo.systemId = systemId;
	XrSession session = XR_NULL_HANDLE;
	CHECK_XR(xrCreateSession(instance, &sessInfo, &session), "xrCreateSession");

	// 5. Walk the workspace lifecycle.
	XrResult activate_r = pfnActivate(session);
	std::printf("[xrActivateSpatialWorkspaceEXT             ] %s\n", xr_result_str(activate_r));

	if (activate_r == XR_ERROR_FEATURE_UNSUPPORTED) {
		std::printf("INFO: standalone launch — workspace orchestrator did not authorize this PID.\n");
		std::printf("      Re-launch under displayxr-shell.exe (or another orchestrator) to exercise\n");
		std::printf("      the success path. This deny is a valid Phase 2.A test outcome.\n");
		xrDestroySession(session);
		xrDestroyInstance(instance);
		return XR_SUCCESS;
	}
	if (XR_FAILED(activate_r)) {
		xrDestroySession(session);
		xrDestroyInstance(instance);
		return activate_r;
	}

	// Authorized path — exercise the full surface.
	XrBool32 active = XR_FALSE;
	CHECK_XR(pfnGetState(session, &active), "xrGetSpatialWorkspaceStateEXT");
	std::printf("INFO: workspace active = %s\n", active ? "XR_TRUE" : "XR_FALSE");

	HWND target = FindWindowA(nullptr, "Calculator");
	if (!target) {
		target = FindWindowA(nullptr, "Notepad");
	}
	if (!target) {
		target = GetDesktopWindow(); // last-resort: a guaranteed valid HWND for the API smoke test
		std::printf("INFO: no Calculator/Notepad window found; using desktop HWND for capture API smoke "
		            "test.\n");
	}

	XrWorkspaceClientId clientId = XR_NULL_WORKSPACE_CLIENT_ID;
	CHECK_XR(pfnAddCapture(session, (uint64_t)(uintptr_t)target, "workspace_minimal smoke test", &clientId),
	         "xrAddWorkspaceCaptureClientEXT");
	if (clientId == XR_NULL_WORKSPACE_CLIENT_ID) {
		std::printf("FAIL: clientId is XR_NULL_WORKSPACE_CLIENT_ID after add\n");
		xrDestroySession(session);
		xrDestroyInstance(instance);
		return XR_ERROR_RUNTIME_FAILURE;
	}
	std::printf("INFO: added capture client id=%u\n", (unsigned)clientId);

	// Window pose + visibility smoke (Phase 2.C). Whether the captured
	// HWND is treated as a positionable workspace client is a runtime
	// decision; we expect either XR_SUCCESS or a documented error code
	// (typically XR_ERROR_VALIDATION_FAILURE if the slot isn't mapped to
	// a positionable client). Either response proves the dispatch path
	// reaches the IPC layer end-to-end.
	{
		XrPosef testPose = {};
		testPose.orientation.w = 1.0f; // identity
		testPose.position.z = 0.5f;     // 0.5m in front of display center
		XrResult sr = pfnSetClientPose(session, clientId, &testPose, 0.20f, 0.15f);
		std::printf("[xrSetWorkspaceClientWindowPoseEXT          ] %s\n", xr_result_str(sr));

		XrPosef readback = {};
		float w = 0.0f, h = 0.0f;
		XrResult gr = pfnGetClientPose(session, clientId, &readback, &w, &h);
		std::printf("[xrGetWorkspaceClientWindowPoseEXT          ] %s\n", xr_result_str(gr));
		if (gr == XR_SUCCESS) {
			std::printf("INFO: readback pos=(%.3f,%.3f,%.3f) size=%.3fx%.3f\n", readback.position.x,
			            readback.position.y, readback.position.z, w, h);
		}

		XrResult vr = pfnSetClientVisibility(session, clientId, XR_FALSE);
		std::printf("[xrSetWorkspaceClientVisibilityEXT(FALSE)   ] %s\n", xr_result_str(vr));
		vr = pfnSetClientVisibility(session, clientId, XR_TRUE);
		std::printf("[xrSetWorkspaceClientVisibilityEXT(TRUE)    ] %s\n", xr_result_str(vr));

		// Hit-test smoke (Phase 2.F + 2.D): probe two cursor positions — one
		// near display center (likely to hit a window if any are positioned
		// there) and one far off-screen (guaranteed miss). Region output
		// added in Phase 2.D. Off-screen probe MUST report BACKGROUND_EXT.
		XrWorkspaceClientId hitId = XR_NULL_WORKSPACE_CLIENT_ID;
		XrVector2f hitUV = {};
		XrWorkspaceHitRegionEXT hitRegion = XR_WORKSPACE_HIT_REGION_BACKGROUND_EXT;
		XrResult hr = pfnHitTest(session, 1920, 1080, &hitId, &hitUV, &hitRegion);
		std::printf("[xrWorkspaceHitTestEXT(1920,1080)           ] %s clientId=%u uv=(%.3f,%.3f) region=%d\n",
		            xr_result_str(hr), (unsigned)hitId, hitUV.x, hitUV.y, (int)hitRegion);

		hitId = 0xFFFFFFFFu;
		hitUV = {0.5f, 0.5f}; // sentinel; should be overwritten to (0,0) on miss
		hitRegion = (XrWorkspaceHitRegionEXT)0x7F; // sentinel
		hr = pfnHitTest(session, -1, -1, &hitId, &hitUV, &hitRegion);
		std::printf("[xrWorkspaceHitTestEXT(-1,-1)               ] %s clientId=%u uv=(%.3f,%.3f) region=%d\n",
		            xr_result_str(hr), (unsigned)hitId, hitUV.x, hitUV.y, (int)hitRegion);
		if (hr == XR_SUCCESS && hitId != XR_NULL_WORKSPACE_CLIENT_ID) {
			std::printf("FAIL: off-screen hit-test reported a hit (clientId=%u)\n",
			            (unsigned)hitId);
		}
		if (hr == XR_SUCCESS && hitRegion != XR_WORKSPACE_HIT_REGION_BACKGROUND_EXT) {
			std::printf("FAIL: off-screen hit-test region=%d, expected BACKGROUND_EXT(0)\n",
			            (int)hitRegion);
		}

		// Phase 2.D: focus + drain + pointer-capture smoke.
		// Set focus to the captured client, then read it back and assert.
		CHECK_XR(pfnSetFocused(session, clientId), "xrSetWorkspaceFocusedClientEXT");
		XrWorkspaceClientId focused = XR_NULL_WORKSPACE_CLIENT_ID;
		CHECK_XR(pfnGetFocused(session, &focused), "xrGetWorkspaceFocusedClientEXT");
		std::printf("[xrGetWorkspaceFocusedClientEXT(roundtrip)] focused=%u expected=%u\n",
		            (unsigned)focused, (unsigned)clientId);
		if (focused != clientId) {
			std::printf("FAIL: focused-client roundtrip mismatch (got %u, set %u)\n",
			            (unsigned)focused, (unsigned)clientId);
		}

		// Drain count-query (capacity=0): expect zero events because no
		// human has pressed anything during the smoke window.
		uint32_t event_count = 0xFFFFFFFFu;
		XrResult er = pfnEnumInputEvents(session, 0, &event_count, nullptr);
		std::printf("[xrEnumerateWorkspaceInputEventsEXT(0)     ] %s count=%u\n",
		            xr_result_str(er), (unsigned)event_count);

		// Pointer-capture toggle: enable then disable. Both should succeed.
		CHECK_XR(pfnEnableCapture(session, 1), "xrEnableWorkspacePointerCaptureEXT");
		CHECK_XR(pfnDisableCapture(session), "xrDisableWorkspacePointerCaptureEXT");

		// Clear focus before removing the captured client to keep state tidy.
		CHECK_XR(pfnSetFocused(session, XR_NULL_WORKSPACE_CLIENT_ID),
		         "xrSetWorkspaceFocusedClientEXT(clear)");
	}

	CHECK_XR(pfnRemoveCapture(session, clientId), "xrRemoveWorkspaceCaptureClientEXT");

	// Launcher smoke: clear, push 3 synthetic tiles, set visible, set running
	// mask, poll once (expect "no click" since no human is interacting),
	// hide, clear, then proceed to deactivate.
	CHECK_XR(pfnClearLauncher(session), "xrClearLauncherAppsEXT");
	for (int i = 0; i < 3; ++i) {
		XrLauncherAppInfoEXT info = {XR_TYPE_LAUNCHER_APP_INFO_EXT};
		std::snprintf(info.name, sizeof(info.name), "TestApp%d", i);
		info.iconPath[0] = '\0'; // No icon — runtime renders a placeholder.
		info.appIndex = i;
		CHECK_XR(pfnAddLauncherApp(session, &info), "xrAddLauncherAppEXT");
	}
	CHECK_XR(pfnSetLauncherVisible(session, XR_TRUE), "xrSetLauncherVisibleEXT(TRUE)");
	CHECK_XR(pfnSetRunningTileMask(session, 0x3 /* tiles 0+1 running */),
	         "xrSetLauncherRunningTileMaskEXT");

	int32_t clicked_index = 0;
	CHECK_XR(pfnPollLauncherClick(session, &clicked_index), "xrPollLauncherClickEXT");
	if (clicked_index != XR_LAUNCHER_INVALID_APPINDEX_EXT) {
		std::printf("INFO: launcher click pending index=%d (unexpected — no human input expected)\n",
		            (int)clicked_index);
	} else {
		std::printf("INFO: launcher poll = no pending click (expected)\n");
	}

	CHECK_XR(pfnSetLauncherVisible(session, XR_FALSE), "xrSetLauncherVisibleEXT(FALSE)");
	CHECK_XR(pfnClearLauncher(session), "xrClearLauncherAppsEXT");
	CHECK_XR(pfnDeactivate(session), "xrDeactivateSpatialWorkspaceEXT");

	xrDestroySession(session);
	xrDestroyInstance(instance);
	return XR_SUCCESS;
}

} // namespace

int
main()
{
	std::printf("workspace_minimal_d3d11_win — XR_EXT_spatial_workspace smoke test\n");
	XrResult r = run_workspace_test();
	if (XR_FAILED(r)) {
		std::printf("RESULT: FAIL (%s)\n", xr_result_str(r));
		return 1;
	}
	std::printf("RESULT: PASS\n");
	return 0;
}
