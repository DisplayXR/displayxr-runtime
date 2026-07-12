// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the XR_DXR_weave probe (#625).
 */

#include "xr_session.h"
#include "logging.h"
#include <cstring>
#include <vector>

bool g_hasWeaveExt = false;
PFN_xrWeaveBindWindowDXR g_pfnWeaveBindWindow = nullptr;
PFN_xrWeaveSubmitDXR g_pfnWeaveSubmit = nullptr;

#define XR_CHECK(call)                                                                                                  \
	do {                                                                                                               \
		XrResult result = (call);                                                                                    \
		if (XR_FAILED(result)) {                                                                                     \
			LogXrResult(#call, result);                                                                          \
			return false;                                                                                        \
		}                                                                                                            \
	} while (0)

#define XR_CHECK_LOG(call)                                                                                             \
	do {                                                                                                               \
		XrResult result = (call);                                                                                    \
		LogXrResult(#call, result);                                                                                  \
		if (XR_FAILED(result)) {                                                                                     \
			return false;                                                                                        \
		}                                                                                                            \
	} while (0)

bool
GetD3D11GraphicsRequirements(XrSessionManager &xr, LUID *outAdapterLuid)
{
	PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
	XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetD3D11GraphicsRequirementsKHR",
	                                        (PFN_xrVoidFunction *)&pfn);
	if (XR_FAILED(result) || !pfn) {
		LOG_ERROR("Failed to get xrGetD3D11GraphicsRequirementsKHR");
		return false;
	}
	XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
	result = pfn(xr.instance, xr.systemId, &req);
	if (XR_FAILED(result)) {
		LogXrResult("xrGetD3D11GraphicsRequirementsKHR", result);
		return false;
	}
	LOG_INFO("D3D11 adapter LUID: 0x%08X%08X", req.adapterLuid.HighPart, req.adapterLuid.LowPart);
	*outAdapterLuid = req.adapterLuid;
	return true;
}

bool
InitializeOpenXR(XrSessionManager &xr)
{
	uint32_t extensionCount = 0;
	XR_CHECK_LOG(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
	std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
	XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

	bool hasD3D11 = false;
	xr.hasWin32WindowBindingExt = false;
	xr.hasDisplayInfoExt = false;
	g_hasWeaveExt = false;
	for (const auto &ext : extensions) {
		if (strcmp(ext.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) {
			hasD3D11 = true;
		}
		if (strcmp(ext.extensionName, XR_DXR_WIN32_WINDOW_BINDING_EXTENSION_NAME) == 0) {
			xr.hasWin32WindowBindingExt = true;
		}
		if (strcmp(ext.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) {
			xr.hasDisplayInfoExt = true;
		}
		if (strcmp(ext.extensionName, XR_DXR_WEAVE_EXTENSION_NAME) == 0) {
			g_hasWeaveExt = true;
		}
	}

	LOG_INFO("XR_KHR_D3D11_enable:         %s", hasD3D11 ? "AVAILABLE" : "NOT FOUND");
	LOG_INFO("XR_DXR_win32_window_binding: %s", xr.hasWin32WindowBindingExt ? "AVAILABLE" : "NOT FOUND");
	LOG_INFO("XR_DXR_display_info:         %s", xr.hasDisplayInfoExt ? "AVAILABLE" : "NOT FOUND");
	LOG_INFO("XR_DXR_weave:                %s", g_hasWeaveExt ? "AVAILABLE" : "NOT FOUND");

	if (!hasD3D11) {
		LOG_ERROR("XR_KHR_D3D11_enable not available - cannot continue");
		return false;
	}
	if (!xr.hasWin32WindowBindingExt) {
		LOG_ERROR("XR_DXR_win32_window_binding not available - cannot bind the present-owner window");
		return false;
	}
	if (!g_hasWeaveExt) {
		LOG_ERROR("XR_DXR_weave not advertised - the weave service is unavailable on this runtime");
		return false;
	}

	std::vector<const char *> enabled;
	enabled.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
	enabled.push_back(XR_DXR_WIN32_WINDOW_BINDING_EXTENSION_NAME);
	if (xr.hasDisplayInfoExt) {
		enabled.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
	}
	enabled.push_back(XR_DXR_WEAVE_EXTENSION_NAME);

	XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy_s(createInfo.applicationInfo.applicationName, "DXRWeaveRpcProbe");
	createInfo.applicationInfo.applicationVersion = 1;
	strcpy_s(createInfo.applicationInfo.engineName, "None");
	createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	createInfo.enabledExtensionCount = (uint32_t)enabled.size();
	createInfo.enabledExtensionNames = enabled.data();
	XR_CHECK_LOG(xrCreateInstance(&createInfo, &xr.instance));

	XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK_LOG(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));

	{
		XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
		if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
			memcpy(xr.systemName, sysProps.systemName, sizeof(xr.systemName));
			LOG_INFO("System name: %s", xr.systemName);
		}
	}
	if (xr.hasDisplayInfoExt) {
		XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
		XrDisplayInfoDXR di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_DXR};
		sysProps.next = &di;
		if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
			xr.displayPixelWidth = di.displayPixelWidth;
			xr.displayPixelHeight = di.displayPixelHeight;
			LOG_INFO("Display pixels: %ux%u", xr.displayPixelWidth, xr.displayPixelHeight);
		}
	}

	// Enumerate view config (required before xrBeginSession / valid session).
	uint32_t viewCount = 0;
	XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
	xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount,
	                                           xr.configViews.data()));
	LOG_INFO("OpenXR init complete (%u views)", viewCount);
	return true;
}

bool
CreateSession(XrSessionManager &xr, ID3D11Device *d3d11Device, HWND appHwnd)
{
	LOG_INFO("Creating forced-IPC session (present-owner: HWND=%p, transparent, no shared texture)",
	         (void *)appHwnd);
	xr.windowHandle = appHwnd;

	XrGraphicsBindingD3D11KHR d3d11Binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	d3d11Binding.device = d3d11Device;

	// Present-owner binding: real HWND + transparentBackgroundEnabled, NO shared
	// texture. The non-null HWND makes the per-client DP bind to this window
	// (correct phase reference); transparent avoids the cross-process swap-chain
	// path (the service can't present our window). We never submit projection
	// layers — the weave service is driven directly via xrWeaveSubmitDXR.
	XrWin32WindowBindingCreateInfoDXR bind = {XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR};
	bind.windowHandle = (void *)appHwnd;
	bind.sharedTextureHandle = nullptr;
	bind.transparentBackgroundEnabled = XR_TRUE;
	d3d11Binding.next = &bind;

	XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
	sessionInfo.next = &d3d11Binding;
	sessionInfo.systemId = xr.systemId;
	XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
	LOG_INFO("Session created: %p", (void *)xr.session);

	// Resolve the weave entry points.
	xrGetInstanceProcAddr(xr.instance, "xrWeaveBindWindowDXR", (PFN_xrVoidFunction *)&g_pfnWeaveBindWindow);
	xrGetInstanceProcAddr(xr.instance, "xrWeaveSubmitDXR", (PFN_xrVoidFunction *)&g_pfnWeaveSubmit);
	LOG_INFO("xrWeaveBindWindowDXR: %s, xrWeaveSubmitDXR: %s", g_pfnWeaveBindWindow ? "resolved" : "NULL",
	         g_pfnWeaveSubmit ? "resolved" : "NULL");
	if (!g_pfnWeaveBindWindow || !g_pfnWeaveSubmit) {
		LOG_ERROR("Failed to resolve weave entry points");
		return false;
	}
	return true;
}
