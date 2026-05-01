// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shell-side OpenXR scaffolding (Phase 2.I).
 *
 * The shell migrates off internal IPC onto the public OpenXR extension
 * surface (XR_EXT_spatial_workspace v5 + XR_EXT_app_launcher v1) over a
 * series of commits. C7 set up the OpenXR instance + session and resolves
 * PFNs into a struct; C8–C10 replace ipc_call_* sites with PFN dispatch.
 * After C10 the shell drops its internal-IPC includes entirely.
 *
 * Pure-C interface so main.c (still compiled as C) can drive setup, look
 * up PFNs, and tear down.
 */
#pragma once

// Pull only the core + extension headers here. Platform-specific bindings
// (XrGraphicsBindingD3D11KHR etc.) live in shell_openxr.cpp where d3d11.h
// is also visible — main.c doesn't need them.
#include <openxr/openxr.h>
#include <openxr/XR_EXT_spatial_workspace.h>
#include <openxr/XR_EXT_app_launcher.h>
#include <openxr/XR_EXT_display_info.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Bundle of session + PFNs the shell dispatches through. Populated by
 * shell_openxr_init(); fields are NULL/XR_NULL_HANDLE before init and
 * after shutdown. PFNs the shell does not call (hit-test, pointer
 * capture, etc.) are intentionally not resolved.
 */
struct shell_openxr_state
{
	XrInstance  instance;
	XrSystemId  system_id;
	XrSession   session;

	PFN_xrActivateSpatialWorkspaceEXT      activate;
	PFN_xrDeactivateSpatialWorkspaceEXT    deactivate;
	PFN_xrGetSpatialWorkspaceStateEXT      get_state;
	PFN_xrAddWorkspaceCaptureClientEXT     add_capture;
	PFN_xrRemoveWorkspaceCaptureClientEXT  remove_capture;
	PFN_xrSetWorkspaceClientWindowPoseEXT  set_pose;
	PFN_xrGetWorkspaceClientWindowPoseEXT  get_pose;
	PFN_xrCaptureWorkspaceFrameEXT         capture_frame;
	PFN_xrClearLauncherAppsEXT             clear_launcher;
	PFN_xrAddLauncherAppEXT                add_launcher_app;
	PFN_xrSetLauncherVisibleEXT            set_launcher_visible;
	PFN_xrPollLauncherClickEXT             poll_launcher_click;
	PFN_xrSetLauncherRunningTileMaskEXT    set_running_tile_mask;
	PFN_xrSetWorkspaceFocusedClientEXT     set_focused;
	PFN_xrEnumerateWorkspaceClientsEXT     enumerate_clients;
	PFN_xrGetWorkspaceClientInfoEXT        get_client_info;
	PFN_xrEnumerateWorkspaceInputEventsEXT enumerate_input_events;
	// Phase 2.K: pointer capture + lifecycle requests for controller-driven
	// motion policy.
	PFN_xrEnableWorkspacePointerCaptureEXT     enable_pointer_capture;
	PFN_xrDisableWorkspacePointerCaptureEXT    disable_pointer_capture;
	PFN_xrRequestWorkspaceClientExitEXT        request_client_exit;
	PFN_xrRequestWorkspaceClientFullscreenEXT  request_client_fullscreen;
	// Phase 2.C: controller-owned chrome.
	PFN_xrCreateWorkspaceClientChromeSwapchainEXT  create_chrome_swapchain;
	PFN_xrDestroyWorkspaceClientChromeSwapchainEXT destroy_chrome_swapchain;
	PFN_xrSetWorkspaceClientChromeLayoutEXT        set_chrome_layout;
	// Phase 2.C spec_version 8: event-driven wakeup so the shell sleeps
	// instead of polling. Acquired once at startup; stored here for
	// main.c to reference.
	PFN_xrAcquireWorkspaceWakeupEventEXT           acquire_wakeup_event;
	void                                          *wakeup_event_handle; // Win32 HANDLE on Windows; uint64_t cast back. NULL if unsupported.

	// Physical display size in meters, pulled from XR_EXT_display_info during
	// init. Falls back to LP-3D dims (0.700 × 0.394 m) if the extension is
	// not enabled or returns zero. Layout-preset math reads these to scale
	// poses to the actual display.
	float display_width_m;
	float display_height_m;

	// Phase 2.C: D3D11 device for chrome rendering. The shell creates this
	// on the same adapter the runtime selected (matched via DXGI LUID from
	// xrGetD3D11GraphicsRequirementsKHR) so SHARED_NTHANDLE chrome textures
	// open cleanly with OpenSharedResource1. NULL on platforms without
	// XR_KHR_D3D11_enable. Declared as void* in this C-visible header so
	// main.c (compiled as C) does not pull d3d11.h.
	void *d3d11_device;        // ID3D11Device *
	void *d3d11_context;       // ID3D11DeviceContext *
};

/*!
 * Initialize the OpenXR scaffolding: create a minimal D3D11 device,
 * xrCreateInstance with the workspace + launcher extensions enabled,
 * xrGetSystem, xrCreateSession with a D3D11 graphics binding, and
 * resolve every PFN this shell uses into the returned struct.
 *
 * Returns NULL on failure with diagnostics printed via stderr. Lifetime
 * is owned by the caller; pair with shell_openxr_shutdown.
 */
struct shell_openxr_state *shell_openxr_init(void);

/*!
 * Tear down the session, instance, and D3D11 device. Safe to pass NULL.
 */
void shell_openxr_shutdown(struct shell_openxr_state *s);

#ifdef __cplusplus
}
#endif
