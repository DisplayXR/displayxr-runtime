// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS implementation of the desktop-rect resolver and monitor
 *         enumeration.
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
 * place to read that. `width_in_caller_dpi` and `native_width` carry the
 * current mode's BACKING pixels (`CGDisplayModeGetPixelWidth`), which is the
 * space a plug-in reports panel dims in, so the panel-confirmation check and
 * the connector-mode selection rule compare like with like; `scale` is the
 * ratio (2.0 on Retina).
 *
 * `CGDisplayPixelsWide` is NOT that: despite its name it returns the mode's
 * POINT width on a HiDPI mode (1920 for a 4K panel at 1920x1080@2x). This file
 * used it for the caller-DPI pair until the multi-screen enumeration landed,
 * which made `isPanelConfirmed` false on every Retina panel.
 */

#include "os_display_desktop.h"
#include "os_display_macos.h"

#include <string.h>
#include <stdio.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
// CGDisplayCreateUUIDFromDisplayID is declared in ColorSync, NOT CoreGraphics —
// it was moved there in 10.13 and CoreGraphics re-exports only the symbol, not
// the declaration. Including CoreGraphics alone builds it as an implicit
// declaration returning int, which is a hard error under this project's
// -Werror-implicit-function-declaration (and would silently truncate the
// pointer if it ever were not).
#include <ColorSync/ColorSync.h>

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

uint32_t
os_display_macos_list_displays(uint32_t *out_ids, uint32_t max_ids)
{
	if (out_ids == NULL || max_ids == 0) {
		return 0;
	}

	CGDirectDisplayID active[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	uint32_t active_count = 0;
	if (CGGetActiveDisplayList(OS_DISPLAY_DESKTOP_MAX_MONITORS, active, &active_count) != kCGErrorSuccess) {
		return 0;
	}

	// Main display first — the "primary" every other platform lists first,
	// and the one the (0, 0) desktop origin is on.
	const CGDirectDisplayID main_id = CGMainDisplayID();
	uint32_t n = 0;
	for (uint32_t pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < active_count && n < max_ids; i++) {
			const CGDirectDisplayID did = active[i];
			if ((pass == 0) != (did == main_id)) {
				continue;
			}
			// A display mirroring another shares its bounds: one placement
			// target, not two. Keep the mirror-set master only.
			if (CGDisplayMirrorsDisplay(did) != kCGNullDirectDisplay) {
				continue;
			}
			out_ids[n++] = (uint32_t)did;
		}
	}
	return n;
}

bool
os_display_macos_fill_desktop_info(uint32_t display_id, struct os_display_desktop_info *out_info)
{
	if (out_info == NULL) {
		return false;
	}
	memset(out_info, 0, sizeof(*out_info));

	const CGDirectDisplayID did = (CGDirectDisplayID)display_id;
	CGRect bounds = CGDisplayBounds(did);
	if (CGRectIsNull(bounds) || CGRectIsEmpty(bounds)) {
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
	uint32_t px_w = 0;
	uint32_t px_h = 0;
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did);
	if (mode != NULL) {
		px_w = (uint32_t)CGDisplayModeGetPixelWidth(mode);
		px_h = (uint32_t)CGDisplayModeGetPixelHeight(mode);
		const double hz = CGDisplayModeGetRefreshRate(mode);
		if (hz > 0.0) {
			out_info->native_refresh_mhz = (uint32_t)(hz * 1000.0 + 0.5);
		}
		CGDisplayModeRelease(mode);
	}
	if (px_w == 0 || px_h == 0) {
		// No mode (a display going away mid-call): points are the only
		// answer left, and they are what this used to report.
		px_w = out_info->width;
		px_h = out_info->height;
	} else {
		out_info->native_width = px_w;
		out_info->native_height = px_h;
		out_info->native_source = OS_DISPLAY_NATIVE_SOURCE_COREGRAPHICS;
		out_info->scale = (double)px_w / (double)out_info->width;
	}
	out_info->width_in_caller_dpi = px_w;
	out_info->height_in_caller_dpi = px_h;

	// CoreGraphics derives this from the EDID; 0x0 when it has none.
	const CGSize mm = CGDisplayScreenSize(did);
	if (mm.width > 0.0 && mm.height > 0.0) {
		out_info->physical_width_mm = (uint32_t)(mm.width + 0.5);
		out_info->physical_height_mm = (uint32_t)(mm.height + 0.5);
	}

	fill_device_name(did, out_info->device_name, sizeof(out_info->device_name));

	return true;
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

	return os_display_macos_fill_desktop_info((uint32_t)did, out_info);
}

/*
 * Every active display, main first. With per-display backing-pixel modes in
 * hand the size rules in @ref os_display_desktop_info_for_panel can fire on
 * macOS: the connector-mode rule compares the plug-in's panel pixels against
 * each display's backing-pixel mode, the same space on both sides. sim_display
 * declares the MAIN display's backing size, so for it that rule picks the main
 * display — the monitor the primary fallback picked before this existed.
 */
uint32_t
os_display_desktop_enumerate(struct os_display_desktop_info *out_infos, uint32_t max_infos)
{
	if (out_infos == NULL || max_infos == 0) {
		return 0;
	}

	uint32_t ids[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	const uint32_t id_count = os_display_macos_list_displays(ids, OS_DISPLAY_DESKTOP_MAX_MONITORS);

	uint32_t n = 0;
	for (uint32_t i = 0; i < id_count && n < max_infos; i++) {
		if (os_display_macos_fill_desktop_info(ids[i], &out_infos[n])) {
			n++;
		}
	}
	return n;
}
