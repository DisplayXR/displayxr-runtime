// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan rendering helpers for native SR cube app
 */

#include "vk_renderer.h"
#include "logging.h"
#include <cstring>
#include <algorithm>
#include <vector>

// ============================================================================
// Embedded SPIR-V shaders (same as sr_cube_openxr_ext_vk)
// ============================================================================

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
    0x0009000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000d, 0x00000011, 0x0000001d,
    0x0000001f, 0x00030003, 0x00000002, 0x000001c2,
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

static bool CreateBufferHelper(VkDevice device, VkPhysicalDevice physDevice,
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

static bool CreateImageHelper(VkDevice device, VkPhysicalDevice physDevice,
    uint32_t width, uint32_t height, VkFormat format,
    VkImageUsageFlags usage, VkMemoryPropertyFlags memProps,
    VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, image, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits, memProps);

    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    vkBindImageMemory(device, image, memory, 0);
    return true;
}

// ============================================================================
// Vulkan instance
// ============================================================================

bool CreateVulkanInstance(VulkanState& vk) {
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SR Cube Native VK";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;

    if (vkCreateInstance(&createInfo, nullptr, &vk.instance) != VK_SUCCESS) {
        LOG_ERROR("Failed to create Vulkan instance");
        return false;
    }

    LOG_INFO("Vulkan instance created (API 1.1)");
    return true;
}

// ============================================================================
// Surface
// ============================================================================

bool CreateVulkanSurface(VulkanState& vk, HWND hwnd, HINSTANCE hInstance) {
    VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hwnd = hwnd;
    surfaceInfo.hinstance = hInstance;

    if (vkCreateWin32SurfaceKHR(vk.instance, &surfaceInfo, nullptr, &vk.surface) != VK_SUCCESS) {
        LOG_ERROR("Failed to create Win32 surface");
        return false;
    }

    LOG_INFO("Win32 Vulkan surface created");
    return true;
}

// ============================================================================
// Physical + logical device
// ============================================================================

bool CreateVulkanDevice(VulkanState& vk) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(vk.instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        LOG_ERROR("No Vulkan physical devices found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(vk.instance, &deviceCount, devices.data());

    // Select first suitable device (prefer discrete GPU)
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    for (auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        // Check for required queue families
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, queueFamilies.data());

        int graphicsIdx = -1, presentIdx = -1;
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                graphicsIdx = (int)i;

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, vk.surface, &presentSupport);
            if (presentSupport)
                presentIdx = (int)i;
        }

        if (graphicsIdx >= 0 && presentIdx >= 0) {
            chosen = dev;
            vk.graphicsFamily = (uint32_t)graphicsIdx;
            vk.presentFamily = (uint32_t)presentIdx;
            LOG_INFO("Selected GPU: %s", props.deviceName);
            break;
        }
    }

    if (chosen == VK_NULL_HANDLE) {
        LOG_ERROR("No suitable Vulkan device found");
        return false;
    }

    vk.physicalDevice = chosen;

    // Create logical device
    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;

    VkDeviceQueueCreateInfo graphicsQueueInfo = {};
    graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphicsQueueInfo.queueFamilyIndex = vk.graphicsFamily;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &queuePriority;
    queueInfos.push_back(graphicsQueueInfo);

    if (vk.presentFamily != vk.graphicsFamily) {
        VkDeviceQueueCreateInfo presentQueueInfo = {};
        presentQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        presentQueueInfo.queueFamilyIndex = vk.presentFamily;
        presentQueueInfo.queueCount = 1;
        presentQueueInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(presentQueueInfo);
    }

    const char* deviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    };

    VkPhysicalDeviceFeatures features = {};

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = (uint32_t)queueInfos.size();
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = 2;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    deviceInfo.pEnabledFeatures = &features;

    if (vkCreateDevice(vk.physicalDevice, &deviceInfo, nullptr, &vk.device) != VK_SUCCESS) {
        LOG_ERROR("Failed to create Vulkan logical device");
        return false;
    }

    vkGetDeviceQueue(vk.device, vk.graphicsFamily, 0, &vk.graphicsQueue);
    vkGetDeviceQueue(vk.device, vk.presentFamily, 0, &vk.presentQueue);

    LOG_INFO("Vulkan device created (graphics queue family: %u, present: %u)",
        vk.graphicsFamily, vk.presentFamily);
    return true;
}

// ============================================================================
// Command pool
// ============================================================================

bool CreateCommandPool(VulkanState& vk) {
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = vk.graphicsFamily;

    if (vkCreateCommandPool(vk.device, &poolInfo, nullptr, &vk.commandPool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create command pool");
        return false;
    }
    return true;
}

// ============================================================================
// Swapchain
// ============================================================================

bool CreateSwapchain(VulkanState& vk, uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physicalDevice, vk.surface, &caps);

    // Choose format
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physicalDevice, vk.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physicalDevice, vk.surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB && fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = fmt;
            break;
        }
        if (fmt.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = fmt;
        }
    }
    vk.swapchainFormat = chosen.format;

    // Image count
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    // Extent
    if (caps.currentExtent.width != UINT32_MAX) {
        vk.swapchainExtent = caps.currentExtent;
    } else {
        vk.swapchainExtent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, width));
        vk.swapchainExtent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, height));
    }

    VkSwapchainCreateInfoKHR swapInfo = {};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = vk.surface;
    swapInfo.minImageCount = imageCount;
    swapInfo.imageFormat = vk.swapchainFormat;
    swapInfo.imageColorSpace = chosen.colorSpace;
    swapInfo.imageExtent = vk.swapchainExtent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queueFamilies[] = {vk.graphicsFamily, vk.presentFamily};
    if (vk.graphicsFamily != vk.presentFamily) {
        swapInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapInfo.queueFamilyIndexCount = 2;
        swapInfo.pQueueFamilyIndices = queueFamilies;
    } else {
        swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(vk.device, &swapInfo, nullptr, &vk.swapchain) != VK_SUCCESS) {
        LOG_ERROR("Failed to create swapchain");
        return false;
    }

    // Get swapchain images
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &imageCount, nullptr);
    vk.swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &imageCount, vk.swapchainImages.data());

    // Create image views
    vk.swapchainImageViews.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vk.swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = vk.swapchainFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(vk.device, &viewInfo, nullptr, &vk.swapchainImageViews[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create swapchain image view %u", i);
            return false;
        }
    }

    LOG_INFO("Swapchain created: %ux%u, %u images, format %d",
        vk.swapchainExtent.width, vk.swapchainExtent.height,
        imageCount, vk.swapchainFormat);
    return true;
}

// ============================================================================
// Swapchain framebuffers (color-only, for weaver output)
// ============================================================================

bool CreateSwapchainFramebuffers(VulkanState& vk) {
    // Create a simple color-only render pass for the swapchain
    {
        VkAttachmentDescription colorAttach = {};
        colorAttach.format = vk.swapchainFormat;
        colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttach.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef = {};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkRenderPassCreateInfo rpInfo = {};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAttach;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;

        if (vkCreateRenderPass(vk.device, &rpInfo, nullptr, &vk.swapchainRenderPass) != VK_SUCCESS) {
            LOG_ERROR("Failed to create swapchain render pass");
            return false;
        }
    }

    // Create framebuffers
    vk.swapchainFramebuffers.resize(vk.swapchainImageViews.size());
    for (size_t i = 0; i < vk.swapchainImageViews.size(); i++) {
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = vk.swapchainRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &vk.swapchainImageViews[i];
        fbInfo.width = vk.swapchainExtent.width;
        fbInfo.height = vk.swapchainExtent.height;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(vk.device, &fbInfo, nullptr, &vk.swapchainFramebuffers[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create swapchain framebuffer %zu", i);
            return false;
        }
    }

    LOG_INFO("Created %zu swapchain framebuffers (color-only, for weaver)",
        vk.swapchainFramebuffers.size());
    return true;
}

// ============================================================================
// Stereo view texture + depth
// ============================================================================

bool CreateViewTexture(VulkanState& vk, uint32_t singleEyeWidth, uint32_t singleEyeHeight) {
    vk.viewWidth = singleEyeWidth;
    vk.viewHeight = singleEyeHeight;

    // Create separate per-eye color + depth images (weaver needs individual left/right VkImageViews)
    for (int eye = 0; eye < 2; eye++) {
        // Color image
        if (!CreateImageHelper(vk.device, vk.physicalDevice,
            singleEyeWidth, singleEyeHeight, vk.swapchainFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            vk.viewImages[eye], vk.viewImageMemory[eye])) {
            LOG_ERROR("Failed to create view image for eye %d", eye);
            return false;
        }

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vk.viewImages[eye];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = vk.swapchainFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(vk.device, &viewInfo, nullptr, &vk.viewImageViews[eye]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create view image view for eye %d", eye);
            return false;
        }

        // Depth image
        if (!CreateImageHelper(vk.device, vk.physicalDevice,
            singleEyeWidth, singleEyeHeight, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            vk.depthImages[eye], vk.depthImageMemory[eye])) {
            LOG_ERROR("Failed to create depth image for eye %d", eye);
            return false;
        }

        VkImageViewCreateInfo depthViewInfo = {};
        depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depthViewInfo.image = vk.depthImages[eye];
        depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
        depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthViewInfo.subresourceRange.levelCount = 1;
        depthViewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(vk.device, &depthViewInfo, nullptr, &vk.depthImageViews[eye]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create depth image view for eye %d", eye);
            return false;
        }
    }

    LOG_INFO("View texture created: %ux%u per eye (2 separate images), depth D32_SFLOAT",
        singleEyeWidth, singleEyeHeight);
    return true;
}

// ============================================================================
// Scene render pass (color + depth)
// ============================================================================

bool CreateSceneRenderPass(VulkanState& vk) {
    VkAttachmentDescription colorAttach = {};
    colorAttach.format = vk.swapchainFormat;
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttach = {};
    depthAttach.format = VK_FORMAT_D32_SFLOAT;
    depthAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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

    if (vkCreateRenderPass(vk.device, &rpInfo, nullptr, &vk.sceneRenderPass) != VK_SUCCESS) {
        LOG_ERROR("Failed to create scene render pass");
        return false;
    }

    // Create per-eye framebuffers
    for (int eye = 0; eye < 2; eye++) {
        VkImageView fbAttachments[] = {vk.viewImageViews[eye], vk.depthImageViews[eye]};

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = vk.sceneRenderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = fbAttachments;
        fbInfo.width = vk.viewWidth;
        fbInfo.height = vk.viewHeight;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(vk.device, &fbInfo, nullptr, &vk.sceneFramebuffers[eye]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create scene framebuffer for eye %d", eye);
            return false;
        }
    }

    LOG_INFO("Scene render pass + 2 per-eye framebuffers created");
    return true;
}

// ============================================================================
// Cube pipeline
// ============================================================================

bool CreateCubePipeline(VulkanState& vk) {
    // Pipeline layout
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(VkPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(vk.device, &layoutInfo, nullptr, &vk.pipelineLayout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create pipeline layout");
        return false;
    }

    // Shader modules
    VkShaderModule cubeVert = CreateShaderModule(vk.device, g_cubeVertSpv, sizeof(g_cubeVertSpv));
    VkShaderModule cubeFrag = CreateShaderModule(vk.device, g_cubeFragSpv, sizeof(g_cubeFragSpv));
    if (!cubeVert || !cubeFrag) {
        LOG_ERROR("Failed to create shader modules");
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = cubeVert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = cubeFrag;
    stages[1].pName = "main";

    // Vertex input
    struct CubeVertex { float pos[3]; float color[4]; };

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
    pipelineInfo.layout = vk.pipelineLayout;
    pipelineInfo.renderPass = vk.sceneRenderPass;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &vk.cubePipeline) != VK_SUCCESS) {
        LOG_ERROR("Failed to create cube pipeline");
        vkDestroyShaderModule(vk.device, cubeVert, nullptr);
        vkDestroyShaderModule(vk.device, cubeFrag, nullptr);
        return false;
    }

    vkDestroyShaderModule(vk.device, cubeVert, nullptr);
    vkDestroyShaderModule(vk.device, cubeFrag, nullptr);

    LOG_INFO("Cube pipeline created");
    return true;
}

// ============================================================================
// Cube geometry
// ============================================================================

bool CreateCubeGeometry(VulkanState& vk) {
    struct CubeVertex { float pos[3]; float color[4]; };

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

    // Vertex buffer
    if (!CreateBufferHelper(vk.device, vk.physicalDevice, sizeof(cubeVerts),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        vk.cubeVertexBuffer, vk.cubeVertexMemory)) {
        LOG_ERROR("Failed to create cube vertex buffer");
        return false;
    }
    void* data;
    vkMapMemory(vk.device, vk.cubeVertexMemory, 0, sizeof(cubeVerts), 0, &data);
    memcpy(data, cubeVerts, sizeof(cubeVerts));
    vkUnmapMemory(vk.device, vk.cubeVertexMemory);

    // Index buffer
    if (!CreateBufferHelper(vk.device, vk.physicalDevice, sizeof(cubeIndices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        vk.cubeIndexBuffer, vk.cubeIndexMemory)) {
        LOG_ERROR("Failed to create cube index buffer");
        return false;
    }
    vkMapMemory(vk.device, vk.cubeIndexMemory, 0, sizeof(cubeIndices), 0, &data);
    memcpy(data, cubeIndices, sizeof(cubeIndices));
    vkUnmapMemory(vk.device, vk.cubeIndexMemory);

    LOG_INFO("Cube geometry created (24 verts, 36 indices)");
    return true;
}

// ============================================================================
// Per-frame sync
// ============================================================================

bool CreateFrameSync(VulkanState& vk) {
    uint32_t count = (uint32_t)vk.swapchainImages.size();
    vk.frames.resize(count);

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = vk.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = count;

    std::vector<VkCommandBuffer> cmdBuffers(count);
    if (vkAllocateCommandBuffers(vk.device, &allocInfo, cmdBuffers.data()) != VK_SUCCESS) {
        LOG_ERROR("Failed to allocate command buffers");
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        vk.frames[i].commandBuffer = cmdBuffers[i];

        VkSemaphoreCreateInfo semInfo = {};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        if (vkCreateSemaphore(vk.device, &semInfo, nullptr, &vk.frames[i].imageAvailable) != VK_SUCCESS ||
            vkCreateSemaphore(vk.device, &semInfo, nullptr, &vk.frames[i].renderFinished) != VK_SUCCESS ||
            vkCreateFence(vk.device, &fenceInfo, nullptr, &vk.frames[i].inFlight) != VK_SUCCESS) {
            LOG_ERROR("Failed to create sync objects for frame %u", i);
            return false;
        }
    }

    LOG_INFO("Frame sync created (%u frames in flight)", count);
    return true;
}

// ============================================================================
// Image layout transition
// ============================================================================

void TransitionImageLayout(VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    // Determine access masks based on layouts
    switch (oldLayout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        barrier.srcAccessMask = 0;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        barrier.srcAccessMask = 0;
        break;
    default:
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        break;
    }

    switch (newLayout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        barrier.dstAccessMask = 0;
        dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
    default:
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        break;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0,
        0, nullptr, 0, nullptr, 1, &barrier);
}

// ============================================================================
// Cleanup
// ============================================================================

void CleanupVulkan(VulkanState& vk) {
    if (!vk.device) return;

    vkDeviceWaitIdle(vk.device);

    // Frame sync
    for (auto& f : vk.frames) {
        if (f.imageAvailable) vkDestroySemaphore(vk.device, f.imageAvailable, nullptr);
        if (f.renderFinished) vkDestroySemaphore(vk.device, f.renderFinished, nullptr);
        if (f.inFlight) vkDestroyFence(vk.device, f.inFlight, nullptr);
    }
    vk.frames.clear();

    // Geometry
    if (vk.cubeIndexBuffer) vkDestroyBuffer(vk.device, vk.cubeIndexBuffer, nullptr);
    if (vk.cubeIndexMemory) vkFreeMemory(vk.device, vk.cubeIndexMemory, nullptr);
    if (vk.cubeVertexBuffer) vkDestroyBuffer(vk.device, vk.cubeVertexBuffer, nullptr);
    if (vk.cubeVertexMemory) vkFreeMemory(vk.device, vk.cubeVertexMemory, nullptr);

    // Pipeline
    if (vk.cubePipeline) vkDestroyPipeline(vk.device, vk.cubePipeline, nullptr);
    if (vk.pipelineLayout) vkDestroyPipelineLayout(vk.device, vk.pipelineLayout, nullptr);

    // Scene framebuffers + render pass
    for (int eye = 0; eye < 2; eye++) {
        if (vk.sceneFramebuffers[eye]) vkDestroyFramebuffer(vk.device, vk.sceneFramebuffers[eye], nullptr);
    }
    if (vk.sceneRenderPass) vkDestroyRenderPass(vk.device, vk.sceneRenderPass, nullptr);

    // Per-eye view textures + depth
    for (int eye = 0; eye < 2; eye++) {
        if (vk.viewImageViews[eye]) vkDestroyImageView(vk.device, vk.viewImageViews[eye], nullptr);
        if (vk.viewImages[eye]) vkDestroyImage(vk.device, vk.viewImages[eye], nullptr);
        if (vk.viewImageMemory[eye]) vkFreeMemory(vk.device, vk.viewImageMemory[eye], nullptr);
        if (vk.depthImageViews[eye]) vkDestroyImageView(vk.device, vk.depthImageViews[eye], nullptr);
        if (vk.depthImages[eye]) vkDestroyImage(vk.device, vk.depthImages[eye], nullptr);
        if (vk.depthImageMemory[eye]) vkFreeMemory(vk.device, vk.depthImageMemory[eye], nullptr);
    }

    // Swapchain framebuffers
    for (auto fb : vk.swapchainFramebuffers) vkDestroyFramebuffer(vk.device, fb, nullptr);
    if (vk.swapchainRenderPass) vkDestroyRenderPass(vk.device, vk.swapchainRenderPass, nullptr);

    // Swapchain image views
    for (auto iv : vk.swapchainImageViews) vkDestroyImageView(vk.device, iv, nullptr);

    // Swapchain
    if (vk.swapchain) vkDestroySwapchainKHR(vk.device, vk.swapchain, nullptr);

    // Command pool
    if (vk.commandPool) vkDestroyCommandPool(vk.device, vk.commandPool, nullptr);

    // Device
    vkDestroyDevice(vk.device, nullptr);
    vk.device = VK_NULL_HANDLE;

    // Surface
    if (vk.surface) vkDestroySurfaceKHR(vk.instance, vk.surface, nullptr);

    // Instance
    if (vk.instance) vkDestroyInstance(vk.instance, nullptr);
    vk.instance = VK_NULL_HANDLE;
}
