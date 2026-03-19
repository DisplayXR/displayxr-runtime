// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management (legacy mode, no XR_EXT_display_info, OpenGL)
 *
 * This version does NOT use the XR_EXT_win32_window_binding extension.
 * OpenXR/DisplayXR will create its own window for rendering.
 */

#pragma once

#define XR_USE_GRAPHICS_API_OPENGL
#include "xr_session_common.h"
#include <GL/gl.h>

// Initialize OpenXR instance (OpenGL only, no display info, no window binding)
bool InitializeOpenXR(XrSessionManager& xr);

// Get OpenGL graphics requirements (min/max GL version)
bool GetOpenGLGraphicsRequirements(XrSessionManager& xr);

// Create session with OpenGL context only (no window handle - DisplayXR creates window)
bool CreateSession(XrSessionManager& xr, HDC hDC, HGLRC hGLRC);
