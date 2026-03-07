// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for OpenGL with shared texture
 *
 * Uses XR_EXT_win32_window_binding with sharedTextureHandle (windowHandle=NULL)
 * for offscreen shared texture compositing via the GL native compositor.
 */

#pragma once

#define XR_USE_GRAPHICS_API_OPENGL
#include "xr_session_common.h"
#include <GL/gl.h>

// Initialize OpenXR instance with OpenGL + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get OpenGL graphics requirements (min/max GL version)
bool GetOpenGLGraphicsRequirements(XrSessionManager& xr);

// Create session with OpenGL context and shared texture handle (offscreen mode)
bool CreateSession(XrSessionManager& xr, HDC hDC, HGLRC hGLRC, HANDLE sharedTextureHandle);
