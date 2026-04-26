// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metal host-readback path for the 'I' key atlas capture.
 *
 * Allocates a shared (CPU+GPU visible) MTLBuffer, encodes a blit
 * copyFromTexture:toBuffer: for the requested sub-rect, commits and waits
 * for completion, then walks the bytes into RGBA (swapping BGRA→RGBA if
 * needed) and writes a PNG via stb_image_write.
 */

#import <Metal/Metal.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "stb_image_write.h"
#include "atlas_capture.h"

namespace dxr_capture {

namespace {

bool IsBgra(MTLPixelFormat f) {
    return f == MTLPixelFormatBGRA8Unorm ||
           f == MTLPixelFormatBGRA8Unorm_sRGB;
}

}  // namespace

bool CaptureAtlasRegionMetal(void* devicePtr,
                             void* queuePtr,
                             void* srcTexPtr,
                             uint32_t rectX,
                             uint32_t rectY,
                             uint32_t rectW,
                             uint32_t rectH,
                             const std::string& outPath) {
    id<MTLDevice>       device = (__bridge id<MTLDevice>)devicePtr;
    id<MTLCommandQueue> queue  = (__bridge id<MTLCommandQueue>)queuePtr;
    id<MTLTexture>      srcTex = (__bridge id<MTLTexture>)srcTexPtr;
    if (device == nil || queue == nil || srcTex == nil) return false;
    if (rectW == 0 || rectH == 0) return false;
    if ((uint64_t)rectX + rectW > srcTex.width) return false;
    if ((uint64_t)rectY + rectH > srcTex.height) return false;

    const NSUInteger rowBytes = (NSUInteger)rectW * 4u;
    const NSUInteger bufBytes = rowBytes * rectH;

    id<MTLBuffer> buf = [device newBufferWithLength:bufBytes
                                            options:MTLResourceStorageModeShared];
    if (buf == nil) return false;

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromTexture:srcTex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(rectX, rectY, 0)
               sourceSize:MTLSizeMake(rectW, rectH, 1)
                 toBuffer:buf
        destinationOffset:0
   destinationBytesPerRow:rowBytes
 destinationBytesPerImage:bufBytes];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];

    std::vector<uint8_t> rgba((size_t)bufBytes);
    std::memcpy(rgba.data(), [buf contents], (size_t)bufBytes);

    if (IsBgra(srcTex.pixelFormat)) {
        for (size_t i = 0; i < rgba.size(); i += 4) {
            std::swap(rgba[i + 0], rgba[i + 2]);
        }
    }

    int ok = stbi_write_png(outPath.c_str(), (int)rectW, (int)rectH, 4,
                            rgba.data(), (int)rowBytes);
    return ok != 0;
}

}  // namespace dxr_capture
