// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS Vulkan OpenXR spinning cube with external window binding
 *
 * Demonstrates XR_EXT_macos_window_binding: the app creates its own
 * NSWindow + MetalView (with CAMetalLayer) and passes the NSView to
 * the runtime via the extension. The runtime renders into the app's
 * window instead of creating its own.
 *
 * Key differences from sim_cube_openxr:
 * - App creates and owns the NSWindow
 * - NSView with CAMetalLayer is passed to runtime via extension
 * - App runs the NSApplication event loop
 * - Keyboard input: ESC to quit, 1/2/3 for sim_display modes
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_macos_window_binding.h>

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <vector>

#include <unistd.h>

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call) \
    do { \
        XrResult _r = (call); \
        if (XR_FAILED(_r)) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

#define VK_CHECK(call) \
    do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static NSWindow *g_window = nil;
static NSView *g_metalView = nil;

// ============================================================================
// Inline math — column-major float[16] matrices
// ============================================================================

static void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            tmp[col * 4 + row] = sum;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void mat4_translation(float* m, float tx, float ty, float tz) {
    mat4_identity(m);
    m[12] = tx;
    m[13] = ty;
    m[14] = tz;
}

static void mat4_rotation_y(float* m, float angle) {
    mat4_identity(m);
    float c = cosf(angle), s = sinf(angle);
    m[0] = c;   m[8] = s;
    m[2] = -s;  m[10] = c;
}

static void mat4_from_xr_fov(float* m, XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft);
    float tanR = tanf(fov.angleRight);
    float tanU = tanf(fov.angleUp);
    float tanD = tanf(fov.angleDown);
    float w = tanR - tanL;
    float h = tanU - tanD;
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (tanR + tanL) / w;
    m[9]  = (tanU + tanD) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float* viewMat, XrPosef pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;

    float rot[16];
    mat4_identity(rot);
    rot[0]  = 1 - 2*(qy*qy + qz*qz);
    rot[1]  = 2*(qx*qy + qz*qw);
    rot[2]  = 2*(qx*qz - qy*qw);
    rot[4]  = 2*(qx*qy - qz*qw);
    rot[5]  = 1 - 2*(qx*qx + qz*qz);
    rot[6]  = 2*(qy*qz + qx*qw);
    rot[8]  = 2*(qx*qz + qy*qw);
    rot[9]  = 2*(qy*qz - qx*qw);
    rot[10] = 1 - 2*(qx*qx + qy*qy);

    // Transpose rotation (inverse of orthonormal)
    float invRot[16];
    mat4_identity(invRot);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            invRot[j*4+i] = rot[i*4+j];

    float invTrans[16];
    mat4_translation(invTrans, -pose.position.x, -pose.position.y, -pose.position.z);
    mat4_multiply(viewMat, invRot, invTrans);
}

// ============================================================================
// SPIR-V shaders (embedded)
// ============================================================================
// Cube vertex shader: push constant MVP, per-vertex color
static const uint32_t cubeVertSpv[] = {
    0x07230203,0x00010000,0x000d000a,0x00000028,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0009000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000d,0x00000012,0x0000001c,
    0x00000025,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,
    0x00060005,0x0000000b,0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,0x0000000b,
    0x00000000,0x505f6c67,0x7469736f,0x006e6f69,0x00030005,0x0000000d,0x00000000,0x00050005,
    0x0000000f,0x68737550,0x736e6f43,0x00000074,0x00040006,0x0000000f,0x00000000,0x0070766d,
    0x00030005,0x00000011,0x00006370,0x00040005,0x00000012,0x6f506e69,0x00000073,0x00050005,
    0x0000001c,0x4374756f,0x726f6c6f,0x00000000,0x00040005,0x00000025,0x6f436e69,0x00726f6c,
    0x00050048,0x0000000b,0x00000000,0x0000000b,0x00000000,0x00030047,0x0000000b,0x00000002,
    0x00040048,0x0000000f,0x00000000,0x00000005,0x00050048,0x0000000f,0x00000000,0x00000023,
    0x00000000,0x00050048,0x0000000f,0x00000000,0x00000007,0x00000010,0x00030047,0x0000000f,
    0x00000002,0x00040047,0x00000012,0x0000001e,0x00000000,0x00040047,0x0000001c,0x0000001e,
    0x00000000,0x00040047,0x00000025,0x0000001e,0x00000001,0x00020013,0x00000002,0x00030021,
    0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,
    0x00000004,0x0003001e,0x0000000b,0x00000007,0x00040020,0x0000000c,0x00000003,0x0000000b,
    0x0004003b,0x0000000c,0x0000000d,0x00000003,0x00040015,0x0000000e,0x00000020,0x00000001,
    0x00040018,0x00000010,0x00000007,0x00000004,0x0003001e,0x0000000f,0x00000010,0x00040020,
    0x00000014,0x00000009,0x0000000f,0x0004003b,0x00000014,0x00000011,0x00000009,0x0004002b,
    0x0000000e,0x00000013,0x00000000,0x00040020,0x00000015,0x00000009,0x00000010,0x00040017,
    0x00000018,0x00000006,0x00000003,0x00040020,0x00000019,0x00000001,0x00000018,0x0004003b,
    0x00000019,0x00000012,0x00000001,0x0004002b,0x00000006,0x0000001a,0x3f800000,0x00040020,
    0x0000001b,0x00000003,0x00000007,0x0004003b,0x0000001b,0x0000001c,0x00000003,0x00040020,
    0x00000024,0x00000001,0x00000007,0x0004003b,0x00000024,0x00000025,0x00000001,0x00050036,
    0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x00000015,
    0x00000016,0x00000011,0x00000013,0x0004003d,0x00000010,0x00000017,0x00000016,0x0004003d,
    0x00000018,0x0000001d,0x00000012,0x00050051,0x00000006,0x0000001e,0x0000001d,0x00000000,
    0x00050051,0x00000006,0x0000001f,0x0000001d,0x00000001,0x00050051,0x00000006,0x00000020,
    0x0000001d,0x00000002,0x00070050,0x00000007,0x00000021,0x0000001e,0x0000001f,0x00000020,
    0x0000001a,0x00050091,0x00000007,0x00000022,0x00000017,0x00000021,0x00050041,0x0000001b,
    0x00000023,0x0000000d,0x00000013,0x0003003e,0x00000023,0x00000022,0x0004003d,0x00000007,
    0x00000026,0x00000025,0x0003003e,0x0000001c,0x00000026,0x000100fd,0x00010038,
};

// Cube fragment shader: pass-through color
static const uint32_t cubeFragSpv[] = {
    0x07230203,0x00010000,0x000d000a,0x0000000d,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x0000000b,0x00030010,
    0x00000004,0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
    0x00000000,0x00050005,0x00000009,0x4374756f,0x726f6c6f,0x00000000,0x00040005,0x0000000b,
    0x6f436e69,0x00726f6c,0x00040047,0x00000009,0x0000001e,0x00000000,0x00040047,0x0000000b,
    0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,
    0x00000003,0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x00040020,0x0000000a,
    0x00000001,0x00000007,0x0004003b,0x0000000a,0x0000000b,0x00000001,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003d,0x00000007,0x0000000c,
    0x0000000b,0x0003003e,0x00000009,0x0000000c,0x000100fd,0x00010038,
};

// Grid vertex shader: push constant MVP, fixed gray color
static const uint32_t gridVertSpv[] = {
    0x07230203,0x00010000,0x000d000a,0x00000024,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0008000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000d,0x00000012,0x0000001c,
    0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00060005,
    0x0000000b,0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,0x0000000b,0x00000000,
    0x505f6c67,0x7469736f,0x006e6f69,0x00030005,0x0000000d,0x00000000,0x00050005,0x0000000f,
    0x68737550,0x736e6f43,0x00000074,0x00040006,0x0000000f,0x00000000,0x0070766d,0x00030005,
    0x00000011,0x00006370,0x00040005,0x00000012,0x6f506e69,0x00000073,0x00050005,0x0000001c,
    0x4374756f,0x726f6c6f,0x00000000,0x00050048,0x0000000b,0x00000000,0x0000000b,0x00000000,
    0x00030047,0x0000000b,0x00000002,0x00040048,0x0000000f,0x00000000,0x00000005,0x00050048,
    0x0000000f,0x00000000,0x00000023,0x00000000,0x00050048,0x0000000f,0x00000000,0x00000007,
    0x00000010,0x00030047,0x0000000f,0x00000002,0x00040047,0x00000012,0x0000001e,0x00000000,
    0x00040047,0x0000001c,0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,
    0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,
    0x0003001e,0x0000000b,0x00000007,0x00040020,0x0000000c,0x00000003,0x0000000b,0x0004003b,
    0x0000000c,0x0000000d,0x00000003,0x00040015,0x0000000e,0x00000020,0x00000001,0x00040018,
    0x00000010,0x00000007,0x00000004,0x0003001e,0x0000000f,0x00000010,0x00040020,0x00000014,
    0x00000009,0x0000000f,0x0004003b,0x00000014,0x00000011,0x00000009,0x0004002b,0x0000000e,
    0x00000013,0x00000000,0x00040020,0x00000015,0x00000009,0x00000010,0x00040017,0x00000018,
    0x00000006,0x00000003,0x00040020,0x00000019,0x00000001,0x00000018,0x0004003b,0x00000019,
    0x00000012,0x00000001,0x0004002b,0x00000006,0x0000001a,0x3f800000,0x00040020,0x0000001b,
    0x00000003,0x00000007,0x0004003b,0x0000001b,0x0000001c,0x00000003,0x0004002b,0x00000006,
    0x0000001d,0x3e4ccccd,0x0004002b,0x00000006,0x0000001e,0x3e99999a,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x00000015,0x00000016,
    0x00000011,0x00000013,0x0004003d,0x00000010,0x00000017,0x00000016,0x0004003d,0x00000018,
    0x00000020,0x00000012,0x00050051,0x00000006,0x00000021,0x00000020,0x00000000,0x00050051,
    0x00000006,0x00000022,0x00000020,0x00000001,0x00050051,0x00000006,0x00000023,0x00000020,
    0x00000002,0x00070050,0x00000007,0x0000001f,0x00000021,0x00000022,0x00000023,0x0000001a,
    0x00050091,0x00000007,0x00000009,0x00000017,0x0000001f,0x00050041,0x0000001b,0x0000000a,
    0x0000000d,0x00000013,0x0003003e,0x0000000a,0x00000009,0x00070050,0x00000007,0x00000008,
    0x0000001d,0x0000001e,0x0000001d,0x0000001a,0x0003003e,0x0000001c,0x00000008,0x000100fd,
    0x00010038,
};

// Grid fragment shader: pass-through color
static const uint32_t gridFragSpv[] = {
    0x07230203,0x00010000,0x000d000a,0x0000000d,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x0000000b,0x00030010,
    0x00000004,0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
    0x00000000,0x00050005,0x00000009,0x4374756f,0x726f6c6f,0x00000000,0x00040005,0x0000000b,
    0x6f436e69,0x00726f6c,0x00040047,0x00000009,0x0000001e,0x00000000,0x00040047,0x0000000b,
    0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,
    0x00000003,0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x00040020,0x0000000a,
    0x00000001,0x00000007,0x0004003b,0x0000000a,0x0000000b,0x00000001,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003d,0x00000007,0x0000000c,
    0x0000000b,0x0003003e,0x00000009,0x0000000c,0x000100fd,0x00010038,
};

// ============================================================================
// Vulkan renderer structures (same as sim_cube_openxr)
// ============================================================================

struct CubeVertex { float pos[3]; float color[4]; };
struct GridVertex { float pos[3]; };

struct SwapchainFramebuffers {
    std::vector<VkImageView> colorViews;
    std::vector<VkImageView> depthViews;
    std::vector<VkFramebuffer> framebuffers;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
};

struct VkRenderer {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline cubePipeline = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE;
    VkBuffer cubeVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeVertexMemory = VK_NULL_HANDLE;
    VkBuffer cubeIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeIndexMemory = VK_NULL_HANDLE;
    VkBuffer gridVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory gridVertexMemory = VK_NULL_HANDLE;
    int gridVertexCount = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence frameFence = VK_NULL_HANDLE;
    float cubeRotation = 0.0f;
    SwapchainFramebuffers eyeFramebuffers[2];
};

// ============================================================================
// Vulkan helpers (same as sim_cube_openxr)
// ============================================================================

static uint32_t FindMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool CreateBuffer(VkDevice device, VkPhysicalDevice physDevice, VkDeviceSize size,
    VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
    VkBuffer& buffer, VkDeviceMemory& memory)
{
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits, properties);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, buffer, memory, 0);
    return true;
}

static bool CreateDepthImage(VkDevice device, VkPhysicalDevice physDevice,
    uint32_t width, uint32_t height,
    VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, image, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, image, memory, 0);
    return true;
}

// Store physDevice globally for depth image creation
static VkPhysicalDevice g_physDevice = VK_NULL_HANDLE;

static bool CreateSwapchainFramebuffers(VkRenderer& renderer, int eye,
    VkImage* images, uint32_t imageCount,
    uint32_t width, uint32_t height, VkFormat colorFormat)
{
    auto& fb = renderer.eyeFramebuffers[eye];
    fb.colorViews.resize(imageCount);
    fb.depthViews.resize(imageCount);
    fb.framebuffers.resize(imageCount);

    // g_physDevice is set during GetVulkanPhysicalDevice
    VkPhysicalDevice physDevice = g_physDevice;

    if (!CreateDepthImage(renderer.device, physDevice, width, height,
        fb.depthImage, fb.depthMemory)) {
        return false;
    }

    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colorFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(renderer.device, &viewInfo, nullptr, &fb.colorViews[i]) != VK_SUCCESS)
            return false;

        VkImageViewCreateInfo depthViewInfo = {};
        depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depthViewInfo.image = fb.depthImage;
        depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
        depthViewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(renderer.device, &depthViewInfo, nullptr, &fb.depthViews[i]) != VK_SUCCESS)
            return false;

        VkImageView attachments[] = {fb.colorViews[i], fb.depthViews[i]};
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderer.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;
        if (vkCreateFramebuffer(renderer.device, &fbInfo, nullptr, &fb.framebuffers[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

static bool InitializeVkRenderer(VkRenderer& renderer, VkDevice device, VkPhysicalDevice physDevice,
    VkQueue graphicsQueue, uint32_t queueFamilyIndex, VkFormat colorFormat)
{
    renderer.device = device;
    renderer.graphicsQueue = graphicsQueue;
    g_physDevice = physDevice;

    // Render pass
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkRenderPassCreateInfo rpInfo = {};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    VK_CHECK(vkCreateRenderPass(device, &rpInfo, nullptr, &renderer.renderPass));

    // Pipeline layout (push constants for MVP)
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = 64; // 4x4 matrix

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &renderer.pipelineLayout));

    // Shader modules
    auto createShaderModule = [&](const uint32_t* code, size_t size) -> VkShaderModule {
        VkShaderModuleCreateInfo ci = {};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = size;
        ci.pCode = code;
        VkShaderModule sm;
        vkCreateShaderModule(device, &ci, nullptr, &sm);
        return sm;
    };

    VkShaderModule cubeVert = createShaderModule(cubeVertSpv, sizeof(cubeVertSpv));
    VkShaderModule cubeFrag = createShaderModule(cubeFragSpv, sizeof(cubeFragSpv));
    VkShaderModule gridVert = createShaderModule(gridVertSpv, sizeof(gridVertSpv));
    VkShaderModule gridFrag = createShaderModule(gridFragSpv, sizeof(gridFragSpv));

    // Common pipeline states
    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttach = {};
    colorBlendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttach;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Cube pipeline
    {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = cubeVert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = cubeFrag;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding = {};
        binding.stride = sizeof(CubeVertex);
        VkVertexInputAttributeDescription attrs[2] = {};
        attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
        attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[1].offset = 12;

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 2;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = renderer.pipelineLayout;
        pipelineInfo.renderPass = renderer.renderPass;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.cubePipeline));
    }

    // Grid pipeline
    {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = gridVert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = gridFrag;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding = {};
        binding.stride = sizeof(GridVertex);
        VkVertexInputAttributeDescription attr = {};
        attr.format = VK_FORMAT_R32G32B32_SFLOAT;

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attr;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

        VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = renderer.pipelineLayout;
        pipelineInfo.renderPass = renderer.renderPass;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.gridPipeline));
    }

    vkDestroyShaderModule(device, cubeVert, nullptr);
    vkDestroyShaderModule(device, cubeFrag, nullptr);
    vkDestroyShaderModule(device, gridVert, nullptr);
    vkDestroyShaderModule(device, gridFrag, nullptr);

    // Vertex/index buffers
    CubeVertex cubeVerts[] = {
        {{-0.5f,-0.5f,-0.5f},{1,0,0,1}}, {{-0.5f,0.5f,-0.5f},{1,0,0,1}},
        {{0.5f,0.5f,-0.5f},{1,0,0,1}},   {{0.5f,-0.5f,-0.5f},{1,0,0,1}},
        {{-0.5f,-0.5f,0.5f},{0,1,0,1}},  {{0.5f,-0.5f,0.5f},{0,1,0,1}},
        {{0.5f,0.5f,0.5f},{0,1,0,1}},    {{-0.5f,0.5f,0.5f},{0,1,0,1}},
        {{-0.5f,0.5f,-0.5f},{0,0,1,1}},  {{-0.5f,0.5f,0.5f},{0,0,1,1}},
        {{0.5f,0.5f,0.5f},{0,0,1,1}},    {{0.5f,0.5f,-0.5f},{0,0,1,1}},
        {{-0.5f,-0.5f,-0.5f},{1,1,0,1}}, {{0.5f,-0.5f,-0.5f},{1,1,0,1}},
        {{0.5f,-0.5f,0.5f},{1,1,0,1}},   {{-0.5f,-0.5f,0.5f},{1,1,0,1}},
        {{-0.5f,-0.5f,0.5f},{1,0,1,1}},  {{-0.5f,0.5f,0.5f},{1,0,1,1}},
        {{-0.5f,0.5f,-0.5f},{1,0,1,1}},  {{-0.5f,-0.5f,-0.5f},{1,0,1,1}},
        {{0.5f,-0.5f,-0.5f},{0,1,1,1}},  {{0.5f,0.5f,-0.5f},{0,1,1,1}},
        {{0.5f,0.5f,0.5f},{0,1,1,1}},    {{0.5f,-0.5f,0.5f},{0,1,1,1}},
    };

    uint16_t cubeIndices[] = {
        0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
        12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23,
    };

    if (!CreateBuffer(device, physDevice, sizeof(cubeVerts),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeVertexBuffer, renderer.cubeVertexMemory)) return false;
    void* data;
    vkMapMemory(device, renderer.cubeVertexMemory, 0, sizeof(cubeVerts), 0, &data);
    memcpy(data, cubeVerts, sizeof(cubeVerts));
    vkUnmapMemory(device, renderer.cubeVertexMemory);

    if (!CreateBuffer(device, physDevice, sizeof(cubeIndices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeIndexBuffer, renderer.cubeIndexMemory)) return false;
    vkMapMemory(device, renderer.cubeIndexMemory, 0, sizeof(cubeIndices), 0, &data);
    memcpy(data, cubeIndices, sizeof(cubeIndices));
    vkUnmapMemory(device, renderer.cubeIndexMemory);

    const int gridSize = 10;
    const float gridSpacing = 1.0f;
    std::vector<GridVertex> gridVerts;
    for (int i = -gridSize; i <= gridSize; i++) {
        gridVerts.push_back({{(float)i * gridSpacing, -1.0f, -gridSize * gridSpacing}});
        gridVerts.push_back({{(float)i * gridSpacing, -1.0f,  gridSize * gridSpacing}});
        gridVerts.push_back({{-gridSize * gridSpacing, -1.0f, (float)i * gridSpacing}});
        gridVerts.push_back({{ gridSize * gridSpacing, -1.0f, (float)i * gridSpacing}});
    }
    renderer.gridVertexCount = (int)gridVerts.size();

    VkDeviceSize gridBufSize = gridVerts.size() * sizeof(GridVertex);
    if (!CreateBuffer(device, physDevice, gridBufSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.gridVertexBuffer, renderer.gridVertexMemory)) return false;
    vkMapMemory(device, renderer.gridVertexMemory, 0, gridBufSize, 0, &data);
    memcpy(data, gridVerts.data(), (size_t)gridBufSize);
    vkUnmapMemory(device, renderer.gridVertexMemory);

    // Command pool + buffer
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &renderer.commandPool));

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = renderer.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &renderer.commandBuffer));

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &renderer.frameFence));

    LOG_INFO("Vulkan renderer initialized");
    return true;
}

static void RenderScene(VkRenderer& renderer, int eye, uint32_t imageIndex,
    uint32_t width, uint32_t height, const float* viewMat, const float* projMat)
{
    VkDevice device = renderer.device;
    vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &renderer.frameFence);

    auto& fb = renderer.eyeFramebuffers[eye];
    VkCommandBuffer cmd = renderer.commandBuffer;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkClearValue clearValues[2] = {};
    clearValues[0].color = {{0.05f, 0.05f, 0.1f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin = {};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderer.renderPass;
    rpBegin.framebuffer = fb.framebuffers[imageIndex];
    rpBegin.renderArea = {{0,0}, {width, height}};
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clearValues;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {0, (float)height, (float)width, -(float)height, 0.0f, 1.0f};
    VkRect2D scissor = {{0,0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Compute VP matrix
    float vp[16];
    mat4_multiply(vp, projMat, viewMat);

    // Draw grid
    float gridMvp[16];
    memcpy(gridMvp, vp, sizeof(vp));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.gridPipeline);
    vkCmdPushConstants(cmd, renderer.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, gridMvp);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &renderer.gridVertexBuffer, &offset);
    vkCmdDraw(cmd, renderer.gridVertexCount, 1, 0, 0);

    // Draw cube
    float model[16], rot[16], trans[16];
    mat4_rotation_y(rot, renderer.cubeRotation);
    mat4_translation(trans, 0, 0, -3);
    mat4_multiply(model, trans, rot);
    float mvp[16];
    mat4_multiply(mvp, vp, model);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.cubePipeline);
    vkCmdPushConstants(cmd, renderer.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
    vkCmdBindVertexBuffers(cmd, 0, 1, &renderer.cubeVertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, renderer.cubeIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(renderer.graphicsQueue, 1, &submitInfo, renderer.frameFence);
}

static void CleanupVkRenderer(VkRenderer& renderer) {
    if (renderer.device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(renderer.device);

    for (int eye = 0; eye < 2; eye++) {
        auto& fb = renderer.eyeFramebuffers[eye];
        for (auto f : fb.framebuffers) vkDestroyFramebuffer(renderer.device, f, nullptr);
        for (auto v : fb.colorViews) vkDestroyImageView(renderer.device, v, nullptr);
        for (auto v : fb.depthViews) vkDestroyImageView(renderer.device, v, nullptr);
        if (fb.depthImage) vkDestroyImage(renderer.device, fb.depthImage, nullptr);
        if (fb.depthMemory) vkFreeMemory(renderer.device, fb.depthMemory, nullptr);
    }

    if (renderer.frameFence) vkDestroyFence(renderer.device, renderer.frameFence, nullptr);
    if (renderer.commandPool) vkDestroyCommandPool(renderer.device, renderer.commandPool, nullptr);
    if (renderer.gridVertexBuffer) vkDestroyBuffer(renderer.device, renderer.gridVertexBuffer, nullptr);
    if (renderer.gridVertexMemory) vkFreeMemory(renderer.device, renderer.gridVertexMemory, nullptr);
    if (renderer.cubeIndexBuffer) vkDestroyBuffer(renderer.device, renderer.cubeIndexBuffer, nullptr);
    if (renderer.cubeIndexMemory) vkFreeMemory(renderer.device, renderer.cubeIndexMemory, nullptr);
    if (renderer.cubeVertexBuffer) vkDestroyBuffer(renderer.device, renderer.cubeVertexBuffer, nullptr);
    if (renderer.cubeVertexMemory) vkFreeMemory(renderer.device, renderer.cubeVertexMemory, nullptr);
    if (renderer.gridPipeline) vkDestroyPipeline(renderer.device, renderer.gridPipeline, nullptr);
    if (renderer.cubePipeline) vkDestroyPipeline(renderer.device, renderer.cubePipeline, nullptr);
    if (renderer.pipelineLayout) vkDestroyPipelineLayout(renderer.device, renderer.pipelineLayout, nullptr);
    if (renderer.renderPass) vkDestroyRenderPass(renderer.device, renderer.renderPass, nullptr);
}

// ============================================================================
// macOS Window + Metal View
// ============================================================================

/*!
 * NSView subclass whose backing layer is a CAMetalLayer.
 * Same pattern as MonadoMetalView in comp_window_macos.m.
 */
@interface AppMetalView : NSView
@end

@implementation AppMetalView
- (CALayer *)makeBackingLayer {
    return [CAMetalLayer layer];
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
@end

static bool CreateMacOSWindow(uint32_t width, uint32_t height) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSRect frame = NSMakeRect(100, 100, width, height);
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;

        g_window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:style
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];

        [g_window setTitle:@"Sim Cube OpenXR (External Window)"];
        [g_window setAcceptsMouseMovedEvents:YES];

        g_metalView = [[AppMetalView alloc] initWithFrame:frame];
        [g_metalView setWantsLayer:YES];

        [g_window setContentView:g_metalView];
        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        // Pump events so the window appears
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:nil
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES]) != nil) {
            [NSApp sendEvent:event];
        }
    }

    if (g_window == nil || g_metalView == nil) {
        LOG_ERROR("Failed to create macOS window");
        return false;
    }

    LOG_INFO("Created macOS window %ux%u with CAMetalLayer-backed NSView", width, height);
    return true;
}

static void PumpMacOSEvents() {
    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:nil
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES]) != nil) {
            // Handle keyboard input
            if ([event type] == NSEventTypeKeyDown) {
                unichar ch = [[event characters] characterAtIndex:0];
                if (ch == 27) { // ESC
                    LOG_INFO("ESC pressed, requesting exit");
                    g_running = false;
                } else if (ch == '1') {
                    setenv("SIM_DISPLAY_OUTPUT", "sbs", 1);
                    LOG_INFO("Switched to SBS mode (takes effect on next session)");
                } else if (ch == '2') {
                    setenv("SIM_DISPLAY_OUTPUT", "anaglyph", 1);
                    LOG_INFO("Switched to anaglyph mode (takes effect on next session)");
                } else if (ch == '3') {
                    setenv("SIM_DISPLAY_OUTPUT", "blend", 1);
                    LOG_INFO("Switched to blend mode (takes effect on next session)");
                }
            }
            [NSApp sendEvent:event];
        }
    }
}

// ============================================================================
// OpenXR session management
// ============================================================================

struct SwapchainInfo {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t imageCount = 0;
};

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;

    SwapchainInfo swapchains[2];

    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews;

    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;
    bool hasMacosWindowBinding = false;
};

static bool InitializeOpenXR(AppXrSession& xr) {
    LOG_INFO("Initializing OpenXR...");

    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(ext.extensionName, XR_EXT_MACOS_WINDOW_BINDING_EXTENSION_NAME) == 0) {
            xr.hasMacosWindowBinding = true;
        }
    }

    if (!hasVulkan) {
        LOG_ERROR("XR_KHR_vulkan_enable not supported");
        return false;
    }

    LOG_INFO("XR_EXT_macos_window_binding: %s", xr.hasMacosWindowBinding ? "available" : "not available");

    // Build extension list
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasMacosWindowBinding) {
        enabledExtensions.push_back(XR_EXT_MACOS_WINDOW_BINDING_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "SimCubeExtMacOS", XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK(xrCreateInstance(&createInfo, &xr.instance));
    LOG_INFO("OpenXR instance created");

    XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &sysInfo, &xr.systemId));
    LOG_INFO("Got system ID: %llu", (unsigned long long)xr.systemId);

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType,
        viewCount, &viewCount, xr.configViews.data()));

    for (uint32_t i = 0; i < viewCount; i++) {
        LOG_INFO("  View %u: %ux%u", i,
            xr.configViews[i].recommendedImageRectWidth,
            xr.configViews[i].recommendedImageRectHeight);
    }

    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfn));

    XrGraphicsRequirementsVulkanKHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_CHECK(pfn(xr.instance, xr.systemId, &graphicsReq));
    return true;
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR",
        (PFN_xrVoidFunction*)&pfn));

    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);
    std::string extensionsStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extensionsStr.data());

    std::vector<std::string> extensionStorage;
    std::vector<const char*> extensionPtrs;
    size_t start = 0;
    for (size_t i = 0; i <= extensionsStr.size(); i++) {
        if (i == extensionsStr.size() || extensionsStr[i] == ' ' || extensionsStr[i] == '\0') {
            if (i > start) {
                extensionStorage.push_back(extensionsStr.substr(start, i - start));
                extensionPtrs.push_back(extensionStorage.back().c_str());
            }
            start = i + 1;
        }
    }

    // Check for portability enumeration
    uint32_t availCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availCount, nullptr);
    std::vector<VkExtensionProperties> availExts(availCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availCount, availExts.data());

    bool hasPortabilityEnum = false;
    for (const auto& e : availExts) {
        if (strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            hasPortabilityEnum = true;
            extensionPtrs.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            break;
        }
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SimCubeExtMacOS";
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensionPtrs.size();
    createInfo.ppEnabledExtensionNames = extensionPtrs.data();
    if (hasPortabilityEnum) {
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &vkInstance));
    LOG_INFO("Vulkan instance created");
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice) {
    PFN_xrGetVulkanGraphicsDeviceKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR",
        (PFN_xrVoidFunction*)&pfn));
    XR_CHECK(pfn(xr.instance, xr.systemId, vkInstance, &physDevice));
    g_physDevice = physDevice;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDevice, &props);
    LOG_INFO("Vulkan physical device: %s", props.deviceName);
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &count, families.data());

    for (uint32_t i = 0; i < count; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamilyIndex = i;
            return true;
        }
    }
    LOG_ERROR("No graphics queue family found");
    return false;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr,
    std::vector<const char*>& deviceExtensions, std::vector<std::string>& extensionStorage,
    VkPhysicalDevice physDevice)
{
    PFN_xrGetVulkanDeviceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR",
        (PFN_xrVoidFunction*)&pfn));

    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);
    std::string extStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extStr.data());

    // Enumerate available device extensions
    uint32_t availCount = 0;
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &availCount, nullptr);
    std::vector<VkExtensionProperties> availExts(availCount);
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &availCount, availExts.data());

    auto isAvailable = [&](const char *name) -> bool {
        for (const auto& e : availExts) {
            if (strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };

    // Phase 1: Build extensionStorage (all strings) first to avoid
    // dangling c_str() pointers from vector reallocation.
    extensionStorage.clear();
    size_t start = 0;
    for (size_t i = 0; i <= extStr.size(); i++) {
        if (i == extStr.size() || extStr[i] == ' ' || extStr[i] == '\0') {
            if (i > start) {
                std::string ext = extStr.substr(start, i - start);
                if (isAvailable(ext.c_str())) {
                    extensionStorage.push_back(ext);
                } else {
                    LOG_WARN("  Device ext UNAVAILABLE (skipping): %s", ext.c_str());
                }
            }
            start = i + 1;
        }
    }

    // Add optional extensions if available
    for (const auto& e : availExts) {
        if (strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) {
            extensionStorage.push_back("VK_KHR_portability_subset");
        }
        if (strcmp(e.extensionName, VK_KHR_MAINTENANCE1_EXTENSION_NAME) == 0) {
            extensionStorage.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
        }
    }

    // Phase 2: Build deviceExtensions pointers from final extensionStorage
    // (safe now since extensionStorage won't be modified again)
    for (const auto& name : extensionStorage) {
        deviceExtensions.push_back(name.c_str());
        LOG_INFO("  Device ext: %s", name.c_str());
    }

    return true;
}

static bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions, VkDevice& device, VkQueue& graphicsQueue)
{
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VK_CHECK(vkCreateDevice(physDevice, &createInfo, nullptr, &device));
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);
    LOG_INFO("Vulkan device created");
    return true;
}

/*!
 * Create OpenXR session WITH the macos_window_binding extension.
 * Chains XrMacOSWindowBindingCreateInfoEXT into session create info.
 */
static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex)
{
    LOG_INFO("Creating OpenXR session with Vulkan binding + macOS window binding...");

    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = physDevice;
    vkBinding.device = device;
    vkBinding.queueFamilyIndex = queueFamilyIndex;
    vkBinding.queueIndex = 0;

    // Chain the macOS window binding extension — pass our NSView to the runtime
    XrMacOSWindowBindingCreateInfoEXT macosBinding = {};
    macosBinding.type = XR_TYPE_MACOS_WINDOW_BINDING_CREATE_INFO_EXT;
    macosBinding.next = nullptr;
    macosBinding.viewHandle = (__bridge void *)g_metalView;

    // Chain: sessionInfo -> vkBinding -> macosBinding
    if (xr.hasMacosWindowBinding) {
        vkBinding.next = &macosBinding;
        LOG_INFO("Chaining XR_EXT_macos_window_binding with NSView %p", macosBinding.viewHandle);
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &vkBinding;
    sessionInfo.systemId = xr.systemId;

    XR_CHECK(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created%s", xr.hasMacosWindowBinding ? " (with external window)" : "");

    return true;
}

static bool CreateSpaces(AppXrSession& xr) {
    XrReferenceSpaceCreateInfo localSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    localSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    localSpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    localSpaceInfo.poseInReferenceSpace.position = {0, 0, 0};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &localSpaceInfo, &xr.localSpace));

    XrReferenceSpaceCreateInfo viewSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    viewSpaceInfo.poseInReferenceSpace.position = {0, 0, 0};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &viewSpaceInfo, &xr.viewSpace));

    LOG_INFO("Reference spaces created");
    return true;
}

static bool CreateSwapchains(AppXrSession& xr) {
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, formatCount, &formatCount, formats.data()));

    int64_t selectedFormat = formats[0];
    LOG_INFO("Selected swapchain format: %lld", (long long)selectedFormat);

    for (int eye = 0; eye < 2; eye++) {
        XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.format = selectedFormat;
        swapchainInfo.sampleCount = 1;
        swapchainInfo.width = xr.configViews[eye].recommendedImageRectWidth;
        swapchainInfo.height = xr.configViews[eye].recommendedImageRectHeight;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;

        XR_CHECK(xrCreateSwapchain(xr.session, &swapchainInfo, &xr.swapchains[eye].swapchain));
        xr.swapchains[eye].format = selectedFormat;
        xr.swapchains[eye].width = swapchainInfo.width;
        xr.swapchains[eye].height = swapchainInfo.height;

        uint32_t imageCount = 0;
        XR_CHECK(xrEnumerateSwapchainImages(xr.swapchains[eye].swapchain, 0, &imageCount, nullptr));
        xr.swapchains[eye].imageCount = imageCount;
        LOG_INFO("  Eye %d: %ux%u, %u images", eye, swapchainInfo.width, swapchainInfo.height, imageCount);
    }

    return true;
}

static bool PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* stateEvent = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = stateEvent->state;

            switch (xr.sessionState) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = xr.viewConfigType;
                if (XR_SUCCEEDED(xrBeginSession(xr.session, &beginInfo))) {
                    xr.sessionRunning = true;
                    LOG_INFO("Session running");
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(xr.session);
                xr.sessionRunning = false;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                xr.exitRequested = true;
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

static bool BeginFrame(AppXrSession& xr, XrFrameState& frameState) {
    frameState = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    XrResult result = xrWaitFrame(xr.session, &waitInfo, &frameState);
    if (XR_FAILED(result)) { xr.exitRequested = true; return false; }

    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    result = xrBeginFrame(xr.session, &beginInfo);
    if (XR_FAILED(result)) return false;
    return true;
}

static bool AcquireSwapchainImage(AppXrSession& xr, int eye, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(xr.swapchains[eye].swapchain, &acquireInfo, &imageIndex);
    if (XR_FAILED(result)) return false;

    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(xr.swapchains[eye].swapchain, &waitInfo);
    if (XR_FAILED(result)) {
        XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(xr.swapchains[eye].swapchain, &releaseInfo);
        return false;
    }
    return true;
}

static bool ReleaseSwapchainImage(AppXrSession& xr, int eye) {
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_SUCCEEDED(xrReleaseSwapchainImage(xr.swapchains[eye].swapchain, &releaseInfo));
}

static bool EndFrame(AppXrSession& xr, XrTime displayTime, const XrCompositionLayerProjectionView* views) {
    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.space = xr.localSpace;
    projectionLayer.viewCount = 2;
    projectionLayer.views = views;

    const XrCompositionLayerBaseHeader* layers[] = {
        (XrCompositionLayerBaseHeader*)&projectionLayer
    };

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 1;
    endInfo.layers = layers;
    return XR_SUCCEEDED(xrEndFrame(xr.session, &endInfo));
}

static void CleanupOpenXR(AppXrSession& xr) {
    for (int eye = 0; eye < 2; eye++) {
        if (xr.swapchains[eye].swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(xr.swapchains[eye].swapchain);
        }
    }
    if (xr.viewSpace != XR_NULL_HANDLE) xrDestroySpace(xr.viewSpace);
    if (xr.localSpace != XR_NULL_HANDLE) xrDestroySpace(xr.localSpace);
    if (xr.session != XR_NULL_HANDLE) xrDestroySession(xr.session);
    if (xr.instance != XR_NULL_HANDLE) xrDestroyInstance(xr.instance);
}

// ============================================================================
// Main
// ============================================================================

static void SignalHandler(int sig) {
    (void)sig;
    g_running = false;
}

int main() {
    // Ensure logs appear immediately even when piped
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== Sim Cube OpenXR + External macOS Window ===");

    // Step 1: Create the macOS window FIRST (app owns it)
    if (!CreateMacOSWindow(1280, 720)) {
        LOG_ERROR("Failed to create macOS window");
        return 1;
    }

    // Step 2: Initialize OpenXR
    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        return 1;
    }

    if (!GetVulkanGraphicsRequirements(xr)) {
        CleanupOpenXR(xr);
        return 1;
    }

    // Step 3: Create Vulkan instance + device
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        CleanupOpenXR(xr);
        return 1;
    }

    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, deviceExtensions, extensionStorage, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    // Step 4: Create OpenXR session WITH the external window binding
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        return 1;
    }

    // Step 5: Enumerate swapchain images + init renderer
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages[2];
    for (int eye = 0; eye < 2; eye++) {
        uint32_t count = xr.swapchains[eye].imageCount;
        swapchainImages[eye].resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchains[eye].swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages[eye].data());
    }

    VkFormat colorFormat = (VkFormat)xr.swapchains[0].format;
    VkRenderer vkRenderer = {};
    if (!InitializeVkRenderer(vkRenderer, vkDevice, physDevice, graphicsQueue, queueFamilyIndex, colorFormat)) {
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        return 1;
    }

    for (int eye = 0; eye < 2; eye++) {
        uint32_t count = xr.swapchains[eye].imageCount;
        LOG_INFO("Creating framebuffers for eye %d: %u images, %ux%u",
                 eye, count, xr.swapchains[eye].width, xr.swapchains[eye].height);
        std::vector<VkImage> images(count);
        for (uint32_t i = 0; i < count; i++) {
            images[i] = swapchainImages[eye][i].image;
        }
        if (!CreateSwapchainFramebuffers(vkRenderer, eye, images.data(), count,
            xr.swapchains[eye].width, xr.swapchains[eye].height, colorFormat)) {
            CleanupVkRenderer(vkRenderer);
            CleanupOpenXR(xr);
            vkDestroyDevice(vkDevice, nullptr);
            vkDestroyInstance(vkInstance, nullptr);
            return 1;
        }
        LOG_INFO("  Eye %d framebuffers created OK", eye);
    }

    LOG_INFO("=== Entering main loop (ESC to quit, 1/2/3 for display modes) ===");

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        // Pump macOS events (keyboard, window, etc.)
        PumpMacOSEvents();

        // Delta time
        auto now = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        vkRenderer.cubeRotation += deltaTime * 0.5f;
        if (vkRenderer.cubeRotation > 2.0f * 3.14159265f) {
            vkRenderer.cubeRotation -= 2.0f * 3.14159265f;
        }

        PollEvents(xr);

        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[2] = {};
                bool rendered = false;

                if (frameState.shouldRender) {
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 2;
                    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

                    XrResult locResult = xrLocateViews(xr.session, &locateInfo, &viewState, 2, &viewCount, views);
                    if (XR_SUCCEEDED(locResult) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
                    {
                        rendered = true;
                        for (int eye = 0; eye < 2; eye++) {
                            uint32_t imageIndex;
                            if (AcquireSwapchainImage(xr, eye, imageIndex)) {
                                float viewMat[16], projMat[16];
                                mat4_view_from_xr_pose(viewMat, views[eye].pose);
                                mat4_from_xr_fov(projMat, views[eye].fov, 0.01f, 100.0f);

                                RenderScene(vkRenderer, eye, imageIndex,
                                    xr.swapchains[eye].width, xr.swapchains[eye].height,
                                    viewMat, projMat);

                                ReleaseSwapchainImage(xr, eye);

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchains[eye].swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {0, 0};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)xr.swapchains[eye].width,
                                    (int32_t)xr.swapchains[eye].height
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = views[eye].pose;
                                projectionViews[eye].fov = views[eye].fov;
                            } else {
                                rendered = false;
                            }
                        }
                    }
                }

                if (rendered) {
                    EndFrame(xr, frameState.predictedDisplayTime, projectionViews);
                } else {
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr.session, &endInfo);
                }
            }
        } else {
            usleep(100000);
        }
    }

    // Clean exit
    if (!xr.exitRequested && xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
        LOG_INFO("Requesting session exit...");
        xrRequestExitSession(xr.session);
        for (int i = 0; i < 100 && !xr.exitRequested; i++) {
            PollEvents(xr);
            usleep(10000);
        }
    }

    LOG_INFO("=== Shutting down ===");

    CleanupVkRenderer(vkRenderer);
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    LOG_INFO("Application shutdown complete");
    return 0;
}
