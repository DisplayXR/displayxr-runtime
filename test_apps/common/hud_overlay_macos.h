// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Reusable HUD overlay NSView for macOS test apps.
 */

#import <Cocoa/Cocoa.h>

@interface HudOverlayView : NSView
@property (nonatomic, copy) NSString *hudText;
@end
