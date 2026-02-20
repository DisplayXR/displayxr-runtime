// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS helpers for sim_display (visible frame query).
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#import <AppKit/NSScreen.h>

void
sim_display_macos_get_visible_frame(uint32_t *out_w, uint32_t *out_h)
{
	NSScreen *screen = [NSScreen mainScreen];
	if (screen == nil) {
		*out_w = 0;
		*out_h = 0;
		return;
	}

	NSRect visible = [screen visibleFrame];
	*out_w = (uint32_t)visible.size.width;
	*out_h = (uint32_t)visible.size.height;
}
