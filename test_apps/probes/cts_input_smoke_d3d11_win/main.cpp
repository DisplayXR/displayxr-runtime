// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  cts_input_smoke_d3d11_win — XR_EXT_conformance_automation input smoke.
 *
 * Automates the CTS Phase-2 input flow from docs/roadmap/cts-windows-handoff.md
 * without needing the Khronos CTS checkout: a real D3D11 handle session reaches
 * FOCUSED, then the probe injects controller state on
 * /interaction_profiles/khr/simple_controller and asserts it surfaces through
 * the action system:
 *
 *   1. xrSetInputDeviceActiveEXT(left+right)          — required; overrides are
 *      suppressed for devices not marked active (oxr_conformance.c).
 *   2. xrSetInputDeviceStateBoolEXT select L=1 R=0 → xrSyncActions →
 *      xrGetActionStateBoolean: L TRUE / R FALSE, isActive, lastChangeTime > 0.
 *   3. Flip L→0 → next sync: FALSE with changedSinceLastSync.
 *   4. xrSetInputDeviceLocationEXT(grip, LOCAL, known pose) → locate the grip
 *      action space in LOCAL: position within tolerance (doc item 3 —
 *      space-relative application).
 *
 * Injection overrides whatever device owns the hand roles (overrides are keyed
 * by source path at the oxr level), so the smoke is valid both with an input
 * provider registered (sim-input square-waving select underneath) and with
 * qwerty defaults. Prints CTS-SMOKE lines and exits: 0 = all pass,
 * 1 = bool/actions failure, 2 = only the pose check failed.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include "logging.h"
#include "d3d11_renderer.h"
#include "xr_session.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static const char* APP_NAME = "cts_input_smoke_d3d11_win";
static const wchar_t* WINDOW_CLASS = L"DXRCtsInputSmokeD3D11Class";
static const wchar_t* WINDOW_TITLE = L"CTS Input Smoke — XR_EXT_conformance_automation";

static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// ---------------------------------------------------------------------------
// Actions state (khr/simple_controller: grip pose + select/click).
// ---------------------------------------------------------------------------
struct SmokeActions {
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction poseAction = XR_NULL_HANDLE;
    XrAction selectAction = XR_NULL_HANDLE;
    XrPath handPaths[2] = {XR_NULL_PATH, XR_NULL_PATH}; // 0 = left, 1 = right
    XrSpace gripSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
};
static SmokeActions g_act;

// Conformance-automation entry points.
static PFN_xrSetInputDeviceActiveEXT g_pfnSetActive = nullptr;
static PFN_xrSetInputDeviceStateBoolEXT g_pfnSetBool = nullptr;
static PFN_xrSetInputDeviceLocationEXT g_pfnSetLocation = nullptr;

// Check bookkeeping.
static int g_checksPassed = 0;
static int g_checksFailed = 0;
static bool g_poseCheckFailed = false;

static void Check(bool ok, const char* what, const char* detail)
{
    if (ok) {
        g_checksPassed++;
        LOG_INFO("CTS-SMOKE PASS: %s%s%s", what, detail[0] ? " — " : "", detail);
    } else {
        g_checksFailed++;
        LOG_ERROR("CTS-SMOKE FAIL: %s%s%s", what, detail[0] ? " — " : "", detail);
    }
}

static bool SetupActions(XrInstance instance, XrSession session)
{
    XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy_s(setInfo.actionSetName, "cts_smoke", sizeof(setInfo.actionSetName) - 1);
    strncpy_s(setInfo.localizedActionSetName, "CTS Smoke", sizeof(setInfo.localizedActionSetName) - 1);
    if (XR_FAILED(xrCreateActionSet(instance, &setInfo, &g_act.actionSet))) {
        LOG_ERROR("smoke: xrCreateActionSet failed");
        return false;
    }

    xrStringToPath(instance, "/user/hand/left", &g_act.handPaths[0]);
    xrStringToPath(instance, "/user/hand/right", &g_act.handPaths[1]);

    XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
    ai.countSubactionPaths = 2;
    ai.subactionPaths = g_act.handPaths;

    ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strncpy_s(ai.actionName, "hand_pose", sizeof(ai.actionName) - 1);
    strncpy_s(ai.localizedActionName, "Hand Pose", sizeof(ai.localizedActionName) - 1);
    if (XR_FAILED(xrCreateAction(g_act.actionSet, &ai, &g_act.poseAction))) {
        LOG_ERROR("smoke: create pose action failed");
        return false;
    }

    ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strncpy_s(ai.actionName, "select", sizeof(ai.actionName) - 1);
    strncpy_s(ai.localizedActionName, "Select", sizeof(ai.localizedActionName) - 1);
    if (XR_FAILED(xrCreateAction(g_act.actionSet, &ai, &g_act.selectAction))) {
        LOG_ERROR("smoke: create select action failed");
        return false;
    }

    XrPath profile;
    xrStringToPath(instance, "/interaction_profiles/khr/simple_controller", &profile);
    XrPath gripL, gripR, selectL, selectR;
    xrStringToPath(instance, "/user/hand/left/input/grip/pose", &gripL);
    xrStringToPath(instance, "/user/hand/right/input/grip/pose", &gripR);
    xrStringToPath(instance, "/user/hand/left/input/select/click", &selectL);
    xrStringToPath(instance, "/user/hand/right/input/select/click", &selectR);

    XrActionSuggestedBinding bindings[4] = {
        {g_act.poseAction, gripL},   {g_act.poseAction, gripR},
        {g_act.selectAction, selectL}, {g_act.selectAction, selectR},
    };
    XrInteractionProfileSuggestedBinding suggest = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggest.interactionProfile = profile;
    suggest.suggestedBindings = bindings;
    suggest.countSuggestedBindings = 4;
    if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &suggest))) {
        LOG_ERROR("smoke: suggest bindings failed");
        return false;
    }

    XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_act.actionSet;
    if (XR_FAILED(xrAttachSessionActionSets(session, &attach))) {
        LOG_ERROR("smoke: xrAttachSessionActionSets failed");
        return false;
    }

    for (int hand = 0; hand < 2; hand++) {
        XrActionSpaceCreateInfo si = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        si.action = g_act.poseAction;
        si.subactionPath = g_act.handPaths[hand];
        si.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(xrCreateActionSpace(session, &si, &g_act.gripSpaces[hand]))) {
            LOG_ERROR("smoke: create action space (hand %d) failed", hand);
            return false;
        }
    }

    LOG_INFO("smoke: action set attached (khr/simple_controller, pose + select)");
    return true;
}

static bool ResolveConformancePfns(XrInstance instance)
{
    xrGetInstanceProcAddr(instance, "xrSetInputDeviceActiveEXT", (PFN_xrVoidFunction*)&g_pfnSetActive);
    xrGetInstanceProcAddr(instance, "xrSetInputDeviceStateBoolEXT", (PFN_xrVoidFunction*)&g_pfnSetBool);
    xrGetInstanceProcAddr(instance, "xrSetInputDeviceLocationEXT", (PFN_xrVoidFunction*)&g_pfnSetLocation);
    return g_pfnSetActive != nullptr && g_pfnSetBool != nullptr && g_pfnSetLocation != nullptr;
}

//! Read select for one hand; returns false if the get call itself failed.
static bool GetSelect(XrSession session, int hand, XrActionStateBoolean* out)
{
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_act.selectAction;
    gi.subactionPath = g_act.handPaths[hand];
    *out = {XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, out));
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;
    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_SYSKEYDOWN:
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register window class, error: %lu", err);
            return nullptr;
        }
    }
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowEx(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }
    return hwnd;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }
    LOG_INFO("=== cts_input_smoke_d3d11_win === conformance-automation input smoke");

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) { ShutdownLogging(); return 1; }

    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr; ShutdownLogging(); return 1;
    }
    if (!HasConformanceAutomationExt()) {
        LOG_ERROR("CTS-SMOKE FAIL: XR_EXT_conformance_automation not advertised by the runtime");
        CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }

    LUID adapterLuid;
    if (!GetD3D11GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D11 graphics requirements");
        CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }

    D3D11Renderer renderer = {};
    if (!InitializeD3D11WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D11 initialization failed");
        CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }

    if (!CreateSession(xr, renderer.device.Get(), hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        CleanupD3D11(renderer); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }
    if (!CreateSpaces(xr) || !CreateSwapchain(xr)) {
        LOG_ERROR("Space/swapchain creation failed");
        CleanupD3D11(renderer); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }
    if (!SetupActions(xr.instance, xr.session) || !ResolveConformancePfns(xr.instance)) {
        LOG_ERROR("CTS-SMOKE FAIL: actions setup / conformance pfn resolution failed");
        CleanupD3D11(renderer); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }

    std::vector<XrSwapchainImageD3D11KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    XrPath profilePath;
    xrStringToPath(xr.instance, "/interaction_profiles/khr/simple_controller", &profilePath);
    XrPath selectPaths[2], gripPaths[2];
    xrStringToPath(xr.instance, "/user/hand/left/input/select/click", &selectPaths[0]);
    xrStringToPath(xr.instance, "/user/hand/right/input/select/click", &selectPaths[1]);
    xrStringToPath(xr.instance, "/user/hand/left/input/grip/pose", &gripPaths[0]);
    xrStringToPath(xr.instance, "/user/hand/right/input/grip/pose", &gripPaths[1]);

    const XrPosef kInjectedPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.25f, 0.35f, -0.45f}};

    // Smoke phases: 0 wait-for-focus, 1 assert L=1/R=0, 2 assert L flipped to 0,
    // 3 assert injected pose, 4 done (request exit).
    int phase = 0;
    int framesInPhase = 0;
    XrTime lastChangeAtInject = 0;

    MSG msg = {};
    while (g_running && !xr.exitRequested) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_running) break;

        PollEvents(xr);
        if (!xr.sessionRunning) { Sleep(50); continue; }

        XrFrameState frameState;
        if (!BeginFrame(xr, frameState)) continue;

        // Sync every frame; the smoke state machine advances on sync SUCCESS
        // (XR_SESSION_NOT_FOCUSED means keep waiting).
        XrActiveActionSet active = {g_act.actionSet, XR_NULL_PATH};
        XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &active;
        XrResult sr = xrSyncActions(xr.session, &syncInfo);
        bool focused = (sr == XR_SUCCESS);

        if (focused) {
            framesInPhase++;
            switch (phase) {
            case 0: {
                // First focused sync: log the resolved profile (doc R1), mark
                // both devices active, inject select L=1 R=0.
                XrInteractionProfileState ips = {XR_TYPE_INTERACTION_PROFILE_STATE};
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(xr.session, g_act.handPaths[0], &ips)) &&
                    ips.interactionProfile != XR_NULL_PATH) {
                    char buf[XR_MAX_PATH_LENGTH];
                    uint32_t len = 0;
                    xrPathToString(xr.instance, ips.interactionProfile, sizeof(buf), &len, buf);
                    Check(ips.interactionProfile == profilePath, "current interaction profile (left)", buf);
                } else {
                    Check(false, "current interaction profile (left)", "xrGetCurrentInteractionProfile failed/NULL");
                }
                XrResult r1 = g_pfnSetActive(xr.session, profilePath, g_act.handPaths[0], XR_TRUE);
                XrResult r2 = g_pfnSetActive(xr.session, profilePath, g_act.handPaths[1], XR_TRUE);
                Check(XR_SUCCEEDED(r1) && XR_SUCCEEDED(r2), "xrSetInputDeviceActiveEXT L+R", "");
                XrResult r3 = g_pfnSetBool(xr.session, g_act.handPaths[0], selectPaths[0], XR_TRUE);
                XrResult r4 = g_pfnSetBool(xr.session, g_act.handPaths[1], selectPaths[1], XR_FALSE);
                Check(XR_SUCCEEDED(r3) && XR_SUCCEEDED(r4), "xrSetInputDeviceStateBoolEXT L=1 R=0", "");
                phase = 1;
                framesInPhase = 0;
                break;
            }
            case 1: {
                XrActionStateBoolean L, R;
                bool okL = GetSelect(xr.session, 0, &L);
                bool okR = GetSelect(xr.session, 1, &R);
                char d[160];
                snprintf(d, sizeof(d), "L(active=%d state=%d t=%lld) R(active=%d state=%d)",
                         L.isActive, L.currentState, (long long)L.lastChangeTime, R.isActive, R.currentState);
                Check(okL && okR && L.isActive && L.currentState == XR_TRUE &&
                      R.isActive && R.currentState == XR_FALSE && L.lastChangeTime > 0,
                      "injected bool surfaces via xrSyncActions", d);
                lastChangeAtInject = L.lastChangeTime;
                g_pfnSetBool(xr.session, g_act.handPaths[0], selectPaths[0], XR_FALSE);
                phase = 2;
                framesInPhase = 0;
                break;
            }
            case 2: {
                XrActionStateBoolean L;
                bool okL = GetSelect(xr.session, 0, &L);
                char d[160];
                snprintf(d, sizeof(d), "L(active=%d state=%d changed=%d t=%lld->%lld)",
                         L.isActive, L.currentState, L.changedSinceLastSync,
                         (long long)lastChangeAtInject, (long long)L.lastChangeTime);
                Check(okL && L.isActive && L.currentState == XR_FALSE && L.changedSinceLastSync == XR_TRUE &&
                      L.lastChangeTime >= lastChangeAtInject,
                      "flip to FALSE observed with changedSinceLastSync", d);
                XrResult rp = g_pfnSetLocation(xr.session, g_act.handPaths[0], gripPaths[0],
                                               xr.localSpace, kInjectedPose);
                Check(XR_SUCCEEDED(rp), "xrSetInputDeviceLocationEXT (grip, LOCAL)", "");
                phase = 3;
                framesInPhase = 0;
                break;
            }
            case 3: {
                XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
                XrResult lr = xrLocateSpace(g_act.gripSpaces[0], xr.localSpace,
                                            frameState.predictedDisplayTime, &loc);
                const XrSpaceLocationFlags needed =
                    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                float dx = loc.pose.position.x - kInjectedPose.position.x;
                float dy = loc.pose.position.y - kInjectedPose.position.y;
                float dz = loc.pose.position.z - kInjectedPose.position.z;
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                char d[160];
                snprintf(d, sizeof(d), "located (%.3f, %.3f, %.3f), injected (0.250, 0.350, -0.450), err=%.4fm",
                         loc.pose.position.x, loc.pose.position.y, loc.pose.position.z, dist);
                bool poseOk = XR_SUCCEEDED(lr) && (loc.locationFlags & needed) == needed && dist < 0.005f;
                Check(poseOk, "injected pose surfaces via grip action space", d);
                if (!poseOk) {
                    g_poseCheckFailed = true;
                }
                phase = 4;
                framesInPhase = 0;
                break;
            }
            case 4:
            default:
                // Give the compositor a few more frames, then exit cleanly.
                if (framesInPhase > 30) {
                    LOG_INFO("CTS-SMOKE: done — %d passed, %d failed", g_checksPassed, g_checksFailed);
                    xrRequestExitSession(xr.session);
                }
                break;
            }
        }

        int eyeCount = 2;
        std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        bool rendered = false;
        if (frameState.shouldRender) {
            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = xr.viewConfigType;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = xr.localSpace;
            XrViewState viewState = {XR_TYPE_VIEW_STATE};
            uint32_t viewCount = 8;
            XrView rawViews[8];
            for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};
            xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCount, rawViews);

            // Scale to the active mode like the other probes, so the compositor
            // sees the expected per-view content dims (no VIEW SIZE MISMATCH).
            if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
                xr.recommendedViewScaleX = xr.renderingModeScaleX[xr.currentModeIndex];
                xr.recommendedViewScaleY = xr.renderingModeScaleY[xr.currentModeIndex];
            }
            uint32_t maxTileW = xr.swapchain.width / 2;
            uint32_t maxTileH = xr.swapchain.height;
            uint32_t renderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
            uint32_t renderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
            if (renderW > maxTileW) renderW = maxTileW;
            if (renderH > maxTileH) renderH = maxTileH;

            uint32_t imageIndex;
            if (AcquireSwapchainImage(xr, imageIndex)) {
                ID3D11Texture2D* swapchainTexture = swapchainImages[imageIndex].texture;
                ID3D11RenderTargetView* rtv = nullptr;
                CreateRenderTargetView(renderer, swapchainTexture,
                    static_cast<DXGI_FORMAT>(xr.swapchain.format), &rtv);
                float clearColor[4] = {0.05f, 0.1f, 0.15f, 1.0f};
                renderer.context->ClearRenderTargetView(rtv, clearColor);
                if (rtv) rtv->Release();

                for (int eye = 0; eye < eyeCount; eye++) {
                    projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                    projectionViews[eye].subImage.imageRect.offset = {(int32_t)(eye * renderW), 0};
                    projectionViews[eye].subImage.imageRect.extent = {(int32_t)renderW, (int32_t)renderH};
                    projectionViews[eye].subImage.imageArrayIndex = 0;
                    int safeIdx = (eye < (int)viewCount) ? eye : 0;
                    projectionViews[eye].pose = rawViews[safeIdx].pose;
                    projectionViews[eye].fov = rawViews[safeIdx].fov;
                }
                ReleaseSwapchainImage(xr);
                rendered = true;
            }
        }
        (void)rendered;
        EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), (uint32_t)eyeCount);
    }

    LOG_INFO("=== Shutting down (passed=%d failed=%d) ===", g_checksPassed, g_checksFailed);
    g_xr = nullptr;
    CleanupOpenXR(xr);
    CleanupD3D11(renderer);
    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);
    ShutdownLogging();
    if (g_checksFailed == 0) {
        return 0;
    }
    return (g_checksFailed == 1 && g_poseCheckFailed) ? 2 : 1;
}
