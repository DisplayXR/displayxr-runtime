// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Wire protocol between the macOS desktop-capture helper and the
 *         compositors that consume its frames (#2264).
 *
 * The helper (`displayxr-desktop-capture`, an LSUIElement app bundle launched
 * on demand by launchd through its MachServices entry) is the ONLY DisplayXR
 * process that calls ScreenCaptureKit, so the Screen Recording grant is held
 * once, by DisplayXR — never by each app that weaves transparently in-process.
 *
 * Transport: XPC dictionaries over the Mach service @ref U_DESKTOP_BG_SERVICE.
 * Every message carries an int64 @ref U_DESKTOP_BG_KEY_OP.
 *
 * ```text
 *   client → helper  START  { display: u64 CGDirectDisplayID, request_access: bool }
 *   helper → client  STATUS { status: enum u_desktop_bg_status }
 *   helper → client  FRAME  { seq: u64, surface: IOSurface (IOSurfaceCreateXPCObject),
 *                             display: u64, width: u64, height: u64 }   // whole panel, device px
 *   client → helper  ACK    { seq: u64 }      // pacing: at most one FRAME in flight per client
 *   client → helper  STOP   {}                // or just cancel the connection
 * ```
 *
 * Surfaces come from a small ring the helper owns. A consumer that samples one
 * holds it with IOSurfaceIncrementUseCount() until its GPU work completes; the
 * helper never writes a ring slot whose IOSurfaceIsInUse().
 *
 * A NOT_GRANTED helper exits once its last client leaves, so the next START
 * (clients retry with backoff while they still want a background) runs in a
 * fresh process that sees a grant made in System Settings meanwhile: TCC grants
 * do not reach an already-running process (measured, #2264).
 *
 * @ingroup aux_util
 */

#pragma once

//! launchd Mach service name (MachServices key of the helper's LaunchAgent).
#define U_DESKTOP_BG_SERVICE "com.displayxr.desktop-capture"

#define U_DESKTOP_BG_KEY_OP "op"
#define U_DESKTOP_BG_KEY_STATUS "status"
#define U_DESKTOP_BG_KEY_DISPLAY "display"
#define U_DESKTOP_BG_KEY_REQUEST_ACCESS "request_access"
#define U_DESKTOP_BG_KEY_SEQ "seq"
#define U_DESKTOP_BG_KEY_SURFACE "surface"
#define U_DESKTOP_BG_KEY_WIDTH "width"
#define U_DESKTOP_BG_KEY_HEIGHT "height"

enum u_desktop_bg_op
{
	U_DESKTOP_BG_OP_START = 1,
	U_DESKTOP_BG_OP_STATUS = 2,
	U_DESKTOP_BG_OP_FRAME = 3,
	U_DESKTOP_BG_OP_ACK = 4,
	U_DESKTOP_BG_OP_STOP = 5,
};

enum u_desktop_bg_status
{
	//! Granted; the stream is (being) started and FRAMEs will follow.
	U_DESKTOP_BG_STATUS_STREAMING = 0,
	//! No Screen Recording grant. The helper prompted at most once ever.
	U_DESKTOP_BG_STATUS_NOT_GRANTED = 1,
	//! ScreenCaptureKit unavailable (macOS < 12.3) or the display is unknown.
	U_DESKTOP_BG_STATUS_UNSUPPORTED = 2,
	//! The stream failed to start; the helper retries on the next START.
	U_DESKTOP_BG_STATUS_ERROR = 3,
};
