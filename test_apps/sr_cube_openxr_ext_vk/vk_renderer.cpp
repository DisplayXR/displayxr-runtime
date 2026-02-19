// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan rendering implementation for cube and grid
 */

#include "vk_renderer.h"
#include "logging.h"
#include <cmath>
#include <vector>
#include <cstring>

using namespace DirectX;

// ============================================================================
// Embedded SPIR-V shaders
// ============================================================================
// These are pre-compiled from simple GLSL shaders.
// Cube vertex shader: takes position + color, applies MVP transform
// Cube fragment shader: outputs interpolated color
// Grid vertex shader: takes position, applies MVP transform
// Grid fragment shader: outputs uniform color

// Cube vertex shader (GLSL 450):
//   layout(push_constant) uniform PC { mat4 transform; vec4 color; } pc;
//   layout(location=0) in vec3 aPos;
//   layout(location=1) in vec4 aColor;
//   layout(location=0) out vec4 vColor;
//   void main() { gl_Position = pc.transform * vec4(aPos, 1.0); vColor = aColor; }
static const uint32_t g_cubeVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000028,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x000a000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000d, 0x00000011, 0x0000001d,
    0x0000001f, 0x00000024, 0x00030003, 0x00000002, 0x000001c2,
    0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00060005, 0x0000000b, 0x505f6c67, 0x65567265,
    0x78657472, 0x00000000, 0x00060006, 0x0000000b,
    0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69,
    0x00030005, 0x0000000d, 0x00000000, 0x00040005,
    0x0000000f, 0x00006350, 0x00000000, 0x00060006,
    0x0000000f, 0x00000000, 0x6e617274, 0x726f6673,
    0x0000006d, 0x00050006, 0x0000000f, 0x00000001,
    0x6f6c6f63, 0x00000072, 0x00030005, 0x00000011,
    0x00006370, 0x00040005, 0x0000001d, 0x736f5061,
    0x00000000,
    0x00040005, 0x0000001f, 0x6c6f4361, 0x0000726f,
    0x00040005, 0x00000024, 0x6c6f4376, 0x0000726f,
    0x00050048, 0x0000000b, 0x00000000, 0x0000000b,
    0x00000000, 0x00030047, 0x0000000b, 0x00000002,
    0x00040048, 0x0000000f, 0x00000000, 0x00000005,
    0x00050048, 0x0000000f, 0x00000000, 0x00000023,
    0x00000000, 0x00050048, 0x0000000f, 0x00000000,
    0x00000007, 0x00000010, 0x00050048, 0x0000000f,
    0x00000001, 0x00000023, 0x00000040, 0x00030047,
    0x0000000f, 0x00000002, 0x00040047, 0x0000001d,
    0x0000001e, 0x00000000, 0x00040047, 0x0000001f,
    0x0000001e, 0x00000001, 0x00040047, 0x00000024,
    0x0000001e, 0x00000000, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x0003001e, 0x0000000b,
    0x00000007, 0x00040020, 0x0000000c, 0x00000003,
    0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d,
    0x00000003, 0x00040015, 0x0000000e, 0x00000020,
    0x00000001, 0x00040018, 0x00000010, 0x00000007,
    0x00000004, 0x0004001e, 0x0000000f, 0x00000010,
    0x00000007, 0x00040020, 0x00000012, 0x00000009,
    0x0000000f, 0x0004003b, 0x00000012, 0x00000011,
    0x00000009, 0x0004002b, 0x0000000e, 0x00000013,
    0x00000000, 0x00040020, 0x00000014, 0x00000009,
    0x00000010, 0x00040017, 0x0000001b, 0x00000006,
    0x00000003, 0x00040020, 0x0000001c, 0x00000001,
    0x0000001b, 0x0004003b, 0x0000001c, 0x0000001d,
    0x00000001, 0x00040020, 0x0000001e, 0x00000001,
    0x00000007, 0x0004003b, 0x0000001e, 0x0000001f,
    0x00000001, 0x0004002b, 0x00000006, 0x00000021,
    0x3f800000, 0x00040020, 0x00000023, 0x00000003,
    0x00000007, 0x0004003b, 0x00000023, 0x00000024,
    0x00000003, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x00050041, 0x00000014, 0x00000015, 0x00000011,
    0x00000013, 0x0004003d, 0x00000010, 0x00000016,
    0x00000015, 0x0004003d, 0x0000001b, 0x00000020,
    0x0000001d, 0x00050051, 0x00000006, 0x00000025,
    0x00000020, 0x00000000, 0x00050051, 0x00000006,
    0x00000026, 0x00000020, 0x00000001, 0x00050051,
    0x00000006, 0x00000027, 0x00000020, 0x00000002,
    0x00070050, 0x00000007, 0x00000022, 0x00000025,
    0x00000026, 0x00000027, 0x00000021, 0x00050091,
    0x00000007, 0x00000017, 0x00000016, 0x00000022,
    0x00050041, 0x00000023, 0x00000018, 0x0000000d,
    0x00000013, 0x0003003e, 0x00000018, 0x00000017,
    0x0004003d, 0x00000007, 0x00000019, 0x0000001f,
    0x0003003e, 0x00000024, 0x00000019, 0x000100fd,
    0x00010038,
};

// Cube fragment shader (GLSL 450):
//   layout(location=0) in vec4 vColor;
//   layout(location=0) out vec4 FragColor;
//   void main() { FragColor = vColor; }
static const uint32_t g_cubeFragSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x0000000d,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d,
    0x00000000, 0x00000009, 0x0000000b, 0x00030003,
    0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00050005, 0x00000009,
    0x67617246, 0x6f6c6f43, 0x00000072, 0x00040005,
    0x0000000b, 0x6c6f4376, 0x0000726f, 0x00040047,
    0x00000009, 0x0000001e, 0x00000000, 0x00040047,
    0x0000000b, 0x0000001e, 0x00000000, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017,
    0x00000007, 0x00000006, 0x00000004, 0x00040020,
    0x00000008, 0x00000003, 0x00000007, 0x0004003b,
    0x00000008, 0x00000009, 0x00000003, 0x00040020,
    0x0000000a, 0x00000001, 0x00000007, 0x0004003b,
    0x0000000a, 0x0000000b, 0x00000001, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x0004003d, 0x00000007,
    0x0000000c, 0x0000000b, 0x0003003e, 0x00000009,
    0x0000000c, 0x000100fd, 0x00010038,
};

// Grid vertex shader (GLSL 450):
//   layout(push_constant) uniform PC { mat4 transform; vec4 color; } pc;
//   layout(location=0) in vec3 aPos;
//   void main() { gl_Position = pc.transform * vec4(aPos, 1.0); }
// Note: Original hand-written SPIR-V had ID collision (0x1e used for both
// OpTypePointer and OpCompositeConstruct). Fixed by using ID 0x22 for the
// composite result and bumping the bound from 0x22 to 0x23.
static const uint32_t g_gridVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000023,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000d, 0x00000017, 0x00030003,
    0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00060005, 0x0000000b,
    0x505f6c67, 0x65567265, 0x78657472, 0x00000000,
    0x00060006, 0x0000000b, 0x00000000, 0x505f6c67,
    0x7469736f, 0x006e6f69, 0x00030005, 0x0000000d,
    0x00000000, 0x00040005, 0x0000000f, 0x00006350,
    0x00000000, 0x00060006, 0x0000000f, 0x00000000,
    0x6e617274, 0x726f6673, 0x0000006d, 0x00050006,
    0x0000000f, 0x00000001, 0x6f6c6f63, 0x00000072,
    0x00030005, 0x00000011, 0x00006370, 0x00040005,
    0x00000017, 0x736f5061, 0x00000000, 0x00050048,
    0x0000000b, 0x00000000, 0x0000000b, 0x00000000,
    0x00030047, 0x0000000b, 0x00000002, 0x00040048,
    0x0000000f, 0x00000000, 0x00000005, 0x00050048,
    0x0000000f, 0x00000000, 0x00000023, 0x00000000,
    0x00050048, 0x0000000f, 0x00000000, 0x00000007,
    0x00000010, 0x00050048, 0x0000000f, 0x00000001,
    0x00000023, 0x00000040, 0x00030047, 0x0000000f,
    0x00000002, 0x00040047, 0x00000017, 0x0000001e,
    0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x0003001e, 0x0000000b, 0x00000007,
    0x00040020, 0x0000000c, 0x00000003, 0x0000000b,
    0x0004003b, 0x0000000c, 0x0000000d, 0x00000003,
    0x00040015, 0x0000000e, 0x00000020, 0x00000001,
    0x00040018, 0x00000010, 0x00000007, 0x00000004,
    0x0004001e, 0x0000000f, 0x00000010, 0x00000007,
    0x00040020, 0x00000012, 0x00000009, 0x0000000f,
    0x0004003b, 0x00000012, 0x00000011, 0x00000009,
    0x0004002b, 0x0000000e, 0x00000013, 0x00000000,
    0x00040020, 0x00000014, 0x00000009, 0x00000010,
    0x00040017, 0x00000016, 0x00000006, 0x00000003,
    0x00040020, 0x00000018, 0x00000001, 0x00000016,
    0x0004003b, 0x00000018, 0x00000017, 0x00000001,
    0x0004002b, 0x00000006, 0x0000001a, 0x3f800000,
    0x00040020, 0x0000001e, 0x00000003, 0x00000007,
    0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x00050041,
    0x00000014, 0x00000015, 0x00000011, 0x00000013,
    0x0004003d, 0x00000010, 0x00000019, 0x00000015,
    0x0004003d, 0x00000016, 0x0000001b, 0x00000017,
    0x00050051, 0x00000006, 0x0000001c, 0x0000001b,
    0x00000000, 0x00050051, 0x00000006, 0x0000001d,
    0x0000001b, 0x00000001, 0x00050051, 0x00000006,
    0x0000001f, 0x0000001b, 0x00000002, 0x00070050,
    0x00000007, 0x00000022, 0x0000001c, 0x0000001d,
    0x0000001f, 0x0000001a, 0x00050091, 0x00000007,
    0x00000020, 0x00000019, 0x00000022, 0x00050041,
    0x0000001e, 0x00000021, 0x0000000d, 0x00000013,
    0x0003003e, 0x00000021, 0x00000020, 0x000100fd,
    0x00010038,
};

// Grid fragment shader (GLSL 450):
//   layout(push_constant) uniform PC { mat4 transform; vec4 color; } pc;
//   layout(location=0) out vec4 FragColor;
//   void main() { FragColor = pc.color; }
static const uint32_t g_gridFragSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000013,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d,
    0x00000000, 0x00000009, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00050005, 0x00000009, 0x67617246,
    0x6f6c6f43, 0x00000072, 0x00040005, 0x0000000b,
    0x00006350, 0x00000000, 0x00060006, 0x0000000b,
    0x00000000, 0x6e617274, 0x726f6673, 0x0000006d,
    0x00050006, 0x0000000b, 0x00000001, 0x6f6c6f63,
    0x00000072, 0x00030005, 0x0000000d, 0x00006370,
    0x00040047, 0x00000009, 0x0000001e, 0x00000000,
    0x00040048, 0x0000000b, 0x00000000, 0x00000005,
    0x00050048, 0x0000000b, 0x00000000, 0x00000023,
    0x00000000, 0x00050048, 0x0000000b, 0x00000000,
    0x00000007, 0x00000010, 0x00050048, 0x0000000b,
    0x00000001, 0x00000023, 0x00000040, 0x00030047,
    0x0000000b, 0x00000002, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040020, 0x00000008,
    0x00000003, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000003, 0x00040018, 0x0000000a,
    0x00000007, 0x00000004, 0x0004001e, 0x0000000b,
    0x0000000a, 0x00000007, 0x00040020, 0x0000000c,
    0x00000009, 0x0000000b, 0x0004003b, 0x0000000c,
    0x0000000d, 0x00000009, 0x00040015, 0x0000000e,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000e,
    0x0000000f, 0x00000001, 0x00040020, 0x00000010,
    0x00000009, 0x00000007, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x00050041, 0x00000010, 0x00000011,
    0x0000000d, 0x0000000f, 0x0004003d, 0x00000007,
    0x00000012, 0x00000011,
    // Fix: use actual count from the ID
    0x0003003e, 0x00000009, 0x00000012, 0x000100fd,
    0x00010038,
};

// ============================================================================
// Helper functions
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

static bool CreateBuffer(VkDevice device, VkPhysicalDevice physDevice,
    VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
    VkBuffer& buffer, VkDeviceMemory& memory)
{
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits, memProps);

    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(device, buffer, memory, 0);
    return true;
}

static VkShaderModule CreateShaderModule(VkDevice device, const uint32_t* code, size_t codeSize) {
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = codeSize;
    createInfo.pCode = code;

    VkShaderModule module;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

// ============================================================================
// Vertex data
// ============================================================================

struct CubeVertex { float pos[3]; float color[4]; };
struct GridVertex { float pos[3]; };

// ============================================================================
// Initialization
// ============================================================================

bool InitializeVkRenderer(VkRenderer& renderer, VkDevice device, VkPhysicalDevice physDevice,
    VkQueue queue, uint32_t queueFamilyIndex, VkFormat colorFormat)
{
    renderer.device = device;
    renderer.physicalDevice = physDevice;
    renderer.graphicsQueue = queue;
    renderer.queueFamilyIndex = queueFamilyIndex;

    // Create render pass
    {
        VkAttachmentDescription colorAttach = {};
        colorAttach.format = colorFormat;
        colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttach.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAttach = {};
        depthAttach.format = VK_FORMAT_D32_SFLOAT;
        depthAttach.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription attachments[] = {colorAttach, depthAttach};

        VkAttachmentReference colorRef = {};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef = {};
        depthRef.attachment = 1;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &renderer.renderPass) != VK_SUCCESS) {
            LOG_ERROR("Failed to create render pass");
            return false;
        }

        // Create a second render pass with LOAD_OP_LOAD for SBS second-eye rendering.
        // This preserves the first eye's content in the shared framebuffer.
        colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttach.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttach.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentDescription attachmentsLoad[] = {colorAttach, depthAttach};
        rpInfo.pAttachments = attachmentsLoad;

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &renderer.renderPassLoad) != VK_SUCCESS) {
            LOG_ERROR("Failed to create load render pass");
            return false;
        }
    }

    // Create pipeline layout with push constants
    {
        VkPushConstantRange pushRange = {};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(VkPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &renderer.pipelineLayout) != VK_SUCCESS) {
            LOG_ERROR("Failed to create pipeline layout");
            return false;
        }
    }

    // Create shader modules
    VkShaderModule cubeVert = CreateShaderModule(device, g_cubeVertSpv, sizeof(g_cubeVertSpv));
    VkShaderModule cubeFrag = CreateShaderModule(device, g_cubeFragSpv, sizeof(g_cubeFragSpv));
    VkShaderModule gridVert = CreateShaderModule(device, g_gridVertSpv, sizeof(g_gridVertSpv));
    VkShaderModule gridFrag = CreateShaderModule(device, g_gridFragSpv, sizeof(g_gridFragSpv));

    if (!cubeVert || !cubeFrag || !gridVert || !gridFrag) {
        LOG_ERROR("Failed to create shader modules");
        return false;
    }

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
        binding.binding = 0;
        binding.stride = sizeof(CubeVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrs[2] = {};
        attrs[0].location = 0;
        attrs[0].binding = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset = offsetof(CubeVertex, pos);
        attrs[1].location = 1;
        attrs[1].binding = 0;
        attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[1].offset = offsetof(CubeVertex, color);

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 2;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer = {};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

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
        pipelineInfo.subpass = 0;

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.cubePipeline) != VK_SUCCESS) {
            LOG_ERROR("Failed to create cube pipeline");
            return false;
        }
    }

    // Grid pipeline (line list, no cull)
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
        binding.binding = 0;
        binding.stride = sizeof(GridVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attr = {};
        attr.location = 0;
        attr.binding = 0;
        attr.format = VK_FORMAT_R32G32B32_SFLOAT;
        attr.offset = 0;

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attr;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

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
        pipelineInfo.subpass = 0;

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.gridPipeline) != VK_SUCCESS) {
            LOG_ERROR("Failed to create grid pipeline");
            return false;
        }
    }

    // Destroy shader modules (no longer needed after pipeline creation)
    vkDestroyShaderModule(device, cubeVert, nullptr);
    vkDestroyShaderModule(device, cubeFrag, nullptr);
    vkDestroyShaderModule(device, gridVert, nullptr);
    vkDestroyShaderModule(device, gridFrag, nullptr);

    // Create vertex/index buffers (host-visible for simplicity)
    CubeVertex cubeVerts[] = {
        // Front (red)
        {{-0.5f,-0.5f,-0.5f},{1,0,0,1}}, {{-0.5f,0.5f,-0.5f},{1,0,0,1}},
        {{0.5f,0.5f,-0.5f},{1,0,0,1}},   {{0.5f,-0.5f,-0.5f},{1,0,0,1}},
        // Back (green)
        {{-0.5f,-0.5f,0.5f},{0,1,0,1}},  {{0.5f,-0.5f,0.5f},{0,1,0,1}},
        {{0.5f,0.5f,0.5f},{0,1,0,1}},    {{-0.5f,0.5f,0.5f},{0,1,0,1}},
        // Top (blue)
        {{-0.5f,0.5f,-0.5f},{0,0,1,1}},  {{-0.5f,0.5f,0.5f},{0,0,1,1}},
        {{0.5f,0.5f,0.5f},{0,0,1,1}},    {{0.5f,0.5f,-0.5f},{0,0,1,1}},
        // Bottom (yellow)
        {{-0.5f,-0.5f,-0.5f},{1,1,0,1}}, {{0.5f,-0.5f,-0.5f},{1,1,0,1}},
        {{0.5f,-0.5f,0.5f},{1,1,0,1}},   {{-0.5f,-0.5f,0.5f},{1,1,0,1}},
        // Left (magenta)
        {{-0.5f,-0.5f,0.5f},{1,0,1,1}},  {{-0.5f,0.5f,0.5f},{1,0,1,1}},
        {{-0.5f,0.5f,-0.5f},{1,0,1,1}},  {{-0.5f,-0.5f,-0.5f},{1,0,1,1}},
        // Right (cyan)
        {{0.5f,-0.5f,-0.5f},{0,1,1,1}},  {{0.5f,0.5f,-0.5f},{0,1,1,1}},
        {{0.5f,0.5f,0.5f},{0,1,1,1}},    {{0.5f,-0.5f,0.5f},{0,1,1,1}},
    };

    uint16_t cubeIndices[] = {
        0,1,2, 0,2,3,  4,5,6, 4,6,7,  8,9,10, 8,10,11,
        12,13,14, 12,14,15,  16,17,18, 16,18,19,  20,21,22, 20,22,23,
    };

    // Cube vertex buffer
    if (!CreateBuffer(device, physDevice, sizeof(cubeVerts),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeVertexBuffer, renderer.cubeVertexMemory)) {
        LOG_ERROR("Failed to create cube vertex buffer");
        return false;
    }
    void* data;
    vkMapMemory(device, renderer.cubeVertexMemory, 0, sizeof(cubeVerts), 0, &data);
    memcpy(data, cubeVerts, sizeof(cubeVerts));
    vkUnmapMemory(device, renderer.cubeVertexMemory);

    // Cube index buffer
    if (!CreateBuffer(device, physDevice, sizeof(cubeIndices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeIndexBuffer, renderer.cubeIndexMemory)) {
        LOG_ERROR("Failed to create cube index buffer");
        return false;
    }
    vkMapMemory(device, renderer.cubeIndexMemory, 0, sizeof(cubeIndices), 0, &data);
    memcpy(data, cubeIndices, sizeof(cubeIndices));
    vkUnmapMemory(device, renderer.cubeIndexMemory);

    // Grid vertex buffer
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
        renderer.gridVertexBuffer, renderer.gridVertexMemory)) {
        LOG_ERROR("Failed to create grid vertex buffer");
        return false;
    }
    vkMapMemory(device, renderer.gridVertexMemory, 0, gridBufSize, 0, &data);
    memcpy(data, gridVerts.data(), (size_t)gridBufSize);
    vkUnmapMemory(device, renderer.gridVertexMemory);

    // Command pool + command buffer
    {
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;

        if (vkCreateCommandPool(device, &poolInfo, nullptr, &renderer.commandPool) != VK_SUCCESS) {
            LOG_ERROR("Failed to create command pool");
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = renderer.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(device, &allocInfo, &renderer.commandBuffer) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate command buffer");
            return false;
        }
    }

    // Frame fence
    {
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        if (vkCreateFence(device, &fenceInfo, nullptr, &renderer.frameFence) != VK_SUCCESS) {
            LOG_ERROR("Failed to create fence");
            return false;
        }
    }

    LOG_INFO("Vulkan renderer initialized");
    return true;
}

bool CreateSwapchainFramebuffers(VkRenderer& renderer, int eye,
    const VkImage* images, uint32_t count,
    uint32_t width, uint32_t height, VkFormat colorFormat)
{
    VkDevice device = renderer.device;

    // Create depth image for this eye
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

        if (vkCreateImage(device, &imageInfo, nullptr, &renderer.depthImages[eye]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create depth image for eye %d", eye);
            return false;
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, renderer.depthImages[eye], &memReqs);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(renderer.physicalDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &renderer.depthMemory[eye]) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate depth memory for eye %d", eye);
            return false;
        }

        vkBindImageMemory(device, renderer.depthImages[eye], renderer.depthMemory[eye], 0);

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = renderer.depthImages[eye];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &renderer.depthViews[eye]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create depth view for eye %d", eye);
            return false;
        }
    }

    // Create image views and framebuffers for each swapchain image
    renderer.swapchainImageViews[eye].resize(count);
    renderer.framebuffers[eye].resize(count);

    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colorFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &renderer.swapchainImageViews[eye][i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create image view for eye %d image %u", eye, i);
            return false;
        }

        VkImageView attachments[] = {
            renderer.swapchainImageViews[eye][i],
            renderer.depthViews[eye]
        };

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderer.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &renderer.framebuffers[eye][i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create framebuffer for eye %d image %u", eye, i);
            return false;
        }
    }

    LOG_INFO("Created %u framebuffers for eye %d (%ux%u)", count, eye, width, height);
    return true;
}

void UpdateScene(VkRenderer& renderer, float deltaTime) {
    renderer.cubeRotation += deltaTime * 0.5f;
    if (renderer.cubeRotation > XM_2PI) {
        renderer.cubeRotation -= XM_2PI;
    }
}

void RenderScene(
    VkRenderer& renderer,
    int eye, uint32_t imageIndex,
    uint32_t viewportX, uint32_t viewportY,
    uint32_t width, uint32_t height,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix,
    float zoomScale,
    bool clear)
{
    VkDevice device = renderer.device;

    // Wait for previous frame's fence
    LOG_INFO("[RenderScene] eye=%d imageIndex=%u: vkWaitForFences (pre-render)...", eye, imageIndex);
    VkResult fenceResult = vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
    LOG_INFO("[RenderScene] eye=%d: vkWaitForFences returned %d", eye, (int)fenceResult);
    vkResetFences(device, 1, &renderer.frameFence);

    // Begin command buffer
    LOG_INFO("[RenderScene] eye=%d: vkResetCommandBuffer + vkBeginCommandBuffer...", eye);
    vkResetCommandBuffer(renderer.commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(renderer.commandBuffer, &beginInfo);

    // Begin render pass
    VkClearValue clearValues[2] = {};
    clearValues[0].color = {{0.05f, 0.05f, 0.25f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin = {};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = clear ? renderer.renderPass : renderer.renderPassLoad;
    rpBegin.framebuffer = renderer.framebuffers[eye][imageIndex];
    rpBegin.renderArea.offset = {(int32_t)viewportX, (int32_t)viewportY};
    rpBegin.renderArea.extent = {width, height};
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clearValues;

    LOG_INFO("[RenderScene] eye=%d: framebuffer=%p, renderPass=%p, extent=%ux%u",
             eye, (void*)renderer.framebuffers[eye][imageIndex], (void*)renderer.renderPass, width, height);
    LOG_INFO("[RenderScene] eye=%d: vkCmdBeginRenderPass...", eye);
    __try {
        vkCmdBeginRenderPass(renderer.commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[RenderScene] eye=%d: CRASH in vkCmdBeginRenderPass! exception=0x%08X", eye, GetExceptionCode());
        return;
    }
    LOG_INFO("[RenderScene] eye=%d: vkCmdBeginRenderPass OK", eye);

    // Set viewport with Y-flip (negative height) for correct NDC convention
    VkViewport viewport = {};
    viewport.x = (float)viewportX;
    viewport.y = (float)(viewportY + height);
    viewport.width = (float)width;
    viewport.height = -(float)height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(renderer.commandBuffer, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset = {(int32_t)viewportX, (int32_t)viewportY};
    scissor.extent = {width, height};
    vkCmdSetScissor(renderer.commandBuffer, 0, 1, &scissor);
    LOG_INFO("[RenderScene] eye=%d: viewport+scissor set", eye);

    // Zoom in eye space: scale only x,y (not z) so perspective division doesn't
    // cancel the effect. Keeps the viewport center fixed on screen.
    XMMATRIX zoom = XMMatrixScaling(zoomScale, zoomScale, 1.0f);

    // Draw cube - base rests on grid at y=0
    {
        const float cubeSize = 0.06f;
        const float cubeHeight = cubeSize / 2.0f;  // Raise by half size so base is at y=0
        XMMATRIX cubeScale = XMMatrixScaling(cubeSize, cubeSize, cubeSize);
        XMMATRIX cubeRot = XMMatrixRotationY(renderer.cubeRotation);
        XMMATRIX cubeTrans = XMMatrixTranslation(0.0f, cubeHeight, 0.0f);
        XMMATRIX cubeWVP = cubeRot * cubeScale * cubeTrans * viewMatrix * zoom * projMatrix;

        VkPushConstants pc = {};
        // Store WITHOUT transpose: SPIR-V ColMajor reads row-major data as columns,
        // naturally producing M^T. Shader then computes M^T * v which is the correct
        // column-vector equivalent of DirectXMath's row-vector v * M convention.
        // (Same implicit transpose that GL gets from glUniformMatrix4fv with GL_FALSE.)
        XMStoreFloat4x4(&pc.transform, cubeWVP);
        pc.color[0] = 1.0f; pc.color[1] = 1.0f; pc.color[2] = 1.0f; pc.color[3] = 1.0f;

        vkCmdBindPipeline(renderer.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.cubePipeline);
        vkCmdPushConstants(renderer.commandBuffer, renderer.pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(renderer.commandBuffer, 0, 1, &renderer.cubeVertexBuffer, &offset);
        vkCmdBindIndexBuffer(renderer.commandBuffer, renderer.cubeIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(renderer.commandBuffer, 36, 1, 0, 0, 0);
        LOG_INFO("[RenderScene] eye=%d: cube draw recorded", eye);
    }

    // Draw grid floor
    {
        const float gridScale = 0.05f;
        XMMATRIX gridWorld = XMMatrixScaling(gridScale, gridScale, gridScale)
                           * XMMatrixTranslation(0, gridScale, 0);
        XMMATRIX gridWVP = gridWorld * viewMatrix * zoom * projMatrix;

        VkPushConstants pc = {};
        XMStoreFloat4x4(&pc.transform, gridWVP);
        pc.color[0] = 0.3f; pc.color[1] = 0.3f; pc.color[2] = 0.35f; pc.color[3] = 1.0f;

        vkCmdBindPipeline(renderer.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.gridPipeline);
        vkCmdPushConstants(renderer.commandBuffer, renderer.pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(renderer.commandBuffer, 0, 1, &renderer.gridVertexBuffer, &offset);
        vkCmdDraw(renderer.commandBuffer, renderer.gridVertexCount, 1, 0, 0);
        LOG_INFO("[RenderScene] eye=%d: grid draw recorded", eye);
    }

    LOG_INFO("[RenderScene] eye=%d: vkCmdEndRenderPass...", eye);
    vkCmdEndRenderPass(renderer.commandBuffer);
    LOG_INFO("[RenderScene] eye=%d: vkEndCommandBuffer...", eye);
    vkEndCommandBuffer(renderer.commandBuffer);

    // Submit
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &renderer.commandBuffer;

    LOG_INFO("[RenderScene] eye=%d: vkQueueSubmit...", eye);
    VkResult submitResult = vkQueueSubmit(renderer.graphicsQueue, 1, &submitInfo, renderer.frameFence);
    LOG_INFO("[RenderScene] eye=%d: vkQueueSubmit returned %d", eye, (int)submitResult);

    // Wait for completion before returning (runtime needs the image ready)
    LOG_INFO("[RenderScene] eye=%d: vkWaitForFences (post-submit)...", eye);
    VkResult waitResult = vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
    LOG_INFO("[RenderScene] eye=%d: vkWaitForFences returned %d - DONE", eye, (int)waitResult);
}

void CleanupVkRenderer(VkRenderer& renderer) {
    if (!renderer.device) return;

    vkDeviceWaitIdle(renderer.device);

    for (int eye = 0; eye < 2; eye++) {
        for (auto fb : renderer.framebuffers[eye])
            vkDestroyFramebuffer(renderer.device, fb, nullptr);
        renderer.framebuffers[eye].clear();

        for (auto iv : renderer.swapchainImageViews[eye])
            vkDestroyImageView(renderer.device, iv, nullptr);
        renderer.swapchainImageViews[eye].clear();

        if (renderer.depthViews[eye]) {
            vkDestroyImageView(renderer.device, renderer.depthViews[eye], nullptr);
            renderer.depthViews[eye] = VK_NULL_HANDLE;
        }
        if (renderer.depthImages[eye]) {
            vkDestroyImage(renderer.device, renderer.depthImages[eye], nullptr);
            renderer.depthImages[eye] = VK_NULL_HANDLE;
        }
        if (renderer.depthMemory[eye]) {
            vkFreeMemory(renderer.device, renderer.depthMemory[eye], nullptr);
            renderer.depthMemory[eye] = VK_NULL_HANDLE;
        }
    }

    if (renderer.frameFence) {
        vkDestroyFence(renderer.device, renderer.frameFence, nullptr);
        renderer.frameFence = VK_NULL_HANDLE;
    }
    if (renderer.commandPool) {
        vkDestroyCommandPool(renderer.device, renderer.commandPool, nullptr);
        renderer.commandPool = VK_NULL_HANDLE;
    }

    if (renderer.gridVertexBuffer) {
        vkDestroyBuffer(renderer.device, renderer.gridVertexBuffer, nullptr);
        renderer.gridVertexBuffer = VK_NULL_HANDLE;
    }
    if (renderer.gridVertexMemory) {
        vkFreeMemory(renderer.device, renderer.gridVertexMemory, nullptr);
        renderer.gridVertexMemory = VK_NULL_HANDLE;
    }
    if (renderer.cubeIndexBuffer) {
        vkDestroyBuffer(renderer.device, renderer.cubeIndexBuffer, nullptr);
        renderer.cubeIndexBuffer = VK_NULL_HANDLE;
    }
    if (renderer.cubeIndexMemory) {
        vkFreeMemory(renderer.device, renderer.cubeIndexMemory, nullptr);
        renderer.cubeIndexMemory = VK_NULL_HANDLE;
    }
    if (renderer.cubeVertexBuffer) {
        vkDestroyBuffer(renderer.device, renderer.cubeVertexBuffer, nullptr);
        renderer.cubeVertexBuffer = VK_NULL_HANDLE;
    }
    if (renderer.cubeVertexMemory) {
        vkFreeMemory(renderer.device, renderer.cubeVertexMemory, nullptr);
        renderer.cubeVertexMemory = VK_NULL_HANDLE;
    }

    if (renderer.gridPipeline) {
        vkDestroyPipeline(renderer.device, renderer.gridPipeline, nullptr);
        renderer.gridPipeline = VK_NULL_HANDLE;
    }
    if (renderer.cubePipeline) {
        vkDestroyPipeline(renderer.device, renderer.cubePipeline, nullptr);
        renderer.cubePipeline = VK_NULL_HANDLE;
    }
    if (renderer.pipelineLayout) {
        vkDestroyPipelineLayout(renderer.device, renderer.pipelineLayout, nullptr);
        renderer.pipelineLayout = VK_NULL_HANDLE;
    }
    if (renderer.renderPassLoad) {
        vkDestroyRenderPass(renderer.device, renderer.renderPassLoad, nullptr);
        renderer.renderPassLoad = VK_NULL_HANDLE;
    }
    if (renderer.renderPass) {
        vkDestroyRenderPass(renderer.device, renderer.renderPass, nullptr);
        renderer.renderPass = VK_NULL_HANDLE;
    }
}
