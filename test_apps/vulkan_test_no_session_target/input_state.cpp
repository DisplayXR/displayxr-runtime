// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keyboard and mouse input state tracking implementation
 */

#include "input_state.h"
#include <cmath>
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
            state.yaw += dx * 0.005f;
            state.pitch += dy * 0.005f;
            // Clamp pitch to avoid gimbal lock
            if (state.pitch > 1.4f) state.pitch = 1.4f;
            if (state.pitch < -1.4f) state.pitch = -1.4f;
            state.dragStartX = state.mouseX;
            state.dragStartY = state.mouseY;
        }
        return true;

    case WM_LBUTTONDOWN:
        state.leftButton = true;
        state.dragging = true;
        state.dragStartX = state.mouseX;
        state.dragStartY = state.mouseY;
        SetCapture(GetActiveWindow());
        return true;

    case WM_LBUTTONUP:
        state.leftButton = false;
        state.dragging = false;
        ReleaseCapture();
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

    case WM_KEYDOWN:
        state.lastKey = GetKeyName(wParam);
        switch (wParam) {
        case 'W': state.keyW = true; break;
        case 'A': state.keyA = true; break;
        case 'S': state.keyS = true; break;
        case 'D': state.keyD = true; break;
        case VK_SPACE: state.keySpace = true; break;
        case VK_SHIFT: state.keyShift = true; break;
        }
        return true;

    case WM_KEYUP:
        switch (wParam) {
        case 'W': state.keyW = false; break;
        case 'A': state.keyA = false; break;
        case 'S': state.keyS = false; break;
        case 'D': state.keyD = false; break;
        case VK_SPACE: state.keySpace = false; break;
        case VK_SHIFT: state.keyShift = false; break;
        }
        return true;
    }

    return false;
}

void UpdateCameraMovement(InputState& state, float deltaTime) {
    const float moveSpeed = 2.0f; // Units per second

    // Calculate forward and right vectors based on yaw
    float forwardX = sinf(state.yaw);
    float forwardZ = -cosf(state.yaw);
    float rightX = cosf(state.yaw);
    float rightZ = sinf(state.yaw);

    // Apply movement
    if (state.keyW) {
        state.cameraPosX += forwardX * moveSpeed * deltaTime;
        state.cameraPosZ += forwardZ * moveSpeed * deltaTime;
    }
    if (state.keyS) {
        state.cameraPosX -= forwardX * moveSpeed * deltaTime;
        state.cameraPosZ -= forwardZ * moveSpeed * deltaTime;
    }
    if (state.keyA) {
        state.cameraPosX -= rightX * moveSpeed * deltaTime;
        state.cameraPosZ -= rightZ * moveSpeed * deltaTime;
    }
    if (state.keyD) {
        state.cameraPosX += rightX * moveSpeed * deltaTime;
        state.cameraPosZ += rightZ * moveSpeed * deltaTime;
    }
    if (state.keySpace) {
        state.cameraPosY += moveSpeed * deltaTime;
    }
    if (state.keyShift) {
        state.cameraPosY -= moveSpeed * deltaTime;
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
