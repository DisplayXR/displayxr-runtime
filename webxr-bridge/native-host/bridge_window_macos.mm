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
    NSWindow *window = [[NSWindow alloc]
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
