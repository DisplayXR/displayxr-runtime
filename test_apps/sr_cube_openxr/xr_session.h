// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management (standard mode, no session_target extension)
 *
 * This version does NOT use the XR_EXT_session_target extension.
 * OpenXR/Monado will create its own window for rendering.
 */

#pragma once

#include "xr_session_common.h"

// Initialize OpenXR instance (D3D11 only, no session_target extension)
bool InitializeOpenXR(XrSessionManager& xr);

// Create session with D3D11 device only (no window handle - Monado creates window)
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device);
