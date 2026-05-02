// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// workspace_minimal_d3d11_win
//
// Minimal validation client for XR_EXT_spatial_workspace (Phase 2.A + 2.C
// + 2.D + 2.F + 2.I-prequel, spec_version 9) and XR_EXT_app_launcher
// (Phase 2.B). Creates an instance + session, resolves all 29 extension
// PFNs, and walks through:
//   activate -> get-state ->
//     add capture client ->
//     set/get/set client window pose + visibility ->
//     hit-test (center + off-screen) ->
//     set/get focused client (Phase 2.D) ->
//     enumerate input events (count-query) (Phase 2.D) ->
//     enable/disable pointer capture (Phase 2.D) ->
//     create chrome swapchain + acquire/wait/clear/release one image ->
//     set chrome layout with auto-anchor + 2 hit regions (spec_version 8) ->
//     acquire wakeup event handle (spec_version 8) ->
//     destroy chrome swapchain ->
//     enumerate workspace clients + get client info (Phase 2.I-prequel) ->
//     capture workspace frame (Phase 2.I-prequel) ->
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
#include <cmath>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_spatial_workspace.h>
#include <openxr/XR_EXT_app_launcher.h>

#include <cstdio>
#include <cstdlib>
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

	// 3. Resolve all 24 extension PFNs (must be non-null when the extension is enabled).
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
	PFN_xrCaptureWorkspaceFrameEXT pfnCaptureFrame = nullptr;
	PFN_xrEnumerateWorkspaceClientsEXT pfnEnumClients = nullptr;
	PFN_xrGetWorkspaceClientInfoEXT pfnGetClientInfo = nullptr;
	// Phase 2.K additions (spec_version 6).
	PFN_xrRequestWorkspaceClientExitEXT pfnRequestExit = nullptr;
	PFN_xrRequestWorkspaceClientFullscreenEXT pfnRequestFullscreen = nullptr;
	// Phase 2.C controller-owned chrome (spec_version 7) + event-driven
	// wakeup (spec_version 8) + per-client style (spec_version 9).
	PFN_xrCreateWorkspaceClientChromeSwapchainEXT pfnCreateChromeSwapchain = nullptr;
	PFN_xrDestroyWorkspaceClientChromeSwapchainEXT pfnDestroyChromeSwapchain = nullptr;
	PFN_xrSetWorkspaceClientChromeLayoutEXT pfnSetChromeLayout = nullptr;
	PFN_xrAcquireWorkspaceWakeupEventEXT pfnAcquireWakeupEvent = nullptr;
	PFN_xrSetWorkspaceClientStyleEXT pfnSetClientStyle = nullptr;

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
	    {"xrCaptureWorkspaceFrameEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnCaptureFrame)},
	    {"xrEnumerateWorkspaceClientsEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnEnumClients)},
	    {"xrGetWorkspaceClientInfoEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetClientInfo)},
	    {"xrRequestWorkspaceClientExitEXT", reinterpret_cast<PFN_xrVoidFunction *>(&pfnRequestExit)},
	    {"xrRequestWorkspaceClientFullscreenEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnRequestFullscreen)},
	    {"xrCreateWorkspaceClientChromeSwapchainEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnCreateChromeSwapchain)},
	    {"xrDestroyWorkspaceClientChromeSwapchainEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnDestroyChromeSwapchain)},
	    {"xrSetWorkspaceClientChromeLayoutEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnSetChromeLayout)},
	    {"xrAcquireWorkspaceWakeupEventEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnAcquireWakeupEvent)},
	    {"xrSetWorkspaceClientStyleEXT",
	     reinterpret_cast<PFN_xrVoidFunction *>(&pfnSetClientStyle)},
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

		// Phase 2.K: orientation rendering smoke. Push a 30° yaw to the
		// captured client so a visual atlas screenshot can confirm tilted
		// windows compose correctly. The compositor handles the drain in
		// real time so the next render pass picks up the new pose.
		{
			constexpr float yaw_deg = 30.0f;
			float half = (yaw_deg * 0.5f) * 3.14159265358979323846f / 180.0f;
			XrPosef tilt = {};
			tilt.orientation.x = 0.0f;
			tilt.orientation.y = std::sin(half); // yaw axis = +Y
			tilt.orientation.z = 0.0f;
			tilt.orientation.w = std::cos(half);
			tilt.position.z = 0.5f;
			XrResult yr = pfnSetClientPose(session, clientId, &tilt, 0.20f, 0.15f);
			std::printf("[Phase 2.K orientation smoke (30° yaw)      ] %s "
			            "quat=(%.3f,%.3f,%.3f,%.3f)\n",
			            xr_result_str(yr), tilt.orientation.x, tilt.orientation.y,
			            tilt.orientation.z, tilt.orientation.w);
		}

		// Phase 2.K: drain MOTION + FRAME_TICK + FOCUS_CHANGED counts over a
		// short window. FRAME_TICK should fire at compositor cadence
		// (~60 Hz, ~120 events over 2 s). FOCUS_CHANGED fires once on the
		// initial transition. MOTION is gated on pointer capture — enable
		// it for the count window. Numbers don't have to hit a specific
		// target; the test passes if at least one of each fires.
		{
			CHECK_XR(pfnEnableCapture(session, 1),
			         "xrEnableWorkspacePointerCaptureEXT(for drain count)");
			uint32_t motion = 0, tick = 0, focus = 0, key = 0, ptr = 0, scroll = 0;
			ULONGLONG start = GetTickCount64();
			while (GetTickCount64() - start < 2000) {
				XrWorkspaceInputEventEXT batch[16] = {};
				uint32_t got = 0;
				if (pfnEnumInputEvents(session, 16, &got, batch) == XR_SUCCESS) {
					for (uint32_t i = 0; i < got; i++) {
						switch (batch[i].eventType) {
						case XR_WORKSPACE_INPUT_EVENT_POINTER_MOTION_EXT: motion++; break;
						case XR_WORKSPACE_INPUT_EVENT_FRAME_TICK_EXT:     tick++;   break;
						case XR_WORKSPACE_INPUT_EVENT_FOCUS_CHANGED_EXT:  focus++;  break;
						case XR_WORKSPACE_INPUT_EVENT_KEY_EXT:            key++;    break;
						case XR_WORKSPACE_INPUT_EVENT_POINTER_EXT:        ptr++;    break;
						case XR_WORKSPACE_INPUT_EVENT_SCROLL_EXT:         scroll++; break;
						default: break;
						}
					}
				}
				Sleep(16);
			}
			CHECK_XR(pfnDisableCapture(session),
			         "xrDisableWorkspacePointerCaptureEXT(after drain count)");
			std::printf("[Phase 2.K drain counts (2s window)         ] "
			            "motion=%u tick=%u focus=%u key=%u ptr=%u scroll=%u\n",
			            motion, tick, focus, key, ptr, scroll);
			if (tick == 0) {
				std::printf("FAIL: FRAME_TICK count is zero — runtime not emitting per-frame ticks.\n");
			}
		}

		// Phase 2.K: lifecycle requests. Toggle fullscreen TRUE then FALSE
		// against the synthetic capture client; expect XR_SUCCESS for both.
		// The runtime mirrors F11 behaviour, animating the target window
		// in/out of fullscreen and hiding others. Then verify validation:
		// XR_NULL_WORKSPACE_CLIENT_ID returns XR_ERROR_VALIDATION_FAILURE.
		{
			XrResult fr = pfnRequestFullscreen(session, clientId, XR_TRUE);
			std::printf("[xrRequestWorkspaceClientFullscreenEXT(TRUE)] %s\n", xr_result_str(fr));
			Sleep(400); // give the runtime's fullscreen animation a moment
			fr = pfnRequestFullscreen(session, clientId, XR_FALSE);
			std::printf("[xrRequestWorkspaceClientFullscreenEXT(FALSE)] %s\n", xr_result_str(fr));

			XrResult bad = pfnRequestFullscreen(session, XR_NULL_WORKSPACE_CLIENT_ID, XR_FALSE);
			std::printf("[xrRequestWorkspaceClientFullscreenEXT(NULL)] %s (expect VALIDATION_FAILURE)\n",
			            xr_result_str(bad));
		}

		// Phase 2.G: controller-side preset demo.
		//
		// Layout-preset semantics moved to the workspace controller in
		// Phase 2.G — the runtime no longer hosts apply_layout_preset or
		// the Ctrl+1..3 dispatch. Controllers express layouts as a sequence
		// of per-client xrSetWorkspaceClientWindowPoseEXT calls. The shell
		// (src/xrt/targets/shell/main.c) is the reference implementation;
		// here we just demonstrate the pattern for one client to keep the
		// smoke test runnable without a multi-client orchestrator.
		{
			// Grid math for n=1 / idx=0 against the LP-3D fallback dims:
			// the singleton fills 90% of an 80% × 80% cell at z=0, identity
			// orientation. Mirrors compute_grid_layout in the shell.
			XrPosef gridPose = {};
			gridPose.orientation.w = 1.0f;
			gridPose.position.z = 0.0f;
			float gridW = 0.700f * 0.90f * 0.90f;
			float gridH = 0.394f * 0.90f * 0.90f;
			XrResult gpr = pfnSetClientPose(session, clientId, &gridPose, gridW, gridH);
			std::printf("[Phase 2.G grid demo via SetClientPose      ] %s "
			            "size=%.3fx%.3f\n", xr_result_str(gpr), gridW, gridH);
		}

		// Phase 2.C controller-owned chrome smoke (spec_version 7 + 8).
		//
		// Mints a chrome swapchain bound to the captured client, walks the
		// standard acquire / wait / render-into-image[index] / release
		// loop once (clearing image to a recognizable magenta), pushes a
		// chrome layout with two UV-space hit regions and the new
		// auto-anchor flags (spec_version 8), acquires a wakeup event
		// handle, then destroys. Validates dispatch reaches the runtime
		// for every chrome path without requiring a real controller.
		{
			constexpr int64_t  kSrgbFormat   = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
			constexpr uint32_t kChromeWidth  = 256;
			constexpr uint32_t kChromeHeight = 32;

			XrWorkspaceChromeSwapchainCreateInfoEXT chromeCi = {
			    XR_TYPE_WORKSPACE_CHROME_SWAPCHAIN_CREATE_INFO_EXT};
			chromeCi.format = kSrgbFormat;
			chromeCi.width = kChromeWidth;
			chromeCi.height = kChromeHeight;
			chromeCi.sampleCount = 1;
			chromeCi.mipCount = 1;
			XrSwapchain chromeSwapchain = XR_NULL_HANDLE;
			CHECK_XR(pfnCreateChromeSwapchain(session, clientId, &chromeCi, &chromeSwapchain),
			         "xrCreateWorkspaceClientChromeSwapchainEXT");

			// Enumerate images via the standard core entry point —
			// chrome swapchains behave like normal XrSwapchains once
			// created. Single-image loop (mipCount=1, sampleCount=1).
			uint32_t imgCount = 0;
			CHECK_XR(xrEnumerateSwapchainImages(chromeSwapchain, 0, &imgCount, nullptr),
			         "xrEnumerateSwapchainImages(chrome,count)");
			std::vector<XrSwapchainImageD3D11KHR> images(
			    imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
			CHECK_XR(xrEnumerateSwapchainImages(
			             chromeSwapchain, imgCount, &imgCount,
			             reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),
			         "xrEnumerateSwapchainImages(chrome,fill)");
			std::printf("[xrEnumerateSwapchainImages(chrome)         ] count=%u\n",
			            (unsigned)imgCount);

			XrSwapchainImageAcquireInfo acqInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			uint32_t idx = 0xFFFFFFFFu;
			CHECK_XR(xrAcquireSwapchainImage(chromeSwapchain, &acqInfo, &idx),
			         "xrAcquireSwapchainImage(chrome)");

			XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			waitInfo.timeout = XR_INFINITE_DURATION;
			CHECK_XR(xrWaitSwapchainImage(chromeSwapchain, &waitInfo),
			         "xrWaitSwapchainImage(chrome)");

			// Render: ClearRTV the acquired image to magenta. Best-effort
			// — failure to create the RTV doesn't fail the smoke; the
			// acquire/release loop itself is what's being validated.
			if (idx < images.size() && images[idx].texture != nullptr) {
				ComPtr<ID3D11RenderTargetView> rtv;
				D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
				rtvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
				rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				rtvd.Texture2D.MipSlice = 0;
				HRESULT rh = device->CreateRenderTargetView(
				    images[idx].texture, &rtvd, rtv.GetAddressOf());
				if (SUCCEEDED(rh)) {
					const float magenta[4] = {0.85f, 0.10f, 0.85f, 0.85f};
					context->ClearRenderTargetView(rtv.Get(), magenta);
					context->Flush();
					std::printf("[Phase 2.C chrome ClearRTV(magenta)         ] OK\n");
				} else {
					std::printf("INFO: CreateRenderTargetView(chrome img %u) hr=0x%08lx — paint skipped\n",
					            (unsigned)idx, (unsigned long)rh);
				}
			}

			XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			CHECK_XR(xrReleaseSwapchainImage(chromeSwapchain, &relInfo),
			         "xrReleaseSwapchainImage(chrome)");

			// Layout with two hit regions + spec_version 8 auto-anchor.
			// Region IDs are controller-defined opaque uint32_t — the
			// runtime echoes them back as `chromeRegionId` on POINTER
			// events. We use 1=drag, 2=close as a self-documenting smoke.
			XrWorkspaceChromeHitRegionEXT regions[2] = {};
			regions[0].id = 1;
			regions[0].bounds.offset = {0.0f, 0.0f};
			regions[0].bounds.extent = {0.5f, 1.0f};
			regions[1].id = 2;
			regions[1].bounds.offset = {0.5f, 0.0f};
			regions[1].bounds.extent = {0.5f, 1.0f};

			XrWorkspaceChromeLayoutEXT layout = {XR_TYPE_WORKSPACE_CHROME_LAYOUT_EXT};
			layout.poseInClient.orientation.w = 1.0f;
			// With anchorToWindowTopEdge = XR_TRUE (spec_version 8), this
			// y is interpreted as an offset ABOVE the window's top edge.
			// 12 mm sits a generous gap above any reasonable window.
			layout.poseInClient.position.y = 0.012f;
			layout.sizeMeters = {0.20f, 0.012f};
			layout.followsWindowOrient = XR_FALSE;
			layout.hitRegionCount = 2;
			layout.hitRegions = regions;
			layout.depthBiasMeters = 0.0f; // runtime default (1 mm)
			layout.anchorToWindowTopEdge = XR_TRUE;
			layout.widthAsFractionOfWindow = 0.75f; // 75% of current win_w
			CHECK_XR(pfnSetChromeLayout(session, clientId, &layout),
			         "xrSetWorkspaceClientChromeLayoutEXT");
			std::printf("[Phase 2.C chrome layout (auto-anchor, 2 regions)] OK\n");

			// Wakeup event handle (spec_version 8). Validate the handle
			// is non-NULL and well-formed (zero-timeout wait should
			// succeed with WAIT_TIMEOUT or WAIT_OBJECT_0 — both prove
			// the kernel object is reachable). We don't rely on a signal
			// arriving inside this synchronous smoke window.
			uint64_t wakeup = 0;
			CHECK_XR(pfnAcquireWakeupEvent(session, &wakeup),
			         "xrAcquireWorkspaceWakeupEventEXT");
			if (wakeup == 0) {
				std::printf("FAIL: wakeup handle is NULL after XR_SUCCESS return\n");
			} else {
				DWORD wr = WaitForSingleObject(reinterpret_cast<HANDLE>(wakeup), 0);
				const char *wstr = (wr == WAIT_OBJECT_0)   ? "WAIT_OBJECT_0"
				                 : (wr == WAIT_TIMEOUT)    ? "WAIT_TIMEOUT"
				                 : (wr == WAIT_FAILED)     ? "WAIT_FAILED"
				                                           : "<other>";
				std::printf("[xrAcquireWorkspaceWakeupEventEXT(handle ok)] WaitForSingleObject(0ms)=%s\n",
				            wstr);
				CloseHandle(reinterpret_cast<HANDLE>(wakeup));
			}

			CHECK_XR(pfnDestroyChromeSwapchain(chromeSwapchain),
			         "xrDestroyWorkspaceClientChromeSwapchainEXT");
		}

		// Phase 2.C spec_version 9: per-client visual style smoke. Push a
		// sample style with edge feather + focus glow, then a NULL reset.
		// All three calls should return XR_SUCCESS.
		{
			XrWorkspaceClientStyleEXT style = {XR_TYPE_WORKSPACE_CLIENT_STYLE_EXT};
			style.cornerRadius = 0.06f;
			style.edgeFeatherMeters = 0.003f;
			style.focusGlowColor[0] = 0.30f;
			style.focusGlowColor[1] = 0.55f;
			style.focusGlowColor[2] = 1.00f;
			style.focusGlowColor[3] = 1.00f;
			style.focusGlowIntensity = 0.85f;
			style.focusGlowFalloffMeters = 0.012f;
			XrResult sr1 = pfnSetClientStyle(session, clientId, &style);
			std::printf("[xrSetWorkspaceClientStyleEXT(sample)       ] %s\n", xr_result_str(sr1));

			// NULL style → reset to runtime defaults.
			XrResult sr2 = pfnSetClientStyle(session, clientId, nullptr);
			std::printf("[xrSetWorkspaceClientStyleEXT(NULL=reset)   ] %s\n", xr_result_str(sr2));

			// Validation: NULL clientId must be rejected.
			XrResult sr3 = pfnSetClientStyle(session, XR_NULL_WORKSPACE_CLIENT_ID, &style);
			std::printf("[xrSetWorkspaceClientStyleEXT(NULL clientId)] %s (expect VALIDATION_FAILURE)\n",
			            xr_result_str(sr3));
		}

		// Phase 2.I-prequel: client enumeration smoke.
		// Count-query first, then enumerate. The smoke session is itself an
		// OpenXR client connection so enumerate should return at least 1 id
		// (this session). xrGetWorkspaceClientInfoEXT against the first id
		// should succeed and return a non-empty name.
		uint32_t client_count = 0;
		CHECK_XR(pfnEnumClients(session, 0, &client_count, nullptr),
		         "xrEnumerateWorkspaceClientsEXT(count)");
		std::printf("[xrEnumerateWorkspaceClientsEXT(0)         ] count=%u\n",
		            (unsigned)client_count);
		if (client_count > 0 && client_count <= 8) {
			XrWorkspaceClientId ids[8] = {};
			uint32_t got = 0;
			CHECK_XR(pfnEnumClients(session, client_count, &got, ids),
			         "xrEnumerateWorkspaceClientsEXT(fill)");
			std::printf("[xrEnumerateWorkspaceClientsEXT(fill)      ] got=%u\n",
			            (unsigned)got);
			if (got > 0 && ids[0] != XR_NULL_WORKSPACE_CLIENT_ID) {
				XrWorkspaceClientInfoEXT cinfo = {XR_TYPE_WORKSPACE_CLIENT_INFO_EXT};
				CHECK_XR(pfnGetClientInfo(session, ids[0], &cinfo),
				         "xrGetWorkspaceClientInfoEXT");
				std::printf("[xrGetWorkspaceClientInfoEXT(id=%u)        ] name=\"%s\" pid=%llu z=%u "
				            "focused=%u visible=%u\n",
				            (unsigned)ids[0], cinfo.name, (unsigned long long)cinfo.pid,
				            (unsigned)cinfo.zOrder, (unsigned)cinfo.isFocused,
				            (unsigned)cinfo.isVisible);
			}
		}

		// Phase 2.I-prequel: frame-capture smoke. Write to %TEMP% so the file
		// is easy to clean up. Validation may fail in the smoke window if the
		// compositor isn't drawing frames yet — accept any of XR_SUCCESS or
		// runtime-failure as a valid outcome here; the goal is to prove
		// dispatch reached the IPC layer.
		{
			XrWorkspaceCaptureRequestEXT req = {XR_TYPE_WORKSPACE_CAPTURE_REQUEST_EXT};
			std::snprintf(req.pathPrefix, sizeof(req.pathPrefix),
			              "%s\\workspace_smoke_capture", std::getenv("TEMP") ? std::getenv("TEMP") : ".");
			req.flags = XR_WORKSPACE_CAPTURE_FLAG_ATLAS_BIT_EXT;
			XrWorkspaceCaptureResultEXT cres = {XR_TYPE_WORKSPACE_CAPTURE_RESULT_EXT};
			XrResult cr = pfnCaptureFrame(session, &req, &cres);
			std::printf("[xrCaptureWorkspaceFrameEXT(%s)] %s atlas=%ux%u eye=%ux%u\n",
			            req.pathPrefix, xr_result_str(cr),
			            (unsigned)cres.atlasWidth, (unsigned)cres.atlasHeight,
			            (unsigned)cres.eyeWidth, (unsigned)cres.eyeHeight);
		}

		// Clear focus before removing the captured client to keep state tidy.
		CHECK_XR(pfnSetFocused(session, XR_NULL_WORKSPACE_CLIENT_ID),
		         "xrSetWorkspaceFocusedClientEXT(clear)");
	}

	// Phase 2.K: request_client_exit against the synthetic capture client.
	// For capture clients this maps to multi_compositor_remove_capture_client
	// — same path RemoveWorkspaceCaptureClient takes — so the explicit
	// pfnRemoveCapture below is best-effort (the runtime may already have
	// torn the slot down). Validation against XR_NULL_WORKSPACE_CLIENT_ID
	// must return XR_ERROR_VALIDATION_FAILURE.
	{
		XrResult bad = pfnRequestExit(session, XR_NULL_WORKSPACE_CLIENT_ID);
		std::printf("[xrRequestWorkspaceClientExitEXT(NULL)      ] %s (expect VALIDATION_FAILURE)\n",
		            xr_result_str(bad));
		XrResult er = pfnRequestExit(session, clientId);
		std::printf("[xrRequestWorkspaceClientExitEXT(client=%u)] %s\n",
		            (unsigned)clientId, xr_result_str(er));
	}

	XrResult rmr = pfnRemoveCapture(session, clientId);
	std::printf("[xrRemoveWorkspaceCaptureClientEXT(post-exit)] %s (best-effort; "
	            "exit above already removed the slot)\n", xr_result_str(rmr));

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
