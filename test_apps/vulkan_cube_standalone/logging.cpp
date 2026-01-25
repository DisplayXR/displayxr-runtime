// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simple file logging implementation
 */

#include "logging.h"
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>

#pragma comment(lib, "shell32.lib")

static FILE* g_logFile = nullptr;
static std::string g_logFilePath;
static std::mutex g_logMutex;

static std::string GetTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

static std::string GetFilenameTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d-%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool InitializeLogging() {
    // Get %LOCALAPPDATA%
    wchar_t localAppData[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        return false;
    }

    // Build log directory path: %LOCALAPPDATA%/LeiaSR/vulkan_cube_standalone/
    std::wstring logDir = localAppData;
    logDir += L"\\LeiaSR";
    CreateDirectoryW(logDir.c_str(), nullptr);
    logDir += L"\\vulkan_cube_standalone";
    CreateDirectoryW(logDir.c_str(), nullptr);

    // Build log file path with timestamp
    std::string timestamp = GetFilenameTimestamp();
    std::wstring logFileName = logDir + L"\\vulkan_cube_standalone_" +
        std::wstring(timestamp.begin(), timestamp.end()) + L".txt";

    // Convert to narrow string for storage
    char narrowPath[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, logFileName.c_str(), -1, narrowPath, MAX_PATH, nullptr, nullptr);
    g_logFilePath = narrowPath;

    // Open log file
    g_logFile = _wfopen(logFileName.c_str(), L"w");
    if (!g_logFile) {
        return false;
    }

    // Write header
    LOG_INFO("=== Vulkan Cube Standalone Application Log ===");
    LOG_INFO("This is a pure Vulkan test app (no OpenXR)");
    LOG_INFO("Log file: %s", g_logFilePath.c_str());
    LOG_INFO("Application starting...");
    LOG_INFO("");

    return true;
}

void ShutdownLogging() {
    if (g_logFile) {
        LOG_INFO("");
        LOG_INFO("Application shutting down");
        LOG_INFO("=== End of Log ===");
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void Log(const char* level, const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    std::string timestamp = GetTimestamp();

    // Format the message
    char message[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // Write to log file
    if (g_logFile) {
        fprintf(g_logFile, "[%s] [%s] %s\n", timestamp.c_str(), level, message);
        fflush(g_logFile);
    }

    // Also write to debug output (visible in debugger) and console
    char debugMsg[4096];
    snprintf(debugMsg, sizeof(debugMsg), "[%s] [%s] %s\n", timestamp.c_str(), level, message);
    OutputDebugStringA(debugMsg);
    printf("%s", debugMsg);  // Also print to console
}

// VkResult to string conversion
static const char* VkResultToString(int result) {
    switch (result) {
        case 0: return "VK_SUCCESS";
        case 1: return "VK_NOT_READY";
        case 2: return "VK_TIMEOUT";
        case 3: return "VK_EVENT_SET";
        case 4: return "VK_EVENT_RESET";
        case 5: return "VK_INCOMPLETE";
        case -1: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case -2: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case -3: return "VK_ERROR_INITIALIZATION_FAILED";
        case -4: return "VK_ERROR_DEVICE_LOST";
        case -5: return "VK_ERROR_MEMORY_MAP_FAILED";
        case -6: return "VK_ERROR_LAYER_NOT_PRESENT";
        case -7: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case -8: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case -9: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case -10: return "VK_ERROR_TOO_MANY_OBJECTS";
        case -11: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case -12: return "VK_ERROR_FRAGMENTED_POOL";
        case -13: return "VK_ERROR_UNKNOWN";
        case -1000069000: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case -1000072003: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case -1000174001: return "VK_ERROR_FRAGMENTATION";
        case -1000161000: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
        case -1000000000: return "VK_ERROR_SURFACE_LOST_KHR";
        case -1000000001: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case 1000001003: return "VK_SUBOPTIMAL_KHR";
        case -1000001004: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VK_UNKNOWN_ERROR";
    }
}

void LogVkResult(const char* context, int result) {
    const char* resultStr = VkResultToString(result);
    if (result >= 0) {
        LOG_INFO("%s returned %s (%d)", context, resultStr, result);
    } else {
        LOG_ERROR("%s failed with %s (%d)", context, resultStr, result);
    }
}

std::string GetLogFilePath() {
    return g_logFilePath;
}
