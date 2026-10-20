// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS desktop control: the capture backend's geometry decisions as
 *         pure functions (XR_DXR_desktop_control spec §10, ADR-056 Amendment 1).
 *         Compiled on every OS so tests_desktop_control pins them; the macOS
 *         backend (u_desktop_macos*.m) is their only caller.
 *
 * Coordinates are the macOS backend's desktop space: global points, origin at
 * the top-left of the primary display, y down (what CGDisplayBounds,
 * CGWindowList and ScreenCaptureKit's display-local sourceRect use).
 */

#pragma once

#include "u_desktop.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Most displays the macOS backend considers (CGGetActiveDisplayList).
#define U_DESKTOP_MACOS_MAX_DISPLAYS 16

//! One active display.
struct u_desktop_macos_display
{
	uint32_t id;                  //!< CGDirectDisplayID (SCDisplay.displayID)
	struct u_desktop_rect bounds; //!< CGDisplayBounds, global points
	double scale;                 //!< backing pixels per point (2 on Retina), >= 1
};

/*!
 * The backing scale of a display mode: @p pixel_w device px over @p point_w
 * points (CGDisplayModeGetPixelWidth / CGDisplayModeGetWidth). 1 when either is
 * 0 or the ratio is below 1; never above 8.
 */
double
u_desktop_macos_display_scale(uint32_t pixel_w, uint32_t point_w);

//! What one display contributes to a capture.
struct u_desktop_macos_capture_part
{
	uint32_t display;           //!< index into the display array
	struct u_desktop_rect rect; //!< desktop points: the region ∩ the display
	//! The display-local source rect for ScreenCaptureKit (points, top-left origin).
	struct u_desktop_rect local;
	uint32_t px_w, px_h; //!< the frame's size in the display's device pixels
};

/*!
 * Plan the still of desktop rect @p src over @p n displays: one part per display
 * the region touches, and the image size — the region at the densest backing
 * scale among those displays (as macOS's own screenshots; lower-density displays
 * are sampled up, nearest). Pixels of the region that no display shows (a gap of
 * a non-rectangular layout) are left out of every part, so they stay withheld.
 * False when the region touches no display or the image would exceed
 * U_DESKTOP_CAPTURE_MAX_NATIVE on a side.
 */
bool
u_desktop_macos_capture_plan(const struct u_desktop_rect *src,
                             const struct u_desktop_macos_display *d,
                             uint32_t n,
                             struct u_desktop_macos_capture_part *parts,
                             uint32_t cap,
                             uint32_t *nparts,
                             uint32_t *img_w,
                             uint32_t *img_h);

//! One on-screen window as CGWindowList reports it.
struct u_desktop_macos_window_info
{
	uint32_t pid;                //!< kCGWindowOwnerPID
	int32_t sharing;             //!< kCGWindowSharingState (0 = kCGWindowSharingNone)
	double alpha;                //!< kCGWindowAlpha
	struct u_desktop_rect frame; //!< kCGWindowBounds, global points
};

/*!
 * Whether a capture withholds this window's whole frame: any visible window of
 * the calling process (@p own_pid, every layer — decision 7; the common layer
 * only knows the layer-0 ones), and any window whose owner asked not to be
 * shared (NSWindowSharingNone ⇒ kCGWindowSharingNone), whatever ScreenCaptureKit
 * itself does with it.
 */
bool
u_desktop_macos_capture_withholds(const struct u_desktop_macos_window_info *w, uint32_t own_pid);

#ifdef __cplusplus
}
#endif
