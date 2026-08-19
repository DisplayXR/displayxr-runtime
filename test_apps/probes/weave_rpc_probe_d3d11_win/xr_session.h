// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the XR_DXR_weave probe (#625).
 *
 * The probe is a present-owner: it owns its OS window, runs forced-IPC, binds
 * its window for DP phase-snap, hands the runtime a pre-weave SBS texture +
 * a window-relative rect per frame via xrWeaveSubmitDXR, and presents the
 * weaved handback itself. The session is created with XR_DXR_win32_window_binding
 * (real HWND + transparentBackgroundEnabled, NO shared texture) so the per-client
 * display processor is created bound to the probe's window and the service never
 * tries to present the cross-process window. There is no frame loop / projection
 * submission — the weave service is synchronous and independent of xrEndFrame.
 */

#pragma once

#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"
#include <openxr/XR_DXR_weave.h>

// XR_DXR_weave available + enabled on the instance, and the resolved entry points.
extern bool g_hasWeaveExt;
//! The spec version the RUNTIME advertises for XR_DXR_weave (extensionVersion), not
//! the version this probe's headers declare. The two can differ — an installed
//! runtime older than these headers still advertises its own number — and every v8
//! feature below is gated on the runtime's, so a stale runtime degrades to a clear
//! log line instead of a silently-ignored chain.
extern uint32_t g_weaveSpecVersion;
extern PFN_xrWeaveBindWindowDXR g_pfnWeaveBindWindow;
extern PFN_xrWeaveSubmitDXR g_pfnWeaveSubmit;
//! Spec v8 (browser#88). NULL on a pre-v8 runtime — the sticky-latch tests skip.
extern PFN_xrWeaveSetScreenFlatRegionsDXR g_pfnWeaveSetScreenFlat;

// Initialize OpenXR instance + system; detect/enable D3D11 + win32_window_binding
// + display_info + weave.
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D11 graphics requirements (adapter LUID) so the probe's device is on
// the adapter the runtime requires (shared handles only work same-adapter).
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create the forced-IPC session: D3D11 binding + win32 window binding (real HWND,
// transparentBackgroundEnabled, NO shared texture). Resolves the weave PFNs.
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HWND appHwnd);
