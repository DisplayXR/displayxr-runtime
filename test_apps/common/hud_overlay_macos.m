// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Reusable HUD overlay NSView for macOS test apps.
 */

#import "hud_overlay_macos.h"

@implementation HudOverlayView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) { _hudText = @""; [self setWantsLayer:YES]; }
    return self;
}
- (BOOL)isOpaque { return NO; }
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (_hudText.length == 0) return;
    NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:6 yRadius:6];
    [[NSColor colorWithCalibratedRed:0 green:0 blue:0 alpha:0.5] setFill];
    [bg fill];
    NSFont *font = [NSFont fontWithName:@"Menlo" size:11];
    if (!font) font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    NSDictionary *attrs = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:0.9 green:0.9 blue:0.9 alpha:1.0]
    };
    NSRect textRect = NSInsetRect(self.bounds, 8, 8);
    [_hudText drawWithRect:textRect
                   options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
                attributes:attrs context:nil];
}
@end
