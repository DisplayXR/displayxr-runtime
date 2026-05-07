// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keyboard and mouse input state tracking implementation
 */

#include "input_handler.h"
#include "display3d_view.h"
#include <DirectXMath.h>
#include <chrono>
#include <cmath>
#include <sstream>

using namespace DirectX;

// Monotonic wall-clock seconds — matches macOS NowSec() semantics.
static double NowSec() {
    using namespace std::chrono;
    return (double)duration_cast<microseconds>(
        high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
}

static void MarkUserInput(InputState& state) {
    state.lastInputTimeSec = NowSec();
    state.animationActive = false;
}

// Helper to get key name from virtual key code
static std::string GetKeyName(WPARAM vk) {
    // Handle special keys
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Escape";
    case VK_TAB: return "Tab";
    case VK_BACK: return "Backspace";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_F1: return "F1";
    case VK_F2: return "F2";
    case VK_F3: return "F3";
    case VK_F4: return "F4";
    case VK_F5: return "F5";
    case VK_F11: return "F11";
    default: break;
    }

    // Letters and numbers
    if (vk >= 'A' && vk <= 'Z') {
        return std::string(1, (char)vk);
    }
    if (vk >= '0' && vk <= '9') {
        return std::string(1, (char)vk);
    }

    // Unknown key
    std::ostringstream oss;
    oss << "0x" << std::hex << vk;
    return oss.str();
}

bool UpdateInputState(InputState& state, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEMOVE:
        state.mouseX = LOWORD(lParam);
        state.mouseY = HIWORD(lParam);

        // Update camera rotation if dragging
        if (state.dragging) {
            MarkUserInput(state);
            int dx = state.mouseX - state.dragStartX;
            int dy = state.mouseY - state.dragStartY;
            state.yaw -= dx * 0.005f;
            state.pitch -= dy * 0.005f;
            // Clamp pitch to avoid gimbal lock
            if (state.pitch > 1.4f) state.pitch = 1.4f;
            if (state.pitch < -1.4f) state.pitch = -1.4f;
            state.dragStartX = state.mouseX;
            state.dragStartY = state.mouseY;
        }
        return true;

    case WM_LBUTTONDBLCLK:
        MarkUserInput(state);
        state.teleportRequested = true;
        state.teleportMouseX = (float)LOWORD(lParam);
        state.teleportMouseY = (float)HIWORD(lParam);
        return true;

    case WM_LBUTTONDOWN:
        MarkUserInput(state);
        state.leftButton = true;
        state.dragging = true;
        state.dragStartX = state.mouseX;
        state.dragStartY = state.mouseY;
        // SetCapture moved to app WndProc — calling it here causes reentrant
        // deadlock in multi-threaded apps that protect UpdateInputState with a mutex
        return true;

    case WM_LBUTTONUP:
        state.leftButton = false;
        state.dragging = false;
        // ReleaseCapture moved to app WndProc — same reason as above
        return true;

    case WM_RBUTTONDOWN:
        state.rightButton = true;
        return true;

    case WM_RBUTTONUP:
        state.rightButton = false;
        return true;

    case WM_MBUTTONDOWN:
        state.middleButton = true;
        return true;

    case WM_MBUTTONUP:
        state.middleButton = false;
        return true;

    case WM_MOUSEWHEEL: {
        MarkUserInput(state);
        int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        float factor = (zDelta > 0) ? 1.1f : (1.0f / 1.1f);
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (shift) {
            // ipd and parallax are conceptually a single "3D effect strength"
            // knob — driving them in lockstep matches the +/- key bindings
            // and the macOS demo's behaviour.
            float v = state.viewParams.ipdFactor * factor;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            state.viewParams.ipdFactor = v;
            state.viewParams.parallaxFactor = v;
        } else if (ctrl) {
            state.viewParams.parallaxFactor *= factor;
            if (state.viewParams.parallaxFactor < 0.0f) state.viewParams.parallaxFactor = 0.0f;
            if (state.viewParams.parallaxFactor > 1.0f) state.viewParams.parallaxFactor = 1.0f;
        } else if (alt) {
            if (state.cameraMode) {
                state.viewParams.invConvergenceDistance *= factor;
                if (state.viewParams.invConvergenceDistance < 0.1f) state.viewParams.invConvergenceDistance = 0.1f;
                if (state.viewParams.invConvergenceDistance > 10.0f) state.viewParams.invConvergenceDistance = 10.0f;
            } else {
                // Cap perspectiveFactor at 1.0 — values above that exaggerate
                // the Kooima eye separation past the display's natural geometry
                // and look wrong (Z-fighting / hyperstereo on the splat scene).
                state.viewParams.perspectiveFactor *= factor;
                if (state.viewParams.perspectiveFactor < 0.1f) state.viewParams.perspectiveFactor = 0.1f;
                if (state.viewParams.perspectiveFactor > 1.0f) state.viewParams.perspectiveFactor = 1.0f;
            }
        } else {
            if (state.cameraMode) {
                state.viewParams.zoomFactor *= factor;
                if (state.viewParams.zoomFactor < 0.1f) state.viewParams.zoomFactor = 0.1f;
                if (state.viewParams.zoomFactor > 10.0f) state.viewParams.zoomFactor = 10.0f;
            } else {
                state.viewParams.scaleFactor *= factor;
                if (state.viewParams.scaleFactor < 0.1f) state.viewParams.scaleFactor = 0.1f;
                if (state.viewParams.scaleFactor > 10.0f) state.viewParams.scaleFactor = 10.0f;
            }
        }
        return true;
    }

    case WM_KEYDOWN:
        MarkUserInput(state);
        state.lastKey = GetKeyName(wParam);
        switch (wParam) {
        case 'W': state.keyW = true; break;
        case 'A': state.keyA = true; break;
        case 'S': state.keyS = true; break;
        case 'D': state.keyD = true; break;
        case 'E': state.keyE = true; break;
        case 'Q': state.keyQ = true; break;
        case 'M':
            state.animateToggleRequested = true;
            break;
        case VK_OEM_MINUS: {
            float v = state.viewParams.ipdFactor - 0.1f;
            if (v < 0.1f) v = 0.1f;
            state.viewParams.ipdFactor = v;
            state.viewParams.parallaxFactor = v;
            break;
        }
        case VK_OEM_PLUS: {
            float v = state.viewParams.ipdFactor + 0.1f;
            if (v > 1.0f) v = 1.0f;
            state.viewParams.ipdFactor = v;
            state.viewParams.parallaxFactor = v;
            break;
        }
        case VK_SPACE:
            state.resetViewRequested = true;
            break;
        case 'P':
            state.keyP = true;
            state.parallaxEnabled = !state.parallaxEnabled;
            break;
        case VK_F11:
            state.keyF11 = true;
            state.fullscreenToggleRequested = true;
            break;
        case VK_TAB:
            // Require SHIFT to avoid colliding with the workspace shell's
            // bare TAB binding (cycle focus across windows). Bare TAB is
            // the focus-cycle hotkey when the cube runs under
            // displayxr-shell, so a non-modified TAB should fall through
            // to the shell rather than toggle the HUD.
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                state.hudVisible = !state.hudVisible;
            }
            break;
        case 'V':
            // Cycle through all rendering modes
            if (state.renderingModeCount > 0) {
                state.currentRenderingMode = (state.currentRenderingMode + 1) % state.renderingModeCount;
            }
            state.renderingModeChangeRequested = true;
            break;
        case 'I':
            // Snapshot the multi-view atlas to a PNG. Render thread
            // consumes the flag after xrEndFrame.
            state.captureAtlasRequested = true;
            break;
        case '0':
            state.currentRenderingMode = 0; // 2D mode
            state.renderingModeChangeRequested = true;
            break;
        case '1':
            if (state.renderingModeCount > 1) state.currentRenderingMode = 1;
            state.renderingModeChangeRequested = true;
            break;
        case '2':
            if (state.renderingModeCount > 2) state.currentRenderingMode = 2;
            state.renderingModeChangeRequested = true;
            break;
        case '3':
            if (state.renderingModeCount > 3) state.currentRenderingMode = 3;
            state.renderingModeChangeRequested = true;
            break;
        case '4':
            if (state.renderingModeCount > 4) state.currentRenderingMode = 4;
            state.renderingModeChangeRequested = true;
            break;
        case '5':
            if (state.renderingModeCount > 5) state.currentRenderingMode = 5;
            state.renderingModeChangeRequested = true;
            break;
        case '6':
            if (state.renderingModeCount > 6) state.currentRenderingMode = 6;
            state.renderingModeChangeRequested = true;
            break;
        case '7':
            if (state.renderingModeCount > 7) state.currentRenderingMode = 7;
            state.renderingModeChangeRequested = true;
            break;
        case '8':
            if (state.renderingModeCount > 8) state.currentRenderingMode = 8;
            state.renderingModeChangeRequested = true;
            break;
        case 'T':
            state.eyeTrackingModeToggleRequested = true;
            break;
        case 'C':
            state.cameraMode = !state.cameraMode;
            if (state.cameraMode) {
                state.cameraPosX = 0.0f;
                state.cameraPosY = 0.0f;
                state.cameraPosZ = state.nominalViewerZ;
                state.yaw = 0.0f;
                state.pitch = 0.0f;
                if (state.nominalViewerZ > 0.0f)
                    state.viewParams.invConvergenceDistance = 1.0f / state.nominalViewerZ;
                state.viewParams.zoomFactor = 1.0f;
            } else {
                state.cameraPosX = 0.0f;
                state.cameraPosY = 0.0f;
                state.cameraPosZ = 0.0f;
                state.yaw = 0.0f;
                state.pitch = 0.0f;
            }
            break;
        }
        return true;

    case WM_KEYUP:
        switch (wParam) {
        case 'W': state.keyW = false; break;
        case 'A': state.keyA = false; break;
        case 'S': state.keyS = false; break;
        case 'D': state.keyD = false; break;
        case 'E': state.keyE = false; break;
        case 'Q': state.keyQ = false; break;
        case 'P': state.keyP = false; break;
        case VK_F11: state.keyF11 = false; break;
        }
        return true;
    }

    return false;
}

void UpdateCameraMovement(InputState& state, float deltaTime, float displayHeightM) {
    // Handle view reset (spacebar)
    if (state.resetViewRequested) {
        state.yaw = 0.0f;
        state.pitch = 0.0f;
        float savedVDH = state.viewParams.virtualDisplayHeight;
        bool savedCameraMode = state.cameraMode;
        state.viewParams = ViewParams{};
        state.viewParams.virtualDisplayHeight = savedVDH;
        state.cameraMode = savedCameraMode;
        if (state.cameraMode) {
            state.cameraPosX = 0.0f;
            state.cameraPosY = 0.0f;
            state.cameraPosZ = state.nominalViewerZ;
            if (state.nominalViewerZ > 0.0f)
                state.viewParams.invConvergenceDistance = 1.0f / state.nominalViewerZ;
        } else {
            state.cameraPosX = 0.0f;
            state.cameraPosY = 0.0f;
            state.cameraPosZ = 0.0f;
        }
        state.resetViewRequested = false;
        state.teleportAnimating = false;
        state.transitioning = false;
        state.animateEnabled = false;
        state.animationActive = false;
        state.lastInputTimeSec = NowSec();
        return;
    }

    // Smooth display-pose transition (slerp) — gaussian-splat demo path.
    if (state.transitioning) {
        state.transitionT += deltaTime;
        float u = state.transitionT / state.transitionDuration;
        if (u >= 1.0f) u = 1.0f;
        float invU = 1.0f - u;
        float eased = 1.0f - invU * invU * invU;   // ease-out cubic
        XrPosef cur;
        display3d_pose_slerp(&state.transitionFrom, &state.transitionTo, eased, &cur);
        state.cameraPosX = cur.position.x;
        state.cameraPosY = cur.position.y;
        state.cameraPosZ = cur.position.z;
        // Decompose quaternion back into yaw/pitch (DirectXMath RollPitchYaw convention).
        XMVECTOR q = XMVectorSet(cur.orientation.x, cur.orientation.y, cur.orientation.z, cur.orientation.w);
        XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), q);
        XMFLOAT3 f;
        XMStoreFloat3(&f, fwd);
        // Inverse of XMQuaternionRotationRollPitchYaw(p, y, 0): for small angles,
        // fwd = q * (0,0,-1) ≈ (-sin(y), sin(p), -cos(y)) (same signs as macOS path).
        state.yaw = atan2f(-f.x, -f.z);
        float fy = f.y;
        if (fy > 1.0f) fy = 1.0f;
        if (fy < -1.0f) fy = -1.0f;
        state.pitch = asinf(fy);
        if (u >= 1.0f) state.transitioning = false;
        return;
    }

    // Meters-to-virtual conversion (matches Kooima projection scaling)
    float m2v = 1.0f;
    if (state.viewParams.virtualDisplayHeight > 0.0f && displayHeightM > 0.0f)
        m2v = state.viewParams.virtualDisplayHeight / displayHeightM;

    const float moveSpeed = 0.1f * m2v / state.viewParams.scaleFactor; // Virtual units per second, scaled with zoom

    // Build orientation quaternion using the same function as LocateViews,
    // guaranteeing movement vectors match the view rotation exactly.
    XMVECTOR ori = XMQuaternionRotationRollPitchYaw(state.pitch, state.yaw, 0.0f);

    // Derive direction vectors by rotating basis vectors with the quaternion
    XMFLOAT3 fwd, rt, up;
    XMStoreFloat3(&fwd, XMVector3Rotate(XMVectorSet(0, 0, -1, 0), ori));  // forward = -Z
    XMStoreFloat3(&rt,  XMVector3Rotate(XMVectorSet(1, 0, 0, 0), ori));   // right = +X
    XMStoreFloat3(&up,  XMVector3Rotate(XMVectorSet(0, 1, 0, 0), ori));   // up = +Y

    // W/S: move along display forward (fly mode)
    if (state.keyW) {
        state.cameraPosX += fwd.x * moveSpeed * deltaTime;
        state.cameraPosY += fwd.y * moveSpeed * deltaTime;
        state.cameraPosZ += fwd.z * moveSpeed * deltaTime;
    }
    if (state.keyS) {
        state.cameraPosX -= fwd.x * moveSpeed * deltaTime;
        state.cameraPosY -= fwd.y * moveSpeed * deltaTime;
        state.cameraPosZ -= fwd.z * moveSpeed * deltaTime;
    }
    // A/D: strafe along display right
    if (state.keyA) {
        state.cameraPosX -= rt.x * moveSpeed * deltaTime;
        state.cameraPosY -= rt.y * moveSpeed * deltaTime;
        state.cameraPosZ -= rt.z * moveSpeed * deltaTime;
    }
    if (state.keyD) {
        state.cameraPosX += rt.x * moveSpeed * deltaTime;
        state.cameraPosY += rt.y * moveSpeed * deltaTime;
        state.cameraPosZ += rt.z * moveSpeed * deltaTime;
    }
    // E/Q: move along display up/down
    if (state.keyE) {
        state.cameraPosX += up.x * moveSpeed * deltaTime;
        state.cameraPosY += up.y * moveSpeed * deltaTime;
        state.cameraPosZ += up.z * moveSpeed * deltaTime;
    }
    if (state.keyQ) {
        state.cameraPosX -= up.x * moveSpeed * deltaTime;
        state.cameraPosY -= up.y * moveSpeed * deltaTime;
        state.cameraPosZ -= up.z * moveSpeed * deltaTime;
    }

    // Teleport animation: exponential ease-out (legacy path used by cube_* apps).
    if (state.teleportAnimating) {
        float t = 1.0f - expf(-10.0f * deltaTime); // ~90% in 0.23s
        state.cameraPosX += (state.teleportTargetX - state.cameraPosX) * t;
        state.cameraPosY += (state.teleportTargetY - state.cameraPosY) * t;
        state.cameraPosZ += (state.teleportTargetZ - state.cameraPosZ) * t;
        float dx = state.teleportTargetX - state.cameraPosX;
        float dy = state.teleportTargetY - state.cameraPosY;
        float dz = state.teleportTargetZ - state.cameraPosZ;
        if (dx*dx + dy*dy + dz*dz < 1e-8f)
            state.teleportAnimating = false;
    }

    // Auto-orbit: if enabled and user idle > 10s, slowly yaw the display.
    if (state.animateEnabled && state.lastInputTimeSec > 0.0) {
        double idleFor = NowSec() - state.lastInputTimeSec;
        state.animationActive = (idleFor > 10.0);
        if (state.animationActive) {
            float rate = 6.2831853f / 20.0f; // one revolution per 20 seconds
            state.yaw += rate * deltaTime;
        }
    } else {
        state.animationActive = false;
    }
}

std::string GetMouseButtonString(const InputState& state) {
    std::string result;
    if (state.leftButton) result += "[LMB]";
    if (state.rightButton) result += "[RMB]";
    if (state.middleButton) result += "[MMB]";
    if (result.empty()) result = "None";
    return result;
}
