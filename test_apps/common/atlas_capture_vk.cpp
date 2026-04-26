// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan host-readback path for the 'I' key atlas capture.
 *
 * Lifted from demos/gaussian_splatting_handle_vk_win/main.cpp:282-398 verbatim
 * so behavior matches the Gaussian demo we ported from. Blocks on queue idle
 * — this is a one-shot user action, not a per-frame path.
 */

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "stb_image_write.h"
#include "atlas_capture.h"

namespace dxr_capture {

bool CaptureAtlasRegionVk(VkDevice device,
                          VkPhysicalDevice physDev,
                          VkQueue queue,
                          VkCommandPool cmdPool,
                          VkImage srcImage,
                          int srcFormat,
                          uint32_t srcImageWidth,
                          uint32_t srcImageHeight,
                          uint32_t rectX,
                          uint32_t rectY,
                          uint32_t rectW,
                          uint32_t rectH,
                          const std::string& outPath,
                          bool linearBytesInSrgbImage) {
    if (rectW == 0 || rectH == 0) return false;
    if ((uint64_t)rectX + rectW > srcImageWidth) return false;
    if ((uint64_t)rectY + rectH > srcImageHeight) return false;

    VkDeviceSize bufBytes = (VkDeviceSize)rectW * rectH * 4;

    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bufBytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer stagingBuf;
    if (vkCreateBuffer(device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, stagingBuf, &memReq);
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
    uint32_t memType = UINT32_MAX;
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & want) == want) {
            memType = i; break;
        }
    }
    if (memType == UINT32_MAX) {
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return false;
    }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = memReq.size;
    ai.memoryTypeIndex = memType;
    VkDeviceMemory stagingMem;
    if (vkAllocateMemory(device, &ai, nullptr, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(device, stagingBuf, stagingMem, 0);

    VkCommandBufferAllocateInfo cba = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cba.commandPool = cmdPool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cba, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier toSrc = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {(int32_t)rectX, (int32_t)rectY, 0};
    region.imageExtent = {rectW, rectH, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1, &region);

    VkImageMemoryBarrier toAtt = toSrc;
    toAtt.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toAtt.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAtt.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toAtt.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toAtt);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);

    void* mapped = nullptr;
    vkMapMemory(device, stagingMem, 0, bufBytes, 0, &mapped);
    std::vector<uint8_t> rgba((size_t)bufBytes);
    std::memcpy(rgba.data(), mapped, (size_t)bufBytes);
    vkUnmapMemory(device, stagingMem);

    bool isBgr = (srcFormat == (int)VK_FORMAT_B8G8R8A8_UNORM ||
                  srcFormat == (int)VK_FORMAT_B8G8R8A8_SRGB);
    if (isBgr) {
        for (size_t i = 0; i < rgba.size(); i += 4) {
            std::swap(rgba[i + 0], rgba[i + 2]);
        }
    }

    // For compute-written sRGB swapchains, the raw bytes we just read back
    // via vkCmdCopyImageToBuffer are linear values (compute `imageStore`
    // skips automatic format encoding even on sRGB storage images, per
    // Vulkan spec). The runtime's compositor/display chain effectively
    // applies `srgbToLinear` to those bytes for screen presentation, so we
    // mirror that step here so the captured PNG matches what the user sees.
    // Render-pass-based callers (the cube_handle_vk_* apps) have bytes that
    // are already correctly encoded on store — they pass false (default)
    // and skip this branch.
    const bool isSrgb = (srcFormat == (int)VK_FORMAT_R8G8B8A8_SRGB ||
                         srcFormat == (int)VK_FORMAT_B8G8R8A8_SRGB ||
                         srcFormat == (int)VK_FORMAT_A8B8G8R8_SRGB_PACK32);
    if (isSrgb && linearBytesInSrgbImage) {
        // Build a 256-entry srgbToLinear LUT once (fast for ~MP captures).
        uint8_t lut[256];
        for (int i = 0; i < 256; i++) {
            double c = i / 255.0;
            double lin = (c <= 0.04045) ? (c / 12.92)
                                        : std::pow((c + 0.055) / 1.055, 2.4);
            int v = (int)(lin * 255.0 + 0.5);
            lut[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i + 0] = lut[rgba[i + 0]];
            rgba[i + 1] = lut[rgba[i + 1]];
            rgba[i + 2] = lut[rgba[i + 2]];
            // alpha stays linear
        }
    }

    int ok = stbi_write_png(outPath.c_str(), (int)rectW, (int)rectH, 4,
                            rgba.data(), (int)rectW * 4);
    vkDestroyBuffer(device, stagingBuf, nullptr);
    vkFreeMemory(device, stagingMem, nullptr);
    return ok != 0;
}

}  // namespace dxr_capture
