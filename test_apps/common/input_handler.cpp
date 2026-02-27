// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keyboard and mouse input state tracking implementation
 */

#include "input_handler.h"
#include <DirectXMath.h>
#include <sstream>

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
        state.resetViewRequested = true;
        return true;

    case WM_LBUTTONDOWN:
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
        int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        float factor = (zDelta > 0) ? 1.1f : (1.0f / 1.1f);
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (shift) {
            state.stereo.ipdFactor *= factor;
            if (state.stereo.ipdFactor < 0.0f) state.stereo.ipdFactor = 0.0f;
            if (state.stereo.ipdFactor > 1.0f) state.stereo.ipdFactor = 1.0f;
        } else if (ctrl) {
            state.stereo.parallaxFactor *= factor;
            if (state.stereo.parallaxFactor < 0.0f) state.stereo.parallaxFactor = 0.0f;
            if (state.stereo.parallaxFactor > 1.0f) state.stereo.parallaxFactor = 1.0f;
        } else if (alt) {
            if (state.cameraMode) {
                state.stereo.invConvergenceDistance *= factor;
                if (state.stereo.invConvergenceDistance < 0.1f) state.stereo.invConvergenceDistance = 0.1f;
                if (state.stereo.invConvergenceDistance > 10.0f) state.stereo.invConvergenceDistance = 10.0f;
            } else {
                state.stereo.perspectiveFactor *= factor;
                if (state.stereo.perspectiveFactor < 0.1f) state.stereo.perspectiveFactor = 0.1f;
                if (state.stereo.perspectiveFactor > 10.0f) state.stereo.perspectiveFactor = 10.0f;
            }
        } else {
            if (state.cameraMode) {
                state.stereo.zoomFactor *= factor;
                if (state.stereo.zoomFactor < 0.1f) state.stereo.zoomFactor = 0.1f;
                if (state.stereo.zoomFactor > 10.0f) state.stereo.zoomFactor = 10.0f;
            } else {
                state.stereo.scaleFactor *= factor;
                if (state.stereo.scaleFactor < 0.1f) state.stereo.scaleFactor = 0.1f;
                if (state.stereo.scaleFactor > 10.0f) state.stereo.scaleFactor = 10.0f;
            }
        }
        return true;
    }

    case WM_KEYDOWN:
        state.lastKey = GetKeyName(wParam);
        switch (wParam) {
        case 'W': state.keyW = true; break;
        case 'A': state.keyA = true; break;
        case 'S': state.keyS = true; break;
        case 'D': state.keyD = true; break;
        case 'E': state.keyE = true; break;
        case 'Q': state.keyQ = true; break;
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
            state.hudVisible = !state.hudVisible;
            break;
        case 'V':
            state.displayMode3D = !state.displayMode3D;
            state.displayModeToggleRequested = true;
            break;
        case '1':
            state.outputMode = 0;
            state.outputModeChangeRequested = true;
            break;
        case '2':
            state.outputMode = 1;
            state.outputModeChangeRequested = true;
            break;
        case '3':
            state.outputMode = 2;
            state.outputModeChangeRequested = true;
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
                    state.stereo.invConvergenceDistance = 1.0f / state.nominalViewerZ;
                state.stereo.zoomFactor = 1.0f;
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
    // Handle view reset (spacebar or double-click)
    if (state.resetViewRequested) {
        state.yaw = 0.0f;
        state.pitch = 0.0f;
        float savedVDH = state.stereo.virtualDisplayHeight;
        bool savedCameraMode = state.cameraMode;
        state.stereo = StereoParams{};
        state.stereo.virtualDisplayHeight = savedVDH;
        state.cameraMode = savedCameraMode;
        if (state.cameraMode) {
            state.cameraPosX = 0.0f;
            state.cameraPosY = 0.0f;
            state.cameraPosZ = state.nominalViewerZ;
            if (state.nominalViewerZ > 0.0f)
                state.stereo.invConvergenceDistance = 1.0f / state.nominalViewerZ;
        } else {
            state.cameraPosX = 0.0f;
            state.cameraPosY = 0.0f;
            state.cameraPosZ = 0.0f;
        }
        state.resetViewRequested = false;
        return;
    }

    // Meters-to-virtual conversion (matches Kooima projection scaling)
    float m2v = 1.0f;
    if (state.stereo.virtualDisplayHeight > 0.0f && displayHeightM > 0.0f)
        m2v = state.stereo.virtualDisplayHeight / displayHeightM;

    const float moveSpeed = 0.1f * m2v / state.stereo.scaleFactor; // Virtual units per second, scaled with zoom

    // Build orientation quaternion using the same function as LocateViews,
    // guaranteeing movement vectors match the view rotation exactly.
    using namespace DirectX;
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
}

std::string GetMouseButtonString(const InputState& state) {
    std::string result;
    if (state.leftButton) result += "[LMB]";
    if (state.rightButton) result += "[RMB]";
    if (state.middleButton) result += "[MMB]";
    if (result.empty()) result = "None";
    return result;
}
