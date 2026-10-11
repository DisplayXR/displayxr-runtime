// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-drawn cursor's default look: macOS OS-cursor mirror.
 * @author David Fattal
 * @ingroup comp_util
 *
 * Compiled without ARC (the NSCursor is retained by hand, as the Metal
 * compositor this was lifted from did).
 */

#include "comp_cursor_look.h"

#include "util/u_logging.h"

#import <AppKit/AppKit.h>

#include <math.h>
#include <stdlib.h>

static float g_backing_scale = 0.0f;

float
comp_cursor_look_platform_backing_scale(void)
{
	if ([NSThread isMainThread]) {
		NSScreen *s = [NSScreen mainScreen];
		if (s != nil && s.backingScaleFactor > 0.0) {
			g_backing_scale = (float)s.backingScaleFactor;
		}
	}
	return g_backing_scale > 0.0f ? g_backing_scale : 2.0f;
}

void
comp_cursor_look_platform_release(void *os_cursor)
{
	[(NSCursor *)os_cursor release];
}

/*!
 * Mirror the CURRENT OS cursor shape.
 *
 * AppKit is main-thread-only, so the shape is read only when this commit runs
 * on the main thread (an app that pumps its own events there, as the
 * cube_*_macos apps do). The image is re-rasterised only when the cursor
 * object changes.
 */
bool
comp_cursor_look_platform_mirror(struct comp_cursor_look *look, float backing_scale)
{
	if (![NSThread isMainThread]) {
		return false;
	}
	NSCursor *cur = [NSCursor currentCursor];
	if (cur == nil || cur.image == nil) {
		return false;
	}
	if ((void *)cur == look->os_cursor && look->rgba != NULL) {
		return true;
	}

	NSImage *img = cur.image;
	const NSSize sz = img.size;     // points
	const NSPoint hs = cur.hotSpot; // points, origin top-left
	const CGFloat scale = backing_scale > 0.0f ? backing_scale : comp_cursor_look_platform_backing_scale();
	const uint32_t w = (uint32_t)ceil(sz.width * scale);
	const uint32_t h = (uint32_t)ceil(sz.height * scale);
	if (sz.width <= 0.0 || sz.height <= 0.0 || w == 0 || h == 0 || w > 512 || h > 512) {
		return false;
	}
	uint8_t *rgba = calloc((size_t)w * h, 4);
	CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGContextRef ctx = (rgba != NULL && cs != NULL)
	                       ? CGBitmapContextCreate(rgba, w, h, 8, (size_t)w * 4, cs,
	                                               kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big)
	                       : NULL;
	bool ok = false;
	if (ctx != NULL) {
		// Bitmap-context memory holds the TOP row first, so the image is
		// top-left-origin, as the shared look promises.
		NSGraphicsContext *gc = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
		[NSGraphicsContext saveGraphicsState];
		[NSGraphicsContext setCurrentContext:gc];
		[img drawInRect:NSMakeRect(0, 0, w, h)
		       fromRect:NSZeroRect
		      operation:NSCompositingOperationCopy
		       fraction:1.0];
		[NSGraphicsContext restoreGraphicsState];
		CGContextFlush(ctx);
		CGContextRelease(ctx);
		ok = true;
	}
	if (cs != NULL) {
		CGColorSpaceRelease(cs);
	}
	if (!ok) {
		free(rgba);
		return false;
	}

	free(look->rgba);
	look->rgba = rgba;
	look->w = w;
	look->h = h;
	if (look->os_cursor != NULL) {
		[(NSCursor *)look->os_cursor release];
	}
	look->os_cursor = [cur retain];
	look->aspect = (float)(sz.width / sz.height);
	look->hot_u = (float)(hs.x / sz.width);
	look->hot_v = (float)(hs.y / sz.height);
	look->height_pt = (float)sz.height;
	look->generation++;
	// Not per frame: only when the app's cursor object changes.
	U_LOG_W("cursor_draw: default look = the current OS cursor (%.0fx%.0f pt, hotspot %.1f,%.1f%s)", sz.width,
	        sz.height, hs.x, hs.y, cur == [NSCursor arrowCursor] ? ", arrowCursor" : "");
	return true;
}
