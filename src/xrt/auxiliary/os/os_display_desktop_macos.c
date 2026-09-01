// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS implementation of the desktop-rect resolver.
 * @ingroup aux_os
 *
 * CoreGraphics only — no Objective-C, no AppKit. Follow-on to #1301's Windows
 * half; see #715 for the panel-origin plumbing this consumes.
 *
 * ## Coordinate space: CoreGraphics, deliberately not NSScreen
 *
 * `CGDisplayBounds` reports the **global display coordinate space**: origin at
 * the top-left of the main display, y increasing DOWNWARD, in POINTS. That
 * matches the top-down convention `XR_DXR_display_info` states, which is why
 * this uses CoreGraphics rather than `NSScreen.frame` — the latter is
 * bottom-left with y increasing UPWARD, and silently produces a vertically
 * mirrored position on any multi-display arrangement.
 *
 * An app placing an `NSWindow` must convert back into Cocoa's bottom-up space
 * itself; the runtime publishes the top-down value so the contract is one
 * convention across Windows, macOS and X11.
 *
 * ## Points, not backing pixels
 *
 * The rect is in points because that is the space windows are placed in. On a
 * Retina display the backing store is 2x larger, so this rect is NOT the panel's
 * pixel resolution — `XrDisplayInfoDXR::displayPixelWidth/Height` remains the
 * place to read that. @ref width_in_caller_dpi carries the pixel dims so the
 * panel-confirmation check has both sides in one space (see below).
 */

#include "os_display_desktop.h"

#include <string.h>
#include <stdio.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

/*!
 * Stable identity for a display.
 *
 * A `CGDirectDisplayID` is NOT stable — it is reassigned across reboots and
 * replug, which is exactly the case the device name exists to survive. The
 * display's UUID is stable, so that is what we publish.
 */
static void
fill_device_name(CGDirectDisplayID did, char *out, size_t out_size)
{
	out[0] = '\0';

	CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(did);
	if (uuid == NULL) {
		// No UUID (rare: a display that vanished between calls). Fall back to
		// the numeric id — unstable across reboots, but better than nothing
		// for same-session re-resolution.
		(void)snprintf(out, out_size, "CGDisplay-%u", (unsigned)did);
		return;
	}

	CFStringRef str = CFUUIDCreateString(NULL, uuid);
	if (str != NULL) {
		if (!CFStringGetCString(str, out, (CFIndex)out_size, kCFStringEncodingUTF8)) {
			out[0] = '\0';
		}
		CFRelease(str);
	}
	CFRelease(uuid);
}

bool
os_display_desktop_info_at(int32_t x, int32_t y, struct os_display_desktop_info *out_info)
{
	if (out_info == NULL) {
		return false;
	}
	memset(out_info, 0, sizeof(*out_info));

	CGDirectDisplayID did = kCGNullDirectDisplay;
	CGDirectDisplayID hits[1] = {0};
	uint32_t hit_count = 0;

	CGPoint pt = CGPointMake((CGFloat)x, (CGFloat)y);
	if (CGGetDisplaysWithPoint(pt, 1, hits, &hit_count) == kCGErrorSuccess && hit_count > 0) {
		did = hits[0];
	} else {
		// The point falls in no display — a gap in a ragged arrangement, or a
		// stale origin after a rearrangement. Windows resolves to the NEAREST
		// monitor; CoreGraphics has no such query, so fall back to the main
		// display, which is the same answer for the (0,0) "no preference" case
		// that dominates in practice.
		did = CGMainDisplayID();
	}

	CGRect bounds = CGDisplayBounds(did);
	if (CGRectIsNull(bounds) || CGRectIsEmpty(bounds)) {
		memset(out_info, 0, sizeof(*out_info));
		return false;
	}

	out_info->left = (int32_t)bounds.origin.x;
	out_info->top = (int32_t)bounds.origin.y;
	out_info->width = (uint32_t)bounds.size.width;
	out_info->height = (uint32_t)bounds.size.height;
	out_info->is_primary = CGDisplayIsMain(did) != 0;

	// The plug-in reports panel dimensions in BACKING PIXELS, so the
	// confirmation check needs the monitor's pixel dims, not its point dims —
	// otherwise every Retina panel reads as "not confirmed" purely because of
	// the 2x scale. Same role the caller-DPI pair plays on Windows.
	out_info->width_in_caller_dpi = (uint32_t)CGDisplayPixelsWide(did);
	out_info->height_in_caller_dpi = (uint32_t)CGDisplayPixelsHigh(did);

	fill_device_name(did, out_info->device_name, sizeof(out_info->device_name));

	return true;
}
