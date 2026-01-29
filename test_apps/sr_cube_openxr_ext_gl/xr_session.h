// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for OpenGL with XR_EXT_session_target
 */

#pragma once

#include <GL/gl.h>
#define XR_USE_GRAPHICS_API_OPENGL
#include "xr_session_common.h"

// Initialize OpenXR instance with OpenGL + session_target extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get OpenGL graphics requirements (min/max GL version)
bool GetOpenGLGraphicsRequirements(XrSessionManager& xr);

// Create session with OpenGL context and window handle
bool CreateSession(XrSessionManager& xr, HDC hDC, HGLRC hGLRC, HWND hwnd);
