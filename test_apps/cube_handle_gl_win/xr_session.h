// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for OpenGL with XR_EXT_win32_window_binding
 */

#pragma once

#define XR_USE_GRAPHICS_API_OPENGL
#include "xr_session_common.h"
#include <GL/gl.h>
#include <openxr/XR_EXT_view_rig.h>

// XR_EXT_view_rig (#396 W7) available + enabled on the instance. App-local
// (not on the shared XrSessionManager); promote to xr_session_common when
// more consumers adopt it.
extern bool g_hasViewRigExt;

// Initialize OpenXR instance with OpenGL + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get OpenGL graphics requirements (min/max GL version)
bool GetOpenGLGraphicsRequirements(XrSessionManager& xr);

// Create session with OpenGL context and window handle
bool CreateSession(XrSessionManager& xr, HDC hDC, HGLRC hGLRC, HWND hwnd);
