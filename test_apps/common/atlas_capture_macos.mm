// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS-side cross-platform helpers for atlas capture: filename /
 *         output-path resolution, white-flash overlay, and the (single)
 *         stb_image_write implementation TU for macOS targets.
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

// Single STB implementation for macOS-linked targets. Windows has its own
// in atlas_capture.cpp — apps link exactly one of the two.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "atlas_capture.h"

namespace dxr_capture {

// ---------------------------------------------------------------------------
// Output path / filename helpers
// ---------------------------------------------------------------------------

std::string PicturesDirectory() {
    NSArray<NSString *> *picsArr = NSSearchPathForDirectoriesInDomains(
        NSPicturesDirectory, NSUserDomainMask, YES);
    NSString *pics = [picsArr firstObject];
    if (pics == nil) return "";
    NSString *dxrDir = [pics stringByAppendingPathComponent:@"DisplayXR"];
    NSError *err = nil;
    [[NSFileManager defaultManager] createDirectoryAtPath:dxrDir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:&err];
    if (err != nil) return "";
    return [dxrDir UTF8String];
}

int NextCaptureNum(const std::string& dir,
                   const std::string& stem,
                   uint32_t cols, uint32_t rows) {
    if (dir.empty()) return 1;
    char suffixBuf[64];
    snprintf(suffixBuf, sizeof(suffixBuf), "_%ux%u.png", cols, rows);
    std::string prefix = stem + "-";
    std::string suffix = suffixBuf;
    int maxNum = 0;
    NSError *err = nil;
    NSString *dirNs = [NSString stringWithUTF8String:dir.c_str()];
    NSArray<NSString *> *files = [[NSFileManager defaultManager]
        contentsOfDirectoryAtPath:dirNs error:&err];
    if (err != nil || files == nil) return 1;
    for (NSString *f in files) {
        std::string fname = [f UTF8String];
        if (fname.size() <= prefix.size() + suffix.size()) continue;
        if (fname.compare(0, prefix.size(), prefix) != 0) continue;
        size_t suffixStart = fname.size() - suffix.size();
        if (fname.compare(suffixStart, suffix.size(), suffix) != 0) continue;
        std::string numStr = fname.substr(prefix.size(),
                                          suffixStart - prefix.size());
        if (numStr.empty()) continue;
        bool allDigits = true;
        for (char c : numStr) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false; break;
            }
        }
        if (!allDigits) continue;
        int n = std::atoi(numStr.c_str());
        if (n > maxNum) maxNum = n;
    }
    return maxNum + 1;
}

std::string MakeCapturePath(const std::string& stem,
                            uint32_t cols, uint32_t rows) {
    std::string dir = PicturesDirectory();
    int n = NextCaptureNum(dir, stem, cols, rows);
    char tail[256];
    snprintf(tail, sizeof(tail), "%s-%d_%ux%u.png",
             stem.c_str(), n, cols, rows);
    return dir.empty() ? std::string(tail) : (dir + "/" + tail);
}

// ---------------------------------------------------------------------------
// White-flash overlay (NSView + Core Animation, fades out over ~250 ms)
// ---------------------------------------------------------------------------

namespace {
NSView * _Nullable g_flashView = nil;  // process-lifetime
}  // namespace

void TriggerCaptureFlash(void* nsviewBridged) {
    NSView *parent = (__bridge NSView *)nsviewBridged;
    if (parent == nil) return;
    // AppKit + Core Animation must run on the main queue.
    dispatch_block_t block = ^{
        // Recreate the flash view from scratch each time. Reusing the same
        // NSView across multiple flashes left stale CALayer animation state
        // around, which made every flash after the first one invisible.
        if (g_flashView != nil) {
            [g_flashView removeFromSuperview];
            g_flashView = nil;
        }
        g_flashView = [[NSView alloc] initWithFrame:[parent bounds]];
        [g_flashView setWantsLayer:YES];
        g_flashView.layer.backgroundColor = [[NSColor whiteColor] CGColor];
        [g_flashView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        g_flashView.alphaValue = 1.0;
        [parent addSubview:g_flashView];

        NSView *fadeView = g_flashView;  // capture for completion block
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.25;
            ctx.timingFunction = [CAMediaTimingFunction
                functionWithName:kCAMediaTimingFunctionEaseOut];
            fadeView.animator.alphaValue = 0.0;
        } completionHandler:^{
            [fadeView removeFromSuperview];
            if (g_flashView == fadeView) g_flashView = nil;
        }];
    };
    if ([NSThread isMainThread]) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}

}  // namespace dxr_capture
