// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan rendering implementation
 */

#include "vulkan_renderer.h"
#include "logging.h"
#include <fstream>
#include <cmath>
#include <cstring>

// Math implementation
Mat4 Mat4::Identity() {
    Mat4 result = {};
    result.m[0][0] = 1.0f;
    result.m[1][1] = 1.0f;
    result.m[2][2] = 1.0f;
    result.m[3][3] = 1.0f;
    return result;
}

Mat4 Mat4::Translation(float x, float y, float z) {
    Mat4 result = Identity();
    result.m[3][0] = x;
    result.m[3][1] = y;
    result.m[3][2] = z;
    return result;
}

Mat4 Mat4::RotationY(float angle) {
    Mat4 result = Identity();
    float c = cosf(angle);
    float s = sinf(angle);
    result.m[0][0] = c;
    result.m[0][2] = -s;
    result.m[2][0] = s;
    result.m[2][2] = c;
    return result;
}

Mat4 Mat4::RotationRollPitchYaw(float pitch, float yaw, float roll) {
    float cp = cosf(pitch), sp = sinf(pitch);
    float cy = cosf(yaw), sy = sinf(yaw);
    float cr = cosf(roll), sr = sinf(roll);

    Mat4 result = {};
    result.m[0][0] = cr * cy + sr * sp * sy;
    result.m[0][1] = sr * cp;
    result.m[0][2] = cr * -sy + sr * sp * cy;
    result.m[1][0] = -sr * cy + cr * sp * sy;
    result.m[1][1] = cr * cp;
    result.m[1][2] = sr * sy + cr * sp * cy;
    result.m[2][0] = cp * sy;
    result.m[2][1] = -sp;
    result.m[2][2] = cp * cy;
    result.m[3][3] = 1.0f;
    return result;
}

Mat4 Mat4::PerspectiveFov(float fovY, float aspect, float nearZ, float farZ) {
    float h = 1.0f / tanf(fovY * 0.5f);
    float w = h / aspect;

    Mat4 result = {};
    result.m[0][0] = w;
    result.m[1][1] = h;
    result.m[2][2] = farZ / (nearZ - farZ);
    result.m[2][3] = -1.0f;
    result.m[3][2] = (nearZ * farZ) / (nearZ - farZ);
    return result;
}

Mat4 Mat4::operator*(const Mat4& other) const {
    Mat4 result = {};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            result.m[i][j] = 0;
            for (int k = 0; k < 4; k++) {
                result.m[i][j] += m[i][k] * other.m[k][j];
            }
        }
    }
    return result;
}

Mat4 Mat4::Transpose() const {
    Mat4 result;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            result.m[i][j] = m[j][i];
        }
    }
    return result;
}

// Vertex structures
struct CubeVertex {
    Vec3 position;
    Vec3 normal;
};

struct GridVertex {
    Vec3 position;
};

// Helper macros
#define VK_CHECK(call) \
    do { \
        VkResult result = (call); \
        if (result != VK_SUCCESS) { \
            LogVkResult(#call, result); \
            return false; \
        } \
    } while (0)

#define VK_CHECK_LOG(call) \
    do { \
        VkResult result = (call); \
        LogVkResult(#call, result); \
        if (result != VK_SUCCESS) { \
            return false; \
        } \
    } while (0)

// Helper to find memory type
static uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

// Helper to create buffer
static bool CreateBuffer(
    VulkanRenderer& renderer,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& bufferMemory
) {
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK(vkCreateBuffer(renderer.device, &bufferInfo, nullptr, &buffer));

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(renderer.device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(renderer.physicalDevice, memRequirements.memoryTypeBits, properties);

    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        LOG_ERROR("Failed to find suitable memory type");
        return false;
    }

    VK_CHECK(vkAllocateMemory(renderer.device, &allocInfo, nullptr, &bufferMemory));
    VK_CHECK(vkBindBufferMemory(renderer.device, buffer, bufferMemory, 0));

    return true;
}

// Helper to create image
static bool CreateImage(
    VulkanRenderer& renderer,
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& imageMemory
) {
    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK(vkCreateImage(renderer.device, &imageInfo, nullptr, &image));

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(renderer.device, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(renderer.physicalDevice, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        LOG_ERROR("Failed to find suitable memory type for image");
        return false;
    }

    VK_CHECK(vkAllocateMemory(renderer.device, &allocInfo, nullptr, &imageMemory));
    VK_CHECK(vkBindImageMemory(renderer.device, image, imageMemory, 0));

    return true;
}

// Helper to load shader module
static VkShaderModule LoadShaderModule(VkDevice device, const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open shader file: %s", path.c_str());
        return VK_NULL_HANDLE;
    }

    size_t fileSize = (size_t)file.tellg();
    std::vector<char> code(fileSize);
    file.seekg(0);
    file.read(code.data(), fileSize);
    file.close();

    VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        LOG_ERROR("Failed to create shader module from: %s", path.c_str());
        return VK_NULL_HANDLE;
    }

    LOG_INFO("Loaded shader: %s", path.c_str());
    return shaderModule;
}

bool InitializeVulkanInstance(VulkanRenderer& renderer, const std::vector<const char*>& requiredExtensions) {
    LOG_INFO("Creating Vulkan instance...");

    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "SessionTargetTestVulkan";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    // Log required extensions
    LOG_INFO("Required instance extensions:");
    for (const auto& ext : requiredExtensions) {
        LOG_INFO("  %s", ext);
    }

    VkInstanceCreateInfo createInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)requiredExtensions.size();
    createInfo.ppEnabledExtensionNames = requiredExtensions.data();

    VK_CHECK_LOG(vkCreateInstance(&createInfo, nullptr, &renderer.instance));
    LOG_INFO("Vulkan instance created: 0x%p", (void*)renderer.instance);

    return true;
}

bool InitializeVulkanDevice(
    VulkanRenderer& renderer,
    VkPhysicalDevice physicalDevice,
    const std::vector<const char*>& requiredExtensions
) {
    renderer.physicalDevice = physicalDevice;

    // Get device properties
    VkPhysicalDeviceProperties deviceProps;
    vkGetPhysicalDeviceProperties(physicalDevice, &deviceProps);
    LOG_INFO("Using physical device: %s", deviceProps.deviceName);

    // Find graphics queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    renderer.graphicsQueueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            renderer.graphicsQueueFamily = i;
            break;
        }
    }

    if (renderer.graphicsQueueFamily == UINT32_MAX) {
        LOG_ERROR("No graphics queue family found");
        return false;
    }
    LOG_INFO("Graphics queue family: %u", renderer.graphicsQueueFamily);

    // Create device
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueCreateInfo.queueFamilyIndex = renderer.graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    LOG_INFO("Required device extensions:");
    for (const auto& ext : requiredExtensions) {
        LOG_INFO("  %s", ext);
    }

    VkDeviceCreateInfo deviceCreateInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = (uint32_t)requiredExtensions.size();
    deviceCreateInfo.ppEnabledExtensionNames = requiredExtensions.data();

    VK_CHECK_LOG(vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &renderer.device));
    LOG_INFO("Vulkan device created: 0x%p", (void*)renderer.device);

    vkGetDeviceQueue(renderer.device, renderer.graphicsQueueFamily, 0, &renderer.graphicsQueue);
    LOG_INFO("Graphics queue: 0x%p", (void*)renderer.graphicsQueue);

    // Create command pool
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = renderer.graphicsQueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VK_CHECK(vkCreateCommandPool(renderer.device, &poolInfo, nullptr, &renderer.commandPool));
    LOG_INFO("Command pool created");

    // Allocate command buffer
    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = renderer.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VK_CHECK(vkAllocateCommandBuffers(renderer.device, &allocInfo, &renderer.commandBuffer));
    LOG_INFO("Command buffer allocated");

    // Create fence
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(renderer.device, &fenceInfo, nullptr, &renderer.renderFence));
    LOG_INFO("Render fence created");

    return true;
}

static bool CreateRenderPass(VulkanRenderer& renderer) {
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = renderer.colorFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttachment = {};
    depthAttachment.format = renderer.depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};

    VkRenderPassCreateInfo renderPassInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = (uint32_t)attachments.size();
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VK_CHECK(vkCreateRenderPass(renderer.device, &renderPassInfo, nullptr, &renderer.renderPass));
    LOG_INFO("Render pass created");

    return true;
}

static bool CreateDescriptorSetLayout(VulkanRenderer& renderer, VkDescriptorSetLayout& layout) {
    VkDescriptorSetLayoutBinding uboBinding = {};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uboBinding;

    VK_CHECK(vkCreateDescriptorSetLayout(renderer.device, &layoutInfo, nullptr, &layout));
    return true;
}

static bool CreatePipeline(
    VulkanRenderer& renderer,
    const std::string& vertShaderPath,
    const std::string& fragShaderPath,
    VkDescriptorSetLayout descriptorSetLayout,
    VkPipelineLayout& pipelineLayout,
    VkPipeline& pipeline,
    bool isLineList,
    bool hasNormals
) {
    // Load shaders
    VkShaderModule vertModule = LoadShaderModule(renderer.device, vertShaderPath);
    VkShaderModule fragModule = LoadShaderModule(renderer.device, fragShaderPath);

    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        if (vertModule) vkDestroyShaderModule(renderer.device, vertModule, nullptr);
        if (fragModule) vkDestroyShaderModule(renderer.device, fragModule, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo vertStage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    // Vertex input
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;

    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = hasNormals ? sizeof(CubeVertex) : sizeof(GridVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings.push_back(binding);

    VkVertexInputAttributeDescription posAttr = {};
    posAttr.binding = 0;
    posAttr.location = 0;
    posAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    posAttr.offset = 0;
    attributes.push_back(posAttr);

    if (hasNormals) {
        VkVertexInputAttributeDescription normalAttr = {};
        normalAttr.binding = 0;
        normalAttr.location = 1;
        normalAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
        normalAttr.offset = sizeof(Vec3);
        attributes.push_back(normalAttr);
    }

    VkPipelineVertexInputStateCreateInfo vertexInput = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = (uint32_t)bindings.size();
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = (uint32_t)attributes.size();
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = isLineList ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = isLineList ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlending = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = (uint32_t)dynamicStates.size();
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;

    VK_CHECK(vkCreatePipelineLayout(renderer.device, &layoutInfo, nullptr, &pipelineLayout));

    VkGraphicsPipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderer.renderPass;
    pipelineInfo.subpass = 0;

    VK_CHECK(vkCreateGraphicsPipelines(renderer.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));

    vkDestroyShaderModule(renderer.device, vertModule, nullptr);
    vkDestroyShaderModule(renderer.device, fragModule, nullptr);

    return true;
}

static bool CreateDescriptorPool(VulkanRenderer& renderer, VkDescriptorPool& pool) {
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;

    VK_CHECK(vkCreateDescriptorPool(renderer.device, &poolInfo, nullptr, &pool));
    return true;
}

static bool CreateDescriptorSet(
    VulkanRenderer& renderer,
    VkDescriptorSetLayout layout,
    VkDescriptorPool pool,
    VkBuffer uniformBuffer,
    VkDescriptorSet& descriptorSet
) {
    VkDescriptorSetAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VK_CHECK(vkAllocateDescriptorSets(renderer.device, &allocInfo, &descriptorSet));

    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = uniformBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(UniformBufferObject);

    VkWriteDescriptorSet descriptorWrite = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    descriptorWrite.dstSet = descriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(renderer.device, 1, &descriptorWrite, 0, nullptr);
    return true;
}

static bool CreateGeometryBuffers(VulkanRenderer& renderer) {
    // Cube vertices with normals for lighting
    CubeVertex cubeVertices[] = {
        // Front face
        {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}},
        {{-0.5f,  0.5f, -0.5f}, {0, 0, -1}},
        {{ 0.5f,  0.5f, -0.5f}, {0, 0, -1}},
        {{ 0.5f, -0.5f, -0.5f}, {0, 0, -1}},
        // Back face
        {{-0.5f, -0.5f,  0.5f}, {0, 0, 1}},
        {{ 0.5f, -0.5f,  0.5f}, {0, 0, 1}},
        {{ 0.5f,  0.5f,  0.5f}, {0, 0, 1}},
        {{-0.5f,  0.5f,  0.5f}, {0, 0, 1}},
        // Top face
        {{-0.5f,  0.5f, -0.5f}, {0, 1, 0}},
        {{-0.5f,  0.5f,  0.5f}, {0, 1, 0}},
        {{ 0.5f,  0.5f,  0.5f}, {0, 1, 0}},
        {{ 0.5f,  0.5f, -0.5f}, {0, 1, 0}},
        // Bottom face
        {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}},
        {{ 0.5f, -0.5f, -0.5f}, {0, -1, 0}},
        {{ 0.5f, -0.5f,  0.5f}, {0, -1, 0}},
        {{-0.5f, -0.5f,  0.5f}, {0, -1, 0}},
        // Left face
        {{-0.5f, -0.5f,  0.5f}, {-1, 0, 0}},
        {{-0.5f,  0.5f,  0.5f}, {-1, 0, 0}},
        {{-0.5f,  0.5f, -0.5f}, {-1, 0, 0}},
        {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}},
        // Right face
        {{ 0.5f, -0.5f, -0.5f}, {1, 0, 0}},
        {{ 0.5f,  0.5f, -0.5f}, {1, 0, 0}},
        {{ 0.5f,  0.5f,  0.5f}, {1, 0, 0}},
        {{ 0.5f, -0.5f,  0.5f}, {1, 0, 0}},
    };

    uint16_t cubeIndices[] = {
        0, 1, 2, 0, 2, 3,       // Front
        4, 5, 6, 4, 6, 7,       // Back
        8, 9, 10, 8, 10, 11,    // Top
        12, 13, 14, 12, 14, 15, // Bottom
        16, 17, 18, 16, 18, 19, // Left
        20, 21, 22, 20, 22, 23, // Right
    };
    renderer.cubeIndexCount = sizeof(cubeIndices) / sizeof(cubeIndices[0]);

    // Create cube vertex buffer
    VkDeviceSize cubeVertexSize = sizeof(cubeVertices);
    if (!CreateBuffer(renderer, cubeVertexSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeVertexBuffer, renderer.cubeVertexMemory)) {
        return false;
    }

    void* data;
    vkMapMemory(renderer.device, renderer.cubeVertexMemory, 0, cubeVertexSize, 0, &data);
    memcpy(data, cubeVertices, cubeVertexSize);
    vkUnmapMemory(renderer.device, renderer.cubeVertexMemory);

    // Create cube index buffer
    VkDeviceSize cubeIndexSize = sizeof(cubeIndices);
    if (!CreateBuffer(renderer, cubeIndexSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeIndexBuffer, renderer.cubeIndexMemory)) {
        return false;
    }

    vkMapMemory(renderer.device, renderer.cubeIndexMemory, 0, cubeIndexSize, 0, &data);
    memcpy(data, cubeIndices, cubeIndexSize);
    vkUnmapMemory(renderer.device, renderer.cubeIndexMemory);

    // Create grid vertices (floor at y = -1)
    const int gridSize = 10;
    const float gridSpacing = 1.0f;
    std::vector<GridVertex> gridVertices;

    for (int i = -gridSize; i <= gridSize; i++) {
        // Lines along Z axis
        gridVertices.push_back({{(float)i * gridSpacing, -1.0f, -gridSize * gridSpacing}});
        gridVertices.push_back({{(float)i * gridSpacing, -1.0f, gridSize * gridSpacing}});
        // Lines along X axis
        gridVertices.push_back({{-gridSize * gridSpacing, -1.0f, (float)i * gridSpacing}});
        gridVertices.push_back({{gridSize * gridSpacing, -1.0f, (float)i * gridSpacing}});
    }
    renderer.gridVertexCount = (uint32_t)gridVertices.size();

    VkDeviceSize gridVertexSize = gridVertices.size() * sizeof(GridVertex);
    if (!CreateBuffer(renderer, gridVertexSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.gridVertexBuffer, renderer.gridVertexMemory)) {
        return false;
    }

    vkMapMemory(renderer.device, renderer.gridVertexMemory, 0, gridVertexSize, 0, &data);
    memcpy(data, gridVertices.data(), gridVertexSize);
    vkUnmapMemory(renderer.device, renderer.gridVertexMemory);

    LOG_INFO("Created geometry buffers");
    return true;
}

static bool CreateUniformBuffers(VulkanRenderer& renderer) {
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);

    // Cube uniform buffer
    if (!CreateBuffer(renderer, bufferSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeUniformBuffer, renderer.cubeUniformMemory)) {
        return false;
    }
    vkMapMemory(renderer.device, renderer.cubeUniformMemory, 0, bufferSize, 0, &renderer.cubeUniformMapped);

    // Grid uniform buffer
    if (!CreateBuffer(renderer, bufferSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.gridUniformBuffer, renderer.gridUniformMemory)) {
        return false;
    }
    vkMapMemory(renderer.device, renderer.gridUniformMemory, 0, bufferSize, 0, &renderer.gridUniformMapped);

    LOG_INFO("Created uniform buffers");
    return true;
}

bool CreateRenderingResources(VulkanRenderer& renderer) {
    LOG_INFO("Creating rendering resources...");

    // Set shader directory - try multiple paths
    // First try the CMake-defined path
    #ifdef SHADER_DIR
    renderer.shaderDir = SHADER_DIR;
    #else
    renderer.shaderDir = "shaders";
    #endif

    // Check if shaders exist, if not try relative path
    std::ifstream testFile(renderer.shaderDir + "/cube.vert.spv");
    if (!testFile.is_open()) {
        renderer.shaderDir = "shaders";
        testFile.open(renderer.shaderDir + "/cube.vert.spv");
        if (!testFile.is_open()) {
            LOG_ERROR("Cannot find shader directory");
            return false;
        }
    }
    testFile.close();
    LOG_INFO("Shader directory: %s", renderer.shaderDir.c_str());

    if (!CreateRenderPass(renderer)) return false;
    if (!CreateGeometryBuffers(renderer)) return false;
    if (!CreateUniformBuffers(renderer)) return false;

    // Cube pipeline
    if (!CreateDescriptorSetLayout(renderer, renderer.cubeDescriptorSetLayout)) return false;
    if (!CreateDescriptorPool(renderer, renderer.cubeDescriptorPool)) return false;
    if (!CreateDescriptorSet(renderer, renderer.cubeDescriptorSetLayout, renderer.cubeDescriptorPool,
        renderer.cubeUniformBuffer, renderer.cubeDescriptorSet)) return false;
    if (!CreatePipeline(renderer, renderer.shaderDir + "/cube.vert.spv", renderer.shaderDir + "/cube.frag.spv",
        renderer.cubeDescriptorSetLayout, renderer.cubePipelineLayout, renderer.cubePipeline, false, true)) {
        return false;
    }
    LOG_INFO("Cube pipeline created");

    // Grid pipeline
    if (!CreateDescriptorSetLayout(renderer, renderer.gridDescriptorSetLayout)) return false;
    if (!CreateDescriptorPool(renderer, renderer.gridDescriptorPool)) return false;
    if (!CreateDescriptorSet(renderer, renderer.gridDescriptorSetLayout, renderer.gridDescriptorPool,
        renderer.gridUniformBuffer, renderer.gridDescriptorSet)) return false;
    if (!CreatePipeline(renderer, renderer.shaderDir + "/grid.vert.spv", renderer.shaderDir + "/grid.frag.spv",
        renderer.gridDescriptorSetLayout, renderer.gridPipelineLayout, renderer.gridPipeline, true, false)) {
        return false;
    }
    LOG_INFO("Grid pipeline created");

    LOG_INFO("Rendering resources created successfully");
    return true;
}

bool SetupSwapchainResources(
    VulkanRenderer& renderer,
    int eye,
    const std::vector<VkImage>& swapchainImages,
    uint32_t width,
    uint32_t height,
    VkFormat format
) {
    LOG_INFO("Setting up swapchain resources for eye %d (%ux%u, %d images)", eye, width, height, (int)swapchainImages.size());

    renderer.swapchainWidth[eye] = width;
    renderer.swapchainHeight[eye] = height;
    renderer.colorFormat = format;

    renderer.eyeResources[eye].resize(swapchainImages.size());

    for (size_t i = 0; i < swapchainImages.size(); i++) {
        SwapchainImageResources& res = renderer.eyeResources[eye][i];
        res.image = swapchainImages[i];

        // Create image view
        VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = res.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(renderer.device, &viewInfo, nullptr, &res.imageView));

        // Create depth buffer
        if (!CreateImage(renderer, width, height, renderer.depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, res.depthImage, res.depthMemory)) {
            return false;
        }

        // Create depth image view
        VkImageViewCreateInfo depthViewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        depthViewInfo.image = res.depthImage;
        depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthViewInfo.format = renderer.depthFormat;
        depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthViewInfo.subresourceRange.baseMipLevel = 0;
        depthViewInfo.subresourceRange.levelCount = 1;
        depthViewInfo.subresourceRange.baseArrayLayer = 0;
        depthViewInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(renderer.device, &depthViewInfo, nullptr, &res.depthView));

        // Create framebuffer
        VkImageView attachments[] = {res.imageView, res.depthView};

        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = renderer.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;

        VK_CHECK(vkCreateFramebuffer(renderer.device, &fbInfo, nullptr, &res.framebuffer));
    }

    LOG_INFO("Swapchain resources for eye %d created successfully", eye);
    return true;
}

void CleanupVulkan(VulkanRenderer& renderer) {
    LOG_INFO("Cleaning up Vulkan resources...");

    if (renderer.device) {
        vkDeviceWaitIdle(renderer.device);

        // Cleanup per-eye resources
        for (int eye = 0; eye < 2; eye++) {
            for (auto& res : renderer.eyeResources[eye]) {
                if (res.framebuffer) vkDestroyFramebuffer(renderer.device, res.framebuffer, nullptr);
                if (res.depthView) vkDestroyImageView(renderer.device, res.depthView, nullptr);
                if (res.depthImage) vkDestroyImage(renderer.device, res.depthImage, nullptr);
                if (res.depthMemory) vkFreeMemory(renderer.device, res.depthMemory, nullptr);
                if (res.imageView) vkDestroyImageView(renderer.device, res.imageView, nullptr);
            }
            renderer.eyeResources[eye].clear();
        }

        // Cleanup uniform buffers
        if (renderer.cubeUniformMapped) {
            vkUnmapMemory(renderer.device, renderer.cubeUniformMemory);
            renderer.cubeUniformMapped = nullptr;
        }
        if (renderer.cubeUniformBuffer) vkDestroyBuffer(renderer.device, renderer.cubeUniformBuffer, nullptr);
        if (renderer.cubeUniformMemory) vkFreeMemory(renderer.device, renderer.cubeUniformMemory, nullptr);

        if (renderer.gridUniformMapped) {
            vkUnmapMemory(renderer.device, renderer.gridUniformMemory);
            renderer.gridUniformMapped = nullptr;
        }
        if (renderer.gridUniformBuffer) vkDestroyBuffer(renderer.device, renderer.gridUniformBuffer, nullptr);
        if (renderer.gridUniformMemory) vkFreeMemory(renderer.device, renderer.gridUniformMemory, nullptr);

        // Cleanup geometry buffers
        if (renderer.cubeVertexBuffer) vkDestroyBuffer(renderer.device, renderer.cubeVertexBuffer, nullptr);
        if (renderer.cubeVertexMemory) vkFreeMemory(renderer.device, renderer.cubeVertexMemory, nullptr);
        if (renderer.cubeIndexBuffer) vkDestroyBuffer(renderer.device, renderer.cubeIndexBuffer, nullptr);
        if (renderer.cubeIndexMemory) vkFreeMemory(renderer.device, renderer.cubeIndexMemory, nullptr);
        if (renderer.gridVertexBuffer) vkDestroyBuffer(renderer.device, renderer.gridVertexBuffer, nullptr);
        if (renderer.gridVertexMemory) vkFreeMemory(renderer.device, renderer.gridVertexMemory, nullptr);

        // Cleanup pipelines
        if (renderer.cubePipeline) vkDestroyPipeline(renderer.device, renderer.cubePipeline, nullptr);
        if (renderer.cubePipelineLayout) vkDestroyPipelineLayout(renderer.device, renderer.cubePipelineLayout, nullptr);
        if (renderer.cubeDescriptorPool) vkDestroyDescriptorPool(renderer.device, renderer.cubeDescriptorPool, nullptr);
        if (renderer.cubeDescriptorSetLayout) vkDestroyDescriptorSetLayout(renderer.device, renderer.cubeDescriptorSetLayout, nullptr);

        if (renderer.gridPipeline) vkDestroyPipeline(renderer.device, renderer.gridPipeline, nullptr);
        if (renderer.gridPipelineLayout) vkDestroyPipelineLayout(renderer.device, renderer.gridPipelineLayout, nullptr);
        if (renderer.gridDescriptorPool) vkDestroyDescriptorPool(renderer.device, renderer.gridDescriptorPool, nullptr);
        if (renderer.gridDescriptorSetLayout) vkDestroyDescriptorSetLayout(renderer.device, renderer.gridDescriptorSetLayout, nullptr);

        if (renderer.renderPass) vkDestroyRenderPass(renderer.device, renderer.renderPass, nullptr);
        if (renderer.renderFence) vkDestroyFence(renderer.device, renderer.renderFence, nullptr);
        if (renderer.commandPool) vkDestroyCommandPool(renderer.device, renderer.commandPool, nullptr);

        vkDestroyDevice(renderer.device, nullptr);
        renderer.device = VK_NULL_HANDLE;
    }

    if (renderer.instance) {
        vkDestroyInstance(renderer.instance, nullptr);
        renderer.instance = VK_NULL_HANDLE;
    }

    LOG_INFO("Vulkan cleanup complete");
}

void UpdateScene(VulkanRenderer& renderer, float deltaTime) {
    renderer.cubeRotation += deltaTime * 0.5f;
    if (renderer.cubeRotation > 6.28318f) {
        renderer.cubeRotation -= 6.28318f;
    }
}

void RenderScene(
    VulkanRenderer& renderer,
    int eye,
    uint32_t imageIndex,
    const Mat4& viewMatrix,
    const Mat4& projMatrix,
    float cameraPosX,
    float cameraPosY,
    float cameraPosZ,
    float cameraYaw,
    float cameraPitch
) {
    if (imageIndex >= renderer.eyeResources[eye].size()) {
        LOG_ERROR("Invalid image index %u for eye %d", imageIndex, eye);
        return;
    }

    SwapchainImageResources& res = renderer.eyeResources[eye][imageIndex];

    // Wait for previous frame
    vkWaitForFences(renderer.device, 1, &renderer.renderFence, VK_TRUE, UINT64_MAX);
    vkResetFences(renderer.device, 1, &renderer.renderFence);

    // Begin command buffer
    vkResetCommandBuffer(renderer.commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(renderer.commandBuffer, &beginInfo);

    // Build camera view matrix from input state
    Mat4 cameraOffset = Mat4::Translation(-cameraPosX, -cameraPosY, -cameraPosZ);
    Mat4 cameraRotation = Mat4::RotationRollPitchYaw(cameraPitch, cameraYaw, 0);
    Mat4 view = cameraRotation * cameraOffset * viewMatrix;

    // Begin render pass
    VkRenderPassBeginInfo renderPassInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    renderPassInfo.renderPass = renderer.renderPass;
    renderPassInfo.framebuffer = res.framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {renderer.swapchainWidth[eye], renderer.swapchainHeight[eye]};

    VkClearValue clearValues[2] = {};
    clearValues[0].color = {{0.1f, 0.1f, 0.15f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = 2;
    renderPassInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(renderer.commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Set viewport and scissor
    VkViewport viewport = {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = (float)renderer.swapchainWidth[eye];
    viewport.height = (float)renderer.swapchainHeight[eye];
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(renderer.commandBuffer, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {renderer.swapchainWidth[eye], renderer.swapchainHeight[eye]};
    vkCmdSetScissor(renderer.commandBuffer, 0, 1, &scissor);

    // Render cube
    {
        Mat4 cubeWorld = Mat4::RotationY(renderer.cubeRotation);
        Mat4 cubeWVP = (cubeWorld * view * projMatrix).Transpose();

        UniformBufferObject ubo;
        ubo.worldViewProj = cubeWVP;
        ubo.color = {0.4f, 0.6f, 0.9f, 1.0f}; // Blue-ish cube
        memcpy(renderer.cubeUniformMapped, &ubo, sizeof(ubo));

        vkCmdBindPipeline(renderer.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.cubePipeline);
        vkCmdBindDescriptorSets(renderer.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            renderer.cubePipelineLayout, 0, 1, &renderer.cubeDescriptorSet, 0, nullptr);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(renderer.commandBuffer, 0, 1, &renderer.cubeVertexBuffer, &offset);
        vkCmdBindIndexBuffer(renderer.commandBuffer, renderer.cubeIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(renderer.commandBuffer, renderer.cubeIndexCount, 1, 0, 0, 0);
    }

    // Render grid
    {
        Mat4 gridWVP = (view * projMatrix).Transpose();

        UniformBufferObject ubo;
        ubo.worldViewProj = gridWVP;
        ubo.color = {0.3f, 0.3f, 0.35f, 1.0f}; // Gray grid
        memcpy(renderer.gridUniformMapped, &ubo, sizeof(ubo));

        vkCmdBindPipeline(renderer.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.gridPipeline);
        vkCmdBindDescriptorSets(renderer.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            renderer.gridPipelineLayout, 0, 1, &renderer.gridDescriptorSet, 0, nullptr);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(renderer.commandBuffer, 0, 1, &renderer.gridVertexBuffer, &offset);
        vkCmdDraw(renderer.commandBuffer, renderer.gridVertexCount, 1, 0, 0);
    }

    vkCmdEndRenderPass(renderer.commandBuffer);
    vkEndCommandBuffer(renderer.commandBuffer);

    // Submit
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &renderer.commandBuffer;

    vkQueueSubmit(renderer.graphicsQueue, 1, &submitInfo, renderer.renderFence);
}

VkInstance GetVulkanInstance(const VulkanRenderer& renderer) {
    return renderer.instance;
}

VkPhysicalDevice GetVulkanPhysicalDevice(const VulkanRenderer& renderer) {
    return renderer.physicalDevice;
}

VkDevice GetVulkanDevice(const VulkanRenderer& renderer) {
    return renderer.device;
}

uint32_t GetGraphicsQueueFamily(const VulkanRenderer& renderer) {
    return renderer.graphicsQueueFamily;
}

VkQueue GetGraphicsQueue(const VulkanRenderer& renderer) {
    return renderer.graphicsQueue;
}
