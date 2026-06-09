// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// cube_texture_vk_android — Android port of cube_texture_d3d11_win.
//
// Texture app class: the app provides a shared VkImage to the runtime, the
// runtime weaves the stereo cube into a CANVAS sub-rect of it (and presents
// nothing), and the app draws the 2D SURROUND (checkerboard + gradient +
// bright canvas border, faithfully ported from the Windows demo's
// g_surroundPSSource) around the canvas and PRESENTS on its own swapchain.
//
// Faithful, minimal port: standard colored cube + the surround pattern. No
// HUD / eye readout / rotation hacks / cold-start dialog.

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_android_surface_binding.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <time.h>

#include "shaders/cube.vert.h"
#include "shaders/cube.frag.h"
#include "shaders/surround.vert.h"
#include "shaders/surround.frag.h"

#define LOG_TAG "cube_texture_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// ─── OpenXR + Vulkan globals ──────────────────────────────────────────────
XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system_id = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;
bool g_session_running = false;
bool g_exit_requested = false;
XrSpace g_app_space = XR_NULL_HANDLE;
XrVersion g_required_vk_version = XR_MAKE_VERSION(1, 1, 0);

VkInstance g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_phys_device = VK_NULL_HANDLE;
VkDevice g_vk_device = VK_NULL_HANDLE;
VkQueue g_vk_queue = VK_NULL_HANDLE;
uint32_t g_vk_queue_family = UINT32_MAX;
VkCommandPool g_cmd_pool = VK_NULL_HANDLE;

struct android_app *g_app = nullptr;
uint64_t g_frame_count = 0;

constexpr uint32_t kViewCount = 2;

// Per-view OpenXR swapchain the app renders the cube into; the runtime atlases
// these and the DP weaves them into the shared image's canvas.
struct PerView {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    uint32_t width{0}, height{0};
    XrSwapchainImageVulkanKHR images[8]{};
    VkImageView views[8]{};
    VkFramebuffer fbs[8]{};
    uint32_t image_count{0};
    VkImage depth_image{VK_NULL_HANDLE};
    VkDeviceMemory depth_mem{VK_NULL_HANDLE};
    VkImageView depth_view{VK_NULL_HANDLE};
};
PerView g_views[kViewCount];
VkFormat g_color_format = VK_FORMAT_UNDEFINED;     // OpenXR view swapchain format
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// Cube pipeline.
VkRenderPass g_cube_rp = VK_NULL_HANDLE;
VkPipelineLayout g_cube_layout = VK_NULL_HANDLE;
VkPipeline g_cube_pipe = VK_NULL_HANDLE;
VkBuffer g_cube_vbuf = VK_NULL_HANDLE;
VkDeviceMemory g_cube_vmem = VK_NULL_HANDLE;
struct CubeVertex { float pos[3]; float color[3]; };
constexpr uint32_t kCubeVertexCount = 36;

// ─── texture-class state ──────────────────────────────────────────────────
constexpr VkFormat kSharedFormat = VK_FORMAT_B8G8R8A8_UNORM;
VkImage g_shared_image = VK_NULL_HANDLE;
VkDeviceMemory g_shared_mem = VK_NULL_HANDLE;
uint32_t g_shared_w = 0, g_shared_h = 0;

constexpr float kCanvasInsetFrac = 0.12f;
int32_t g_canvas_x = 0, g_canvas_y = 0;
uint32_t g_canvas_w = 0, g_canvas_h = 0;
PFN_xrSetSharedTextureOutputRectEXT g_set_output_rect = nullptr;

// App-owned present swapchain + surround pipeline.
VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_present_swapchain = VK_NULL_HANDLE;
VkFormat g_present_format = VK_FORMAT_UNDEFINED;
uint32_t g_present_w = 0, g_present_h = 0;
VkImage g_present_images[8] = {};
VkImageView g_present_views[8] = {};
VkFramebuffer g_present_fbs[8] = {};
uint32_t g_present_image_count = 0;
VkSemaphore g_acquire_sem = VK_NULL_HANDLE;
VkSemaphore g_blit_sem = VK_NULL_HANDLE;
VkRenderPass g_surround_rp = VK_NULL_HANDLE;
VkPipelineLayout g_surround_layout = VK_NULL_HANDLE;
VkPipeline g_surround_pipe = VK_NULL_HANDLE;
bool g_present_ready = false;

struct SurroundPC {
    float windowSize[2];
    float _pad0[2];
    int32_t canvas[4];
    float time;
    float _pad1[3];
};

// ─── logging helper ───────────────────────────────────────────────────────
const char *xr_str(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance != XR_NULL_HANDLE && xrResultToString(g_instance, r, buf) == XR_SUCCESS) return buf;
    snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
    return buf;
}
void log_xr(const char *what, XrResult r) {
    if (XR_SUCCEEDED(r)) LOGI("%s -> %s", what, xr_str(r));
    else LOGE("%s -> %s", what, xr_str(r));
}

// ─── math (column-major mat4) ─────────────────────────────────────────────
struct Mat4 { float m[16]; };
Mat4 mat4_identity() {
    Mat4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f; return r;
}
Mat4 mat4_mul(const Mat4 &a, const Mat4 &b) {
    Mat4 c{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[col * 4 + k];
            c.m[col * 4 + row] = s;
        }
    return c;
}
// View = inverse(pose). R from quaternion, then transpose + (-R^T t).
Mat4 view_from_pose(const XrPosef &p) {
    float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    // World-space rotation columns.
    float r00 = 1 - 2 * (yy + zz), r01 = 2 * (xy - wz), r02 = 2 * (xz + wy);
    float r10 = 2 * (xy + wz), r11 = 1 - 2 * (xx + zz), r12 = 2 * (yz - wx);
    float r20 = 2 * (xz - wy), r21 = 2 * (yz + wx), r22 = 1 - 2 * (xx + yy);
    float tx = p.position.x, ty = p.position.y, tz = p.position.z;
    Mat4 v{};
    // view = R^T (transpose of the rotation) — store column-major.
    v.m[0] = r00; v.m[1] = r01; v.m[2] = r02; v.m[3] = 0;
    v.m[4] = r10; v.m[5] = r11; v.m[6] = r12; v.m[7] = 0;
    v.m[8] = r20; v.m[9] = r21; v.m[10] = r22; v.m[11] = 0;
    // translation = -R^T t
    v.m[12] = -(r00 * tx + r10 * ty + r20 * tz);
    v.m[13] = -(r01 * tx + r11 * ty + r21 * tz);
    v.m[14] = -(r02 * tx + r12 * ty + r22 * tz);
    v.m[15] = 1.0f;
    return v;
}
// Asymmetric-FOV projection for Vulkan clip (z in [0,1], y down). Mirrors
// XrMatrix4x4f_CreateProjectionFov(GRAPHICS_VULKAN).
Mat4 proj_from_fov(const XrFovf &fov, float nearZ, float farZ) {
    float tl = tanf(fov.angleLeft), tr = tanf(fov.angleRight);
    float tu = tanf(fov.angleUp), td = tanf(fov.angleDown);
    float w = tr - tl, h = (td - tu);  // Vulkan: y down
    Mat4 r{};
    r.m[0] = 2.0f / w;
    r.m[5] = 2.0f / h;
    r.m[8] = (tr + tl) / w;
    r.m[9] = (tu + td) / h;
    r.m[10] = -(farZ) / (farZ - nearZ);
    r.m[11] = -1.0f;
    r.m[14] = -(farZ * nearZ) / (farZ - nearZ);
    return r;
}
Mat4 cube_model(float angle) {
    float c = cosf(angle), s = sinf(angle);
    Mat4 r = mat4_identity();
    // Spin about Y.
    r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
    return r;
}

uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g_vk_phys_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}
VkShaderModule make_module(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes; ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g_vk_device, &ci, nullptr, &m);
    return m;
}

// ─── OpenXR / Vulkan bring-up ─────────────────────────────────────────────
bool initialize_loader(struct android_app *app) {
    PFN_xrInitializeLoaderKHR fn = nullptr;
    if (xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                              reinterpret_cast<PFN_xrVoidFunction *>(&fn)) != XR_SUCCESS || !fn)
        return false;
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = app->activity->vm;
    li.applicationContext = app->activity->clazz;
    XrResult r = fn(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&li));
    log_xr("xrInitializeLoaderKHR", r);
    return r == XR_SUCCESS;
}

bool create_instance(struct android_app *app) {
    const char *exts[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
        XR_EXT_ANDROID_SURFACE_BINDING_EXTENSION_NAME,  // exposes the canvas/surround entry points
    };
    XrInstanceCreateInfoAndroidKHR ai{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = app->activity->vm;
    ai.applicationActivity = app->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ai;
    std::strncpy(ci.applicationInfo.applicationName, "cube_texture_vk_android", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    std::strncpy(ci.applicationInfo.engineName, "displayxr", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]);
    ci.enabledExtensionNames = exts;

    XrResult r = XR_ERROR_RUNTIME_UNAVAILABLE;
    for (int attempt = 0; attempt < 5; ++attempt) {
        r = xrCreateInstance(&ci, &g_instance);
        if (r != XR_ERROR_RUNTIME_UNAVAILABLE) break;
        LOGW("xrCreateInstance: runtime unavailable (attempt %d/5)", attempt + 1);
        struct timespec ts{0, 400 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    log_xr("xrCreateInstance", r);
    if (r != XR_SUCCESS) return false;
    LOGI("ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS");
    return true;
}

bool query_system_and_graphics_reqs() {
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (xrGetSystem(g_instance, &si, &g_system_id) != XR_SUCCESS) return false;

    PFN_xrGetVulkanGraphicsRequirements2KHR fn = nullptr;
    if (xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsRequirements2KHR",
                              reinterpret_cast<PFN_xrVoidFunction *>(&fn)) != XR_SUCCESS || !fn)
        return false;
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XrResult r = fn(g_instance, g_system_id, &req);
    log_xr("xrGetVulkanGraphicsRequirements2KHR", r);
    return r == XR_SUCCESS;
}

bool create_vulkan_instance() {
    PFN_xrCreateVulkanInstanceKHR fn = nullptr;
    if (xrGetInstanceProcAddr(g_instance, "xrCreateVulkanInstanceKHR",
                              reinterpret_cast<PFN_xrVoidFunction *>(&fn)) != XR_SUCCESS || !fn)
        return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "cube_texture_vk_android";
    app.apiVersion = VK_MAKE_VERSION(XR_VERSION_MAJOR(g_required_vk_version),
                                     XR_VERSION_MINOR(g_required_vk_version), 0);
    VkInstanceCreateInfo vci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vci.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xi.systemId = g_system_id;
    xi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xi.vulkanCreateInfo = &vci;
    VkResult vr = VK_SUCCESS;
    XrResult r = fn(g_instance, &xi, &g_vk_instance, &vr);
    log_xr("xrCreateVulkanInstanceKHR", r);
    return r == XR_SUCCESS && vr == VK_SUCCESS;
}

bool pick_physical_device() {
    PFN_xrGetVulkanGraphicsDevice2KHR fn = nullptr;
    if (xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsDevice2KHR",
                              reinterpret_cast<PFN_xrVoidFunction *>(&fn)) != XR_SUCCESS || !fn)
        return false;
    XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = g_system_id;
    gi.vulkanInstance = g_vk_instance;
    XrResult r = fn(g_instance, &gi, &g_vk_phys_device);
    log_xr("xrGetVulkanGraphicsDevice2KHR", r);
    return r == XR_SUCCESS;
}

bool create_vulkan_device() {
    uint32_t qf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf, nullptr);
    VkQueueFamilyProperties props[16]{};
    if (qf > 16) qf = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf, props);
    for (uint32_t i = 0; i < qf; ++i)
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_vk_queue_family = i; break; }
    if (g_vk_queue_family == UINT32_MAX) return false;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_vk_queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures feats{};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &feats;

    PFN_xrCreateVulkanDeviceKHR fn = nullptr;
    if (xrGetInstanceProcAddr(g_instance, "xrCreateVulkanDeviceKHR",
                              reinterpret_cast<PFN_xrVoidFunction *>(&fn)) != XR_SUCCESS || !fn)
        return false;
    XrVulkanDeviceCreateInfoKHR xc{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xc.systemId = g_system_id;
    xc.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xc.vulkanPhysicalDevice = g_vk_phys_device;
    xc.vulkanCreateInfo = &dci;
    VkResult vr = VK_SUCCESS;
    XrResult r = fn(g_instance, &xc, &g_vk_device, &vr);
    log_xr("xrCreateVulkanDeviceKHR", r);
    if (r != XR_SUCCESS || vr != VK_SUCCESS) return false;
    vkGetDeviceQueue(g_vk_device, g_vk_queue_family, 0, &g_vk_queue);
    return true;
}

bool create_shared_image() {
    g_shared_w = (uint32_t)ANativeWindow_getWidth(g_app->window);
    g_shared_h = (uint32_t)ANativeWindow_getHeight(g_app->window);
    if (!g_shared_w || !g_shared_h) return false;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kSharedFormat;
    ici.extent = {g_shared_w, g_shared_h, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_vk_device, &ici, nullptr, &g_shared_image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_vk_device, g_shared_image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g_vk_device, &mai, nullptr, &g_shared_mem) != VK_SUCCESS)
        return false;
    vkBindImageMemory(g_vk_device, g_shared_image, g_shared_mem, 0);

    uint32_t ix = (uint32_t)(kCanvasInsetFrac * (float)g_shared_w);
    uint32_t iy = (uint32_t)(kCanvasInsetFrac * (float)g_shared_h);
    g_canvas_x = (int32_t)ix; g_canvas_y = (int32_t)iy;
    g_canvas_w = g_shared_w - 2 * ix; g_canvas_h = g_shared_h - 2 * iy;
    LOGI("Shared image %ux%u, canvas=(%d,%d %ux%u)", g_shared_w, g_shared_h, g_canvas_x, g_canvas_y, g_canvas_w, g_canvas_h);
    return true;
}

bool create_session() {
    XrGraphicsBindingVulkanKHR vb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vb.instance = g_vk_instance;
    vb.physicalDevice = g_vk_phys_device;
    vb.device = g_vk_device;
    vb.queueFamilyIndex = g_vk_queue_family;
    vb.queueIndex = 0;
    XrAndroidSurfaceBindingCreateInfoEXT ab{};
    ab.type = XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_EXT;
    ab.window = nullptr;  // app owns presentation
    ab.sharedImage = (uint64_t)(uintptr_t)g_shared_image;
    ab.sharedImageWidth = g_shared_w;
    ab.sharedImageHeight = g_shared_h;
    ab.sharedImageFormat = (uint32_t)kSharedFormat;
    vb.next = &ab;

    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &vb;
    ci.systemId = g_system_id;
    XrResult r = xrCreateSession(g_instance, &ci, &g_session);
    log_xr("xrCreateSession", r);
    if (r != XR_SUCCESS) return false;

    if (xrGetInstanceProcAddr(g_instance, "xrSetSharedTextureOutputRectEXT",
                              reinterpret_cast<PFN_xrVoidFunction *>(&g_set_output_rect)) == XR_SUCCESS && g_set_output_rect) {
        log_xr("xrSetSharedTextureOutputRectEXT", g_set_output_rect(g_session, g_canvas_x, g_canvas_y, g_canvas_w, g_canvas_h));
    } else {
        LOGW("xrSetSharedTextureOutputRectEXT unavailable — canvas = full image");
    }
    return true;
}

bool create_reference_space() {
    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace.orientation.w = 1.0f;
    XrResult r = xrCreateReferenceSpace(g_session, &ci, &g_app_space);
    log_xr("xrCreateReferenceSpace", r);
    return r == XR_SUCCESS;
}

bool create_swapchains() {
    uint32_t vc = 0;
    if (xrEnumerateViewConfigurationViews(g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          0, &vc, nullptr) != XR_SUCCESS || vc != kViewCount)
        return false;
    XrViewConfigurationView cfg[kViewCount]{};
    for (auto &c : cfg) c.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (xrEnumerateViewConfigurationViews(g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          kViewCount, &vc, cfg) != XR_SUCCESS)
        return false;

    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &fc, nullptr);
    int64_t fmts[64]{};
    if (fc > 64) fc = 64;
    xrEnumerateSwapchainFormats(g_session, fc, &fc, fmts);
    const int64_t pref[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    for (int64_t pf : pref) {
        for (uint32_t i = 0; i < fc && g_color_format == VK_FORMAT_UNDEFINED; ++i)
            if (fmts[i] == pf) g_color_format = (VkFormat)pf;
        if (g_color_format != VK_FORMAT_UNDEFINED) break;
    }
    if (g_color_format == VK_FORMAT_UNDEFINED) g_color_format = (VkFormat)fmts[0];

    for (uint32_t v = 0; v < kViewCount; ++v) {
        g_views[v].width = cfg[v].recommendedImageRectWidth;
        g_views[v].height = cfg[v].recommendedImageRectHeight;
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci.format = g_color_format;
        sci.sampleCount = 1;
        sci.width = g_views[v].width;
        sci.height = g_views[v].height;
        sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
        if (xrCreateSwapchain(g_session, &sci, &g_views[v].swapchain) != XR_SUCCESS) return false;
        uint32_t ic = 0;
        xrEnumerateSwapchainImages(g_views[v].swapchain, 0, &ic, nullptr);
        if (ic > 8) ic = 8;
        for (uint32_t i = 0; i < ic; ++i) g_views[v].images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        xrEnumerateSwapchainImages(g_views[v].swapchain, ic, &ic,
                                   reinterpret_cast<XrSwapchainImageBaseHeader *>(g_views[v].images));
        g_views[v].image_count = ic;
    }
    LOGI("OpenXR swapchains: %ux%u/view, fmt=0x%x", g_views[0].width, g_views[0].height, g_color_format);
    return true;
}

bool create_present_swapchain() {
    VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    sci.window = g_app->window;
    if (vkCreateAndroidSurfaceKHR(g_vk_instance, &sci, nullptr, &g_surface) != VK_SUCCESS) return false;
    VkBool32 sup = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_vk_phys_device, g_vk_queue_family, g_surface, &sup);
    if (!sup) return false;

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk_phys_device, g_surface, &caps);
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk_phys_device, g_surface, &nf, nullptr);
    VkSurfaceFormatKHR sf[32]{};
    if (nf > 32) nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk_phys_device, g_surface, &nf, sf);
    VkSurfaceFormatKHR chosen = sf[0];
    for (uint32_t i = 0; i < nf; ++i) if (sf[i].format == kSharedFormat) { chosen = sf[i]; break; }
    g_present_format = chosen.format;
    g_present_w = caps.currentExtent.width;
    g_present_h = caps.currentExtent.height;
    if (g_present_w == 0xFFFFFFFFu) { g_present_w = g_shared_w; g_present_h = g_shared_h; }
    uint32_t minImg = caps.minImageCount + 1;
    if (caps.maxImageCount && minImg > caps.maxImageCount) minImg = caps.maxImageCount;
    VkSurfaceTransformFlagBitsKHR xform =
        (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = minImg;
    ci.imageFormat = g_present_format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = {g_present_w, g_present_h};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = xform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(g_vk_device, &ci, nullptr, &g_present_swapchain) != VK_SUCCESS) return false;
    g_present_image_count = 0;
    vkGetSwapchainImagesKHR(g_vk_device, g_present_swapchain, &g_present_image_count, nullptr);
    if (g_present_image_count > 8) g_present_image_count = 8;
    vkGetSwapchainImagesKHR(g_vk_device, g_present_swapchain, &g_present_image_count, g_present_images);

    VkSemaphoreCreateInfo se{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(g_vk_device, &se, nullptr, &g_acquire_sem);
    vkCreateSemaphore(g_vk_device, &se, nullptr, &g_blit_sem);
    LOGI("Present swapchain %ux%u, %u images, fmt=0x%x", g_present_w, g_present_h, g_present_image_count, g_present_format);
    return true;
}

bool create_cmd_pool() {
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = g_vk_queue_family;
    return vkCreateCommandPool(g_vk_device, &ci, nullptr, &g_cmd_pool) == VK_SUCCESS;
}

VkRenderPass make_render_pass(VkFormat color, VkFormat depth, VkImageLayout finalLayout) {
    VkAttachmentDescription att[2]{};
    att[0].format = color;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = finalLayout;
    uint32_t count = 1;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    if (depth != VK_FORMAT_UNDEFINED) {
        att[1].format = depth;
        att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sub.pDepthStencilAttachment = &dref;
        count = 2;
    }
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = count;
    ci.pAttachments = att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    vkCreateRenderPass(g_vk_device, &ci, nullptr, &rp);
    return rp;
}

bool create_cube_pipeline() {
    g_cube_rp = make_render_pass(g_color_format, kDepthFormat, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(g_vk_device, &pli, nullptr, &g_cube_layout);

    VkShaderModule vs = make_module(shaders_cube_vert, sizeof(shaders_cube_vert));
    VkShaderModule fs = make_module(shaders_cube_frag, sizeof(shaders_cube_frag));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription vbind{0, sizeof(CubeVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vattr[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vbind;
    vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = vattr;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE; depth.depthWriteEnable = VK_TRUE; depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pDepthStencilState = &depth;
    gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
    gp.layout = g_cube_layout; gp.renderPass = g_cube_rp;
    VkResult r = vkCreateGraphicsPipelines(g_vk_device, VK_NULL_HANDLE, 1, &gp, nullptr, &g_cube_pipe);
    vkDestroyShaderModule(g_vk_device, vs, nullptr);
    vkDestroyShaderModule(g_vk_device, fs, nullptr);
    return r == VK_SUCCESS;
}

bool create_cube_vertex_buffer() {
    const float h = 0.12f;
    // 6 faces, per-face color, 2 tris each.
    CubeVertex verts[kCubeVertexCount];
    const float c[6][3] = {{0.9f,0.2f,0.2f},{0.2f,0.9f,0.2f},{0.2f,0.4f,0.95f},
                           {0.9f,0.9f,0.2f},{0.9f,0.4f,0.9f},{0.2f,0.9f,0.9f}};
    // cube corners
    const float P[8][3] = {{-h,-h,-h},{h,-h,-h},{h,h,-h},{-h,h,-h},
                           {-h,-h,h},{h,-h,h},{h,h,h},{-h,h,h}};
    const int F[6][4] = {{4,5,6,7},{1,0,3,2},{5,1,2,6},{0,4,7,3},{7,6,2,3},{0,1,5,4}};
    uint32_t n = 0;
    for (int f = 0; f < 6; ++f) {
        int a = F[f][0], b = F[f][1], cc = F[f][2], d = F[f][3];
        int tri[6] = {a, b, cc, a, cc, d};
        for (int t = 0; t < 6; ++t) {
            verts[n].pos[0] = P[tri[t]][0]; verts[n].pos[1] = P[tri[t]][1]; verts[n].pos[2] = P[tri[t]][2];
            verts[n].color[0] = c[f][0]; verts[n].color[1] = c[f][1]; verts[n].color[2] = c[f][2];
            ++n;
        }
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = sizeof(verts);
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(g_vk_device, &bi, nullptr, &g_cube_vbuf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_vk_device, g_cube_vbuf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g_vk_device, &mai, nullptr, &g_cube_vmem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g_vk_device, g_cube_vbuf, g_cube_vmem, 0);
    void *mapped = nullptr;
    vkMapMemory(g_vk_device, g_cube_vmem, 0, sizeof(verts), 0, &mapped);
    std::memcpy(mapped, verts, sizeof(verts));
    vkUnmapMemory(g_vk_device, g_cube_vmem);
    return true;
}

bool create_view_targets(uint32_t v) {
    // Depth.
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kDepthFormat;
    ici.extent = {g_views[v].width, g_views[v].height, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (vkCreateImage(g_vk_device, &ici, nullptr, &g_views[v].depth_image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_vk_device, g_views[v].depth_image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(g_vk_device, &mai, nullptr, &g_views[v].depth_mem);
    vkBindImageMemory(g_vk_device, g_views[v].depth_image, g_views[v].depth_mem, 0);
    VkImageViewCreateInfo dvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvi.image = g_views[v].depth_image; dvi.viewType = VK_IMAGE_VIEW_TYPE_2D; dvi.format = kDepthFormat;
    dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(g_vk_device, &dvi, nullptr, &g_views[v].depth_view);

    for (uint32_t i = 0; i < g_views[v].image_count; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = g_views[v].images[i].image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = g_color_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(g_vk_device, &vi, nullptr, &g_views[v].views[i]);
        VkImageView atts[2] = {g_views[v].views[i], g_views[v].depth_view};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g_cube_rp; fi.attachmentCount = 2; fi.pAttachments = atts;
        fi.width = g_views[v].width; fi.height = g_views[v].height; fi.layers = 1;
        vkCreateFramebuffer(g_vk_device, &fi, nullptr, &g_views[v].fbs[i]);
    }
    return true;
}

bool create_surround_pipeline() {
    g_surround_rp = make_render_pass(g_present_format, VK_FORMAT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SurroundPC)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(g_vk_device, &pli, nullptr, &g_surround_layout);

    VkShaderModule vs = make_module(shaders_surround_vert, sizeof(shaders_surround_vert));
    VkShaderModule fs = make_module(shaders_surround_frag, sizeof(shaders_surround_frag));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
    gp.layout = g_surround_layout; gp.renderPass = g_surround_rp;
    VkResult r = vkCreateGraphicsPipelines(g_vk_device, VK_NULL_HANDLE, 1, &gp, nullptr, &g_surround_pipe);
    vkDestroyShaderModule(g_vk_device, vs, nullptr);
    vkDestroyShaderModule(g_vk_device, fs, nullptr);
    if (r != VK_SUCCESS) return false;

    for (uint32_t i = 0; i < g_present_image_count; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = g_present_images[i]; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = g_present_format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(g_vk_device, &vci, nullptr, &g_present_views[i]);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g_surround_rp; fi.attachmentCount = 1; fi.pAttachments = &g_present_views[i];
        fi.width = g_present_w; fi.height = g_present_h; fi.layers = 1;
        vkCreateFramebuffer(g_vk_device, &fi, nullptr, &g_present_fbs[i]);
    }
    g_present_ready = true;
    return true;
}

// Render the cube for one eye into its OpenXR swapchain image.
void record_cube(uint32_t v, uint32_t img, const XrView &view) {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_cmd_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(g_vk_device, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clears[2];
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = g_cube_rp; rp.framebuffer = g_views[v].fbs[img];
    rp.renderArea = {{0, 0}, {g_views[v].width, g_views[v].height}};
    rp.clearValueCount = 2; rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vpr{0, 0, (float)g_views[v].width, (float)g_views[v].height, 0, 1};
    VkRect2D sc{{0, 0}, {g_views[v].width, g_views[v].height}};
    vkCmdSetViewport(cmd, 0, 1, &vpr);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_cube_pipe);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_cube_vbuf, &off);

    Mat4 proj = proj_from_fov(view.fov, 0.05f, 100.0f);
    Mat4 vmat = view_from_pose(view.pose);
    Mat4 model = cube_model((float)g_frame_count * 0.01f);
    Mat4 mvp = mat4_mul(proj, mat4_mul(vmat, model));
    vkCmdPushConstants(cmd, g_cube_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), mvp.m);
    vkCmdDraw(cmd, kCubeVertexCount, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(g_vk_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk_queue);
    vkFreeCommandBuffers(g_vk_device, g_cmd_pool, 1, &cmd);
}

// Draw the 2D surround + blit the woven canvas into the present swapchain.
void present_frame() {
    if (!g_present_ready || g_shared_image == VK_NULL_HANDLE) return;
    uint32_t idx = 0;
    VkResult ar = vkAcquireNextImageKHR(g_vk_device, g_present_swapchain, UINT64_MAX, g_acquire_sem, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) return;  // rotation/resize TODO
    if (ar != VK_SUCCESS) { LOGW("acquire %d", (int)ar); return; }

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_cmd_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(g_vk_device, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // 1) Surround render pass over the whole present image (writes black in the
    //    canvas hole; the blit fills that next).
    VkClearValue clr; clr.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = g_surround_rp; rp.framebuffer = g_present_fbs[idx];
    rp.renderArea = {{0, 0}, {g_present_w, g_present_h}};
    rp.clearValueCount = 1; rp.pClearValues = &clr;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vpr{0, 0, (float)g_present_w, (float)g_present_h, 0, 1};
    VkRect2D scr{{0, 0}, {g_present_w, g_present_h}};
    vkCmdSetViewport(cmd, 0, 1, &vpr);
    vkCmdSetScissor(cmd, 0, 1, &scr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_surround_pipe);
    SurroundPC spc{};
    spc.windowSize[0] = (float)g_present_w; spc.windowSize[1] = (float)g_present_h;
    spc.canvas[0] = g_canvas_x; spc.canvas[1] = g_canvas_y; spc.canvas[2] = (int32_t)g_canvas_w; spc.canvas[3] = (int32_t)g_canvas_h;
    spc.time = (float)g_frame_count / 60.0f;
    vkCmdPushConstants(cmd, g_surround_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(spc), &spc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);  // present image now COLOR_ATTACHMENT_OPTIMAL

    const VkImageSubresourceRange full{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // present: COLOR_ATTACHMENT -> TRANSFER_DST
    VkImageMemoryBarrier pb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    pb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; pb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; pb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pb.srcQueueFamilyIndex = pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pb.image = g_present_images[idx]; pb.subresourceRange = full;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &pb);
    // shared: COLOR_ATTACHMENT (DP finalLayout) -> TRANSFER_SRC
    VkImageMemoryBarrier sb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    sb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; sb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sb.srcQueueFamilyIndex = sb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sb.image = g_shared_image; sb.subresourceRange = full;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &sb);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {g_canvas_x, g_canvas_y, 0};
    blit.srcOffsets[1] = {g_canvas_x + (int32_t)g_canvas_w, g_canvas_y + (int32_t)g_canvas_h, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {g_canvas_x, g_canvas_y, 0};
    blit.dstOffsets[1] = {g_canvas_x + (int32_t)g_canvas_w, g_canvas_y + (int32_t)g_canvas_h, 1};
    vkCmdBlitImage(cmd, g_shared_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   g_present_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    // present -> PRESENT_SRC
    pb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; pb.dstAccessMask = 0;
    pb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; pb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &pb);
    // shared -> COLOR_ATTACHMENT (restore for next weave)
    sb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; sb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; sb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &sb);

    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_acquire_sem; si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &g_blit_sem;
    vkQueueSubmit(g_vk_queue, 1, &si, VK_NULL_HANDLE);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &g_blit_sem;
    pi.swapchainCount = 1; pi.pSwapchains = &g_present_swapchain; pi.pImageIndices = &idx;
    vkQueuePresentKHR(g_vk_queue, &pi);
    vkQueueWaitIdle(g_vk_queue);
    vkFreeCommandBuffers(g_vk_device, g_cmd_pool, 1, &cmd);
}

void handle_session_state(XrSessionState s) {
    g_session_state = s;
    if (s == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (xrBeginSession(g_session, &bi) == XR_SUCCESS) g_session_running = true;
    } else if (s == XR_SESSION_STATE_STOPPING) {
        xrEndSession(g_session); g_session_running = false;
    } else if (s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING) {
        g_exit_requested = true;
    }
}

void poll_events() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *e = reinterpret_cast<XrEventDataSessionStateChanged *>(&ev);
            handle_session_state(e->state);
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool render_frame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (xrWaitFrame(g_session, &wi, &fs) != XR_SUCCESS) return false;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (xrBeginFrame(g_session, &bi) != XR_SUCCESS) return false;

    XrCompositionLayerProjectionView pv[kViewCount]{};
    bool rendered = false;
    if (fs.shouldRender) {
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = g_app_space;
        XrView views[kViewCount]{};
        for (auto &v : views) v.type = XR_TYPE_VIEW;
        uint32_t got = 0;
        if (xrLocateViews(g_session, &li, &vs, kViewCount, &got, views) == XR_SUCCESS && got == kViewCount) {
            for (uint32_t v = 0; v < kViewCount; ++v) {
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                uint32_t img = 0;
                if (xrAcquireSwapchainImage(g_views[v].swapchain, &ai, &img) != XR_SUCCESS) break;
                XrSwapchainImageWaitInfo wii{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wii.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(g_views[v].swapchain, &wii);
                record_cube(v, img, views[v]);
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(g_views[v].swapchain, &ri);
                pv[v].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                pv[v].pose = views[v].pose;
                pv[v].fov = views[v].fov;
                pv[v].subImage.swapchain = g_views[v].swapchain;
                pv[v].subImage.imageRect.offset = {0, 0};
                pv[v].subImage.imageRect.extent = {(int32_t)g_views[v].width, (int32_t)g_views[v].height};
            }
            rendered = true;
        }
    }

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_app_space; layer.viewCount = kViewCount; layer.views = pv;
    const XrCompositionLayerBaseHeader *layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader *>(&layer)};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = rendered ? 1 : 0;
    ei.layers = rendered ? layers : nullptr;
    if (xrEndFrame(g_session, &ei) != XR_SUCCESS) return false;

    present_frame();  // surround + woven-canvas composite onto our swapchain
    if ((++g_frame_count % 60) == 0) LOGI("frame %llu", (unsigned long long)g_frame_count);
    return true;
}

void destroy_all() {
    if (g_vk_device != VK_NULL_HANDLE) vkDeviceWaitIdle(g_vk_device);
    for (uint32_t i = 0; i < g_present_image_count; ++i) {
        if (g_present_fbs[i]) vkDestroyFramebuffer(g_vk_device, g_present_fbs[i], nullptr);
        if (g_present_views[i]) vkDestroyImageView(g_vk_device, g_present_views[i], nullptr);
    }
    if (g_surround_pipe) vkDestroyPipeline(g_vk_device, g_surround_pipe, nullptr);
    if (g_surround_layout) vkDestroyPipelineLayout(g_vk_device, g_surround_layout, nullptr);
    if (g_surround_rp) vkDestroyRenderPass(g_vk_device, g_surround_rp, nullptr);
    if (g_blit_sem) vkDestroySemaphore(g_vk_device, g_blit_sem, nullptr);
    if (g_acquire_sem) vkDestroySemaphore(g_vk_device, g_acquire_sem, nullptr);
    if (g_present_swapchain) vkDestroySwapchainKHR(g_vk_device, g_present_swapchain, nullptr);
    if (g_surface) vkDestroySurfaceKHR(g_vk_instance, g_surface, nullptr);
    for (uint32_t v = 0; v < kViewCount; ++v) {
        for (uint32_t i = 0; i < g_views[v].image_count; ++i) {
            if (g_views[v].fbs[i]) vkDestroyFramebuffer(g_vk_device, g_views[v].fbs[i], nullptr);
            if (g_views[v].views[i]) vkDestroyImageView(g_vk_device, g_views[v].views[i], nullptr);
        }
        if (g_views[v].depth_view) vkDestroyImageView(g_vk_device, g_views[v].depth_view, nullptr);
        if (g_views[v].depth_image) vkDestroyImage(g_vk_device, g_views[v].depth_image, nullptr);
        if (g_views[v].depth_mem) vkFreeMemory(g_vk_device, g_views[v].depth_mem, nullptr);
        if (g_views[v].swapchain) xrDestroySwapchain(g_views[v].swapchain);
    }
    if (g_cube_pipe) vkDestroyPipeline(g_vk_device, g_cube_pipe, nullptr);
    if (g_cube_layout) vkDestroyPipelineLayout(g_vk_device, g_cube_layout, nullptr);
    if (g_cube_rp) vkDestroyRenderPass(g_vk_device, g_cube_rp, nullptr);
    if (g_cube_vbuf) vkDestroyBuffer(g_vk_device, g_cube_vbuf, nullptr);
    if (g_cube_vmem) vkFreeMemory(g_vk_device, g_cube_vmem, nullptr);
    if (g_cmd_pool) vkDestroyCommandPool(g_vk_device, g_cmd_pool, nullptr);
    if (g_app_space) xrDestroySpace(g_app_space);
    if (g_session) xrDestroySession(g_session);
    // shared image is ours (app-owned); destroy after the session/compositor is gone.
    if (g_shared_image) vkDestroyImage(g_vk_device, g_shared_image, nullptr);
    if (g_shared_mem) vkFreeMemory(g_vk_device, g_shared_mem, nullptr);
    if (g_vk_device) vkDestroyDevice(g_vk_device, nullptr);
    if (g_vk_instance) vkDestroyInstance(g_vk_instance, nullptr);
    if (g_instance) xrDestroyInstance(g_instance);
    g_instance = XR_NULL_HANDLE;
}

void handle_cmd(struct android_app *app, int32_t cmd) {
    if (cmd == APP_CMD_INIT_WINDOW && g_instance == XR_NULL_HANDLE) {
        LOGI("APP_CMD_INIT_WINDOW (window=%p)", (void *)app->window);
        bool ok = create_instance(app) && query_system_and_graphics_reqs() && create_vulkan_instance() &&
                  pick_physical_device() && create_vulkan_device() && create_shared_image() && create_session() &&
                  create_reference_space() && create_swapchains() && create_present_swapchain() && create_cmd_pool() &&
                  create_cube_pipeline() && create_cube_vertex_buffer() && create_view_targets(0) && create_view_targets(1) &&
                  create_surround_pipeline();
        LOGI(ok ? "Bring-up complete." : "Bring-up FAILED.");
    } else if (cmd == APP_CMD_DESTROY) {
        destroy_all();
    }
}

}  // namespace

extern "C" void android_main(struct android_app *app) {
    LOGI("cube_texture_vk_android: android_main entered");
    g_app = app;
    app->onAppCmd = handle_cmd;
    if (!initialize_loader(app)) LOGE("loader init failed");

    while (true) {
        int events;
        struct android_poll_source *src;
        const int timeout = g_session_running ? 0 : 250;
        while (ALooper_pollAll(timeout, nullptr, &events, (void **)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { destroy_all(); return; }
        }
        if (g_instance != XR_NULL_HANDLE) {
            poll_events();
            if (g_exit_requested) { destroy_all(); return; }
            if (g_session_running && (g_session_state == XR_SESSION_STATE_SYNCHRONIZED ||
                                      g_session_state == XR_SESSION_STATE_VISIBLE ||
                                      g_session_state == XR_SESSION_STATE_FOCUSED))
                render_frame();
        }
    }
}
