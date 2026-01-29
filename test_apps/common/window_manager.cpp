// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window creation and fullscreen management implementation
 */

#include "window_manager.h"
#include "logging.h"
#include <vector>

// Monitor enumeration callback data
struct MonitorEnumData {
    std::vector<RECT> monitors;
    int srMonitorIndex = -1;  // Index of SR display if found
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MonitorEnumData* data = (MonitorEnumData*)dwData;

    MONITORINFOEX info = {};
    info.cbSize = sizeof(MONITORINFOEX);
    if (GetMonitorInfo(hMonitor, &info)) {
        data->monitors.push_back(info.rcMonitor);

        // Log monitor info
        LOG_DEBUG("Monitor %d: %dx%d at (%d,%d) - %ls",
            (int)data->monitors.size() - 1,
            info.rcMonitor.right - info.rcMonitor.left,
            info.rcMonitor.bottom - info.rcMonitor.top,
            info.rcMonitor.left, info.rcMonitor.top,
            info.szDevice);

        // Check if this looks like an SR display
        // SR displays are typically 2560x1600 (stereo) or specific Leia panel sizes
        int width = info.rcMonitor.right - info.rcMonitor.left;
        int height = info.rcMonitor.bottom - info.rcMonitor.top;

        // Common SR display resolutions
        if ((width == 2560 && height == 1600) ||  // 2K SR
            (width == 3840 && height == 2400) ||  // 4K SR
            (width == 1280 && height == 800)) {   // Smaller SR
            data->srMonitorIndex = (int)data->monitors.size() - 1;
            LOG_INFO("Found potential SR display at monitor %d", data->srMonitorIndex);
        }
    }

    return TRUE;
}

int FindSRDisplayMonitor() {
    MonitorEnumData data;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, (LPARAM)&data);

    if (data.srMonitorIndex >= 0) {
        LOG_INFO("SR display found at monitor index %d", data.srMonitorIndex);
        return data.srMonitorIndex;
    }

    // If no SR display found by resolution, return secondary monitor if available
    if (data.monitors.size() > 1) {
        LOG_INFO("No SR display found by resolution, using monitor 1");
        return 1;
    }

    LOG_INFO("Only one monitor found, using primary");
    return 0;
}

bool GetMonitorInfo(int monitorIndex, RECT& monitorRect) {
    MonitorEnumData data;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, (LPARAM)&data);

    if (monitorIndex >= 0 && monitorIndex < (int)data.monitors.size()) {
        monitorRect = data.monitors[monitorIndex];
        return true;
    }

    // Fallback to primary monitor
    if (!data.monitors.empty()) {
        monitorRect = data.monitors[0];
        return true;
    }

    // Default fallback
    monitorRect = { 0, 0, 1920, 1080 };
    return false;
}

bool CreateAppWindow(
    WindowInfo& info,
    HINSTANCE hInstance,
    const wchar_t* className,
    const wchar_t* title,
    int width,
    int height,
    WNDPROC wndProc,
    int monitorIndex
) {
    LOG_INFO("Creating application window (%dx%d)", width, height);

    info.hInstance = hInstance;
    info.width = width;
    info.height = height;
    info.className = className;

    // Register window class
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = className;

    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register window class, error: %lu", err);
            return false;
        }
    }
    LOG_INFO("Window class registered");

    // Get monitor position
    RECT monitorRect = { 0, 0, 1920, 1080 };
    if (monitorIndex >= 0) {
        GetMonitorInfo(monitorIndex, monitorRect);
    }
    info.monitorX = monitorRect.left;
    info.monitorY = monitorRect.top;
    info.monitorWidth = monitorRect.right - monitorRect.left;
    info.monitorHeight = monitorRect.bottom - monitorRect.top;

    LOG_INFO("Target monitor: %dx%d at (%d,%d)",
        info.monitorWidth, info.monitorHeight, info.monitorX, info.monitorY);

    // Calculate window size to get desired client area
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    // Center window on target monitor
    int windowX = info.monitorX + (info.monitorWidth - (rect.right - rect.left)) / 2;
    int windowY = info.monitorY + (info.monitorHeight - (rect.bottom - rect.top)) / 2;

    info.hwnd = CreateWindowEx(
        0,
        className,
        title,
        WS_OVERLAPPEDWINDOW,
        windowX, windowY,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!info.hwnd) {
        DWORD err = GetLastError();
        LOG_ERROR("Failed to create window, error: %lu", err);
        return false;
    }

    LOG_INFO("Window created successfully, HWND: 0x%p", info.hwnd);
    return true;
}

void ToggleFullscreen(WindowInfo& info) {
    if (info.fullscreen) {
        ExitFullscreen(info);
    } else {
        EnterFullscreen(info, -1);
    }
}

void EnterFullscreen(WindowInfo& info, int monitorIndex) {
    if (info.fullscreen) return;

    LOG_INFO("Entering fullscreen mode");

    // Save current window state
    info.savedStyle = GetWindowLong(info.hwnd, GWL_STYLE);
    info.savedExStyle = GetWindowLong(info.hwnd, GWL_EXSTYLE);
    GetWindowRect(info.hwnd, &info.savedWindowRect);

    // Get target monitor
    RECT monitorRect;
    if (monitorIndex >= 0) {
        GetMonitorInfo(monitorIndex, monitorRect);
    } else {
        // Use monitor that contains the window
        HMONITOR hMonitor = MonitorFromWindow(info.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        ::GetMonitorInfo(hMonitor, &mi);
        monitorRect = mi.rcMonitor;
    }

    // Remove window decorations and go fullscreen
    SetWindowLong(info.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLong(info.hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

    SetWindowPos(info.hwnd, HWND_TOP,
        monitorRect.left, monitorRect.top,
        monitorRect.right - monitorRect.left,
        monitorRect.bottom - monitorRect.top,
        SWP_FRAMECHANGED);

    info.fullscreen = true;
    LOG_INFO("Fullscreen enabled");
}

void ExitFullscreen(WindowInfo& info) {
    if (!info.fullscreen) return;

    LOG_INFO("Exiting fullscreen mode");

    // Restore window style
    SetWindowLong(info.hwnd, GWL_STYLE, info.savedStyle);
    SetWindowLong(info.hwnd, GWL_EXSTYLE, info.savedExStyle);

    // Restore window position
    SetWindowPos(info.hwnd, HWND_TOP,
        info.savedWindowRect.left,
        info.savedWindowRect.top,
        info.savedWindowRect.right - info.savedWindowRect.left,
        info.savedWindowRect.bottom - info.savedWindowRect.top,
        SWP_FRAMECHANGED);

    info.fullscreen = false;
    LOG_INFO("Fullscreen disabled");
}

void DestroyAppWindow(WindowInfo& info) {
    if (info.hwnd) {
        DestroyWindow(info.hwnd);
        info.hwnd = nullptr;
    }
    if (info.hInstance && !info.className.empty()) {
        UnregisterClass(info.className.c_str(), info.hInstance);
    }
}
