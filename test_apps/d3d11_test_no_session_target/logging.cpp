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

    // Build log directory path: %LOCALAPPDATA%/LeiaSR/session_target_test/
    std::wstring logDir = localAppData;
    logDir += L"\\LeiaSR";
    CreateDirectoryW(logDir.c_str(), nullptr);
    logDir += L"\\session_target_test";
    CreateDirectoryW(logDir.c_str(), nullptr);

    // Build log file path with timestamp
    std::string timestamp = GetFilenameTimestamp();
    std::wstring logFileName = logDir + L"\\session_target_test_" +
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
    LOG_INFO("=== Session Target Test Application Log ===");
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

    // Also write to debug output (visible in debugger)
    char debugMsg[4096];
    snprintf(debugMsg, sizeof(debugMsg), "[%s] [%s] %s\n", timestamp.c_str(), level, message);
    OutputDebugStringA(debugMsg);
}

void LogHResult(const char* context, HRESULT hr) {
    char* errorMsg = nullptr;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&errorMsg, 0, nullptr);

    if (errorMsg) {
        // Remove trailing newline
        size_t len = strlen(errorMsg);
        while (len > 0 && (errorMsg[len-1] == '\n' || errorMsg[len-1] == '\r')) {
            errorMsg[--len] = '\0';
        }
        LOG_ERROR("%s failed with HRESULT 0x%08X: %s", context, hr, errorMsg);
        LocalFree(errorMsg);
    } else {
        LOG_ERROR("%s failed with HRESULT 0x%08X", context, hr);
    }
}

// XrResult to string conversion
static const char* XrResultToString(int64_t result) {
    switch (result) {
        case 0: return "XR_SUCCESS";
        case 1: return "XR_TIMEOUT_EXPIRED";
        case 3: return "XR_SESSION_LOSS_PENDING";
        case 4: return "XR_EVENT_UNAVAILABLE";
        case 7: return "XR_SPACE_BOUNDS_UNAVAILABLE";
        case 8: return "XR_SESSION_NOT_FOCUSED";
        case 9: return "XR_FRAME_DISCARDED";
        case -1: return "XR_ERROR_VALIDATION_FAILURE";
        case -2: return "XR_ERROR_RUNTIME_FAILURE";
        case -3: return "XR_ERROR_OUT_OF_MEMORY";
        case -4: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case -6: return "XR_ERROR_INITIALIZATION_FAILED";
        case -7: return "XR_ERROR_FUNCTION_UNSUPPORTED";
        case -8: return "XR_ERROR_FEATURE_UNSUPPORTED";
        case -9: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case -10: return "XR_ERROR_LIMIT_REACHED";
        case -11: return "XR_ERROR_SIZE_INSUFFICIENT";
        case -12: return "XR_ERROR_HANDLE_INVALID";
        case -13: return "XR_ERROR_INSTANCE_LOST";
        case -14: return "XR_ERROR_SESSION_RUNNING";
        case -16: return "XR_ERROR_SESSION_NOT_RUNNING";
        case -17: return "XR_ERROR_SESSION_LOST";
        case -18: return "XR_ERROR_SYSTEM_INVALID";
        case -19: return "XR_ERROR_PATH_INVALID";
        case -20: return "XR_ERROR_PATH_COUNT_EXCEEDED";
        case -21: return "XR_ERROR_PATH_FORMAT_INVALID";
        case -22: return "XR_ERROR_PATH_UNSUPPORTED";
        case -23: return "XR_ERROR_LAYER_INVALID";
        case -24: return "XR_ERROR_LAYER_LIMIT_EXCEEDED";
        case -25: return "XR_ERROR_SWAPCHAIN_RECT_INVALID";
        case -26: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
        case -27: return "XR_ERROR_ACTION_TYPE_MISMATCH";
        case -28: return "XR_ERROR_SESSION_NOT_READY";
        case -29: return "XR_ERROR_SESSION_NOT_STOPPING";
        case -30: return "XR_ERROR_TIME_INVALID";
        case -31: return "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED";
        case -32: return "XR_ERROR_FILE_ACCESS_ERROR";
        case -33: return "XR_ERROR_FILE_CONTENTS_INVALID";
        case -34: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
        case -35: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case -36: return "XR_ERROR_API_LAYER_NOT_PRESENT";
        case -37: return "XR_ERROR_CALL_ORDER_INVALID";
        case -38: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
        case -39: return "XR_ERROR_POSE_INVALID";
        case -40: return "XR_ERROR_INDEX_OUT_OF_RANGE";
        case -41: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
        case -42: return "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED";
        case -44: return "XR_ERROR_NAME_DUPLICATED";
        case -45: return "XR_ERROR_NAME_INVALID";
        case -46: return "XR_ERROR_ACTIONSET_NOT_ATTACHED";
        case -47: return "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED";
        case -48: return "XR_ERROR_LOCALIZED_NAME_DUPLICATED";
        case -49: return "XR_ERROR_LOCALIZED_NAME_INVALID";
        case -50: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
        case -51: return "XR_ERROR_RUNTIME_UNAVAILABLE";
        default: return "XR_UNKNOWN_ERROR";
    }
}

void LogXrResult(const char* context, int64_t result) {
    const char* resultStr = XrResultToString(result);
    if (result >= 0) {
        LOG_INFO("%s returned %s (%lld)", context, resultStr, result);
    } else {
        LOG_ERROR("%s failed with %s (%lld)", context, resultStr, result);
    }
}

std::string GetLogFilePath() {
    return g_logFilePath;
}
