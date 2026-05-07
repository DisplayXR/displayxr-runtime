// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "hud_renderer_macos.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>

static NSString* WStringToNSString(const std::wstring& w)
{
    if (w.empty()) return @"";
    // wchar_t on macOS is 4 bytes; convert via UTF-32LE.
    NSData* data = [NSData dataWithBytes:w.data() length:w.size() * sizeof(wchar_t)];
    NSString* s = [[NSString alloc] initWithData:data encoding:NSUTF32LittleEndianStringEncoding];
    return s ?: @"";
}

static int CountLines(const std::wstring& text)
{
    if (text.empty()) return 0;
    int n = 1;
    for (wchar_t c : text) if (c == L'\n') n++;
    return n;
}

bool InitializeHudRenderer(HudRendererMacOS& hud, uint32_t w, uint32_t h, uint32_t fontBaseHeight)
{
    hud.width = w;
    hud.height = h;

    // Match the Windows scale base (470px → 20pt / 15pt) so HUD pixel sizes
    // are identical across platforms.
    uint32_t scaleH = (fontBaseHeight > 0) ? fontBaseHeight : h;
    float fontScale = scaleH / 470.0f;
    hud.normalFontSize = 20.0f * fontScale;
    hud.smallFontSize = 15.0f * fontScale;

    size_t rowBytes = (size_t)w * 4;
    hud.bitmapData = calloc((size_t)h, rowBytes);
    if (!hud.bitmapData) return false;

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
        hud.bitmapData,
        w, h, 8, rowBytes,
        cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        free(hud.bitmapData);
        hud.bitmapData = nullptr;
        return false;
    }
    hud.cgContext = (void*)CFRetain(ctx);
    CGContextRelease(ctx);

    NSFont* nsNormal = [NSFont fontWithName:@"Menlo" size:hud.normalFontSize];
    if (!nsNormal) nsNormal = [NSFont monospacedSystemFontOfSize:hud.normalFontSize weight:NSFontWeightRegular];
    NSFont* nsSmall = [NSFont fontWithName:@"Menlo" size:hud.smallFontSize];
    if (!nsSmall) nsSmall = [NSFont monospacedSystemFontOfSize:hud.smallFontSize weight:NSFontWeightRegular];
    hud.normalFont = (void*)CFRetain((__bridge CTFontRef)nsNormal);
    hud.smallFont = (void*)CFRetain((__bridge CTFontRef)nsSmall);

    return true;
}

static void DrawAttributedTextBox(CGContextRef ctx, CTFontRef font,
                                  NSString* text, CGRect rect)
{
    if (text.length == 0) return;
    NSDictionary* attrs = @{
        (NSString*)kCTFontAttributeName: (__bridge id)font,
        (NSString*)kCTForegroundColorAttributeName:
            (__bridge id)[NSColor colorWithSRGBRed:0.95 green:0.95 blue:0.95 alpha:1.0].CGColor,
    };
    NSAttributedString* attr = [[NSAttributedString alloc] initWithString:text attributes:attrs];
    CTFramesetterRef fs = CTFramesetterCreateWithAttributedString((CFAttributedStringRef)attr);
    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddRect(path, NULL, rect);
    CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, attr.length), path, NULL);
    // The caller flips Y on the CTM for top-down layout; CT rasterizes
    // glyphs through the text matrix × CTM, so an identity text matrix
    // would render each glyph upside-down. Pre-flip the text matrix on Y
    // so the two cancel and glyphs come out upright.
    CGAffineTransform savedTextMatrix = CGContextGetTextMatrix(ctx);
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1.0, -1.0));
    CTFrameDraw(frame, ctx);
    CGContextSetTextMatrix(ctx, savedTextMatrix);
    CFRelease(frame);
    CGPathRelease(path);
    CFRelease(fs);
}

static void DrawRoundedRect(CGContextRef ctx, CGRect r, CGFloat radius,
                            CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha)
{
    CGPathRef path = CGPathCreateWithRoundedRect(r, radius, radius, NULL);
    CGContextSetRGBFillColor(ctx, red, green, blue, alpha);
    CGContextAddPath(ctx, path);
    CGContextFillPath(ctx);
    CGPathRelease(path);
}

const void* RenderHudAndMap(HudRendererMacOS& hud, uint32_t* rowPitch,
    const std::wstring& sessionText, const std::wstring& modeText,
    const std::wstring& perfText, const std::wstring& displayInfoText,
    const std::wstring& eyeText,
    const std::wstring& cameraText,
    const std::wstring& stereoText,
    const std::wstring& helpText,
    const std::vector<HudButtonMacOS>& buttons,
    bool drawBody,
    bool bodyAtBottom)
{
    CGContextRef ctx = (CGContextRef)hud.cgContext;
    if (!ctx) return nullptr;

    // Clear (fully transparent in bodyAtBottom mode; translucent black backdrop
    // otherwise — matches the Windows HudRenderer behavior).
    size_t rowBytes = (size_t)hud.width * 4;
    if (bodyAtBottom || !drawBody) {
        memset(hud.bitmapData, 0, rowBytes * hud.height);
    } else {
        memset(hud.bitmapData, 0, rowBytes * hud.height);
        CGContextSaveGState(ctx);
        CGContextSetRGBFillColor(ctx, 0.0, 0.0, 0.0, 0.5);
        CGContextFillRect(ctx, CGRectMake(0, 0, hud.width, hud.height));
        CGContextRestoreGState(ctx);
    }

    CTFontRef normalFont = (CTFontRef)hud.normalFont;
    CTFontRef smallFont = (CTFontRef)hud.smallFont;

    float normalLineH = hud.normalFontSize * 1.4f;
    float smallLineH = hud.smallFontSize * 1.4f;
    float padX = 12.0f * (hud.width / 380.0f);
    float textW = (float)hud.width - 2.0f * padX;
    float gap = smallLineH * 0.3f;

    struct Section { const std::wstring& text; bool isSmall; };
    Section sections[] = {
        {sessionText, false},
        {modeText, true},
        {perfText, true},
        {displayInfoText, true},
        {eyeText, true},
        {cameraText, true},
        {stereoText, true},
        {helpText, true},
    };

    if (drawBody) {
        // Reserve top of the texture for buttons (if any) so body text
        // doesn't overlap them.
        float buttonBandBottom = 0.0f;
        for (const auto& b : buttons) {
            float bb = b.y + b.height;
            if (bb > buttonBandBottom) buttonBandBottom = bb;
        }

        float bodyH = 0.0f;
        bool firstSec = true;
        for (const auto& s : sections) {
            if (s.text.empty()) continue;
            float lh = s.isSmall ? smallLineH : normalLineH;
            bodyH += lh * CountLines(s.text);
            if (!firstSec) bodyH += gap;
            firstSec = false;
        }

        // Core Graphics origin is bottom-left; convert top-down layout to
        // bottom-up by flipping the Y axis with a transform around the bitmap.
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, 0, hud.height);
        CGContextScaleCTM(ctx, 1.0, -1.0);

        float yTop;
        if (bodyAtBottom) {
            float bottomPad = gap * 1.5f;
            yTop = (float)hud.height - bodyH - bottomPad;
            float topLimit = (buttonBandBottom > 0.0f) ? (buttonBandBottom + gap) : gap;
            if (yTop < topLimit) yTop = topLimit;
            float pad = gap;
            // Tight backdrop behind body region only.
            // Draw in flipped (top-down) coords; CGContextFillRect uses the
            // current CTM so the rect is interpreted top-down here too.
            DrawRoundedRect(ctx,
                CGRectMake(padX - pad, yTop - pad, textW + 2.0f * pad, bodyH + 2.0f * pad),
                8.0f, 0.0, 0.0, 0.0, 0.5);
        } else {
            yTop = (buttonBandBottom > 0.0f) ? (buttonBandBottom + gap) : gap;
        }

        for (const auto& s : sections) {
            if (s.text.empty()) continue;
            float lh = s.isSmall ? smallLineH : normalLineH;
            int lineCount = CountLines(s.text);
            float h = lh * lineCount;
            // Core Text draws bottom-up within the rect; we've flipped the
            // CTM so rect Y is top-down. Pad the rect height a touch so the
            // first line isn't clipped against the top edge of the box.
            CTFontRef f = s.isSmall ? smallFont : normalFont;
            DrawAttributedTextBox(ctx, f,
                WStringToNSString(s.text),
                CGRectMake(padX, yTop, textW, h));
            yTop += h + gap;
        }

        CGContextRestoreGState(ctx);
    }

    // Buttons drawn last so they sit on top of any backdrop.
    if (!buttons.empty()) {
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, 0, hud.height);
        CGContextScaleCTM(ctx, 1.0, -1.0);
        for (const auto& b : buttons) {
            CGFloat alpha = b.hovered ? 0.85 : 0.65;
            DrawRoundedRect(ctx, CGRectMake(b.x, b.y, b.width, b.height),
                            6.0f, 0.15, 0.15, 0.15, alpha);
            DrawAttributedTextBox(ctx, smallFont,
                WStringToNSString(b.label),
                CGRectMake(b.x + 6, b.y + 4, b.width - 12, b.height - 4));
        }
        CGContextRestoreGState(ctx);
    }

    if (rowPitch) *rowPitch = (uint32_t)((size_t)hud.width * 4);
    return hud.bitmapData;
}

void UnmapHud(HudRendererMacOS& hud)
{
    (void)hud;  // No GPU staging round-trip needed.
}

void CleanupHudRenderer(HudRendererMacOS& hud)
{
    if (hud.normalFont) { CFRelease((CTFontRef)hud.normalFont); hud.normalFont = nullptr; }
    if (hud.smallFont) { CFRelease((CTFontRef)hud.smallFont); hud.smallFont = nullptr; }
    if (hud.cgContext) { CFRelease((CGContextRef)hud.cgContext); hud.cgContext = nullptr; }
    if (hud.bitmapData) { free(hud.bitmapData); hud.bitmapData = nullptr; }
    hud.width = hud.height = 0;
}
