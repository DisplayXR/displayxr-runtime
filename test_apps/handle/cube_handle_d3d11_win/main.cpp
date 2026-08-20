// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext - OpenXR with XR_DXR_win32_window_binding extension
 *
 * This application demonstrates OpenXR with the XR_DXR_win32_window_binding extension.
 * The application creates and controls its own window for rendering.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include "logging.h"
#include "input_handler.h"
#include "d3d11_renderer.h"

// #918 review F1 — ID3D11DeviceContext1::ClearView, for the Tier-3 mask stroke.
#include "atlas_capture.h"
#include "hud_renderer.h"
#include "projection_depth.h"
#include "text_overlay.h"
#include "xr_session.h"
#include <d3d11_1.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <string>
#include <sstream>
#include <vector>

// ============================================================================
// Actions mode (#823 Phase 3) — optional, `--actions` / DXR_ACTIONS=1.
//
// First in-repo consumer of the OpenXR action system: an action set with a
// pose + select + haptic action on khr/simple_controller, synced every
// frame; tracked controllers render as small marker cubes (bigger while
// select is pressed), and a select press fires xrApplyHapticFeedback.
// Drive the controllers with the sim_input provider (deterministic
// circles; `register_dev_plugin.bat input`) or net_input +
// scripts/net_input_feeder.py.
// ============================================================================

static bool g_actionsMode = false;

struct ActionsState {
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction poseAction = XR_NULL_HANDLE;
    XrAction selectAction = XR_NULL_HANDLE;
    XrAction hapticAction = XR_NULL_HANDLE;
    XrPath handPaths[2] = {XR_NULL_PATH, XR_NULL_PATH}; // 0 = left, 1 = right
    XrSpace gripSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    // Per-frame results consumed by the renderer.
    bool poseValid[2] = {false, false};
    XrPosef pose[2];
    bool selectPressed[2] = {false, false};
    bool prevSelectPressed[2] = {false, false};
    uint64_t syncCount = 0;
    uint64_t hapticsFired = 0;
};
static ActionsState g_actions;

static bool SetupActions(XrInstance instance, XrSession session)
{
    XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy_s(setInfo.actionSetName, "cube_actions", sizeof(setInfo.actionSetName) - 1);
    strncpy_s(setInfo.localizedActionSetName, "Cube Actions", sizeof(setInfo.localizedActionSetName) - 1);
    if (XR_FAILED(xrCreateActionSet(instance, &setInfo, &g_actions.actionSet))) {
        LOG_ERROR("actions: xrCreateActionSet failed");
        return false;
    }

    xrStringToPath(instance, "/user/hand/left", &g_actions.handPaths[0]);
    xrStringToPath(instance, "/user/hand/right", &g_actions.handPaths[1]);

    XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
    ai.countSubactionPaths = 2;
    ai.subactionPaths = g_actions.handPaths;

    ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strncpy_s(ai.actionName, "hand_pose", sizeof(ai.actionName) - 1);
    strncpy_s(ai.localizedActionName, "Hand Pose", sizeof(ai.localizedActionName) - 1);
    if (XR_FAILED(xrCreateAction(g_actions.actionSet, &ai, &g_actions.poseAction))) {
        LOG_ERROR("actions: create pose action failed");
        return false;
    }

    ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strncpy_s(ai.actionName, "select", sizeof(ai.actionName) - 1);
    strncpy_s(ai.localizedActionName, "Select", sizeof(ai.localizedActionName) - 1);
    if (XR_FAILED(xrCreateAction(g_actions.actionSet, &ai, &g_actions.selectAction))) {
        LOG_ERROR("actions: create select action failed");
        return false;
    }

    ai.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    strncpy_s(ai.actionName, "haptic", sizeof(ai.actionName) - 1);
    strncpy_s(ai.localizedActionName, "Haptic", sizeof(ai.localizedActionName) - 1);
    if (XR_FAILED(xrCreateAction(g_actions.actionSet, &ai, &g_actions.hapticAction))) {
        LOG_ERROR("actions: create haptic action failed");
        return false;
    }

    // khr/simple_controller bindings for both hands.
    XrPath profile;
    xrStringToPath(instance, "/interaction_profiles/khr/simple_controller", &profile);
    XrPath gripL, gripR, selectL, selectR, hapticL, hapticR;
    xrStringToPath(instance, "/user/hand/left/input/grip/pose", &gripL);
    xrStringToPath(instance, "/user/hand/right/input/grip/pose", &gripR);
    xrStringToPath(instance, "/user/hand/left/input/select/click", &selectL);
    xrStringToPath(instance, "/user/hand/right/input/select/click", &selectR);
    xrStringToPath(instance, "/user/hand/left/output/haptic", &hapticL);
    xrStringToPath(instance, "/user/hand/right/output/haptic", &hapticR);

    XrActionSuggestedBinding bindings[6] = {
        {g_actions.poseAction, gripL},     {g_actions.poseAction, gripR},
        {g_actions.selectAction, selectL}, {g_actions.selectAction, selectR},
        {g_actions.hapticAction, hapticL}, {g_actions.hapticAction, hapticR},
    };
    XrInteractionProfileSuggestedBinding suggest = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggest.interactionProfile = profile;
    suggest.suggestedBindings = bindings;
    suggest.countSuggestedBindings = 6;
    if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &suggest))) {
        LOG_ERROR("actions: suggest bindings failed");
        return false;
    }

    XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_actions.actionSet;
    if (XR_FAILED(xrAttachSessionActionSets(session, &attach))) {
        LOG_ERROR("actions: xrAttachSessionActionSets failed");
        return false;
    }

    for (int hand = 0; hand < 2; hand++) {
        XrActionSpaceCreateInfo si = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        si.action = g_actions.poseAction;
        si.subactionPath = g_actions.handPaths[hand];
        si.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(xrCreateActionSpace(session, &si, &g_actions.gripSpaces[hand]))) {
            LOG_ERROR("actions: create action space (hand %d) failed", hand);
            return false;
        }
    }

    LOG_INFO("actions: action set attached (khr/simple_controller, pose + select + haptic)");
    return true;
}

//! Per-frame: sync + read state + locate the grip spaces at display time.
static void UpdateActions(XrSession session, XrSpace baseSpace, XrTime displayTime)
{
    XrActiveActionSet active = {g_actions.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    XrResult sr = xrSyncActions(session, &syncInfo);
    if (sr == XR_SESSION_NOT_FOCUSED) {
        // Normal until the session reaches FOCUSED; markers stay hidden.
        g_actions.poseValid[0] = g_actions.poseValid[1] = false;
        return;
    }
    if (XR_FAILED(sr)) {
        return;
    }
    g_actions.syncCount++;

    for (int hand = 0; hand < 2; hand++) {
        g_actions.prevSelectPressed[hand] = g_actions.selectPressed[hand];
        g_actions.poseValid[hand] = false;
        g_actions.selectPressed[hand] = false;

        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath = g_actions.handPaths[hand];

        gi.action = g_actions.poseAction;
        XrActionStatePose poseState = {XR_TYPE_ACTION_STATE_POSE};
        if (XR_FAILED(xrGetActionStatePose(session, &gi, &poseState)) || !poseState.isActive) {
            continue;
        }

        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(g_actions.gripSpaces[hand], baseSpace, displayTime, &loc))) {
            continue;
        }
        const XrSpaceLocationFlags needed =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((loc.locationFlags & needed) != needed) {
            continue;
        }
        g_actions.poseValid[hand] = true;
        g_actions.pose[hand] = loc.pose;

        gi.action = g_actions.selectAction;
        XrActionStateBoolean sel = {XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &sel)) && sel.isActive) {
            g_actions.selectPressed[hand] = sel.currentState == XR_TRUE;
        }

        // Rising edge → haptic pulse on that hand (full round-trip:
        // xrApplyHapticFeedback → set_output → provider).
        if (g_actions.selectPressed[hand] && !g_actions.prevSelectPressed[hand]) {
            XrHapticVibration vib = {XR_TYPE_HAPTIC_VIBRATION};
            vib.amplitude = 0.8f;
            vib.frequency = XR_FREQUENCY_UNSPECIFIED;
            vib.duration = 100 * 1000 * 1000; // 100 ms
            XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
            hi.action = g_actions.hapticAction;
            hi.subactionPath = g_actions.handPaths[hand];
            if (XR_SUCCEEDED(xrApplyHapticFeedback(session, &hi, (const XrHapticBaseHeader *)&vib))) {
                g_actions.hapticsFired++;
            }
        }
    }

    // Throttled progress line (~every 2 s at 60 FPS) for headless-log runs.
    if (g_actions.syncCount % 120 == 1) {
        LOG_INFO("actions: sync #%llu L(valid=%d sel=%d %.2f,%.2f,%.2f) R(valid=%d sel=%d %.2f,%.2f,%.2f) haptics=%llu",
                 (unsigned long long)g_actions.syncCount,
                 g_actions.poseValid[0], g_actions.selectPressed[0],
                 g_actions.pose[0].position.x, g_actions.pose[0].position.y, g_actions.pose[0].position.z,
                 g_actions.poseValid[1], g_actions.selectPressed[1],
                 g_actions.pose[1].position.x, g_actions.pose[1].position.y, g_actions.pose[1].position.z,
                 (unsigned long long)g_actions.hapticsFired);
    }
}

// ============================================================================
// Hands mode (#825 Tier 2) — optional, `--hands` / DXR_HANDS=1.
//
// First in-repo consumer of XR_EXT_hand_tracking: creates a hand tracker per
// hand, locates all 26 joints at display time every frame, and renders each
// tracked joint as a tiny marker cube. Drive it with any hand-tracking input
// provider — sim_input (scripted curl wave, hardware-free) or ultraleap.
// Composes with --actions (grip markers + joint markers together).
// ============================================================================

static bool g_handsMode = false;

struct HandsState {
    PFN_xrCreateHandTrackerEXT pfnCreate = nullptr;
    PFN_xrDestroyHandTrackerEXT pfnDestroy = nullptr;
    PFN_xrLocateHandJointsEXT pfnLocate = nullptr;
    XrHandTrackerEXT tracker[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE}; // 0 = left, 1 = right

    // Per-frame results consumed by the renderer.
    bool active[2] = {false, false};
    XrHandJointLocationEXT joints[2][XR_HAND_JOINT_COUNT_EXT];
    uint64_t locateCount = 0;
};
static HandsState g_hands;

static bool SetupHands(XrInstance instance, XrSystemId systemId, XrSession session)
{
    if (!g_hasHandTrackingExt) {
        LOG_ERROR("hands: XR_EXT_hand_tracking not available on this runtime");
        return false;
    }

    // The runtime only supports hand tracking when a hand-tracking-capable
    // input provider claimed the roles — surface that verdict up front.
    XrSystemHandTrackingPropertiesEXT htProps = {XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES, &htProps};
    if (XR_SUCCEEDED(xrGetSystemProperties(instance, systemId, &sysProps))) {
        LOG_INFO("hands: XrSystemHandTrackingPropertiesEXT.supportsHandTracking = %d",
                 htProps.supportsHandTracking);
        if (!htProps.supportsHandTracking) {
            LOG_ERROR("hands: system reports no hand tracking (no provider with hand-tracking "
                      "devices registered?)");
            return false;
        }
    }

    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT",
                                        (PFN_xrVoidFunction*)&g_hands.pfnCreate)) ||
        XR_FAILED(xrGetInstanceProcAddr(instance, "xrDestroyHandTrackerEXT",
                                        (PFN_xrVoidFunction*)&g_hands.pfnDestroy)) ||
        XR_FAILED(xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT",
                                        (PFN_xrVoidFunction*)&g_hands.pfnLocate))) {
        LOG_ERROR("hands: xrGetInstanceProcAddr for XR_EXT_hand_tracking functions failed");
        return false;
    }

    for (int hand = 0; hand < 2; hand++) {
        XrHandTrackerCreateInfoEXT ci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        ci.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        XrResult cr = g_hands.pfnCreate(session, &ci, &g_hands.tracker[hand]);
        if (XR_FAILED(cr)) {
            LOG_ERROR("hands: xrCreateHandTrackerEXT (hand %d) failed: %d", hand, cr);
            return false;
        }
    }

    LOG_INFO("hands: hand trackers created (default joint set, %d joints)", XR_HAND_JOINT_COUNT_EXT);
    return true;
}

//! Per-frame: locate all joints of both hands at display time.
static void UpdateHands(XrSpace baseSpace, XrTime displayTime)
{
    for (int hand = 0; hand < 2; hand++) {
        g_hands.active[hand] = false;
        if (g_hands.tracker[hand] == XR_NULL_HANDLE) {
            continue;
        }

        XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        li.baseSpace = baseSpace;
        li.time = displayTime;

        XrHandJointLocationsEXT locations = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locations.jointLocations = g_hands.joints[hand];

        if (XR_FAILED(g_hands.pfnLocate(g_hands.tracker[hand], &li, &locations))) {
            continue;
        }
        g_hands.active[hand] = locations.isActive == XR_TRUE;
    }
    g_hands.locateCount++;

    // Throttled progress line (~every 2 s at 60 FPS) for headless-log runs.
    if (g_hands.locateCount % 120 == 1) {
        const XrHandJointLocationEXT &lw = g_hands.joints[0][XR_HAND_JOINT_WRIST_EXT];
        const XrHandJointLocationEXT &rw = g_hands.joints[1][XR_HAND_JOINT_WRIST_EXT];
        LOG_INFO("hands: locate #%llu L(active=%d wrist %.2f,%.2f,%.2f) R(active=%d wrist %.2f,%.2f,%.2f)",
                 (unsigned long long)g_hands.locateCount,
                 g_hands.active[0], lw.pose.position.x, lw.pose.position.y, lw.pose.position.z,
                 g_hands.active[1], rw.pose.position.x, rw.pose.position.y, rw.pose.position.z);
    }
}

static void CleanupHands()
{
    for (int hand = 0; hand < 2; hand++) {
        if (g_hands.tracker[hand] != XR_NULL_HANDLE && g_hands.pfnDestroy != nullptr) {
            g_hands.pfnDestroy(g_hands.tracker[hand]);
            g_hands.tracker[hand] = XR_NULL_HANDLE;
        }
    }
}

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "cube_handle_d3d11_win";

// Window settings
static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtClass";
static const wchar_t* WINDOW_TITLE = L"D3D11 Cube \u2014 D3D11 Native Compositor (External Window)";

// Global state (single-threaded — all accessed from the main thread only)
// Cube test apps start with the WSUI HUD hidden — it skews perf comparisons
// against HUD-less apps (avatar, Unity). Shift+Tab shows it when wanted.
static InputState g_inputState = [] { InputState s; s.hudVisible = false; return s; }();
static bool g_running = true;
static XrSessionManager* g_xr = nullptr;

// #542 'H' (validation affordance): toggle the HARDWARE display state alone
// for the current mode via xrRequestDisplayModeDXR — the mode, the app's
// content, and the DP's weave/blit processing are untouched. In 3D the panel
// shows the woven atlas flat (blurry); fading parallax to zero converges
// back to sharp — the MANUAL tracking-loss transition shape (#522). The
// runtime auto-clears the override on the next mode request, so the local
// note resets whenever the mode changes.
static bool g_hwToggleRequested = false;
static bool g_hwOverrideActive = false;
static bool g_hwOverride3D = false;
static uint32_t g_hwOverrideModeIndex = 0;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_inSizeMove = false;  // True while user is dragging/resizing the window
static const uint32_t HUD_PIXEL_WIDTH = 380;
static const uint32_t HUD_PIXEL_HEIGHT = 470;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f; // tan(18°) → 36° vFOV
// Disturbance-free C toggle + absolute SPACE reset live in displayxr-common
// (common/rig_mode.{h,cpp}, driven by UpdateInputState/UpdateCameraMovement).
// The app only feeds InputState the canvas size + initial vHeight and submits
// XrCameraRigDXR::metersToVirtual = viewParams.cameraM2v (see below).
static const float HUD_WIDTH_FRACTION = 0.30f;

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// #439 Phase 3 — handle + mask + Local2D layer modes (§8 cases 2/3/4).
// DXR_LOCAL2D_PANEL=1  — submit a Local2D panel layer (case 3: layer-only,
//                        IMPLICIT mask from the panel rect, zero mask calls).
// DXR_LOCAL2D_MASK=1   — additionally create + submit an explicit Tier-2 mask
//                        with 3D island rects (case 2: the first
//                        handle + mask + layer app — islands weave, panel
//                        crisp, desktop visible where neither covers).
// DXR_LOCAL2D_PANEL2=1 — additionally submit a second, overlapping panel with
//                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT (case 4:
//                        list-order stacking + alpha fringing).
static bool g_l2dPanel = false;
static bool g_l2dMask = false;
// #918 review F1 — DXR_LOCAL2D_MASK=3 additionally acquires the Tier-3
// (app-drawn) render target and strokes it, which is what latches
// `app_authored` in the runtime and puts the STICKY mask on the bridge plane
// under the output-device split. Nothing in the handle-class apps exercised
// that path before.
static bool g_l2dMaskTier3 = false;
// #918 review F2 — DXR_LOCAL2D_MASK_DIM=WxH creates the mask at explicit dims
// instead of letting the runtime pick the window backing size. An authored mask
// maps STRETCH-TO-REGION, so a mask whose dims differ from the window is the
// case that distinguishes stretching from cropping.
static uint32_t g_l2dMaskW = 0;
static uint32_t g_l2dMaskH = 0;
static bool g_l2dPanel2 = false;
// #491 part 3 — 2D-under backdrop (0=off, 2=opaque, 3=semi-transparent).
static int g_l2dBackdropVariant = 0;
static bool g_l2dActive = false; // set once panels (+ optional mask) are live
static long g_l2dFrameCounter = 0;
static const long g_l2dActivationFrame = 10;

struct L2DPanel {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
static L2DPanel g_panel1, g_panel2, g_backdrop;
static XrRect2Di g_panel1Rect, g_panel2Rect, g_backdropRect;

// XR_DXR_view_rig (#396 W7 dogfood): the app chains a rig descriptor on every
// xrLocateViews and consumes the runtime's render-ready XrView{pose, fov}
// directly — the per-frame Kooima generation (display3d_resolve_window_rect +
// *_compute_views) is deleted; only clip policy stays app-side. Per-view
// staging container for the consumed views (matrices column-major):
struct RigView {
    float view_matrix[16];
    float projection_matrix[16];
    XrFovf fov;
};

// Column-major view matrix from a render-ready XrView pose:
// viewMatrix = R^T * translate(-position) — same construction as the
// displayxr::math rigs, so the runtime-rig path feeds the renderer the
// identical convention.
static void ViewMatrixFromXrPose(const XrPosef& pose, float* out) {
    const float qx = pose.orientation.x, qy = pose.orientation.y;
    const float qz = pose.orientation.z, qw = pose.orientation.w;
    float rot[16] = {};
    rot[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
    rot[1] = 2.0f * (qx * qy + qz * qw);
    rot[2] = 2.0f * (qx * qz - qy * qw);
    rot[4] = 2.0f * (qx * qy - qz * qw);
    rot[5] = 1.0f - 2.0f * (qx * qx + qz * qz);
    rot[6] = 2.0f * (qy * qz + qx * qw);
    rot[8] = 2.0f * (qx * qz + qy * qw);
    rot[9] = 2.0f * (qy * qz - qx * qw);
    rot[10] = 1.0f - 2.0f * (qx * qx + qy * qy);
    rot[15] = 1.0f;
    for (int i = 0; i < 16; i++) out[i] = 0.0f;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[j * 4 + i] = rot[i * 4 + j]; // R^T
    out[15] = 1.0f;
    out[12] = -(out[0] * pose.position.x + out[4] * pose.position.y + out[8] * pose.position.z);
    out[13] = -(out[1] * pose.position.x + out[5] * pose.position.y + out[9] * pose.position.z);
    out[14] = -(out[2] * pose.position.x + out[6] * pose.position.y + out[10] * pose.position.z);
}

// Column-major GL ([-1,1] clip-z) off-axis projection from a render-ready
// XrView fov + the app's own clip policy (fov is clip-independent). Pair with
// convert_projection_gl_to_zero_to_one() for D3D.
static void ProjectionFromXrFov(const XrFovf& fov, float nearZ, float farZ, float* out) {
    const float l = tanf(fov.angleLeft) * nearZ;
    const float r = tanf(fov.angleRight) * nearZ;
    const float b = tanf(fov.angleDown) * nearZ;
    const float t = tanf(fov.angleUp) * nearZ;
    for (int i = 0; i < 16; i++) out[i] = 0.0f;
    out[0] = 2.0f * nearZ / (r - l);
    out[5] = 2.0f * nearZ / (t - b);
    out[8] = (r + l) / (r - l);
    out[9] = (t + b) / (t - b);
    out[10] = -(farZ + nearZ) / (farZ - nearZ);
    out[11] = -1.0f;
    out[14] = -2.0f * farZ * nearZ / (farZ - nearZ);
}

// Display-local eye distance for the ZDP-anchored clip: z of (rigPose^-1 *
// eyeWorld). The display rig places the virtual display plane at the rig
// pose, so the eye's distance to that plane is the rig-local z — NOT
// pose.position.z once the player flies (non-identity rig pose). Degenerates
// to pose.position.z at identity.
static float RigLocalEyeZ(const XrPosef& rig, const XrVector3f& eyeWorld) {
    const float dx = eyeWorld.x - rig.position.x;
    const float dy = eyeWorld.y - rig.position.y;
    const float dz = eyeWorld.z - rig.position.z;
    // Rotate the delta by the conjugate quaternion (inverse rotation).
    const float qx = -rig.orientation.x, qy = -rig.orientation.y;
    const float qz = -rig.orientation.z, qw = rig.orientation.w;
    // v' = v + 2*q_vec x (q_vec x v + qw*v); we only need the z component.
    const float cx = qy * dz - qz * dy + qw * dx;
    const float cy = qz * dx - qx * dz + qw * dy;
    return dz + 2.0f * (qx * cy - qy * cx);
}

// Forward declaration — defined after PerformanceStats
struct RenderState;
static RenderState* g_renderState = nullptr;
static void RenderOneFrame(RenderState& rs);

// Toggle fullscreen mode for the app window
static void ToggleFullscreen(HWND hwnd) {
    if (g_fullscreen) {
        // Exit fullscreen - restore window style and position
        SetWindowLong(hwnd, GWL_STYLE, g_savedWindowStyle);
        SetWindowPos(hwnd, HWND_TOP,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            SWP_FRAMECHANGED);
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen mode");
    } else {
        // Enter fullscreen - save state and go borderless
        g_savedWindowStyle = GetWindowLong(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &g_savedWindowRect);

        HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMonitor, &mi);

        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED);
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen mode");
    }
}

// Window procedure (runs on main thread — single-threaded, no locking needed)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // C (disturbance-free rig toggle) and SPACE (absolute reset) are handled by
    // the shared displayxr-common input path: UpdateInputState sets the request,
    // UpdateCameraMovement runs the rig_mode conversion / reset using the canvas
    // size + initial vHeight the render loop feeds into InputState.
    UpdateInputState(g_inputState, msg, wParam, lParam);

    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        return 0;

    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_ENTERSIZEMOVE:
        g_inSizeMove = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_EXITSIZEMOVE:
        g_inSizeMove = false;
        return 0;

    case WM_PAINT:
        // During drag/resize, DefWindowProc runs a modal loop that blocks our
        // main message pump.  By leaving the window invalidated (no
        // BeginPaint/EndPaint), Windows keeps sending WM_PAINT inside that
        // modal loop, giving us a chance to keep rendering frames.
        if (g_inSizeMove && g_renderState != nullptr) {
            RenderOneFrame(*g_renderState);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_CLOSE:
        // Graceful shutdown: ask OpenXR to end the session so the state machine
        // runs STOPPING -> xrEndSession -> EXITING -> exitRequested before cleanup.
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running = false;
        PostQuitMessage(0);
        return 0;

    case WM_SYSKEYDOWN:
        // Prevent ALT from activating the system menu modal loop, which would
        // freeze rendering on this single-threaded app.  The ALT key state is
        // still readable via GetKeyState() for our input handler.
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        // #542: 'H' toggles the HARDWARE display state alone for the current
        // mode (xrRequestDisplayModeDXR) — handled in RenderOneFrame.
        if (wParam == 'H') {
            g_hwToggleRequested = true;
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == dxr_capture::kFlashTimerId) {
            dxr_capture::TickCaptureFlash(hwnd);
            return 0;
        }
        break;

    case dxr_capture::kFlashUserMsg:
        // Render thread requested a capture-flash; start it on this thread
        // (the message-pump thread that owns the HWND).
        dxr_capture::TriggerCaptureFlash(hwnd);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// DISPLAYXR_TRANSPARENT_BG=1 → cube clears RGBA(0,0,0,0), window uses
// WS_EX_NOREDIRECTIONBITMAP + null brush so DComp can show the desktop
// through the cube's transparent regions. Mirrors cube_handle_vk_win.
static bool TransparentBackgroundEnabled() {
    static const bool e = []() {
        const char *v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return e;
}

// DISPLAYXR_ARRAY_LAYOUT=1 submits the projection as a LAYERED / single-pass-
// instanced swapchain (arraySize=2: each view in array slice imageArrayIndex)
// instead of the default TILED atlas (views packed side-by-side by imageRect).
// This exercises the runtime's per-view array-slice sampling — the same content
// must weave with disparity either way. It only drives view_count<=2 modes (the
// array has 2 slices), so mode 1/2 (SBS/2D) work; a >2-view mode is capped to 2.
// Default off (tiled). Mirrors the cube_zones_*_win DISPLAYXR_ARRAY_LAYOUT toggle.
static bool ArrayLayoutEnabled() {
    static const bool e = []() {
        const char *v = getenv("DISPLAYXR_ARRAY_LAYOUT");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return e;
}
static const uint32_t kArraySlices = 2; // stereo

// Create the application window
static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    const bool transparent = TransparentBackgroundEnabled();
    LOG_INFO("Creating application window (%dx%d, transparent=%d)", width, height, transparent);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Null brush in transparent mode so the redirection bitmap doesn't paint
    // black under the DComp composition swap chain.
    wc.hbrBackground = transparent ? nullptr : (HBRUSH)GetStockObject(BLACK_BRUSH);
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

    DWORD exStyle = transparent ? WS_EX_NOREDIRECTIONBITMAP : 0;
    HWND hwnd = CreateWindowEx(
        exStyle,
        WINDOW_CLASS,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }

    LOG_INFO("Window created: 0x%p", hwnd);
    return hwnd;
}

// Performance tracking
struct PerformanceStats {
    std::chrono::high_resolution_clock::time_point lastTime;
    float deltaTime = 0.0f;
    float fps = 0.0f;
    float frameTimeMs = 0.0f;
    int frameCount = 0;
    float fpsAccumulator = 0.0f;
};

static void UpdatePerformanceStats(PerformanceStats& stats) {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - stats.lastTime);
    stats.deltaTime = duration.count() / 1000000.0f;
    stats.frameTimeMs = duration.count() / 1000.0f;
    stats.lastTime = now;

    stats.fpsAccumulator += stats.deltaTime;
    stats.frameCount++;

    if (stats.fpsAccumulator >= 1.0f) {
        stats.fps = stats.frameCount / stats.fpsAccumulator;
        stats.frameCount = 0;
        stats.fpsAccumulator = 0.0f;
    }
}

// State passed to RenderOneFrame (and accessible from WM_PAINT via g_renderState)
struct RenderState {
    HWND hwnd;
    XrSessionManager* xr;
    D3D11Renderer* renderer;
    HudRenderer* hudRenderer;
    bool hudOk;
    std::vector<XrSwapchainImageD3D11KHR>* hudSwapchainImages;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    std::vector<XrSwapchainImageD3D11KHR>* swapchainImages;
    PerformanceStats* perfStats;
};

// #439 Phase 3 — create a window-anchored Local2D panel swapchain and fill it
// once (static content: acquire/fill/release once; the layer references the
// released image every frame). Fill via UpdateSubresource (the swapchain image
// is DEFAULT-usage RT+SRV, same as the HUD path uses).
//  variant 0 — crispness panel: opaque fine 8-px checker core with a 24-px
//              half-transparent green border (PREMULTIPLIED bytes), so the
//              border resolves against the desktop where M=0.
//  variant 1 — stacking/alpha panel: UNPREMULTIPLIED orange at a=128 with
//              opaque white diagonal stripes; submitted with
//              XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT (fringing check
//              for the SrcAlpha flatten path).
static bool CreateAndFillL2DPanel(XrSessionManager& xr, ID3D11Device* device, ID3D11DeviceContext* context,
                                  uint32_t w, uint32_t h, int variant, L2DPanel& out) {
    (void)device;
    if (w == 0 || h == 0) {
        return false;
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM;
    sci.sampleCount = 1;
    sci.width = w;
    sci.height = h;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &out.swapchain))) {
        LOG_ERROR("Local2D panel: xrCreateSwapchain failed");
        return false;
    }
    out.w = w;
    out.h = h;

    uint32_t n = 0;
    xrEnumerateSwapchainImages(out.swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(out.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader*)imgs.data()))) {
        LOG_ERROR("Local2D panel: xrEnumerateSwapchainImages failed");
        return false;
    }

    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(out.swapchain, &ai, &idx))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(out.swapchain, &wi);

    size_t stride = (size_t)w * 4; // BGRA8
    std::vector<uint8_t> buf(stride * h);
    const uint32_t border = 24;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = buf.data() + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* px = row + (size_t)x * 4; // B,G,R,A
            if (variant == 2) {
                // #491 part 3 backdrop (opaque): coarse cyan/blue checker.
                bool check = (((x / 32) + (y / 32)) & 1) != 0;
                px[0] = check ? 200 : 90; px[1] = check ? 120 : 40; px[2] = 0; px[3] = 255;
            } else if (variant == 3) {
                // #491 part 3 backdrop (semi-transparent ~50%, PREMULTIPLIED) —
                // the desktop shows through it.
                bool check = (((x / 32) + (y / 32)) & 1) != 0;
                if (check) { px[0] = 110; px[1] = 0; px[2] = 110; px[3] = 128; }
                else       { px[0] = 0; px[1] = 90; px[2] = 90; px[3] = 128; }
            } else if (variant == 0) {
                bool inBorder = (x < border || y < border || x >= w - border || y >= h - border);
                if (inBorder) {
                    // Half-transparent green, PREMULTIPLIED bytes.
                    px[0] = 0;   // B
                    px[1] = 128; // G
                    px[2] = 0;   // R
                    px[3] = 128; // A
                } else {
                    // Opaque fine checker (crispness probe).
                    bool check = (((x / 8) + (y / 8)) & 1) != 0;
                    uint8_t v = check ? 235 : 40;
                    px[0] = v;
                    px[1] = v;
                    px[2] = v;
                    px[3] = 255;
                }
            } else {
                bool stripe = (((x + y) / 16) & 1) != 0;
                if (stripe) {
                    // Opaque white stripes.
                    px[0] = 255;
                    px[1] = 255;
                    px[2] = 255;
                    px[3] = 255;
                } else {
                    // UNPREMULTIPLIED orange (B,G,R) at a=128 — channels carry
                    // the full color; the compositor's SrcAlpha blend multiplies.
                    px[0] = 0;   // B
                    px[1] = 165; // G
                    px[2] = 255; // R
                    px[3] = 128; // A
                }
            }
        }
    }
    context->UpdateSubresource(imgs[idx].texture, 0, nullptr, buf.data(), (UINT)stride, 0);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(out.swapchain, &ri);
    return true;
}

// Render a single frame — called from the main loop and from WM_PAINT during
// drag/resize so that rendering never stalls.
static void RenderOneFrame(RenderState& rs) {
    XrSessionManager& xr = *rs.xr;
    D3D11Renderer& renderer = *rs.renderer;

    // Update performance stats
    UpdatePerformanceStats(*rs.perfStats);

    // Update input-based camera movement (clears resetViewRequested internally)
    UpdateCameraMovement(g_inputState, rs.perfStats->deltaTime, rs.xr->displayHeightM);

    // Handle fullscreen toggle (F11)
    if (g_inputState.fullscreenToggleRequested) {
        ToggleFullscreen(rs.hwnd);
        g_inputState.fullscreenToggleRequested = false;
    }

    // Handle rendering mode requests (V=cycle next, 0-8=jump absolute) through
    // the shared ModeSwitch sequencer: it consumes the request flags, eases the
    // stereo disparity (viewParams.ipdFactor) around the switch, and issues
    // xrRequestDisplayRenderingModeDXR on the right frame. The runtime still owns
    // the current mode; the XrEventDataRenderingModeChangedDXR event updates
    // xr.currentModeIndex, which render paths and the HUD read directly.
    XrSessionUpdateModeSwitch(xr, g_inputState, rs.perfStats->deltaTime);

    // #228 Tier 1 smoke test: 'B' fires xrRequestFilePickerDXR and prints
    // the immediate return code. The completion event (success/cancel +
    // path) lands later through PollEvents → XR_TYPE_EVENT_DATA_FILE_PICKER_COMPLETE_DXR.
    //
    // ------------------------------------------------------------------
    // CI / headless smoke-test hook for #228.
    // ------------------------------------------------------------------
    // Setting `DISPLAYXR_AUTOFIRE_FILE_PICKER=1` in this app's
    // environment causes ONE xrRequestFilePickerDXR call to fire
    // automatically ~300 frames after the session enters running
    // state (≈5 s at 60 Hz), without any keypress. The flag is reset
    // after the first fire — there is no per-frame call cost.
    //
    // Why this exists: PostMessage(WM_KEYDOWN, 'B') from an external
    // PowerShell test driver against the cube's hidden-under-workspace
    // HWND has been flaky in practice (the keypress can arrive at the
    // exact instant the IPC pipe transitions to broken on some runs,
    // racing a session-lost teardown). The auto-fire path bypasses
    // Win32 input entirely so headless CI can verify the end-to-end
    // OpenXR contract regardless of input plumbing.
    //
    // Off-by-default, gated on an env var so a normal user double-
    // clicking the cube never gets a spurious picker request.
    {
        static int s_autofire_frames = 0;
        static bool s_autofire_done = false;
        if (!s_autofire_done && xr.sessionRunning) {
            s_autofire_frames++;
            if (s_autofire_frames > 300) {
                const char *autofire = getenv("DISPLAYXR_AUTOFIRE_FILE_PICKER");
                if (autofire && autofire[0] == '1') {
                    LOG_INFO("[#228] AUTO-FIRE: setting filePickerRequestRequested");
                    g_inputState.filePickerRequestRequested = true;
                }
                s_autofire_done = true;
            }
        }
    }
    if (g_inputState.filePickerRequestRequested) {
        g_inputState.filePickerRequestRequested = false;
        if (xr.pfnRequestFilePickerEXT && xr.session != XR_NULL_HANDLE) {
            XrFilePickerInfoDXR info = {XR_TYPE_FILE_PICKER_INFO_DXR};
            info.next = nullptr;
            info.mode = XR_FILE_PICKER_MODE_OPEN_DXR;
            info.flags = XR_FILE_PICKER_FLAG_NONE_DXR;
            strncpy_s(info.title, "Smoke test (#228)", _TRUNCATE);
            strncpy_s(info.defaultPath, "C:\\", _TRUNCATE);
            info.filterCount = 1;
            strncpy_s(info.filters[0].description, "Images", _TRUNCATE);
            strncpy_s(info.filters[0].extensions, "*.png;*.jpg", _TRUNCATE);
            XrAsyncRequestIdDXR rid = XR_NULL_ASYNC_REQUEST_ID_DXR;
            XrResult fr = xr.pfnRequestFilePickerEXT(xr.session, &info, &rid);
            LOG_INFO("[#228] xrRequestFilePickerDXR -> rc=0x%x requestId=%llu",
                (unsigned)fr, (unsigned long long)rid);
            if (XR_SUCCEEDED(fr)) {
                // Feed the shared PollEvents completion state machine
                // (displayxr::common) so the completion event matches the
                // live request instead of logging a stale-id warning.
                xr.filePickerRequestId = rid;
                xr.filePickerInFlight = true;
                xr.filePickerHasResult = false;
            }
        } else {
            LOG_INFO("[#228] xrRequestFilePickerDXR not available (ext missing or PFN NULL)");
        }
    }

    // Handle eye tracking mode toggle (T key)
    if (g_inputState.eyeTrackingModeToggleRequested) {
        g_inputState.eyeTrackingModeToggleRequested = false;
        if (xr.pfnRequestEyeTrackingModeEXT && xr.session != XR_NULL_HANDLE) {
            XrEyeTrackingModeDXR newMode = (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_DXR)
                ? XR_EYE_TRACKING_MODE_MANUAL_DXR : XR_EYE_TRACKING_MODE_MANAGED_DXR;
            XrResult etResult = xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
            LOG_INFO("Eye tracking mode -> %s (%s)",
                newMode == XR_EYE_TRACKING_MODE_MANUAL_DXR ? "MANUAL" : "MANAGED",
                XR_SUCCEEDED(etResult) ? "OK" : "unsupported");
        }
    }

    // #542 'H': hardware-state-only toggle for the current mode.
    {
        // A mode change resets the hardware to the new mode's default —
        // drop the local override note so the next H starts from there.
        if (g_hwOverrideActive && xr.currentModeIndex != g_hwOverrideModeIndex) {
            g_hwOverrideActive = false;
        }
        if (g_hwToggleRequested) {
            g_hwToggleRequested = false;
            if (xr.pfnRequestDisplayModeEXT != nullptr && xr.session != XR_NULL_HANDLE &&
                xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
                bool mode_default_3d = xr.renderingModeDisplay3D[xr.currentModeIndex];
                bool cur_3d = g_hwOverrideActive ? g_hwOverride3D : mode_default_3d;
                bool want_3d = !cur_3d;
                XrResult hres = xr.pfnRequestDisplayModeEXT(xr.session,
                    want_3d ? XR_DISPLAY_MODE_3D_DXR : XR_DISPLAY_MODE_2D_DXR);
                if (XR_SUCCEEDED(hres)) {
                    g_hwOverrideActive = (want_3d != mode_default_3d);
                    g_hwOverride3D = want_3d;
                    g_hwOverrideModeIndex = xr.currentModeIndex;
                }
                LOG_INFO("[#542] H: hardware-only -> %s (mode %u unchanged, rc=0x%x)",
                    want_3d ? "3D" : "2D", xr.currentModeIndex, (unsigned)hres);
            } else {
                LOG_INFO("[#542] H: xrRequestDisplayModeDXR unavailable");
            }
        }
    }

    // Update scene (cube rotation) — speed agent-settable via cube-d3d11__set_spin (#457)
    UpdateScene(renderer, rs.perfStats->deltaTime, xr.spinSpeed);

    // Poll OpenXR events
    PollEvents(xr);

    // Only render if session is running
    if (xr.sessionRunning) {
        XrFrameState frameState;
        if (BeginFrame(xr, frameState)) {
                // #823 actions mode: sync + locate the controllers for this
                // frame's display time; the marker draws read g_actions.
                if (g_handsMode) {
                    UpdateHands(xr.localSpace, frameState.predictedDisplayTime);
                }
                if (g_actionsMode) {
                    UpdateActions(xr.session, xr.localSpace, frameState.predictedDisplayTime);
                }
                uint32_t modeViewCount = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
                    ? xr.renderingModeViewCounts[xr.currentModeIndex] : 2;
                uint32_t tileColumns = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
                    ? xr.renderingModeTileColumns[xr.currentModeIndex] : 2;
                uint32_t tileRows = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
                    ? xr.renderingModeTileRows[xr.currentModeIndex] : 1;
                bool monoMode = (xr.renderingModeCount > 0 && !xr.renderingModeDisplay3D[xr.currentModeIndex]);
                if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
                    xr.recommendedViewScaleX = xr.renderingModeScaleX[xr.currentModeIndex];
                    xr.recommendedViewScaleY = xr.renderingModeScaleY[xr.currentModeIndex];
                }
                int eyeCount = monoMode ? 1 : (int)modeViewCount;
                // Layered swapchains hold arraySize slices, so array mode can
                // only submit that many views — cap to it (a >2-view mode is
                // driven as its first 2 views).
                if (ArrayLayoutEnabled() && eyeCount > (int)xr.swapchain.arraySize) {
                    eyeCount = (int)xr.swapchain.arraySize;
                }
                std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
            bool hudSubmitted = false;

            if (frameState.shouldRender) {
                if (LocateViews(xr, frameState.predictedDisplayTime,
                    g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                    g_inputState.yaw, g_inputState.pitch,
                    g_inputState.viewParams)) {

                    // Get raw view poses (pre-player-transform) for projection views.
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 8;
                    XrView rawViews[8];
                    for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};

                    // XR_DXR_view_rig raw-channel verification (#396 W7): chain
                    // XrViewDisplayRawDXR so the runtime reports the DP's full
                    // per-view eye set. Logged once below to confirm
                    // eyeCountOutput == viewCount with sane positions (esp. in
                    // >2-view modes, where the DP — not the runtime — fills N).
                    XrViewDisplayRawDXR rawProbe = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
                    if (g_hasViewRigExt) {
                        viewState.next = &rawProbe;
                    }

                    // XR_DXR_view_rig (#396 W7): drive the runtime rig matching
                    // the app's current mode (C selects the rig) with the app's
                    // tunables — the runtime owns the window/canvas resolve and
                    // the Kooima math, and returns render-ready XrView{pose, fov}.
                    // Per-locate semantics: the rig must be chained on every
                    // consume locate.
                    const bool useAppProjection =
                        xr.hasDisplayInfoExt && xr.displayWidthM > 0.0f && g_hasViewRigExt;
                    const bool rigCamera = useAppProjection && g_inputState.cameraMode;
                    XrCameraRigDXR cameraRig = {XR_TYPE_CAMERA_RIG_DXR};
                    XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
                    XrPosef rigPose = {{0, 0, 0, 1}, {0, 0, 0}};
                    if (useAppProjection) {
                        XMVECTOR rigOri = XMQuaternionRotationRollPitchYaw(
                            g_inputState.pitch, g_inputState.yaw, 0);
                        XMFLOAT4 rq;
                        XMStoreFloat4(&rq, rigOri);
                        rigPose.orientation = {rq.x, rq.y, rq.z, rq.w};
                        rigPose.position = {g_inputState.cameraPosX, g_inputState.cameraPosY,
                                            g_inputState.cameraPosZ};
                        if (rigCamera) {
                            cameraRig.pose = rigPose;
                            cameraRig.ipdFactor = g_inputState.viewParams.ipdFactor;
                            cameraRig.parallaxFactor = g_inputState.viewParams.parallaxFactor;
                            cameraRig.convergenceDiopters = g_inputState.viewParams.invConvergenceDistance;
                            cameraRig.verticalFov =
                                2.0f * atanf(CAMERA_HALF_TAN_VFOV / g_inputState.viewParams.zoomFactor);
                            // metersToVirtual carries the eye scale the C-toggle
                            // converter derived from the display rig, so the
                            // camera rig reproduces the display rig exactly.
                            cameraRig.metersToVirtual = g_inputState.viewParams.cameraM2v;
                            locateInfo.next = &cameraRig;
                        } else {
                            displayRig.pose = rigPose;
                            displayRig.virtualDisplayHeight =
                                g_inputState.viewParams.virtualDisplayHeight / g_inputState.viewParams.scaleFactor;
                            displayRig.ipdFactor = g_inputState.viewParams.ipdFactor;
                            displayRig.parallaxFactor = g_inputState.viewParams.parallaxFactor;
                            displayRig.perspectiveFactor = g_inputState.viewParams.perspectiveFactor;
                            locateInfo.next = &displayRig;
                        }
                    }

                    xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCount, rawViews);

                    // Capture the runtime's resolved CANVAS size (the window
                    // client area in meters) — this is the physical_height_m the
                    // Kooima/rig math runs on, which the C-toggle converter must
                    // match. Fullscreen → canvas == display; windowed → smaller.
                    if (g_hasViewRigExt && rawProbe.canvasSizeMeters.height > 0.0f) {
                        g_inputState.canvasWidthM = rawProbe.canvasSizeMeters.width;
                        g_inputState.canvasHeightM = rawProbe.canvasSizeMeters.height;
                    }

                    // XR_DXR_view_rig raw-channel verification (#396 W7): one-shot
                    // proof the raw channel reports the DP's full per-view set.
                    if (g_hasViewRigExt) {
                        static int rawLogged = 0;
                        if (rawLogged < 3) {
                            rawLogged++;
                            LOG_INFO("view-rig RAW: eyeCountOutput=%u viewCount=%u isTracking=%d",
                                     rawProbe.eyeCountOutput, viewCount, (int)rawProbe.isTracking);
                            for (uint32_t i = 0; i < rawProbe.eyeCountOutput && i < 8; i++) {
                                LOG_INFO("  rawEyes[%u]=(%.4f,%.4f,%.4f)", i, rawProbe.rawEyes[i].x,
                                         rawProbe.rawEyes[i].y, rawProbe.rawEyes[i].z);
                            }
                        }
                    }

                    // Max per-tile capacity from swapchain
                    uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
                    uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;

                    std::vector<RigView> stereoViews(eyeCount);
                    if (useAppProjection) {
                        // Consume the runtime's render-ready XrView{pose, fov}.
                        // Only clip policy (near/far + the GL→[0,1] depth
                        // remap) stays app-side, by design (fov is
                        // clip-independent). Camera rig: same absolute clip as
                        // the old app-side camera path. Display rig: the
                        // ZDP-anchored clip (near = ez - vH, far = ez +
                        // 1000·vH; ez = eye distance to the virtual display
                        // plane = rig-local z of the view pose).
                        const float rigVH =
                            g_inputState.viewParams.virtualDisplayHeight / g_inputState.viewParams.scaleFactor;
                        for (int i = 0; i < eyeCount; i++) {
                            const XrView& v = rawViews[(i < (int)viewCount) ? i : 0];
                            ViewMatrixFromXrPose(v.pose, stereoViews[i].view_matrix);
                            float nearZ = 0.01f, farZ = 100.0f;
                            if (!rigCamera) {
                                float ez = RigLocalEyeZ(rigPose, v.pose.position);
                                nearZ = (ez - rigVH > 0.001f) ? (ez - rigVH) : 0.001f;
                                farZ = ez + 1000.0f * rigVH;
                            }
                            ProjectionFromXrFov(v.fov, nearZ, farZ, stereoViews[i].projection_matrix);
                            convert_projection_gl_to_zero_to_one(stereoViews[i].projection_matrix);
                            stereoViews[i].fov = v.fov;
                        }
                    }

                    // [Commented out — will be reused for 3D-positioned HUD later]
                    // ConvergencePlane convPlane = LocateConvergencePlane(rawViews);

                    // Render HUD to window-space layer swapchain (once per frame, before eye loop)
                    if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty() && rs.hudOk) {
                        uint32_t hudImageIndex;
                        if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                            std::wstring sessionText(xr.systemName, xr.systemName + strlen(xr.systemName));
                            sessionText += L"\nSession: ";
                            sessionText += FormatSessionState((int)xr.sessionState);
                            std::wstring modeText = xr.hasWin32WindowBindingExt ?
                                L"XR_DXR_win32_window_binding: ACTIVE (D3D11)" :
                                L"XR_DXR_win32_window_binding: NOT AVAILABLE (D3D11)";
                            modeText += g_inputState.cameraMode ?
                                L"\nKooima: Camera-Centric [C=Toggle]" :
                                L"\nKooima: Display-Centric [C=Toggle]";
                            modeText += g_hasViewRigExt ?
                                (g_inputState.cameraMode ?
                                    L"\nView rig: RUNTIME camera rig (XR_DXR_view_rig)" :
                                    L"\nView rig: RUNTIME display rig (XR_DXR_view_rig)") :
                                L"\nView rig: unavailable (legacy views)";

                            // Dynamic render dims matching the actual viewport computation
                            bool dispMonoMode = monoMode;
                            uint32_t dispRenderW, dispRenderH;
                            if (dispMonoMode) {
                                dispRenderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                                dispRenderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                                if (dispRenderW > xr.swapchain.width) dispRenderW = xr.swapchain.width;
                                if (dispRenderH > xr.swapchain.height) dispRenderH = xr.swapchain.height;
                            } else {
                                dispRenderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                                dispRenderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                                if (dispRenderW > maxTileW) dispRenderW = maxTileW;
                                if (dispRenderH > maxTileH) dispRenderH = maxTileH;
                            }
                            std::wstring perfText = FormatPerformanceInfo(rs.perfStats->fps, rs.perfStats->frameTimeMs,
                                dispRenderW, dispRenderH,
                                g_windowWidth, g_windowHeight);
                            std::wstring dispText = FormatDisplayInfo(xr.displayWidthM, xr.displayHeightM,
                                xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ);
                            dispText += L"\n" + FormatScaleInfo(xr.recommendedViewScaleX, xr.recommendedViewScaleY);
                            dispText += L"\n" + FormatMode(xr.currentModeIndex, xr.pfnRequestDisplayRenderingModeEXT != nullptr,
                                (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) ? xr.renderingModeNames[xr.currentModeIndex] : nullptr,
                                xr.renderingModeCount,
                                xr.renderingModeCount > 0 ? xr.renderingModeDisplay3D[xr.currentModeIndex] : true,
                                xr.renderingModeCount > 0 ? xr.renderingModeIsRequestable[xr.currentModeIndex] : true);
                            if (g_hwOverrideActive) {
                                dispText += g_hwOverride3D
                                    ? L"\nHW OVERRIDE [H]: 3D (mode default 2D)"
                                    : L"\nHW OVERRIDE [H]: 2D (mode default 3D)";
                            }
                            std::wstring eyeText = FormatEyeTrackingInfo(
                                xr.eyePositions, (uint32_t)eyeCount,
                                xr.eyeTrackingActive, xr.isEyeTracking,
                                xr.activeEyeTrackingMode, xr.supportedEyeTrackingModes);

                            float fwdX = -sinf(g_inputState.yaw) * cosf(g_inputState.pitch);
                            float fwdY =  sinf(g_inputState.pitch);
                            float fwdZ = -cosf(g_inputState.yaw) * cosf(g_inputState.pitch);
                            std::wstring cameraText = FormatCameraInfo(
                                g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                                fwdX, fwdY, fwdZ, g_inputState.cameraMode);
                            float dispP1 = g_inputState.cameraMode ? g_inputState.viewParams.invConvergenceDistance : g_inputState.viewParams.perspectiveFactor;
                            float dispP2 = g_inputState.cameraMode ? g_inputState.viewParams.zoomFactor : g_inputState.viewParams.scaleFactor;
                            std::wstring stereoText = FormatViewParams(
                                g_inputState.viewParams.ipdFactor, g_inputState.viewParams.parallaxFactor,
                                dispP1, dispP2, g_inputState.cameraMode);
                            {
                                wchar_t vhBuf[64];
                                if (g_inputState.cameraMode) {
                                    float tanHFOV = CAMERA_HALF_TAN_VFOV / g_inputState.viewParams.zoomFactor;
                                    swprintf(vhBuf, 64, L"\ntanHFOV: %.3f  m2v: %.3f", tanHFOV, g_inputState.viewParams.cameraM2v);
                                } else {
                                    float hudM2v = 1.0f;
                                    if (g_inputState.viewParams.virtualDisplayHeight > 0.0f && xr.displayHeightM > 0.0f)
                                        hudM2v = g_inputState.viewParams.virtualDisplayHeight / xr.displayHeightM;
                                    swprintf(vhBuf, 64, L"\nvHeight: %.3f  m2v: %.3f",
                                        g_inputState.viewParams.virtualDisplayHeight, hudM2v);
                                }
                                stereoText += vhBuf;
                            }
                            std::wstring helpText = FormatHelpText(xr.pfnRequestDisplayRenderingModeEXT != nullptr, g_inputState.cameraMode, xr.renderingModeCount);

                            uint32_t srcRowPitch = 0;
                            const void* pixels = RenderHudAndMap(*rs.hudRenderer, &srcRowPitch,
                                sessionText, modeText, perfText, dispText, eyeText,
                                cameraText, stereoText, helpText);
                            if (pixels) {
                                ID3D11Texture2D* hudTexture = (*rs.hudSwapchainImages)[hudImageIndex].texture;
                                D3D11_BOX box = {0, 0, 0, xr.hudSwapchain.width, xr.hudSwapchain.height, 1};
                                renderer.context->UpdateSubresource(hudTexture, 0, &box, pixels, srcRowPitch, 0);
                                UnmapHud(*rs.hudRenderer);
                            }

                            ReleaseHudSwapchainImage(xr);
                            hudSubmitted = true;
                        }
                    }

                    // For mono: compute center eye position and projection
                    XMMATRIX monoViewMatrix, monoProjMatrix;
                    XrFovf monoFov = {};
                    XrPosef monoPose = rawViews[0].pose;
                    if (monoMode) {
                        // Center eye = average of all view positions
                        monoPose.position = {0.0f, 0.0f, 0.0f};
                        int cnt = (int)viewCount;
                        if (cnt < 1) cnt = 1;
                        for (int v = 0; v < cnt; v++) {
                            monoPose.position.x += rawViews[v].pose.position.x;
                            monoPose.position.y += rawViews[v].pose.position.y;
                            monoPose.position.z += rawViews[v].pose.position.z;
                        }
                        monoPose.position.x /= cnt;
                        monoPose.position.y /= cnt;
                        monoPose.position.z /= cnt;

                        // When useAppProjection, mono view+proj come from stereoViews[0]
                        // (the runtime centers the rig view poses in 2D mode).
                        // Only need fallback when !useAppProjection.
                        if (!useAppProjection) {
                            monoProjMatrix = xr.projMatrices[0];  // Close enough for 2D
                            monoFov = rawViews[0].fov;

                            // Build center-eye view matrix from scratch (same as LocateViews)
                            XMVECTOR centerLocalPos = XMVectorSet(
                                monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                            XMVECTOR localOri = XMVectorSet(
                                rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);

                            float monoM2vView = 1.0f;
                            if (g_inputState.viewParams.virtualDisplayHeight > 0.0f && xr.displayHeightM > 0.0f)
                                monoM2vView = g_inputState.viewParams.virtualDisplayHeight / xr.displayHeightM;
                            float eyeScale = g_inputState.viewParams.perspectiveFactor * monoM2vView / g_inputState.viewParams.scaleFactor;
                            XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(
                                g_inputState.pitch, g_inputState.yaw, 0);
                            XMVECTOR playerPos = XMVectorSet(
                                g_inputState.cameraPosX, g_inputState.cameraPosY,
                                g_inputState.cameraPosZ, 0.0f);

                            XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                            XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);

                            XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                            XMFLOAT3 wp;
                            XMStoreFloat3(&wp, worldPos);
                            monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                        }
                    }

                    // Single swapchain: acquire once, render all views, release once
                    uint32_t imageIndex;
                    if (AcquireSwapchainImage(xr, imageIndex)) {
                        ID3D11Texture2D* swapchainTexture = (*rs.swapchainImages)[imageIndex].texture;

                        // Transparent mode (DISPLAYXR_TRANSPARENT_BG=1): clear to
                        // RGBA(0,0,0,0) so the Leia DP's compose-under-bg pass
                        // shows the desktop wherever the cube didn't draw.
                        float clearColor[4];
                        if (TransparentBackgroundEnabled()) {
                            clearColor[0] = 0.0f; clearColor[1] = 0.0f;
                            clearColor[2] = 0.0f; clearColor[3] = 0.0f;
                        } else {
                            clearColor[0] = 0.05f; clearColor[1] = 0.05f;
                            clearColor[2] = 0.25f; clearColor[3] = 1.0f;
                        }

                        // TILED: one shared RTV over the whole atlas image, cleared
                        // once (each view is drawn at its tile viewport). ARRAY: a
                        // per-slice RTV is created + cleared inside the eye loop.
                        const bool arrayLayout = ArrayLayoutEnabled();
                        ID3D11RenderTargetView* rtv = nullptr;
                        if (!arrayLayout) {
                            CreateRenderTargetView(renderer, swapchainTexture,
                                static_cast<DXGI_FORMAT>(xr.swapchain.format), &rtv);
                            renderer.context->ClearRenderTargetView(rtv, clearColor);
                        }
                        renderer.context->ClearDepthStencilView(rs.depthDSV.Get(),
                            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                        // Dynamic render dims based on window size, clamped to swapchain capacity
                        uint32_t renderW, renderH;
                        if (monoMode) {
                            // 2D: the single view fills the full content region —
                            // window × the active mode's view scale (#575; same
                            // window×scale recipe the 3D branch uses). Dropping the
                            // scale here left the view under-filling the 2D tile →
                            // content shifted left + right eye black.
                            renderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                            renderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                            if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                            if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
                        } else {
                            renderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                            renderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                            // ARRAY: each slice is a full per-view image, so the
                            // cap is the whole swapchain image, not a sub-tile.
                            uint32_t capW = arrayLayout ? xr.swapchain.width : maxTileW;
                            uint32_t capH = arrayLayout ? xr.swapchain.height : maxTileH;
                            if (renderW > capW) renderW = capW;
                            if (renderH > capH) renderH = capH;
                        }

                        for (int eye = 0; eye < eyeCount; eye++) {
                            uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                            uint32_t tileY = monoMode ? 0 : (eye / tileColumns);

                            // ARRAY: render into array slice `eye` via a per-slice
                            // TEXTURE2DARRAY RTV at a full (0,0) viewport. TILED:
                            // draw into this view's tile of the shared RTV.
                            ID3D11RenderTargetView* viewRtv = rtv;
                            ID3D11RenderTargetView* sliceRtv = nullptr;
                            if (arrayLayout) {
                                D3D11_RENDER_TARGET_VIEW_DESC rd = {};
                                rd.Format = static_cast<DXGI_FORMAT>(xr.swapchain.format);
                                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                                rd.Texture2DArray.MipSlice = 0;
                                rd.Texture2DArray.FirstArraySlice = (UINT)eye;
                                rd.Texture2DArray.ArraySize = 1;
                                if (FAILED(renderer.device->CreateRenderTargetView(
                                        swapchainTexture, &rd, &sliceRtv))) {
                                    LOG_ERROR("array RTV creation failed for slice %d", eye);
                                    continue;
                                }
                                renderer.context->ClearRenderTargetView(sliceRtv, clearColor);
                                // Each slice is rendered full-viewport into the SAME
                                // shared depth buffer, so depth MUST be cleared per
                                // slice — otherwise slice 1 z-tests against slice 0's
                                // depth and the other eye's cube punches a shadow
                                // through (the #613 gotcha). TILED views don't need
                                // this: their tile viewports occupy disjoint depth
                                // regions, cleared once before the loop.
                                renderer.context->ClearDepthStencilView(rs.depthDSV.Get(),
                                    D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
                                viewRtv = sliceRtv;
                            }

                            D3D11_VIEWPORT vp = {};
                            vp.TopLeftX = arrayLayout ? 0.0f : (FLOAT)(tileX * renderW);
                            vp.TopLeftY = arrayLayout ? 0.0f : (FLOAT)(tileY * renderH);
                            vp.Width = (FLOAT)renderW;
                            vp.Height = (FLOAT)renderH;
                            vp.MaxDepth = 1.0f;
                            renderer.context->RSSetViewports(1, &vp);

                            XMMATRIX viewMatrix, projMatrix;
                            if (useAppProjection) {
                                int vi = monoMode ? 0 : eye;
                                viewMatrix = ColumnMajorToXMMatrix(stereoViews[vi].view_matrix);
                                projMatrix = ColumnMajorToXMMatrix(stereoViews[vi].projection_matrix);
                            } else if (monoMode) {
                                viewMatrix = monoViewMatrix;
                                projMatrix = monoProjMatrix;
                            } else {
                                int vi = (eye < (int)viewCount) ? eye : 0;
                                viewMatrix = xr.viewMatrices[vi];
                                projMatrix = xr.projMatrices[vi];
                            }

                            RenderScene(renderer, viewRtv, rs.depthDSV.Get(),
                                renderW, renderH,
                                viewMatrix, projMatrix,
                                useAppProjection ? 1.0f : g_inputState.viewParams.scaleFactor,
                                0.03f);

                            // #823 actions mode: draw a small marker cube at
                            // each tracked controller's grip pose (bigger
                            // while select is held). Inherits this eye's
                            // viewport; same zoom convention as RenderScene.
                            if (g_actionsMode) {
                                float zoomS = useAppProjection ? 1.0f : g_inputState.viewParams.scaleFactor;
                                XMMATRIX zoomM = XMMatrixScaling(zoomS, zoomS, 1.0f);
                                for (int hnd = 0; hnd < 2; hnd++) {
                                    if (!g_actions.poseValid[hnd]) {
                                        continue;
                                    }
                                    const XrPosef &p = g_actions.pose[hnd];
                                    float s = g_actions.selectPressed[hnd] ? 0.035f : 0.02f;
                                    XMVECTOR q = XMVectorSet(p.orientation.x, p.orientation.y,
                                                             p.orientation.z, p.orientation.w);
                                    XMMATRIX world = XMMatrixScaling(s, s, s) *
                                                     XMMatrixRotationQuaternion(q) *
                                                     XMMatrixTranslation(p.position.x, p.position.y,
                                                                         p.position.z);
                                    XMMATRIX wvp = world * viewMatrix * zoomM * projMatrix;
                                    XMFLOAT4X4 wvpOut;
                                    XMStoreFloat4x4(&wvpOut, wvp);
                                    RenderCubeWithMVP(renderer, viewRtv, rs.depthDSV.Get(),
                                                      &wvpOut.m[0][0]);
                                }
                            }

                            // #825 hands mode: draw a tiny marker cube at each
                            // tracked hand joint (26/hand), sized by the
                            // runtime-reported joint radius. Same zoom
                            // convention as the controller markers above.
                            if (g_handsMode) {
                                float zoomS = useAppProjection ? 1.0f : g_inputState.viewParams.scaleFactor;
                                XMMATRIX zoomM = XMMatrixScaling(zoomS, zoomS, 1.0f);
                                const XrSpaceLocationFlags needed =
                                    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                                for (int hnd = 0; hnd < 2; hnd++) {
                                    if (!g_hands.active[hnd]) {
                                        continue;
                                    }
                                    for (uint32_t j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
                                        const XrHandJointLocationEXT &jl = g_hands.joints[hnd][j];
                                        if ((jl.locationFlags & needed) != needed) {
                                            continue;
                                        }
                                        float s = jl.radius > 0.001f ? jl.radius : 0.008f;
                                        XMVECTOR q = XMVectorSet(jl.pose.orientation.x, jl.pose.orientation.y,
                                                                 jl.pose.orientation.z, jl.pose.orientation.w);
                                        XMMATRIX world = XMMatrixScaling(s, s, s) *
                                                         XMMatrixRotationQuaternion(q) *
                                                         XMMatrixTranslation(jl.pose.position.x, jl.pose.position.y,
                                                                             jl.pose.position.z);
                                        XMMATRIX wvp = world * viewMatrix * zoomM * projMatrix;
                                        XMFLOAT4X4 wvpOut;
                                        XMStoreFloat4x4(&wvpOut, wvp);
                                        RenderCubeWithMVP(renderer, viewRtv, rs.depthDSV.Get(),
                                                          &wvpOut.m[0][0]);
                                    }
                                }
                            }

                            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                            projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                            // ARRAY: full image at slice `eye`. TILED: this view's tile.
                            projectionViews[eye].subImage.imageRect.offset = {
                                arrayLayout ? 0 : (int32_t)(tileX * renderW),
                                arrayLayout ? 0 : (int32_t)(tileY * renderH)
                            };
                            projectionViews[eye].subImage.imageRect.extent = {
                                (int32_t)renderW,
                                (int32_t)renderH
                            };
                            projectionViews[eye].subImage.imageArrayIndex = arrayLayout ? (uint32_t)eye : 0;

                            int safeIdx = (eye < (int)viewCount) ? eye : 0;
                            projectionViews[eye].pose = monoMode ? monoPose : rawViews[safeIdx].pose;
                            projectionViews[eye].fov = useAppProjection ?
                                stereoViews[monoMode ? 0 : eye].fov :
                                (monoMode ? monoFov : rawViews[safeIdx].fov);

                            if (sliceRtv) sliceRtv->Release();
                        }

                        if (rtv) rtv->Release();

                        // 'I' key: snapshot the multi-view atlas to a PNG via the
                        // runtime (XR_DXR_atlas_capture). Skipped for mono (1×1).
                        if (g_inputState.captureAtlasRequested) {
                            g_inputState.captureAtlasRequested = false;
                            dxr_capture::RequestRuntimeAtlasCapture(
                                xr, APP_NAME, tileColumns, tileRows, rs.hwnd);
                        }

                        ReleaseSwapchainImage(xr);
                    }
                }
            }

            // #439 cases 2/3/4 activation: create + fill the panel swapchain(s)
            // (+ the explicit Tier-2 island mask for case 2) a few frames in,
            // once the session is running and window dims are settled.
            if (g_l2dPanel && !g_l2dActive && g_l2dFrameCounter >= g_l2dActivationFrame) {
                static bool attempted = false;
                if (!attempted) {
                    attempted = true;
                    uint32_t winW = g_windowWidth;
                    uint32_t winH = g_windowHeight;
                    uint32_t pw = winW * 3 / 8;
                    uint32_t ph = winH * 5 / 16;
                    // #491 validation aid: DXR_LOCAL2D_OVERCUBE centers the panel
                    // over the cube + uses the diagonal-stripes variant (clearest
                    // read of glass-over-3D).
                    const char* oc = getenv("DXR_LOCAL2D_OVERCUBE");
                    bool overCube = (oc && *oc == '1');
                    int p1variant = overCube ? 1 : 0;
                    if (overCube) {
                        g_panel1Rect.offset = {(int32_t)(winW / 2 - pw / 2), (int32_t)(winH / 2 - ph / 2)};
                    } else {
                        g_panel1Rect.offset = {(int32_t)(winW / 16), (int32_t)(winH * 9 / 16)};
                    }
                    g_panel1Rect.extent = {(int32_t)pw, (int32_t)ph};
                    bool ok = CreateAndFillL2DPanel(xr, renderer.device.Get(), renderer.context.Get(), pw, ph,
                                                    p1variant, g_panel1);

                    // #491 part 3 — large backdrop submitted BEFORE the projection
                    // (a 2D-under layer): the flat 2D plane the cube floats in front of.
                    if (ok && g_l2dBackdropVariant != 0) {
                        uint32_t bw = winW * 3 / 4;
                        uint32_t bh = winH * 3 / 4;
                        g_backdropRect.offset = {(int32_t)(winW / 2 - bw / 2), (int32_t)(winH / 2 - bh / 2)};
                        g_backdropRect.extent = {(int32_t)bw, (int32_t)bh};
                        ok = CreateAndFillL2DPanel(xr, renderer.device.Get(), renderer.context.Get(), bw, bh,
                                                   g_l2dBackdropVariant, g_backdrop);
                    }

                    if (ok && g_l2dPanel2) {
                        // Overlaps panel 1's top-right quadrant — list-order
                        // stacking check (panel 2 is later in the list = on top).
                        g_panel2Rect.offset = {g_panel1Rect.offset.x + (int32_t)(pw / 2),
                                               g_panel1Rect.offset.y - (int32_t)(ph / 4)};
                        g_panel2Rect.extent = {(int32_t)pw, (int32_t)ph};
                        ok = CreateAndFillL2DPanel(xr, renderer.device.Get(), renderer.context.Get(), pw, ph,
                                                   1, g_panel2);
                    }

                    if (ok && g_l2dMask && g_zone.available && g_zone.pfnCreate && g_zone.pfnSetRects &&
                        g_zone.pfnSubmit) {
                        XrLocal3DZoneMaskCreateInfoDXR mci = {
                            (XrStructureType)XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_DXR};
                        mci.maskWidth = 0; // runtime picks the window backing size
                        mci.maskHeight = 0;
                        mci.maskWidth = g_l2dMaskW;
                        mci.maskHeight = g_l2dMaskH;
                        ok = XR_SUCCEEDED(g_zone.pfnCreate(xr.session, &mci, &g_zone.mask));
                        if (ok) {
                            // Two 3D islands: a large center-right one and a
                            // small top-left one. Everything else is 2D — the
                            // panel where it covers, desktop (final.a = 0)
                            // where nothing does.
                            XrRect2Di islands[2];
                            islands[0].offset = {(int32_t)(winW * 7 / 16), (int32_t)(winH / 4)};
                            islands[0].extent = {(int32_t)(winW * 7 / 16), (int32_t)(winH / 2)};
                            islands[1].offset = {(int32_t)(winW / 16), (int32_t)(winH / 16)};
                            islands[1].extent = {(int32_t)(winW / 4), (int32_t)(winH / 4)};
                            ok = XR_SUCCEEDED(
                                g_zone.pfnSetRects(g_zone.mask, 2, islands));
                            /*
                             * #918 review F1 — Tier 3. Acquiring the render
                             * target is what latches `app_authored` in the
                             * runtime, and a stroke drawn here cannot be
                             * reproduced by re-running the rect raster on
                             * another device, so this is the mask that
                             * genuinely has to cross the adapter boundary. One
                             * stroke: a 3D bar down the left edge, outside both
                             * rect islands, so a capture can tell the app's
                             * pixels from the rects'.
                             */
                            if (ok && g_l2dMaskTier3 && g_zone.pfnAcquire) {
                              XrLocal3DZoneRenderTargetD3D11DXR rtb = {
                                  (XrStructureType)
                                      XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_DXR};
                              XrResult ar =
                                  g_zone.pfnAcquire(g_zone.mask, &rtb);
                              if (XR_SUCCEEDED(ar) &&
                                  rtb.renderTargetView != nullptr) {
                                ID3D11DeviceContext1 *ctx1 = nullptr;
                                if (SUCCEEDED(renderer.context->QueryInterface(
                                        __uuidof(ID3D11DeviceContext1),
                                        (void **)&ctx1)) &&
                                    ctx1 != nullptr) {
                                  const float m3d[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                                  D3D11_RECT bar = {(LONG)(rtb.width / 16),
                                                    (LONG)(rtb.height / 3),
                                                    (LONG)(rtb.width / 8),
                                                    (LONG)(rtb.height * 2 / 3)};
                                  ctx1->ClearView((ID3D11RenderTargetView *)
                                                      rtb.renderTargetView,
                                                  m3d, &bar, 1);
                                  ctx1->Release();
                                  LOG_INFO("[mask] Tier-3 stroke drawn into "
                                           "the %ux%u authored "
                                           "mask (bar %ld,%ld %ldx%ld)",
                                           rtb.width, rtb.height, bar.left,
                                           bar.top, bar.right - bar.left,
                                           bar.bottom - bar.top);
                                }
                              } else {
                                LOG_ERROR("[mask] "
                                          "xrAcquireLocal3DZoneRenderTargetDXR "
                                          "failed (0x%x)",
                                          (unsigned)ar);
                              }
                            }
                            ok = ok &&
                                 XR_SUCCEEDED(g_zone.pfnSubmit(g_zone.mask));
                        }
                    }

                    if (ok) {
                        g_l2dActive = true;
                        LOG_INFO("Local2D panels active: panel1 %d,%d %ux%u%s%s",
                                 g_panel1Rect.offset.x, g_panel1Rect.offset.y, pw, ph,
                                 g_l2dPanel2 ? " + panel2 (unpremultiplied, overlapping)" : "",
                                 g_l2dMask ? " + explicit Tier-2 island mask" : " (implicit mask)");
                    } else {
                        LOG_ERROR("Local2D panel activation failed");
                    }
                }
            }

            // Submit frame. #439 cases 2/3/4: when Local2D panels are active,
            // build the layer list manually (projection + panels in list order)
            // and submit raw — the shared EndFrame helpers don't carry the
            // Local2D layer type. Otherwise use the normal helper paths.
            if (!frameState.shouldRender) {
                // Not visible: xrEndFrame is still required (keeps the frame loop
                // paced) but with NO layers — the projection views were never
                // located this frame and would be rejected (POSE_INVALID).
                XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                endInfo.displayTime = frameState.predictedDisplayTime;
                endInfo.environmentBlendMode = xr.runtimeSupportsAlphaBlend
                    ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                    : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                endInfo.layerCount = 0;
                endInfo.layers = nullptr;
                xrEndFrame(xr.session, &endInfo);
            } else if (g_l2dActive && g_panel1.swapchain != XR_NULL_HANDLE) {
                XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                projLayer.space = xr.localSpace;
                projLayer.viewCount = (uint32_t)eyeCount;
                projLayer.views = projectionViews.data();

                XrCompositionLayerLocal2DDXR panel1Layer = {
                    (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR};
                XrCompositionLayerLocal2DDXR panel2Layer = {
                    (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR};
                XrCompositionLayerLocal2DDXR backdropLayer = {
                    (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR};
                const XrCompositionLayerBaseHeader* layers[4] = {nullptr, nullptr, nullptr, nullptr};
                uint32_t layerCount = 0;

                // #491 part 3 — backdrop BEFORE the projection (a 2D-under layer).
                if (g_l2dBackdropVariant != 0 && g_backdrop.swapchain != XR_NULL_HANDLE) {
                    backdropLayer.layerFlags = 0; // premultiplied bytes
                    backdropLayer.subImage.swapchain = g_backdrop.swapchain;
                    backdropLayer.subImage.imageRect.offset = {0, 0};
                    backdropLayer.subImage.imageRect.extent = {(int32_t)g_backdrop.w, (int32_t)g_backdrop.h};
                    backdropLayer.subImage.imageArrayIndex = 0;
                    backdropLayer.rect = g_backdropRect;
                    layers[layerCount++] = (XrCompositionLayerBaseHeader*)&backdropLayer;
                }

                layers[layerCount++] = (XrCompositionLayerBaseHeader*)&projLayer;

                panel1Layer.layerFlags = 0; // premultiplied bytes
                panel1Layer.subImage.swapchain = g_panel1.swapchain;
                panel1Layer.subImage.imageRect.offset = {0, 0};
                panel1Layer.subImage.imageRect.extent = {(int32_t)g_panel1.w, (int32_t)g_panel1.h};
                panel1Layer.subImage.imageArrayIndex = 0;
                panel1Layer.rect = g_panel1Rect;
                layers[layerCount++] = (XrCompositionLayerBaseHeader*)&panel1Layer;

                if (g_l2dPanel2 && g_panel2.swapchain != XR_NULL_HANDLE) {
                    panel2Layer.layerFlags = XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                    panel2Layer.subImage.swapchain = g_panel2.swapchain;
                    panel2Layer.subImage.imageRect.offset = {0, 0};
                    panel2Layer.subImage.imageRect.extent = {(int32_t)g_panel2.w, (int32_t)g_panel2.h};
                    panel2Layer.subImage.imageArrayIndex = 0;
                    panel2Layer.rect = g_panel2Rect;
                    layers[layerCount++] = (XrCompositionLayerBaseHeader*)&panel2Layer;
                }

                // Honor DISPLAYXR_TRANSPARENT_BG the same way the shared
                // EndFrame path does: the pre-activation frames ran
                // SelectEnvBlendMode, so runtimeSupportsAlphaBlend is already
                // resolved. ALPHA_BLEND is what makes the desktop show through
                // where the mask is 2D + the layer alpha < 1 (the §4.2 output
                // rule + the panel's half-transparent border).
                XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                endInfo.displayTime = frameState.predictedDisplayTime;
                endInfo.environmentBlendMode = xr.runtimeSupportsAlphaBlend
                    ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                    : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                endInfo.layerCount = layerCount;
                endInfo.layers = layers;
                xrEndFrame(xr.session, &endInfo);
            } else if (hudSubmitted) {
                float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
                float windowAR = (g_windowWidth > 0 && g_windowHeight > 0) ? (float)g_windowWidth / (float)g_windowHeight : 1.0f;
                float fracW = HUD_WIDTH_FRACTION;
                float fracH = fracW * windowAR / hudAR;
                if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                EndFrameWithWindowSpaceHud(xr, frameState.predictedDisplayTime, projectionViews.data(),
                    0.0f, 0.0f, fracW, fracH, 0.0f, eyeCount);
            } else {
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), eyeCount);
            }
            g_l2dFrameCounter++;
        }
    } else {
        Sleep(100);
    }
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    // #823 actions mode: `--actions` on the command line or DXR_ACTIONS=1.
    if (lpCmdLine != nullptr && strstr(lpCmdLine, "--actions") != nullptr) {
        g_actionsMode = true;
    }
    {
        char buf[8] = {0};
        DWORD n = GetEnvironmentVariableA("DXR_ACTIONS", buf, sizeof(buf));
        if (n > 0 && buf[0] != '0') {
            g_actionsMode = true;
        }
    }
    // #825 hands mode: `--hands` on the command line or DXR_HANDS=1.
    if (lpCmdLine != nullptr && strstr(lpCmdLine, "--hands") != nullptr) {
        g_handsMode = true;
    }
    {
        char buf[8] = {0};
        DWORD n = GetEnvironmentVariableA("DXR_HANDS", buf, sizeof(buf));
        if (n > 0 && buf[0] != '0') {
            g_handsMode = true;
        }
    }

    // Initialize logging
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Ext Application ===");
    LOG_INFO("OpenXR with XR_DXR_win32_window_binding extension");
    LOG_INFO("Application creates and controls its own window");

    // #439 Phase 3 — handle + mask + Local2D layer modes (§8 cases 2/3/4).
    {
        const char* e = getenv("DXR_LOCAL2D_PANEL");
        if (e && *e == '1') g_l2dPanel = true;
        e = getenv("DXR_LOCAL2D_MASK");
        if (e && (*e == '1' || *e == '3')) {
          g_l2dMask = true;
          g_l2dMaskTier3 = (*e == '3'); // #918 review F1
        }
        // #918 review F2 — DXR_LOCAL2D_MASK_DIM=WxH (0x0 / unset = window
        // size).
        e = getenv("DXR_LOCAL2D_MASK_DIM");
        if (e != nullptr && *e != 0) {
          unsigned mw = 0, mh = 0;
          if (sscanf(e, "%ux%u", &mw, &mh) == 2) {
            g_l2dMaskW = mw;
            g_l2dMaskH = mh;
          }
        }
        e = getenv("DXR_LOCAL2D_PANEL2");
        if (e && *e == '1') g_l2dPanel2 = true;
        // #491 part 3 — DXR_LOCAL2D_BACKDROP=1 ⟹ opaque (variant 2); =2 ⟹
        // semi-transparent (variant 3, desktop shows through). Implies the panel path.
        e = getenv("DXR_LOCAL2D_BACKDROP");
        if (e && (*e == '1' || *e == '2')) {
            g_l2dBackdropVariant = (*e == '2') ? 3 : 2;
            g_l2dPanel = true;
        }
        if (g_l2dPanel) {
            LOG_INFO("DXR_LOCAL2D_PANEL=1 — Local2D panel layer%s%s%s",
                g_l2dPanel2 ? " + panel2 (unpremultiplied, overlapping)" : "",
                g_l2dMask ? " + explicit Tier-2 island mask" : " (implicit mask)",
                g_l2dBackdropVariant == 2 ? " + opaque 2D-under backdrop" :
                g_l2dBackdropVariant == 3 ? " + semi-transparent 2D-under backdrop" : "");
        }
    }

    // Create window FIRST (needed for XR_DXR_win32_window_binding)
    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    // Initialize OpenXR
    LOG_INFO("Initializing OpenXR...");
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        MessageBox(hwnd, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // INV-1.3: open on the 3D panel (#715) — one-shot move to the panel's
    // desktop position reported by xrGetSystemProperties (virtual-screen
    // coords, top-down; (0,0) = primary/unknown is safe), BEFORE
    // xrCreateSession so the display processor tracks the window on the
    // panel from the start.
    SetWindowPos(hwnd, nullptr, g_displayScreenLeft, g_displayScreenTop, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Check for session target extension
    if (!xr.hasWin32WindowBindingExt) {
        LOG_WARN("XR_DXR_win32_window_binding not available - runtime will create its own window");
        MessageBox(hwnd, L"XR_DXR_win32_window_binding extension not available.\nRuntime will create its own window.",
            L"Warning", MB_OK | MB_ICONWARNING);
    } else {
        LOG_INFO("XR_DXR_win32_window_binding extension is available - using app window");
    }

    // Get the required GPU adapter LUID from OpenXR
    LUID adapterLuid;
    if (!GetD3D11GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D11 graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D11 on the correct adapter
    LOG_INFO("Initializing D3D11...");
    D3D11Renderer renderer = {};
    if (!InitializeD3D11WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D11 initialization failed");
        MessageBox(hwnd, L"Failed to initialize D3D11", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize HUD renderer (standalone D3D11 device for text rendering)
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create OpenXR session WITH window handle (XR_DXR_win32_window_binding)
    LOG_INFO("Creating OpenXR session with XR_DXR_win32_window_binding (HWND: 0x%p)...", hwnd);
    if (!CreateSession(xr, renderer.device.Get(), hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(hwnd, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // #823 actions mode: set up the action set + bindings right after
    // session creation. Failure downgrades to the plain cube.
    if (g_actionsMode) {
        LOG_INFO("Actions mode ENABLED (--actions): khr/simple_controller pose+select+haptic");
        if (!SetupActions(xr.instance, xr.session)) {
            LOG_ERROR("actions: setup failed — continuing without actions mode");
            g_actionsMode = false;
        }
    }

    // #825 hands mode: create the hand trackers right after session
    // creation. Failure downgrades to the plain cube.
    if (g_handsMode) {
        LOG_INFO("Hands mode ENABLED (--hands): XR_EXT_hand_tracking joint markers");
        if (!SetupHands(xr.instance, xr.systemId, xr.session)) {
            LOG_ERROR("hands: setup failed — continuing without hands mode");
            g_handsMode = false;
        }
    }

    // Create reference spaces
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Create the projection swapchain — tiled by default, or a layered/array
    // swapchain (arraySize=2) when DISPLAYXR_ARRAY_LAYOUT=1.
    if (!CreateSwapchain(xr, ArrayLayoutEnabled() ? kArraySlices : 1)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D11 swapchain images
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u D3D11 swapchain images", count);
    }

    // Create HUD swapchain for window-space layer submission
    if (!CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
        LOG_WARN("Failed to create HUD swapchain - HUD will not be displayed");
    }

    // Enumerate HUD swapchain images
    std::vector<XrSwapchainImageD3D11KHR> hudSwapchainImages;
    if (xr.hasHudSwapchain) {
        uint32_t count = xr.hudSwapchain.imageCount;
        hudSwapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)hudSwapchainImages.data());
        LOG_INFO("HUD: enumerated %u D3D11 swapchain images", count);
    }

    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Create single depth buffer at full swapchain dimensions
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    {
        ID3D11Texture2D* depthTex = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        if (!CreateDepthStencilView(renderer, xr.swapchain.width, xr.swapchain.height, &depthTex, &dsv)) {
            LOG_ERROR("Failed to create depth buffer");
            CleanupOpenXR(xr);
            if (hudOk) CleanupHudRenderer(hudRenderer);
            CleanupD3D11(renderer);
            ShutdownLogging();
            return 1;
        }
        depthTexture.Attach(depthTex);
        depthDSV.Attach(dsv);
    }

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("XR rendering happens in the application window (XR_DXR_win32_window_binding)");
    LOG_INFO("Single-threaded: message pump + render on the main thread (WM_PAINT during drag/resize)");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, P=Parallax, V=Mode, SHIFT+TAB=HUD, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    // DXR_TESTAPP_MAX_HZ — unconditional app-side frame cap, default off.
    // Same env name, semantics and pacing maths as cube_handle_vk_win: the
    // capped-D3D11 vs capped-VK comparison is only meaningful if both apps are
    // rate-limited by identical code, so keep these two in lockstep.
    int capHz = 0;
    if (const char *capEnv = getenv("DXR_TESTAPP_MAX_HZ")) {
        capHz = atoi(capEnv);
        if (capHz < 0) capHz = 0;
        if (capHz > 240) capHz = 240;
    }
    const std::chrono::nanoseconds capInterval(capHz > 0 ? 1000000000LL / capHz : 0);
    std::chrono::steady_clock::time_point capNext = std::chrono::steady_clock::now();
    if (capHz > 0) {
        LOG_WARN("[RenderLoop] DXR_TESTAPP_MAX_HZ=%d — app frame cap active (min interval %.2f ms)",
                 capHz, capInterval.count() / 1e6);
    } else {
        LOG_INFO("[RenderLoop] DXR_TESTAPP_MAX_HZ unset — no app frame cap");
    }

    // Set virtual display height (app units). 0.24 = 4x the 0.06m cube height.
    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.initialVirtualDisplayHeight = g_inputState.viewParams.virtualDisplayHeight; // SPACE-reset target
    g_inputState.nominalViewerZ = xr.nominalViewerZ;
    g_inputState.renderingModeCount = xr.renderingModeCount;

    RenderState rs = {};
    rs.hwnd = hwnd;
    rs.xr = &xr;
    rs.renderer = &renderer;
    rs.hudRenderer = &hudRenderer;
    rs.hudOk = hudOk;
    rs.hudSwapchainImages = &hudSwapchainImages;
    rs.depthTexture = depthTexture;
    rs.depthDSV = depthDSV;
    rs.swapchainImages = &swapchainImages;
    rs.perfStats = &perfStats;
    g_renderState = &rs;

    // Single-threaded main loop: pump messages, then render one frame.
    // During drag/resize, DefWindowProc enters a modal loop that blocks
    // PeekMessage — WM_PAINT fires inside that modal loop to keep rendering.
    MSG msg = {};
    while (g_running && !xr.exitRequested) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_running) break;

        // Frame cap. NOTE: unlike the VK app, this loop also pumps messages, so
        // the wait below stalls the pump for up to one capped interval (~33 ms
        // at cap 30). That is accepted deliberately — identical pacing to the VK
        // app is worth more here than input latency in an unattended cell — but
        // it does make the window feel sluggish to drag while a cap is active.
        if (capHz > 0) {
            auto now = std::chrono::steady_clock::now();
            if (capNext > now) {
                // Sleep only — NO yield-spin. A spin here burns a core for most
                // of the capped interval and starves the runtime's repaint
                // thread, which collapses total panel cadence (measured: repaint
                // fell to 44 Hz and the app to 19 Hz against a 30 Hz cap).
                // Sleep jitter is the cheaper error: it perturbs the app's own
                // cadence slightly, it does not steal from the thing under test.
                std::this_thread::sleep_for(capNext - now);
                now = std::chrono::steady_clock::now();
            }
            capNext = (capNext + capInterval > now) ? capNext + capInterval : now + capInterval;
        }

        RenderOneFrame(rs);
    }

    g_renderState = nullptr;

    // Cleanup
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    depthDSV.Reset();
    depthTexture.Reset();

    g_xr = nullptr;
    CleanupHands();
    CleanupOpenXR(xr);
    if (hudOk) CleanupHudRenderer(hudRenderer);
    CleanupD3D11(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
