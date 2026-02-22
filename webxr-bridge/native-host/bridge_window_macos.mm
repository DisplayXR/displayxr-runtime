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
