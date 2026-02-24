// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Creates a hidden NSWindow + CAMetalLayer-backed NSView for
 *         XR_EXT_macos_window_binding. This triggers Monado's per-session
 *         rendering path so no popup window is created.
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

@interface BridgeMetalView : NSView
@end

@implementation BridgeMetalView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
@end

// Overlay window that refuses keyboard focus — prevents TAB from stealing
// focus away from the browser and causing the overlay to hide.
@interface BridgeOverlayWindow : NSWindow
@end

@implementation BridgeOverlayWindow
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

// ============================================================================
// Overlay window — borderless, click-through, floating, tracks browser canvas
// ============================================================================

struct BridgeOverlay {
    NSWindow *window;
    BridgeMetalView *view;
};

extern "C" BridgeOverlay* bridge_overlay_create(uint32_t w, uint32_t h, void** outViewHandle) {
    [NSApplication sharedApplication];

    NSRect frame = NSMakeRect(0, 0, w, h);
    NSWindow *window = [[BridgeOverlayWindow alloc]
        initWithContentRect:frame
        styleMask:NSWindowStyleMaskBorderless
        backing:NSBackingStoreBuffered
        defer:NO];
    [window setReleasedWhenClosed:NO];
    [window setIgnoresMouseEvents:YES];
    [window setLevel:NSFloatingWindowLevel];
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
    [window setCollectionBehavior:NSWindowCollectionBehaviorStationary |
                                  NSWindowCollectionBehaviorCanJoinAllSpaces |
                                  NSWindowCollectionBehaviorFullScreenAuxiliary];
    // Start hidden — shown after first canvasRect arrives
    [window orderOut:nil];

    BridgeMetalView *view = [[BridgeMetalView alloc] initWithFrame:frame];
    view.wantsLayer = YES;
    window.contentView = view;

    BridgeOverlay *ov = (BridgeOverlay*)calloc(1, sizeof(BridgeOverlay));
    ov->window = window;
    ov->view = view;
    *outViewHandle = (__bridge_retained void *)view;
    return ov;
}

extern "C" void bridge_overlay_set_frame(BridgeOverlay* ov, double x, double y, double w, double h) {
    if (!ov) return;
    NSWindow *win = ov->window;
    NSRect rect = NSMakeRect(x, y, w, h);
    dispatch_async(dispatch_get_main_queue(), ^{
        [win setFrame:rect display:YES];
    });
}

extern "C" void bridge_overlay_set_visible(BridgeOverlay* ov, bool visible) {
    if (!ov) return;
    NSWindow *win = ov->window;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (visible) {
            [win orderFront:nil];
        } else {
            [win orderOut:nil];
        }
    });
}

extern "C" void bridge_overlay_destroy(BridgeOverlay* ov) {
    if (!ov) return;
    NSWindow *win = ov->window;
    dispatch_async(dispatch_get_main_queue(), ^{
        [win close];
    });
    free(ov);
}

// ============================================================================
// Utility — screen height for coordinate conversion
// ============================================================================

extern "C" double bridge_get_main_screen_height(void) {
    return [[NSScreen mainScreen] frame].size.height;
}

// ============================================================================
// HUD text rendering — CoreGraphics to RGBA buffer
// ============================================================================

extern "C" void bridge_render_hud_text(
    const char* text,
    uint8_t* buffer,
    uint32_t width,
    uint32_t height)
{
    @autoreleasepool {
        // Clear buffer to fully transparent
        memset(buffer, 0, (size_t)width * height * 4);

        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(
            buffer, width, height, 8, width * 4, colorSpace,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(colorSpace);
        if (!ctx) return;

        // Flip coordinate system (CoreGraphics is bottom-up, we want top-down)
        CGContextTranslateCTM(ctx, 0, height);
        CGContextScaleCTM(ctx, 1, -1);

        // Draw semi-transparent black rounded rect background
        CGRect bgRect = CGRectMake(4, 4, width - 8, height - 8);
        CGPathRef path = CGPathCreateWithRoundedRect(bgRect, 6, 6, NULL);
        CGContextSetRGBFillColor(ctx, 0, 0, 0, 0.6);
        CGContextAddPath(ctx, path);
        CGContextFillPath(ctx);
        CGPathRelease(path);

        // Draw text using NSString/NSGraphicsContext
        NSGraphicsContext *nsCtx = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:YES];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:nsCtx];

        NSFont *font = [NSFont fontWithName:@"Menlo" size:11];
        if (!font) font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        NSDictionary *attrs = @{
            NSFontAttributeName: font,
            NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:0.9 green:0.9 blue:0.9 alpha:1.0]
        };

        NSString *str = [NSString stringWithUTF8String:text];
        NSRect textRect = NSMakeRect(10, 10, width - 20, height - 20);
        [str drawWithRect:textRect
                  options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
               attributes:attrs
                  context:nil];

        [NSGraphicsContext restoreGraphicsState];
        CGContextRelease(ctx);

        // Convert premultiplied alpha → straight alpha for OpenXR compositor
        for (uint32_t i = 0; i < width * height; i++) {
            uint8_t *px = buffer + i * 4;
            uint8_t a = px[3];
            if (a > 0 && a < 255) {
                px[0] = (uint8_t)((uint16_t)px[0] * 255 / a);
                px[1] = (uint8_t)((uint16_t)px[1] * 255 / a);
                px[2] = (uint8_t)((uint16_t)px[2] * 255 / a);
            }
        }
    }
}

// ============================================================================
// Hidden Metal view — for default and readback modes
// ============================================================================

extern "C" void* bridge_create_hidden_metal_view(uint32_t w, uint32_t h) {
    // Ensure NSApplication is initialized (needed for NSWindow)
    [NSApplication sharedApplication];

    NSRect frame = NSMakeRect(0, 0, w, h);
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled
        backing:NSBackingStoreBuffered
        defer:NO];
    [window setReleasedWhenClosed:NO];
    // Don't show the window — it stays hidden

    BridgeMetalView *view = [[BridgeMetalView alloc] initWithFrame:frame];
    view.wantsLayer = YES;
    window.contentView = view;

    return (__bridge_retained void *)view;
}
