// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// mediaplayer_vk_android entry point. Android port of David's
// displayxr-demo-mediaplayer (stereo SBS image/video player): reuses
// cube_handle/model_viewer's OpenXR-Android harness (loader → instance →
// Vulkan device → stereo swapchains → session), but the renderer is a minimal
// SBS blit — each eye gets one UV half of the source via fullscreen.vert +
// sbs.frag. The runtime's Leia DP weaves the two views to the 3D panel.
// Stage 1: SBS image (stb, mode 0). Stage 2 will add AMediaCodec video.

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_display_info.h>  // display rendering-mode enumerate/request

#include <atomic>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jni.h>
#include <string>
#include <sys/system_properties.h>
#include <unistd.h>

#include "sbs_renderer.h"
#include "video_decoder.h"
#include "audio_player.h"
#include "transport_ui.h"
#include "stb_image.h"  // declarations only; impl is in stb_impl.cpp

#define LOG_TAG "mediaplayer_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef XRT_DEBUG_ANDROID_VERBOSE
#define DXR_HW_DBG(...)      __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP: " __VA_ARGS__)
#define DXR_HW_DBG_ONCE(...) do {                                                                  \
		static bool _logged = false;                                                                \
		if (!_logged) { __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP[once]: " __VA_ARGS__); _logged = true; } \
	} while (0)
#else
#define DXR_HW_DBG(...)      ((void)0)
#define DXR_HW_DBG_ONCE(...) ((void)0)
#endif

namespace {

// Format seconds as "M:SS" (or "H:MM:SS" past an hour) into buf (>=12 bytes).
void
fmt_time(double seconds, char *buf)
{
	if (seconds < 0.0 || seconds > 360000.0) {
		std::strcpy(buf, "0:00");
		return;
	}
	int total = (int)(seconds + 0.5);
	int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
	if (h > 0) {
		std::snprintf(buf, 12, "%d:%02d:%02d", h, m, s);
	} else {
		std::snprintf(buf, 12, "%d:%02d", m, s);
	}
}

const char *
xr_result_str(XrResult r)
{
	switch (r) {
	case XR_SUCCESS:                       return "XR_SUCCESS";
	case XR_ERROR_RUNTIME_FAILURE:         return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_RUNTIME_UNAVAILABLE:     return "XR_ERROR_RUNTIME_UNAVAILABLE";
	case XR_ERROR_INSTANCE_LOST:           return "XR_ERROR_INSTANCE_LOST";
	case XR_ERROR_INITIALIZATION_FAILED:   return "XR_ERROR_INITIALIZATION_FAILED";
	case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
	case XR_ERROR_EXTENSION_NOT_PRESENT:   return "XR_ERROR_EXTENSION_NOT_PRESENT";
	default:                               return nullptr;
	}
}

void
log_xr_result(const char *what, XrResult r)
{
	const char *name = xr_result_str(r);
	if (name != nullptr) {
		LOGI("%s -> %s", what, name);
	} else {
		LOGI("%s -> XrResult(%d)", what, (int)r);
	}
}

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system_id = XR_NULL_SYSTEM_ID;
XrVersion g_required_vk_version = XR_MAKE_VERSION(1, 1, 0);

VkInstance g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_phys_device = VK_NULL_HANDLE;
VkDevice g_vk_device = VK_NULL_HANDLE;
VkQueue g_vk_queue = VK_NULL_HANDLE;
uint32_t g_vk_queue_family = UINT32_MAX;

XrSession g_session = XR_NULL_HANDLE;
XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;
bool g_session_running = false;
bool g_exit_requested = false;
XrSpace g_app_space = XR_NULL_HANDLE;

// ── Display rendering-mode switching (XR_EXT_display_info) ─────────────────
// The runtime advertises a set of display rendering modes (e.g. 3D-stereo,
// 2D-mono); double-tap-with-two-fingers cycles them. Mode requests are async —
// the runtime applies them and (if view count changes) the next frame adapts.
PFN_xrEnumerateDisplayRenderingModesEXT g_pfnEnumModes = nullptr;
PFN_xrRequestDisplayRenderingModeEXT g_pfnReqMode = nullptr;
uint32_t g_rmode_count = 0;
uint32_t g_rmode_current = 0;
bool g_rmode_requestable = false;
std::atomic<bool> g_cycle_mode_request{false};

constexpr uint32_t kViewCount = 2;

struct PerView
{
	XrSwapchain swapchain{XR_NULL_HANDLE};
	uint32_t width{0};
	uint32_t height{0};
	XrSwapchainImageVulkanKHR images[8]{};
	uint32_t image_count{0};
};
PerView g_views[kViewCount];

VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;

// The 3DGS compute renderer (8 compute shaders). Renders sample.glb to
// each eye's swapchain image; the runtime's Leia DP weaves them.
SbsRenderer g_sbs;
bool g_sbs_ready = false;
std::atomic<bool> g_scene_loaded{false};

// Media sources. The default at launch is an SBS video in the app's external
// files dir (pushed there; too big to bundle); double-tap opens the SAF picker
// to choose another file, handed down as an fd via nativeOpenVideoFd. If no
// video opens, fall back to the bundled SBS test image (stb).
VideoDecoder g_video;
AudioPlayer g_audio;  // A/V master clock; video paces to it
bool g_is_video = false;
const char *const kDefaultVideo = "Aquaman_half_2x1.mp4";  // in externalDataPath
const char *const kFallbackImage = "test_LR_2x1.png";      // bundled asset

// A file the user picked via SAF: fd + byte range, published from the JNI
// thread and serviced (decoder reopen) on the android_main thread.
std::atomic<int> g_pick_fd{-1};
std::atomic<long long> g_pick_off{0};
std::atomic<long long> g_pick_len{0};

std::atomic<int> g_display_rotation{0};
std::atomic<bool> g_runtime_unavailable{false};

// Transport auto-hide: the bar stays up while paused and for a few seconds
// after the last touch; during steady playback it fades away. Every touch
// refreshes the timestamp (so any tap re-reveals the controls).
std::atomic<int64_t> g_ui_interaction_ns{0};
constexpr int64_t kOverlayHideAfterNs = 3000000000LL;  // 3 s

int64_t
now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}
uint64_t g_frame_count = 0;

// ─── matrix helpers removed — the SBS player does a flat per-eye blit and
// needs no view/projection math; pose+fov from xrLocateViews flow straight into
// the projection layer. (Reusable bring-up below is verbatim from the cube.) ──
#if 0
struct Mat4 { float m[16]; };

Mat4
view_matrix_from_pose(const XrPosef &pose)
{
	const float x = pose.orientation.x, y = pose.orientation.y;
	const float z = pose.orientation.z, w = pose.orientation.w;
	const float xx = x * x, yy = y * y, zz = z * z;
	const float xy = x * y, xz = x * z, yz = y * z;
	const float wx = w * x, wy = w * y, wz = w * z;

	const float r00 = 1.0f - 2.0f * (yy + zz);
	const float r01 = 2.0f * (xy + wz);
	const float r02 = 2.0f * (xz - wy);
	const float r10 = 2.0f * (xy - wz);
	const float r11 = 1.0f - 2.0f * (xx + zz);
	const float r12 = 2.0f * (yz + wx);
	const float r20 = 2.0f * (xz + wy);
	const float r21 = 2.0f * (yz - wx);
	const float r22 = 1.0f - 2.0f * (xx + yy);

	const float tx = -(r00 * pose.position.x + r01 * pose.position.y + r02 * pose.position.z);
	const float ty = -(r10 * pose.position.x + r11 * pose.position.y + r12 * pose.position.z);
	const float tz = -(r20 * pose.position.x + r21 * pose.position.y + r22 * pose.position.z);

	Mat4 v{};
	v.m[0]  = r00; v.m[1]  = r10; v.m[2]  = r20; v.m[3]  = 0.0f;
	v.m[4]  = r01; v.m[5]  = r11; v.m[6]  = r21; v.m[7]  = 0.0f;
	v.m[8]  = r02; v.m[9]  = r12; v.m[10] = r22; v.m[11] = 0.0f;
	v.m[12] = tx;  v.m[13] = ty;  v.m[14] = tz;  v.m[15] = 1.0f;
	return v;
}

// Asymmetric perspective from OpenXR FOV. NOTE: unlike the cube app, we do
// NOT flip Y here — ModelRenderer::updateUniforms negates the projection Y row
// itself (its world is Y-mirrored), so a Y-flip here would double-cancel.
Mat4
projection_matrix_from_fov(const XrFovf &fov, float aspect_w_over_h, float near_z, float far_z)
{
	const float tan_l = std::tan(fov.angleLeft);
	const float tan_r = std::tan(fov.angleRight);
	const float tan_d = std::tan(fov.angleDown);
	const float tan_u = std::tan(fov.angleUp);
	const float tan_w = tan_r - tan_l;
	const float tan_h = tan_u - tan_d;

	Mat4 p{};
	p.m[0]  = 2.0f / tan_w;
	p.m[5]  = 2.0f / tan_h;
	p.m[8]  = (tan_r + tan_l) / tan_w;
	p.m[9]  = (tan_u + tan_d) / tan_h;
	p.m[10] = -far_z / (far_z - near_z);
	p.m[11] = -1.0f;
	p.m[14] = -(far_z * near_z) / (far_z - near_z);

	if (aspect_w_over_h > 0.0f) {
		p.m[0] = (2.0f / tan_h) / aspect_w_over_h;
		p.m[8] = 0.0f;
	}
	return p;
}

Mat4
mat4_identity()
{
	Mat4 r{};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

// Column-major a*b.
Mat4
mat4_mul(const Mat4 &a, const Mat4 &b)
{
	Mat4 r{};
	for (int c = 0; c < 4; ++c) {
		for (int row = 0; row < 4; ++row) {
			float s = 0.0f;
			for (int k = 0; k < 4; ++k) {
				s += a.m[k * 4 + row] * b.m[c * 4 + k];
			}
			r.m[c * 4 + row] = s;
		}
	}
	return r;
}
#endif  // matrix helpers (unused by the SBS blit)

// ─── OpenXR-Android bring-up (reused verbatim from cube_handle_vk_android) ─
bool
initialize_loader(struct android_app *app)
{
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xrInitializeLoaderKHR));
	if (res != XR_SUCCESS || xrInitializeLoaderKHR == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrInitializeLoaderKHR) failed (%d)", (int)res);
		return false;
	}
	XrLoaderInitInfoAndroidKHR loader_init = {};
	loader_init.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
	loader_init.applicationVM = app->activity->vm;
	loader_init.applicationContext = app->activity->clazz;
	res = xrInitializeLoaderKHR(
	    reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader_init));
	log_xr_result("xrInitializeLoaderKHR", res);
	return res == XR_SUCCESS;
}

bool
create_instance(struct android_app *app)
{
	g_runtime_unavailable.store(false, std::memory_order_relaxed);
	const char *extensions[] = {
	    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
	    XR_EXT_DISPLAY_INFO_EXTENSION_NAME,  // display rendering-mode switching
	};
	XrInstanceCreateInfoAndroidKHR android_info = {};
	android_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	android_info.applicationVM = app->activity->vm;
	android_info.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo create_info = {};
	create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
	create_info.next = &android_info;
	std::strncpy(create_info.applicationInfo.applicationName,
	             "model_viewer_vk_android", XR_MAX_APPLICATION_NAME_SIZE - 1);
	create_info.applicationInfo.applicationVersion = 1;
	std::strncpy(create_info.applicationInfo.engineName, "displayxr",
	             XR_MAX_ENGINE_NAME_SIZE - 1);
	create_info.applicationInfo.engineVersion = 1;
	create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	create_info.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
	create_info.enabledExtensionNames = extensions;

	XrResult res = XR_ERROR_RUNTIME_UNAVAILABLE;
	for (int attempt = 0; attempt < 5; ++attempt) {
		res = xrCreateInstance(&create_info, &g_instance);
		if (res != XR_ERROR_RUNTIME_UNAVAILABLE) {
			break;
		}
		LOGW("xrCreateInstance: runtime unavailable (attempt %d/5); launch the "
		     "DisplayXR app once if this persists…", attempt + 1);
		usleep(400 * 1000);
	}
	log_xr_result("xrCreateInstance", res);
	if (res != XR_SUCCESS) {
		if (res == XR_ERROR_RUNTIME_UNAVAILABLE) {
			g_runtime_unavailable.store(true, std::memory_order_relaxed);
		}
		return false;
	}
	LOGI("ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS");
	return true;
}

bool
query_system_and_graphics_reqs()
{
	XrSystemGetInfo sys_info = {};
	sys_info.type = XR_TYPE_SYSTEM_GET_INFO;
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult res = xrGetSystem(g_instance, &sys_info, &g_system_id);
	log_xr_result("xrGetSystem(HMD)", res);
	if (res != XR_SUCCESS) {
		sys_info.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
		res = xrGetSystem(g_instance, &sys_info, &g_system_id);
		log_xr_result("xrGetSystem(HANDHELD)", res);
		if (res != XR_SUCCESS) {
			return false;
		}
	}

	PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = nullptr;
	res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsRequirements2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&get_reqs));
	if (res != XR_SUCCESS || get_reqs == nullptr) {
		LOGE("xrGetInstanceProcAddr(GraphicsRequirements2) failed (%d)", (int)res);
		return false;
	}
	XrGraphicsRequirementsVulkanKHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
	res = get_reqs(g_instance, g_system_id, &reqs);
	log_xr_result("xrGetVulkanGraphicsRequirements2KHR", res);
	if (res != XR_SUCCESS) {
		return false;
	}
	g_required_vk_version = reqs.minApiVersionSupported;
	return true;
}

bool
create_vulkan_instance()
{
	PFN_xrCreateVulkanInstanceKHR xr_create_vk_instance = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanInstanceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_instance));
	if (res != XR_SUCCESS || xr_create_vk_instance == nullptr) {
		LOGE("xrGetInstanceProcAddr(CreateVulkanInstance) failed (%d)", (int)res);
		return false;
	}
	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "model_viewer_vk_android";
	app_info.applicationVersion = 1;
	app_info.pEngineName = "displayxr";
	app_info.engineVersion = 1;
	app_info.apiVersion = VK_MAKE_VERSION(
	    XR_VERSION_MAJOR(g_required_vk_version),
	    XR_VERSION_MINOR(g_required_vk_version), 0);

	VkInstanceCreateInfo vk_ci = {};
	vk_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	vk_ci.pApplicationInfo = &app_info;

	XrVulkanInstanceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanCreateInfo = &vk_ci;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_instance(g_instance, &xr_ci, &g_vk_instance, &vk_result);
	log_xr_result("xrCreateVulkanInstanceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanInstanceKHR vk_result=%d", (int)vk_result);
		return false;
	}
	return true;
}

bool
pick_physical_device()
{
	PFN_xrGetVulkanGraphicsDevice2KHR xr_get_phys = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsDevice2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_get_phys));
	if (res != XR_SUCCESS || xr_get_phys == nullptr) {
		LOGE("xrGetInstanceProcAddr(GraphicsDevice2) failed (%d)", (int)res);
		return false;
	}
	XrVulkanGraphicsDeviceGetInfoKHR info = {};
	info.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	info.systemId = g_system_id;
	info.vulkanInstance = g_vk_instance;
	res = xr_get_phys(g_instance, &info, &g_vk_phys_device);
	log_xr_result("xrGetVulkanGraphicsDevice2KHR", res);
	return res == XR_SUCCESS;
}

bool
create_vulkan_device()
{
	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, nullptr);
	if (qf_count == 0) {
		LOGE("No Vulkan queue families");
		return false;
	}
	VkQueueFamilyProperties qf_props[16] = {};
	const uint32_t qf_cap = sizeof(qf_props) / sizeof(qf_props[0]);
	if (qf_count > qf_cap) {
		qf_count = qf_cap;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, qf_props);

	g_vk_queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < qf_count; ++i) {
		if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g_vk_queue_family = i;
			break;
		}
	}
	if (g_vk_queue_family == UINT32_MAX) {
		LOGE("No graphics-capable queue family");
		return false;
	}

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = g_vk_queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;

	// The 3DGS radix-sort compute shaders (sort/hist/preprocess_sort) use
	// uint64_t keys via GL_EXT_shader_explicit_arithmetic_types_int64, so the
	// VkDevice MUST enable shaderInt64 or those pipelines fail to LINK on
	// Adreno (-> null pipeline -> renderEye SIGSEGV). The runtime's
	// xrCreateVulkanDeviceKHR copies our pEnabledFeatures through verbatim
	// (oxr_vk_create_vulkan_device), so requesting it here is enough. Query
	// the BASE 1.0 features (not vkGetPhysicalDeviceFeatures2 — that returns
	// zeros in this 1.0-instance context and falsely reported shaderInt64
	// absent); the base query always carries shaderInt64.
	VkPhysicalDeviceFeatures supported = {};
	vkGetPhysicalDeviceFeatures(g_vk_phys_device, &supported);
	LOGI("device shaderInt64=%d", (int)supported.shaderInt64);

	VkPhysicalDeviceFeatures features = {};
	features.shaderInt64 = supported.shaderInt64;

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.pEnabledFeatures = &features;

	PFN_xrCreateVulkanDeviceKHR xr_create_vk_device = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanDeviceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_device));
	if (res != XR_SUCCESS || xr_create_vk_device == nullptr) {
		LOGE("xrGetInstanceProcAddr(CreateVulkanDevice) failed (%d)", (int)res);
		return false;
	}
	XrVulkanDeviceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanPhysicalDevice = g_vk_phys_device;
	xr_ci.vulkanCreateInfo = &dci;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_device(g_instance, &xr_ci, &g_vk_device, &vk_result);
	log_xr_result("xrCreateVulkanDeviceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanDeviceKHR vk_result=%d", (int)vk_result);
		return false;
	}
	vkGetDeviceQueue(g_vk_device, g_vk_queue_family, 0, &g_vk_queue);
	LOGI("Vulkan device ready: queue_family=%u", g_vk_queue_family);
	return true;
}

bool
create_session()
{
	XrGraphicsBindingVulkanKHR binding = {};
	binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
	binding.instance = g_vk_instance;
	binding.physicalDevice = g_vk_phys_device;
	binding.device = g_vk_device;
	binding.queueFamilyIndex = g_vk_queue_family;
	binding.queueIndex = 0;

	XrSessionCreateInfo ci = {};
	ci.type = XR_TYPE_SESSION_CREATE_INFO;
	ci.next = &binding;
	ci.systemId = g_system_id;
	XrResult res = xrCreateSession(g_instance, &ci, &g_session);
	log_xr_result("xrCreateSession", res);
	return res == XR_SUCCESS;
}

// Resolve the rendering-mode entry points and enumerate what the runtime/DP
// advertises. Best-effort: logs each mode so we can see (on this device) whether
// modes differ in view count/tiling (→ render-loop work) or are a pure DP-side
// 2D/3D toggle (→ app just requests them). Never fails bring-up.
bool
enumerate_rendering_modes()
{
	xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRenderingModesEXT",
	                      (PFN_xrVoidFunction *)&g_pfnEnumModes);
	xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRenderingModeEXT",
	                      (PFN_xrVoidFunction *)&g_pfnReqMode);
	if (g_pfnEnumModes == nullptr || g_pfnReqMode == nullptr) {
		LOGI("Display rendering-mode ext entry points not resolved — mode switch disabled");
		return true;
	}
	uint32_t count = 0;
	XrResult res = g_pfnEnumModes(g_session, 0, &count, nullptr);
	if (res != XR_SUCCESS || count == 0) {
		LOGI("xrEnumerateDisplayRenderingModesEXT: count=%u res=%d", count, (int)res);
		return true;
	}
	std::vector<XrDisplayRenderingModeInfoEXT> modes(count);
	for (auto &m : modes) {
		m.type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
		m.next = nullptr;
	}
	res = g_pfnEnumModes(g_session, count, &count, modes.data());
	if (res != XR_SUCCESS) {
		LOGI("xrEnumerateDisplayRenderingModesEXT (fill) res=%d", (int)res);
		return true;
	}
	g_rmode_count = count;
	for (uint32_t i = 0; i < count; ++i) {
		const auto &m = modes[i];
		if (m.isActive) {
			g_rmode_current = m.modeIndex;
			g_rmode_requestable = m.isRequestable;
		}
		LOGI("RMODE[%u] idx=%u \"%s\" views=%u scale=(%.3f,%.3f) hw3D=%d tiles=%ux%u "
		     "%ux%u active=%d requestable=%d",
		     i, m.modeIndex, m.modeName, m.viewCount, m.viewScaleX, m.viewScaleY,
		     (int)m.hardwareDisplay3D, m.tileColumns, m.tileRows, m.viewWidthPixels,
		     m.viewHeightPixels, (int)m.isActive, (int)m.isRequestable);
	}
	LOGI("Display rendering modes: %u total, current=%u, requestable=%d",
	     g_rmode_count, g_rmode_current, (int)g_rmode_requestable);
	return true;
}

bool
create_swapchains()
{
	uint32_t expected_view_count = 0;
	XrResult res = xrEnumerateViewConfigurationViews(
	    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	    0, &expected_view_count, nullptr);
	if (res != XR_SUCCESS || expected_view_count != kViewCount) {
		LOGE("Expected %u views, runtime reports %u", kViewCount, expected_view_count);
		return false;
	}
	XrViewConfigurationView view_configs[kViewCount] = {};
	for (uint32_t i = 0; i < kViewCount; ++i) {
		view_configs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	}
	res = xrEnumerateViewConfigurationViews(
	    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	    kViewCount, &expected_view_count, view_configs);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateViewConfigurationViews", res);
		return false;
	}

	uint32_t format_count = 0;
	res = xrEnumerateSwapchainFormats(g_session, 0, &format_count, nullptr);
	if (res != XR_SUCCESS || format_count == 0) {
		log_xr_result("xrEnumerateSwapchainFormats(count)", res);
		return false;
	}
	int64_t formats[64] = {};
	if (format_count > 64) {
		format_count = 64;
	}
	res = xrEnumerateSwapchainFormats(g_session, format_count, &format_count, formats);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainFormats(fill)", res);
		return false;
	}
	const int64_t preferred[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
	for (int64_t pref : preferred) {
		for (uint32_t i = 0; i < format_count && g_swapchain_format == VK_FORMAT_UNDEFINED; ++i) {
			if (formats[i] == pref) {
				g_swapchain_format = (VkFormat)pref;
			}
		}
		if (g_swapchain_format != VK_FORMAT_UNDEFINED) {
			break;
		}
	}
	if (g_swapchain_format == VK_FORMAT_UNDEFINED) {
		g_swapchain_format = (VkFormat)formats[0];
	}
	LOGI("Chose swapchain format: 0x%x", (uint32_t)g_swapchain_format);

	for (uint32_t i = 0; i < kViewCount; ++i) {
		g_views[i].width = view_configs[i].recommendedImageRectWidth;
		g_views[i].height = view_configs[i].recommendedImageRectHeight;

		XrSwapchainCreateInfo ci = {};
		ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
		ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
		                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		ci.format = g_swapchain_format;
		ci.sampleCount = view_configs[i].recommendedSwapchainSampleCount;
		ci.width = g_views[i].width;
		ci.height = g_views[i].height;
		ci.faceCount = 1;
		ci.arraySize = 1;
		ci.mipCount = 1;
		res = xrCreateSwapchain(g_session, &ci, &g_views[i].swapchain);
		if (res != XR_SUCCESS) {
			log_xr_result("xrCreateSwapchain", res);
			return false;
		}
		uint32_t img_count = 0;
		res = xrEnumerateSwapchainImages(g_views[i].swapchain, 0, &img_count, nullptr);
		if (res != XR_SUCCESS) {
			log_xr_result("xrEnumerateSwapchainImages(count)", res);
			return false;
		}
		if (img_count > 8) {
			img_count = 8;
		}
		for (uint32_t j = 0; j < img_count; ++j) {
			g_views[i].images[j].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
		}
		res = xrEnumerateSwapchainImages(
		    g_views[i].swapchain, img_count, &img_count,
		    reinterpret_cast<XrSwapchainImageBaseHeader *>(g_views[i].images));
		if (res != XR_SUCCESS) {
			log_xr_result("xrEnumerateSwapchainImages(fill)", res);
			return false;
		}
		g_views[i].image_count = img_count;
		LOGI("View %u swapchain: %ux%u, %u images", i, g_views[i].width,
		     g_views[i].height, img_count);
	}
	return true;
}

bool
create_reference_space()
{
	XrReferenceSpaceCreateInfo ci = {};
	ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	ci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
	ci.poseInReferenceSpace.position = {0, 0, 0};
	XrResult res = xrCreateReferenceSpace(g_session, &ci, &g_app_space);
	log_xr_result("xrCreateReferenceSpace(LOCAL)", res);
	return res == XR_SUCCESS;
}

// Initialize the SBS blit renderer against the runtime's Vulkan resources.
bool
gs_init()
{
	if (!g_sbs.init(g_vk_phys_device, g_vk_device, g_vk_queue, g_vk_queue_family,
	                g_swapchain_format)) {
		LOGE("SbsRenderer::init failed");
		return false;
	}
	g_sbs_ready = true;
	LOGI("SbsRenderer ready (%ux%u/eye)", g_views[0].width, g_views[0].height);
	return true;
}

// Decode the bundled SBS image (APK asset) to RGBA8 via stb and upload it.
bool
load_fallback_image(struct android_app *app)
{
	AAssetManager *mgr = app->activity->assetManager;
	AAsset *asset = AAssetManager_open(mgr, kFallbackImage, AASSET_MODE_BUFFER);
	if (asset == nullptr) {
		LOGE("%s not found in assets", kFallbackImage);
		return false;
	}
	const uint8_t *buf = (const uint8_t *)AAsset_getBuffer(asset);
	const off_t len = AAsset_getLength(asset);
	int w = 0, h = 0, comp = 0;
	stbi_uc *pixels = nullptr;
	if (buf != nullptr && len > 0) {
		pixels = stbi_load_from_memory(buf, (int)len, &w, &h, &comp, 4);  // force RGBA
	}
	AAsset_close(asset);
	if (pixels == nullptr) {
		LOGE("stbi_load_from_memory failed for %s: %s", kFallbackImage, stbi_failure_reason());
		return false;
	}
	const bool ok = g_sbs.uploadTexture(pixels, (uint32_t)w, (uint32_t)h);
	stbi_image_free(pixels);
	if (ok) {
		LOGI("Loaded SBS image: %s (%dx%d)", kFallbackImage, w, h);
		g_scene_loaded.store(true, std::memory_order_relaxed);
	}
	return ok;
}

// Default media: try the SBS video in the app's external files dir; if it can't
// be opened, show the bundled SBS test image instead. Video frames stream in on
// the decoder thread and get uploaded per-frame in render_frame.
bool
load_media(struct android_app *app)
{
	if (!g_sbs_ready) {
		return false;
	}
	std::string videoPath =
	    std::string(app->activity->externalDataPath ? app->activity->externalDataPath : ".") +
	    "/" + kDefaultVideo;
	if (g_video.openPath(videoPath)) {
		g_is_video = true;
		g_ui_interaction_ns.store(now_ns(), std::memory_order_relaxed);  // show controls at start
		// Audio is the A/V master: open its own extractor over the same file and
		// have the video decoder pace frames to the audio clock. Silent + wall-
		// clock paced if the file has no audio track.
		g_audio.openPath(videoPath);
		g_video.setMasterClock(&AudioPlayer::clockThunk, &g_audio);
		LOGI("Playing SBS video: %s (audio=%s)", videoPath.c_str(),
		     g_audio.hasAudio() ? "yes" : "none");
		return true;  // g_scene_loaded flips true on the first decoded frame
	}
	LOGW("No video at %s — falling back to bundled image", videoPath.c_str());
	return load_fallback_image(app);
}

void
handle_session_state(XrSessionState new_state)
{
	g_session_state = new_state;
	switch (new_state) {
	case XR_SESSION_STATE_READY: {
		XrSessionBeginInfo begin = {};
		begin.type = XR_TYPE_SESSION_BEGIN_INFO;
		begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		XrResult res = xrBeginSession(g_session, &begin);
		log_xr_result("xrBeginSession", res);
		if (res == XR_SUCCESS) {
			g_session_running = true;
		}
		break;
	}
	case XR_SESSION_STATE_STOPPING: {
		XrResult res = xrEndSession(g_session);
		log_xr_result("xrEndSession", res);
		g_session_running = false;
		break;
	}
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		g_exit_requested = true;
		break;
	default:
		break;
	}
}

void
poll_xr_events()
{
	for (;;) {
		XrEventDataBuffer ev = {};
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		XrResult res = xrPollEvent(g_instance, &ev);
		if (res == XR_EVENT_UNAVAILABLE) {
			break;
		}
		if (res != XR_SUCCESS) {
			log_xr_result("xrPollEvent", res);
			break;
		}
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const auto *e = reinterpret_cast<const XrEventDataSessionStateChanged *>(&ev);
			if (e->session == g_session) {
				LOGI("session state -> %d", (int)e->state);
				handle_session_state(e->state);
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			g_exit_requested = true;
		}
	}
}

bool
render_frame()
{
	XrFrameWaitInfo wait_info = {};
	wait_info.type = XR_TYPE_FRAME_WAIT_INFO;
	XrFrameState frame_state = {};
	frame_state.type = XR_TYPE_FRAME_STATE;
	XrResult res = xrWaitFrame(g_session, &wait_info, &frame_state);
	if (res != XR_SUCCESS) {
		log_xr_result("xrWaitFrame", res);
		return false;
	}
	XrFrameBeginInfo begin_info = {};
	begin_info.type = XR_TYPE_FRAME_BEGIN_INFO;
	res = xrBeginFrame(g_session, &begin_info);
	if (res != XR_SUCCESS) {
		log_xr_result("xrBeginFrame", res);
		return false;
	}

	// Pull the latest decoded video frame (if any) and upload its YUV planes —
	// the GPU does the BT.709 convert + per-eye downscale in sbs.frag. First
	// frame flips g_scene_loaded so the render block below engages.
	if (g_is_video) {
		if (const VideoDecoder::Frame *vf = g_video.acquireLatest()) {
			g_sbs.uploadYUV(vf->y.data(), vf->uv.data(),
			                vf->nv12 ? nullptr : vf->v.data(), (uint32_t)vf->width,
			                (uint32_t)vf->height, vf->nv12, vf->fullRange);
			g_scene_loaded.store(true, std::memory_order_relaxed);
		}
		// Transport overlay (scrub bar + play/pause + load + time).
		const double pos = g_video.positionSeconds();
		const double dur = g_video.durationSeconds();
		char left[12], right[12];
		fmt_time(pos, left);
		fmt_time(dur, right);
		// Auto-hide while playing: show if paused or recently touched.
		const bool show =
		    g_video.paused() ||
		    (now_ns() - g_ui_interaction_ns.load(std::memory_order_relaxed)) < kOverlayHideAfterNs;
		g_sbs.setOverlay(show, dur > 0.0 ? (float)(pos / dur) : 0.0f, g_video.paused(), left,
		                 right);
	} else {
		g_sbs.setOverlay(false, 0.0f, false, "", "");  // image: no transport
	}

	XrCompositionLayerProjectionView projection_views[kViewCount] = {};
	bool rendered = false;
	if (frame_state.shouldRender && g_scene_loaded.load(std::memory_order_relaxed)) {
		XrViewState view_state = {};
		view_state.type = XR_TYPE_VIEW_STATE;
		XrViewLocateInfo locate_info = {};
		locate_info.type = XR_TYPE_VIEW_LOCATE_INFO;
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = frame_state.predictedDisplayTime;
		locate_info.space = g_app_space;

		XrView views[kViewCount] = {};
		for (uint32_t i = 0; i < kViewCount; ++i) {
			views[i].type = XR_TYPE_VIEW;
		}
		uint32_t located = 0;
		res = xrLocateViews(g_session, &locate_info, &view_state, kViewCount, &located, views);
		if (res == XR_SUCCESS && located == kViewCount) {
			DXR_HW_DBG_ONCE("first xrLocateViews success");
			for (uint32_t i = 0; i < kViewCount; ++i) {
				XrSwapchainImageAcquireInfo acq = {};
				acq.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
				uint32_t img_idx = 0;
				res = xrAcquireSwapchainImage(g_views[i].swapchain, &acq, &img_idx);
				if (res != XR_SUCCESS) {
					log_xr_result("xrAcquireSwapchainImage", res);
					break;
				}
				XrSwapchainImageWaitInfo wait_img = {};
				wait_img.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
				wait_img.timeout = XR_INFINITE_DURATION;
				res = xrWaitSwapchainImage(g_views[i].swapchain, &wait_img);
				if (res != XR_SUCCESS) {
					log_xr_result("xrWaitSwapchainImage", res);
					break;
				}

				// SBS split: left view samples the source's left half, right
				// view the right half. The DP weaves the two views.
				const float offX = (i == 0) ? 0.0f : 0.5f;
				g_sbs.drawEye(g_views[i].images[img_idx].image, g_views[i].width,
				              g_views[i].height, offX, 0.0f, 0.5f, 1.0f);

				XrSwapchainImageReleaseInfo rel = {};
				rel.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
				res = xrReleaseSwapchainImage(g_views[i].swapchain, &rel);
				if (res != XR_SUCCESS) {
					log_xr_result("xrReleaseSwapchainImage", res);
					break;
				}

				projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				projection_views[i].pose = views[i].pose;
				projection_views[i].fov = views[i].fov;
				projection_views[i].subImage.swapchain = g_views[i].swapchain;
				projection_views[i].subImage.imageRect.offset = {0, 0};
				projection_views[i].subImage.imageRect.extent = {
				    (int32_t)g_views[i].width, (int32_t)g_views[i].height};
				projection_views[i].subImage.imageArrayIndex = 0;
			}
			rendered = (res == XR_SUCCESS);
		} else {
			log_xr_result("xrLocateViews", res);
		}
	}

	XrCompositionLayerProjection projection_layer = {};
	projection_layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	projection_layer.space = g_app_space;
	projection_layer.viewCount = kViewCount;
	projection_layer.views = projection_views;
	const XrCompositionLayerBaseHeader *layers[1] = {
	    reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection_layer)};

	XrFrameEndInfo end_info = {};
	end_info.type = XR_TYPE_FRAME_END_INFO;
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = rendered ? 1 : 0;
	end_info.layers = rendered ? layers : nullptr;
	res = xrEndFrame(g_session, &end_info);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEndFrame", res);
		return false;
	}

	g_frame_count++;
	if ((g_frame_count % 120) == 0) {
		static auto last = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(now - last).count() / 120.0;
		last = now;
		LOGI("frame %llu  ~%.1f ms/frame (%.1f fps)", (unsigned long long)g_frame_count,
		     ms, ms > 0.0 ? 1000.0 / ms : 0.0);
	}
	return true;
}

void
destroy_all()
{
	g_audio.stop();
	g_video.stop();
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(g_vk_device);
	}
	if (g_sbs_ready) {
		g_sbs.cleanup();
		g_sbs_ready = false;
	}
	if (g_session != XR_NULL_HANDLE) {
		xrDestroySession(g_session);
		g_session = XR_NULL_HANDLE;
	}
	for (uint32_t i = 0; i < kViewCount; ++i) {
		if (g_views[i].swapchain != XR_NULL_HANDLE) {
			xrDestroySwapchain(g_views[i].swapchain);
			g_views[i].swapchain = XR_NULL_HANDLE;
		}
	}
	if (g_app_space != XR_NULL_HANDLE) {
		xrDestroySpace(g_app_space);
		g_app_space = XR_NULL_HANDLE;
	}
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDestroyDevice(g_vk_device, nullptr);
		g_vk_device = VK_NULL_HANDLE;
	}
	if (g_vk_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(g_vk_instance, nullptr);
		g_vk_instance = VK_NULL_HANDLE;
	}
	if (g_instance != XR_NULL_HANDLE) {
		xrDestroyInstance(g_instance);
		g_instance = XR_NULL_HANDLE;
	}
}

void
handle_cmd(struct android_app *app, int32_t cmd)
{
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		LOGI("APP_CMD_INIT_WINDOW (window=%p)", app->window);
		if (g_instance == XR_NULL_HANDLE) {
			bool ok =
			    create_instance(app) &&
			    query_system_and_graphics_reqs() &&
			    create_vulkan_instance() &&
			    pick_physical_device() &&
			    create_vulkan_device() &&
			    create_session() &&
			    create_swapchains() &&
			    create_reference_space() &&
			    enumerate_rendering_modes() &&
			    gs_init() &&
			    load_media(app);
			LOGI(ok ? "Bring-up complete." : "Bring-up failed; see logs.");
		}
		break;
	case APP_CMD_DESTROY:
		LOGI("APP_CMD_DESTROY");
		destroy_all();
		break;
	default:
		break;
	}
}

} // namespace

// ─── JNI bridge to MainActivity ──────────────────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MainActivity_nativeSetRotation(
    JNIEnv * /*env*/, jobject /*thiz*/, jint rotation)
{
	g_display_rotation.store(rotation & 3, std::memory_order_relaxed);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MainActivity_nativeRuntimeUnavailable(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	return g_runtime_unavailable.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MainActivity_nativeXrReady(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	return (g_instance != XR_NULL_HANDLE) ? JNI_TRUE : JNI_FALSE;
}

// The user picked a file via SAF (double-tap → ACTION_OPEN_DOCUMENT). Java
// passes an open, dup'd file descriptor + its byte range; we publish it and the
// android_main loop reopens the decoder on its own thread (AMediaExtractor reads
// the fd, so it must outlive this call — Java keeps the ParcelFileDescriptor).
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MainActivity_nativeOpenVideoFd(
    JNIEnv * /*env*/, jobject /*thiz*/, jint fd, jlong offset, jlong length)
{
	g_pick_off.store((long long)offset, std::memory_order_relaxed);
	g_pick_len.store((long long)length, std::memory_order_relaxed);
	g_pick_fd.store((int)fd, std::memory_order_release);  // publish last
}

// Raw touch (normalized screen coords) hit-tested against the transport bar
// (transport_ui.h). Returns 1 to ask Java to open the SAF picker (Load button
// tapped — only Java can launch ACTION_OPEN_DOCUMENT); 0 otherwise. The button
// toggles pause, the bar seeks (tap or drag = absolute), and a tap on the video
// also toggles pause. Decoder methods are thread-safe.
extern "C" JNIEXPORT jint JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MainActivity_nativeTouch(
    JNIEnv * /*env*/, jobject /*thiz*/, jint action, jfloat nx, jfloat ny)
{
	static tui::Region downRegion = tui::Region::None;
	static bool moved = false;
	static float downX = 0.0f, downY = 0.0f;
	constexpr int kDown = 0, kUp = 1, kMove = 2;
	if (!g_is_video) return 0;
	g_ui_interaction_ns.store(now_ns(), std::memory_order_relaxed);  // re-reveal + keep controls up

	auto seekBar = [&](float x) {
		const double frac = tui::barFraction(x);
		const double t = frac * g_video.durationSeconds();
		g_video.seekTo(t);
		g_audio.seekTo(t);
		LOGI("transport: seek -> %.1fs (%.0f%%)", t, frac * 100.0);
	};
	auto togglePause = [&]() {
		g_video.togglePaused();
		g_audio.setPaused(g_video.paused());
		LOGI("transport: %s @ %.1fs", g_video.paused() ? "PAUSE" : "PLAY",
		     g_video.positionSeconds());
	};

	if (action == kDown) {
		downX = nx;
		downY = ny;
		moved = false;
		downRegion = tui::hit(nx, ny);
		const char *rn = downRegion == tui::Region::Button  ? "Button"
		                 : downRegion == tui::Region::Bar    ? "Bar"
		                 : downRegion == tui::Region::Load   ? "Load"
		                                                     : "None";
		LOGI("touch DOWN (%.3f,%.3f) -> %s", (double)nx, (double)ny, rn);
		if (downRegion == tui::Region::Bar) seekBar(nx);
	} else if (action == kMove) {
		if (std::fabs(nx - downX) + std::fabs(ny - downY) > 0.01f) moved = true;
		if (downRegion == tui::Region::Bar) seekBar(nx);
	} else if (action == kUp) {
		const tui::Region r = downRegion;
		downRegion = tui::Region::None;
		if (r == tui::Region::Button) {
			togglePause();
		} else if (r == tui::Region::Load) {
			LOGI("touch UP -> Load (open picker)");
			return 1;  // Java opens the picker
		} else if (r == tui::Region::None && !moved) {
			togglePause();  // tap on the video area
		}
	}
	return 0;
}

extern "C" void
android_main(struct android_app *app)
{
	LOGI("mediaplayer_vk_android: android_main entered");
	app->onAppCmd = handle_cmd;
	// No touch input in the image MVP. (Stage 3 will add playback gestures via
	// MainActivity.dispatchTouchEvent → JNI, since a NativeActivity behind the
	// runtime's MonadoView overlay never receives native input.)

	if (!initialize_loader(app)) {
		LOGE("OpenXR loader init failed");
	}

	while (true) {
		const int poll_timeout_ms = g_session_running ? 0 : 250;
		int events;
		struct android_poll_source *source;
		while (ALooper_pollAll(poll_timeout_ms, nullptr, &events, (void **)&source) >= 0) {
			if (source != nullptr) {
				source->process(app, source);
			}
			if (app->destroyRequested != 0) {
				destroy_all();
				return;
			}
		}
		if (g_instance != XR_NULL_HANDLE) {
			poll_xr_events();
			if (g_exit_requested) {
				destroy_all();
				return;
			}
			// Service a file the user picked (double-tap → SAF). Reopen the
			// decoder on this thread; the old decode thread is joined in stop().
			const int pick = g_pick_fd.exchange(-1, std::memory_order_acquire);
			if (pick >= 0) {
				g_audio.stop();  // SAF gives one fd (video only) — no audio for picks
				g_video.stop();
				g_scene_loaded.store(false, std::memory_order_relaxed);
				if (g_video.openFd(pick, g_pick_off.load(std::memory_order_relaxed),
				                   g_pick_len.load(std::memory_order_relaxed))) {
					g_is_video = true;
					g_ui_interaction_ns.store(now_ns(), std::memory_order_relaxed);
					LOGI("Opened picked video (fd=%d)", pick);
				} else {
					LOGE("Failed to open picked video (fd=%d)", pick);
				}
			}
			// Drive frames from READY (not SYNCHRONIZED+): a CTS-compliant
			// runtime only advances READY->SYNCHRONIZED on the first
			// xrBeginFrame, so gating on SYNCHRONIZED+ deadlocks at READY ->
			// black (David's #507). render_frame honors shouldRender.
			if (app->window != nullptr && g_session_running) {
				render_frame();
			}
		}
	}
}
