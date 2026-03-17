// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for OpenGL (standard mode, no win32_window_binding)
 *
 * This version does NOT use the XR_EXT_win32_window_binding extension.
 * OpenXR/DisplayXR will create its own window for rendering.
 */

#pragma once

#define XR_USE_GRAPHICS_API_OPENGL
#include <windows.h>
#include <GL/gl.h>
#include "xr_session_common.h"

// Initialize OpenXR instance (OpenGL + display_info only, no win32_window_binding)
bool InitializeOpenXR(XrSessionManager& xr);

// Get OpenGL graphics requirements (min/max GL version)
bool GetOpenGLGraphicsRequirements(XrSessionManager& xr);

// Create session with OpenGL context (no window handle - DisplayXR creates window)
bool CreateSession(XrSessionManager& xr, HDC hDC, HGLRC hGLRC);
