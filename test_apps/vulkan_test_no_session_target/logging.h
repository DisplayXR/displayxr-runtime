// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simple file logging for debugging
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>

// Initialize logging - creates log directory and opens log file
// Log files are stored in %LOCALAPPDATA%/LeiaSR/session_target_test_vulkan/
// Returns true on success
bool InitializeLogging();

// Close the log file
void ShutdownLogging();

// Log a message with timestamp
void Log(const char* level, const char* format, ...);

// Convenience macros
#define LOG_INFO(fmt, ...)  Log("INFO",  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Log("WARN",  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Log("ERROR", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) Log("DEBUG", fmt, ##__VA_ARGS__)

// Log an HRESULT with description
void LogHResult(const char* context, HRESULT hr);

// Log an XrResult with description
void LogXrResult(const char* context, int64_t result);

// Log a VkResult with description
void LogVkResult(const char* context, int result);

// Get the log file path (for display to user)
std::string GetLogFilePath();
