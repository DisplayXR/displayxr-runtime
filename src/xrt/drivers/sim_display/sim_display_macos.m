// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS helpers for sim_display (visible frame query).
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#import <AppKit/NSScreen.h>
#import <AppKit/NSWindow.h>

void
sim_display_macos_get_visible_frame(uint32_t *out_w, uint32_t *out_h)
{
	NSScreen *screen = [NSScreen mainScreen];
	if (screen == nil) {
		*out_w = 0;
		*out_h = 0;
		return;
	}

	// visibleFrame = screen area minus menu bar and dock.
	// Subtract the title bar height to get the actual window content area.
	NSRect visible = [screen visibleFrame];
	NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                   NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;
	NSRect content = [NSWindow contentRectForFrameRect:visible styleMask:style];
	*out_w = (uint32_t)content.size.width;
	*out_h = (uint32_t)content.size.height;
}

float
sim_display_macos_get_backing_scale(void)
{
	NSScreen *screen = [NSScreen mainScreen];
	if (screen == nil) {
		return 1.0f;
	}
	return (float)[screen backingScaleFactor];
}
