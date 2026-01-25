// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Standalone Vulkan cube renderer (no OpenXR)
 *
 * This is a simple test application to verify that Vulkan rendering works
 * correctly without any OpenXR involvement.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <vector>
#include <array>
#include <fstream>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <chrono>

// Simple logging
#define LOG_INFO(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) printf("[ERROR] " fmt "\n", ##__VA_ARGS__)

// Window settings
static const wchar_t* WINDOW_CLASS = L"VulkanCubeStandaloneClass";
static const wchar_t* WINDOW_TITLE = L"Vulkan Cube Standalone";
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_windowResized = false;

// Math types
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };
struct Mat4 {
    float m[4][4];
    static Mat4 Identity() {
        Mat4 r = {};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
    static Mat4 Translation(float x, float y, float z) {
        Mat4 r = Identity();
        r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z;
        return r;
    }
    static Mat4 RotationY(float angle) {
        Mat4 r = Identity();
        float c = cosf(angle), s = sinf(angle);
        r.m[0][0] = c; r.m[0][2] = -s;
        r.m[2][0] = s; r.m[2][2] = c;
        return r;
    }
    static Mat4 PerspectiveFov(float fovY, float aspect, float nearZ, float farZ) {
        float h = 1.0f / tanf(fovY * 0.5f);
        float w = h / aspect;
        Mat4 r = {};
        r.m[0][0] = w;
        r.m[1][1] = h;
        r.m[2][2] = farZ / (nearZ - farZ);
        r.m[2][3] = -1.0f;
        r.m[3][2] = (nearZ * farZ) / (nearZ - farZ);
        return r;
    }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r = {};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                for (int k = 0; k < 4; k++)
                    r.m[i][j] += m[i][k] * o.m[k][j];
        return r;
    }
    Mat4 Transpose() const {
        Mat4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = m[j][i];
        return r;
    }
};

// Vertex types
struct CubeVertex { Vec3 position; Vec3 normal; };
struct UniformBufferObject { Mat4 worldViewProj; Vec4 color; };

// Vulkan state
struct VulkanApp {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D swapchainExtent = {};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> framebuffers;

    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;

    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    VkBuffer uniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uniformMemory = VK_NULL_HANDLE;
    void* uniformMapped = nullptr;

    float cubeRotation = 0.0f;
    std::string shaderDir;
};

// Helper macros
#define VK_CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { LOG_ERROR("%s failed: %d", #call, r); return false; } } while(0)

// Find memory type
static uint32_t FindMemoryType(VkPhysicalDevice pd, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        if ((filter & (1 << i)) && (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

// Create buffer
static bool CreateBuffer(VulkanApp& app, VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(app.device, &info, nullptr, &buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(app.device, buffer, &req);

    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(app.physicalDevice, req.memoryTypeBits, props);
    if (alloc.memoryTypeIndex == UINT32_MAX) return false;

    VK_CHECK(vkAllocateMemory(app.device, &alloc, nullptr, &memory));
    VK_CHECK(vkBindBufferMemory(app.device, buffer, memory, 0));
    return true;
}

// Load shader
static VkShaderModule LoadShader(VkDevice device, const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) { LOG_ERROR("Cannot open shader: %s", path.c_str()); return VK_NULL_HANDLE; }
    size_t size = (size_t)file.tellg();
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), size);
    file.close();

    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
            g_windowResized = true;
        }
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { PostQuitMessage(0); return 0; }
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Create window
static HWND CreateAppWindow(HINSTANCE hInstance) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassEx(&wc)) return nullptr;

    RECT rect = {0, 0, (LONG)g_windowWidth, (LONG)g_windowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowEx(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);
}

// Initialize Vulkan
static bool InitVulkan(VulkanApp& app, HWND hwnd, HINSTANCE hInstance) {
    // Instance
    const char* instExts[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "VulkanCubeStandalone";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instInfo.pApplicationInfo = &appInfo;
    instInfo.enabledExtensionCount = 2;
    instInfo.ppEnabledExtensionNames = instExts;
    VK_CHECK(vkCreateInstance(&instInfo, nullptr, &app.instance));
    LOG_INFO("Vulkan instance created");

    // Surface
    VkWin32SurfaceCreateInfoKHR surfInfo = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfInfo.hinstance = hInstance;
    surfInfo.hwnd = hwnd;
    VK_CHECK(vkCreateWin32SurfaceKHR(app.instance, &surfInfo, nullptr, &app.surface));
    LOG_INFO("Surface created");

    // Physical device
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(app.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(app.instance, &count, devices.data());
    app.physicalDevice = devices[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(app.physicalDevice, &props);
    LOG_INFO("Using GPU: %s", props.deviceName);

    // Queue family
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(app.physicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfProps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(app.physicalDevice, &qfCount, qfProps.data());
    for (uint32_t i = 0; i < qfCount; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(app.physicalDevice, i, app.surface, &present);
        if ((qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            app.graphicsQueueFamily = i;
            break;
        }
    }

    // Device
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qInfo.queueFamilyIndex = app.graphicsQueueFamily;
    qInfo.queueCount = 1;
    qInfo.pQueuePriorities = &priority;

    const char* devExts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo devInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    devInfo.queueCreateInfoCount = 1;
    devInfo.pQueueCreateInfos = &qInfo;
    devInfo.enabledExtensionCount = 1;
    devInfo.ppEnabledExtensionNames = devExts;
    VK_CHECK(vkCreateDevice(app.physicalDevice, &devInfo, nullptr, &app.device));
    vkGetDeviceQueue(app.device, app.graphicsQueueFamily, 0, &app.graphicsQueue);
    LOG_INFO("Device created");

    // Command pool
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = app.graphicsQueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(app.device, &poolInfo, nullptr, &app.commandPool));

    return true;
}

// Create/recreate swapchain
static bool CreateSwapchain(VulkanApp& app, uint32_t width, uint32_t height) {
    vkDeviceWaitIdle(app.device);

    // Cleanup old
    for (auto fb : app.framebuffers) vkDestroyFramebuffer(app.device, fb, nullptr);
    for (auto iv : app.swapchainImageViews) vkDestroyImageView(app.device, iv, nullptr);
    if (app.depthView) vkDestroyImageView(app.device, app.depthView, nullptr);
    if (app.depthImage) vkDestroyImage(app.device, app.depthImage, nullptr);
    if (app.depthMemory) vkFreeMemory(app.device, app.depthMemory, nullptr);
    app.framebuffers.clear();
    app.swapchainImageViews.clear();

    VkSwapchainKHR oldSwapchain = app.swapchain;

    // Surface caps
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(app.physicalDevice, app.surface, &caps);
    app.swapchainExtent = {width, height};
    if (caps.currentExtent.width != UINT32_MAX)
        app.swapchainExtent = caps.currentExtent;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR swInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swInfo.surface = app.surface;
    swInfo.minImageCount = imageCount;
    swInfo.imageFormat = app.swapchainFormat;
    swInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swInfo.imageExtent = app.swapchainExtent;
    swInfo.imageArrayLayers = 1;
    swInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swInfo.preTransform = caps.currentTransform;
    swInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swInfo.clipped = VK_TRUE;
    swInfo.oldSwapchain = oldSwapchain;
    VK_CHECK(vkCreateSwapchainKHR(app.device, &swInfo, nullptr, &app.swapchain));

    if (oldSwapchain) vkDestroySwapchainKHR(app.device, oldSwapchain, nullptr);

    // Get images
    vkGetSwapchainImagesKHR(app.device, app.swapchain, &imageCount, nullptr);
    app.swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(app.device, app.swapchain, &imageCount, app.swapchainImages.data());

    // Image views
    app.swapchainImageViews.resize(imageCount);
    for (size_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo vInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vInfo.image = app.swapchainImages[i];
        vInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format = app.swapchainFormat;
        vInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(app.device, &vInfo, nullptr, &app.swapchainImageViews[i]));
    }

    // Depth buffer
    VkImageCreateInfo dInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    dInfo.imageType = VK_IMAGE_TYPE_2D;
    dInfo.extent = {app.swapchainExtent.width, app.swapchainExtent.height, 1};
    dInfo.mipLevels = 1;
    dInfo.arrayLayers = 1;
    dInfo.format = app.depthFormat;
    dInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    dInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    dInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    VK_CHECK(vkCreateImage(app.device, &dInfo, nullptr, &app.depthImage));

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(app.device, app.depthImage, &memReq);
    VkMemoryAllocateInfo aInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    aInfo.allocationSize = memReq.size;
    aInfo.memoryTypeIndex = FindMemoryType(app.physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(app.device, &aInfo, nullptr, &app.depthMemory));
    VK_CHECK(vkBindImageMemory(app.device, app.depthImage, app.depthMemory, 0));

    VkImageViewCreateInfo dvInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvInfo.image = app.depthImage;
    dvInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvInfo.format = app.depthFormat;
    dvInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(app.device, &dvInfo, nullptr, &app.depthView));

    // Framebuffers
    app.framebuffers.resize(imageCount);
    for (size_t i = 0; i < imageCount; i++) {
        VkImageView attachments[] = {app.swapchainImageViews[i], app.depthView};
        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = app.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = app.swapchainExtent.width;
        fbInfo.height = app.swapchainExtent.height;
        fbInfo.layers = 1;
        VK_CHECK(vkCreateFramebuffer(app.device, &fbInfo, nullptr, &app.framebuffers[i]));
    }

    // Command buffers
    app.commandBuffers.resize(imageCount);
    VkCommandBufferAllocateInfo cbInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbInfo.commandPool = app.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = imageCount;
    VK_CHECK(vkAllocateCommandBuffers(app.device, &cbInfo, app.commandBuffers.data()));

    LOG_INFO("Swapchain created: %ux%u, %d images", app.swapchainExtent.width, app.swapchainExtent.height, (int)imageCount);
    return true;
}

// Create render pass
static bool CreateRenderPass(VulkanApp& app) {
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = app.swapchainFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    attachments[1].format = app.depthFormat;
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

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(app.device, &rpInfo, nullptr, &app.renderPass));
    return true;
}

// Create pipeline
static bool CreatePipeline(VulkanApp& app) {
    // Descriptor set layout
    VkDescriptorSetLayoutBinding uboBinding = {};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dsInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsInfo.bindingCount = 1;
    dsInfo.pBindings = &uboBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(app.device, &dsInfo, nullptr, &app.descriptorSetLayout));

    // Pipeline layout
    VkPipelineLayoutCreateInfo plInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &app.descriptorSetLayout;
    VK_CHECK(vkCreatePipelineLayout(app.device, &plInfo, nullptr, &app.pipelineLayout));

    // Shaders
    VkShaderModule vertModule = LoadShader(app.device, app.shaderDir + "/cube.vert.spv");
    VkShaderModule fragModule = LoadShader(app.device, app.shaderDir + "/cube.frag.spv");
    if (!vertModule || !fragModule) return false;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Vertex input
    VkVertexInputBindingDescription binding = {0, sizeof(CubeVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(Vec3)};

    VkPipelineVertexInputStateCreateInfo viInfo = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    viInfo.vertexBindingDescriptionCount = 1;
    viInfo.pVertexBindingDescriptions = &binding;
    viInfo.vertexAttributeDescriptionCount = 2;
    viInfo.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo iaInfo = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    iaInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpInfo = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpInfo.viewportCount = 1;
    vpInfo.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rsInfo = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rsInfo.polygonMode = VK_POLYGON_MODE_FILL;
    rsInfo.cullMode = VK_CULL_MODE_BACK_BIT;
    rsInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsInfo.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msInfo = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsState = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsState.depthTestEnable = VK_TRUE;
    dsState.depthWriteEnable = VK_TRUE;
    dsState.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cbAttach = {};
    cbAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cbInfo = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbInfo.attachmentCount = 1;
    cbInfo.pAttachments = &cbAttach;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynInfo = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pInfo.stageCount = 2;
    pInfo.pStages = stages;
    pInfo.pVertexInputState = &viInfo;
    pInfo.pInputAssemblyState = &iaInfo;
    pInfo.pViewportState = &vpInfo;
    pInfo.pRasterizationState = &rsInfo;
    pInfo.pMultisampleState = &msInfo;
    pInfo.pDepthStencilState = &dsState;
    pInfo.pColorBlendState = &cbInfo;
    pInfo.pDynamicState = &dynInfo;
    pInfo.layout = app.pipelineLayout;
    pInfo.renderPass = app.renderPass;
    VK_CHECK(vkCreateGraphicsPipelines(app.device, VK_NULL_HANDLE, 1, &pInfo, nullptr, &app.pipeline));

    vkDestroyShaderModule(app.device, vertModule, nullptr);
    vkDestroyShaderModule(app.device, fragModule, nullptr);

    LOG_INFO("Pipeline created");
    return true;
}

// Create geometry
static bool CreateGeometry(VulkanApp& app) {
    CubeVertex vertices[] = {
        // Front
        {{-0.5f,-0.5f,-0.5f},{0,0,-1}}, {{-0.5f,0.5f,-0.5f},{0,0,-1}}, {{0.5f,0.5f,-0.5f},{0,0,-1}}, {{0.5f,-0.5f,-0.5f},{0,0,-1}},
        // Back
        {{-0.5f,-0.5f,0.5f},{0,0,1}}, {{0.5f,-0.5f,0.5f},{0,0,1}}, {{0.5f,0.5f,0.5f},{0,0,1}}, {{-0.5f,0.5f,0.5f},{0,0,1}},
        // Top
        {{-0.5f,0.5f,-0.5f},{0,1,0}}, {{-0.5f,0.5f,0.5f},{0,1,0}}, {{0.5f,0.5f,0.5f},{0,1,0}}, {{0.5f,0.5f,-0.5f},{0,1,0}},
        // Bottom
        {{-0.5f,-0.5f,-0.5f},{0,-1,0}}, {{0.5f,-0.5f,-0.5f},{0,-1,0}}, {{0.5f,-0.5f,0.5f},{0,-1,0}}, {{-0.5f,-0.5f,0.5f},{0,-1,0}},
        // Left
        {{-0.5f,-0.5f,0.5f},{-1,0,0}}, {{-0.5f,0.5f,0.5f},{-1,0,0}}, {{-0.5f,0.5f,-0.5f},{-1,0,0}}, {{-0.5f,-0.5f,-0.5f},{-1,0,0}},
        // Right
        {{0.5f,-0.5f,-0.5f},{1,0,0}}, {{0.5f,0.5f,-0.5f},{1,0,0}}, {{0.5f,0.5f,0.5f},{1,0,0}}, {{0.5f,-0.5f,0.5f},{1,0,0}},
    };
    uint16_t indices[] = {
        0,1,2,0,2,3, 4,5,6,4,6,7, 8,9,10,8,10,11, 12,13,14,12,14,15, 16,17,18,16,18,19, 20,21,22,20,22,23
    };
    app.indexCount = sizeof(indices)/sizeof(indices[0]);

    if (!CreateBuffer(app, sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        app.vertexBuffer, app.vertexMemory)) return false;
    void* data;
    vkMapMemory(app.device, app.vertexMemory, 0, sizeof(vertices), 0, &data);
    memcpy(data, vertices, sizeof(vertices));
    vkUnmapMemory(app.device, app.vertexMemory);

    if (!CreateBuffer(app, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        app.indexBuffer, app.indexMemory)) return false;
    vkMapMemory(app.device, app.indexMemory, 0, sizeof(indices), 0, &data);
    memcpy(data, indices, sizeof(indices));
    vkUnmapMemory(app.device, app.indexMemory);

    // Uniform buffer
    if (!CreateBuffer(app, sizeof(UniformBufferObject), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        app.uniformBuffer, app.uniformMemory)) return false;
    vkMapMemory(app.device, app.uniformMemory, 0, sizeof(UniformBufferObject), 0, &app.uniformMapped);

    // Descriptor pool & set
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpInfo.poolSizeCount = 1;
    dpInfo.pPoolSizes = &poolSize;
    dpInfo.maxSets = 1;
    VK_CHECK(vkCreateDescriptorPool(app.device, &dpInfo, nullptr, &app.descriptorPool));

    VkDescriptorSetAllocateInfo daInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    daInfo.descriptorPool = app.descriptorPool;
    daInfo.descriptorSetCount = 1;
    daInfo.pSetLayouts = &app.descriptorSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(app.device, &daInfo, &app.descriptorSet));

    VkDescriptorBufferInfo bufInfo = {app.uniformBuffer, 0, sizeof(UniformBufferObject)};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = app.descriptorSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(app.device, 1, &write, 0, nullptr);

    LOG_INFO("Geometry created");
    return true;
}

// Create sync objects
static bool CreateSyncObjects(VulkanApp& app) {
    VkSemaphoreCreateInfo sInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateSemaphore(app.device, &sInfo, nullptr, &app.imageAvailableSemaphore));
    VK_CHECK(vkCreateSemaphore(app.device, &sInfo, nullptr, &app.renderFinishedSemaphore));
    VK_CHECK(vkCreateFence(app.device, &fInfo, nullptr, &app.inFlightFence));
    return true;
}

// Render frame
static void RenderFrame(VulkanApp& app) {
    vkWaitForFences(app.device, 1, &app.inFlightFence, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(app.device, app.swapchain, UINT64_MAX,
        app.imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || g_windowResized) {
        g_windowResized = false;
        CreateSwapchain(app, g_windowWidth, g_windowHeight);
        return;
    }

    vkResetFences(app.device, 1, &app.inFlightFence);

    // Update uniform buffer
    float aspect = (float)app.swapchainExtent.width / (float)app.swapchainExtent.height;
    Mat4 model = Mat4::RotationY(app.cubeRotation);
    Mat4 view = Mat4::Translation(0, 0, -3.0f);
    Mat4 proj = Mat4::PerspectiveFov(3.14159f / 4.0f, aspect, 0.1f, 100.0f);
    Mat4 mvp = (model * view * proj).Transpose();

    UniformBufferObject ubo;
    ubo.worldViewProj = mvp;
    ubo.color = {0.4f, 0.6f, 0.9f, 1.0f};
    memcpy(app.uniformMapped, &ubo, sizeof(ubo));

    // Record command buffer
    VkCommandBuffer cmd = app.commandBuffers[imageIndex];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkRenderPassBeginInfo rpBegin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass = app.renderPass;
    rpBegin.framebuffer = app.framebuffers[imageIndex];
    rpBegin.renderArea = {{0,0}, app.swapchainExtent};
    VkClearValue clears[2] = {};
    clears[0].color = {{0.1f, 0.1f, 0.15f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.pipelineLayout, 0, 1, &app.descriptorSet, 0, nullptr);

    VkViewport vp = {0, 0, (float)app.swapchainExtent.width, (float)app.swapchainExtent.height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = {{0,0}, app.swapchainExtent};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &app.vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, app.indexBuffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, app.indexCount, 1, 0, 0, 0);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // Submit
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &app.imageAvailableSemaphore;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &app.renderFinishedSemaphore;
    vkQueueSubmit(app.graphicsQueue, 1, &submit, app.inFlightFence);

    // Present
    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &app.renderFinishedSemaphore;
    present.swapchainCount = 1;
    present.pSwapchains = &app.swapchain;
    present.pImageIndices = &imageIndex;
    vkQueuePresentKHR(app.graphicsQueue, &present);
}

// Cleanup
static void Cleanup(VulkanApp& app) {
    if (app.device) {
        vkDeviceWaitIdle(app.device);

        if (app.uniformMapped) vkUnmapMemory(app.device, app.uniformMemory);
        if (app.uniformBuffer) vkDestroyBuffer(app.device, app.uniformBuffer, nullptr);
        if (app.uniformMemory) vkFreeMemory(app.device, app.uniformMemory, nullptr);
        if (app.vertexBuffer) vkDestroyBuffer(app.device, app.vertexBuffer, nullptr);
        if (app.vertexMemory) vkFreeMemory(app.device, app.vertexMemory, nullptr);
        if (app.indexBuffer) vkDestroyBuffer(app.device, app.indexBuffer, nullptr);
        if (app.indexMemory) vkFreeMemory(app.device, app.indexMemory, nullptr);

        if (app.descriptorPool) vkDestroyDescriptorPool(app.device, app.descriptorPool, nullptr);
        if (app.descriptorSetLayout) vkDestroyDescriptorSetLayout(app.device, app.descriptorSetLayout, nullptr);
        if (app.pipeline) vkDestroyPipeline(app.device, app.pipeline, nullptr);
        if (app.pipelineLayout) vkDestroyPipelineLayout(app.device, app.pipelineLayout, nullptr);

        for (auto fb : app.framebuffers) vkDestroyFramebuffer(app.device, fb, nullptr);
        for (auto iv : app.swapchainImageViews) vkDestroyImageView(app.device, iv, nullptr);
        if (app.depthView) vkDestroyImageView(app.device, app.depthView, nullptr);
        if (app.depthImage) vkDestroyImage(app.device, app.depthImage, nullptr);
        if (app.depthMemory) vkFreeMemory(app.device, app.depthMemory, nullptr);

        if (app.renderPass) vkDestroyRenderPass(app.device, app.renderPass, nullptr);
        if (app.swapchain) vkDestroySwapchainKHR(app.device, app.swapchain, nullptr);

        if (app.imageAvailableSemaphore) vkDestroySemaphore(app.device, app.imageAvailableSemaphore, nullptr);
        if (app.renderFinishedSemaphore) vkDestroySemaphore(app.device, app.renderFinishedSemaphore, nullptr);
        if (app.inFlightFence) vkDestroyFence(app.device, app.inFlightFence, nullptr);
        if (app.commandPool) vkDestroyCommandPool(app.device, app.commandPool, nullptr);

        vkDestroyDevice(app.device, nullptr);
    }
    if (app.surface) vkDestroySurfaceKHR(app.instance, app.surface, nullptr);
    if (app.instance) vkDestroyInstance(app.instance, nullptr);
}

// Main
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    LOG_INFO("=== Vulkan Cube Standalone ===");

    HWND hwnd = CreateAppWindow(hInstance);
    if (!hwnd) { LOG_ERROR("Failed to create window"); return 1; }

    VulkanApp app = {};

    // Set shader directory
    #ifdef SHADER_DIR
    app.shaderDir = SHADER_DIR;
    #else
    app.shaderDir = "shaders";
    #endif

    if (!InitVulkan(app, hwnd, hInstance)) { Cleanup(app); return 1; }
    if (!CreateRenderPass(app)) { Cleanup(app); return 1; }
    if (!CreateSwapchain(app, g_windowWidth, g_windowHeight)) { Cleanup(app); return 1; }
    if (!CreatePipeline(app)) { Cleanup(app); return 1; }
    if (!CreateGeometry(app)) { Cleanup(app); return 1; }
    if (!CreateSyncObjects(app)) { Cleanup(app); return 1; }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    LOG_INFO("Initialization complete, entering main loop");

    auto lastTime = std::chrono::high_resolution_clock::now();
    MSG msg = {};
    bool running = true;

    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        app.cubeRotation += dt * 0.5f;
        if (app.cubeRotation > 6.28318f) app.cubeRotation -= 6.28318f;

        RenderFrame(app);
    }

    LOG_INFO("Shutting down");
    Cleanup(app);
    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);
    LOG_INFO("Done");
    return 0;
}
