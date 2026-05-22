// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// cube_handle_vk_android entry point. Wires the Khronos OpenXR loader
// (`xrInitializeLoaderKHR` + `xrCreateInstance`) and logs the runtime
// the loader binds to. No Vulkan / session / cube renderer yet — that
// lands in follow-up commits. The point of this step is to prove the
// loader can find our runtime APK via the
// org.khronos.openxr.OpenXRRuntimeService intent.

#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
// openxr_platform.h references VkInstance / VkDevice / VkFormat under
// XR_USE_GRAPHICS_API_VULKAN but does NOT include <vulkan/vulkan.h>
// itself — the consumer must do that first.
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstring>

#define LOG_TAG "cube_handle_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Hardware-bring-up verbose debug. Gated on XRT_DEBUG_ANDROID_VERBOSE
// passed from build.gradle's debug variant. Compiled out in release.
// Tag "HW_DBG_APP:" greppable separation from runtime-side HW_DBG_CNSDK
// / HW_DBG_DP logs.
#ifdef XRT_DEBUG_ANDROID_VERBOSE
#define DXR_HW_DBG(...)       __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP: " __VA_ARGS__)
#define DXR_HW_DBG_ONCE(...)  do {                                                                 \
		static bool _logged = false;                                                                \
		if (!_logged) {                                                                             \
			__android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP[once]: " __VA_ARGS__);       \
			_logged = true;                                                                         \
		}                                                                                           \
	} while (0)
#else
#define DXR_HW_DBG(...)       ((void)0)
#define DXR_HW_DBG_ONCE(...)  ((void)0)
#endif

namespace {

const char *
xr_result_str(XrResult r)
{
	// Just the codes we expect to see; anything else falls through to a
	// numeric format in the caller.
	switch (r) {
	case XR_SUCCESS:                            return "XR_SUCCESS";
	case XR_ERROR_RUNTIME_FAILURE:              return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_RUNTIME_UNAVAILABLE:          return "XR_ERROR_RUNTIME_UNAVAILABLE";
	case XR_ERROR_INSTANCE_LOST:                return "XR_ERROR_INSTANCE_LOST";
	case XR_ERROR_INITIALIZATION_FAILED:        return "XR_ERROR_INITIALIZATION_FAILED";
	case XR_ERROR_API_VERSION_UNSUPPORTED:      return "XR_ERROR_API_VERSION_UNSUPPORTED";
	case XR_ERROR_EXTENSION_NOT_PRESENT:        return "XR_ERROR_EXTENSION_NOT_PRESENT";
	case XR_ERROR_API_LAYER_NOT_PRESENT:        return "XR_ERROR_API_LAYER_NOT_PRESENT";
	case XR_ERROR_OUT_OF_MEMORY:                return "XR_ERROR_OUT_OF_MEMORY";
	case XR_ERROR_FUNCTION_UNSUPPORTED:         return "XR_ERROR_FUNCTION_UNSUPPORTED";
	case XR_ERROR_VALIDATION_FAILURE:           return "XR_ERROR_VALIDATION_FAILURE";
	default:                                    return nullptr;
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

// Reference space for the projection layer. STAGE if available, else LOCAL.
XrSpace g_app_space = XR_NULL_HANDLE;

// View configuration + per-view swapchains. Hardcoded to stereo since
// that's the only mode the runtime currently exposes on Android.
constexpr uint32_t kViewCount = 2;

struct PerView
{
	XrSwapchain swapchain{XR_NULL_HANDLE};
	uint32_t width{0};
	uint32_t height{0};
	// Image arrays sized at xrEnumerateSwapchainImages time. We cap at
	// 8 (matches typical OpenXR runtime budgets) and abort if more come
	// back — keeps the test app std-lib-free in case the prefab AAR
	// surprises us.
	XrSwapchainImageVulkanKHR images[8]{};
	uint32_t image_count{0};
};
PerView g_views[kViewCount];

VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;
VkCommandPool g_app_cmd_pool = VK_NULL_HANDLE;

// Frame counter for throttled logging — log heartbeat every 60 frames.
uint64_t g_frame_count = 0;

// Wire the Khronos OpenXR loader's Android-specific init. Required on
// Android because the loader needs the JavaVM + Activity context to
// discover the runtime APK via the OpenXRRuntimeService intent.
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
	const char *extensions[] = {
	    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	    // _enable2 lets the runtime help create our VkInstance/VkDevice
	    // (next step, B13c) and is the modern replacement for _enable.
	    // The graphics-requirements query in this commit needs only this
	    // extension to be enabled.
	    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
	};

	XrInstanceCreateInfoAndroidKHR android_info = {};
	android_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	android_info.applicationVM = app->activity->vm;
	android_info.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo create_info = {};
	create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
	create_info.next = &android_info;
	std::strncpy(create_info.applicationInfo.applicationName,
	             "cube_handle_vk_android",
	             XR_MAX_APPLICATION_NAME_SIZE - 1);
	create_info.applicationInfo.applicationVersion = 1;
	std::strncpy(create_info.applicationInfo.engineName, "displayxr",
	             XR_MAX_ENGINE_NAME_SIZE - 1);
	create_info.applicationInfo.engineVersion = 1;
	create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	create_info.enabledExtensionCount =
	    sizeof(extensions) / sizeof(extensions[0]);
	create_info.enabledExtensionNames = extensions;

	XrResult res = xrCreateInstance(&create_info, &g_instance);
	log_xr_result("xrCreateInstance", res);
	if (res != XR_SUCCESS) {
		return false;
	}

	XrInstanceProperties props = {};
	props.type = XR_TYPE_INSTANCE_PROPERTIES;
	res = xrGetInstanceProperties(g_instance, &props);
	if (res == XR_SUCCESS) {
		LOGI("Runtime: \"%s\" v%u.%u.%u",
		     props.runtimeName,
		     XR_VERSION_MAJOR(props.runtimeVersion),
		     XR_VERSION_MINOR(props.runtimeVersion),
		     XR_VERSION_PATCH(props.runtimeVersion));
	} else {
		LOGW("xrGetInstanceProperties failed (%d)", (int)res);
	}
	return true;
}

// Query the runtime's XrSystemId for a Vulkan handheld/HMD form factor
// and ask what Vulkan API version the runtime needs. Logs everything to
// logcat — this is the last step we can do without actually creating a
// VkInstance. B13c picks up here and uses these values to create one.
bool
query_system_and_graphics_reqs()
{
	if (g_instance == XR_NULL_HANDLE) {
		return false;
	}

	// Try HMD first (runtime default — see u_system.c), then fall back
	// to handheld for tablet-style displays.
	XrSystemGetInfo sys_info = {};
	sys_info.type = XR_TYPE_SYSTEM_GET_INFO;
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult res = xrGetSystem(g_instance, &sys_info, &g_system_id);
	log_xr_result("xrGetSystem(HEAD_MOUNTED_DISPLAY)", res);
	if (res != XR_SUCCESS) {
		sys_info.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
		res = xrGetSystem(g_instance, &sys_info, &g_system_id);
		log_xr_result("xrGetSystem(HANDHELD_DISPLAY)", res);
		if (res != XR_SUCCESS) {
			return false;
		}
	}

	XrSystemProperties sys_props = {};
	sys_props.type = XR_TYPE_SYSTEM_PROPERTIES;
	res = xrGetSystemProperties(g_instance, g_system_id, &sys_props);
	if (res == XR_SUCCESS) {
		LOGI("System: \"%s\" vendor=0x%08x maxSwapchain=%ux%u maxLayers=%u",
		     sys_props.systemName, sys_props.vendorId,
		     sys_props.graphicsProperties.maxSwapchainImageWidth,
		     sys_props.graphicsProperties.maxSwapchainImageHeight,
		     sys_props.graphicsProperties.maxLayerCount);
	} else {
		LOGW("xrGetSystemProperties failed (%d)", (int)res);
	}

	// Resolve the extension entry point — it lives in libopenxr_loader.so
	// but the loader exposes it only after the corresponding extension
	// is enabled on the instance (which we did in create_instance).
	PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = nullptr;
	res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsRequirements2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&get_reqs));
	if (res != XR_SUCCESS || get_reqs == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrGetVulkanGraphicsRequirements2KHR) failed (%d)",
		     (int)res);
		return false;
	}

	XrGraphicsRequirementsVulkanKHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
	res = get_reqs(g_instance, g_system_id, &reqs);
	log_xr_result("xrGetVulkanGraphicsRequirements2KHR", res);
	if (res != XR_SUCCESS) {
		return false;
	}

	LOGI("Vulkan API: min=%u.%u.%u max=%u.%u.%u",
	     XR_VERSION_MAJOR(reqs.minApiVersionSupported),
	     XR_VERSION_MINOR(reqs.minApiVersionSupported),
	     XR_VERSION_PATCH(reqs.minApiVersionSupported),
	     XR_VERSION_MAJOR(reqs.maxApiVersionSupported),
	     XR_VERSION_MINOR(reqs.maxApiVersionSupported),
	     XR_VERSION_PATCH(reqs.maxApiVersionSupported));

	// Save the runtime's minimum for VkApplicationInfo::apiVersion. Going
	// higher is also allowed (up to max), but min is the safest choice
	// that the runtime promises to accept.
	g_required_vk_version = reqs.minApiVersionSupported;
	return true;
}

// Create a VkInstance via the runtime's xrCreateVulkanInstanceKHR — that
// path lets the runtime inject any platform-specific extensions it needs
// (e.g. swapchain-image-import KHRs) on top of our base
// VkInstanceCreateInfo. No app-side extensions needed at this stage.
bool
create_vulkan_instance()
{
	PFN_xrCreateVulkanInstanceKHR xr_create_vk_instance = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanInstanceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_instance));
	if (res != XR_SUCCESS || xr_create_vk_instance == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrCreateVulkanInstanceKHR) failed (%d)", (int)res);
		return false;
	}

	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "cube_handle_vk_android";
	app_info.applicationVersion = 1;
	app_info.pEngineName = "displayxr";
	app_info.engineVersion = 1;
	app_info.apiVersion = VK_MAKE_VERSION(
	    XR_VERSION_MAJOR(g_required_vk_version),
	    XR_VERSION_MINOR(g_required_vk_version),
	    0);

	VkInstanceCreateInfo vk_ci = {};
	vk_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	vk_ci.pApplicationInfo = &app_info;

	XrVulkanInstanceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanCreateInfo = &vk_ci;
	xr_ci.vulkanAllocator = nullptr;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_instance(g_instance, &xr_ci, &g_vk_instance, &vk_result);
	log_xr_result("xrCreateVulkanInstanceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanInstanceKHR vk_result=%d", (int)vk_result);
		return false;
	}
	return true;
}

// Ask the runtime which physical device backs our XrSystemId. On Android
// this is typically the one and only GPU, but the API surface is the
// same on multi-GPU desktop platforms.
bool
pick_physical_device()
{
	PFN_xrGetVulkanGraphicsDevice2KHR xr_get_phys = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsDevice2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_get_phys));
	if (res != XR_SUCCESS || xr_get_phys == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrGetVulkanGraphicsDevice2KHR) failed (%d)", (int)res);
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

// Create a VkDevice via xrCreateVulkanDeviceKHR — same pattern as the
// instance, the runtime injects whatever device extensions it needs on
// top of our base info. We supply one graphics queue.
bool
create_vulkan_device()
{
	// Find a queue family with graphics support. On Android there's
	// usually exactly one, but the proper enumeration is cheap.
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
		LOGE("No graphics-capable Vulkan queue family");
		return false;
	}

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = g_vk_queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;

	VkPhysicalDeviceFeatures features = {};

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
		LOGE("xrGetInstanceProcAddr(xrCreateVulkanDeviceKHR) failed (%d)", (int)res);
		return false;
	}

	XrVulkanDeviceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanPhysicalDevice = g_vk_phys_device;
	xr_ci.vulkanCreateInfo = &dci;
	xr_ci.vulkanAllocator = nullptr;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_device(g_instance, &xr_ci, &g_vk_device, &vk_result);
	log_xr_result("xrCreateVulkanDeviceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanDeviceKHR vk_result=%d", (int)vk_result);
		return false;
	}

	vkGetDeviceQueue(g_vk_device, g_vk_queue_family, 0, &g_vk_queue);
	LOGI("Vulkan device ready: queue_family=%u queue=%p", g_vk_queue_family, g_vk_queue);
	return true;
}

bool
create_session()
{
	// XR_KHR_vulkan_enable2 reuses XrGraphicsBindingVulkanKHR — there's
	// no `2` suffix on the binding struct, only on the create/get fns.
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

// Enumerate the runtime's view configuration for PRIMARY_STEREO and
// create one swapchain per view using a runtime-supported color format.
// xrEndFrame will reference these swapchains via the projection layer.
bool
create_swapchains()
{
	uint32_t expected_view_count = 0;
	XrResult res = xrEnumerateViewConfigurationViews(
	    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	    0, &expected_view_count, nullptr);
	log_xr_result("xrEnumerateViewConfigurationViews(count)", res);
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
		log_xr_result("xrEnumerateViewConfigurationViews(fill)", res);
		return false;
	}

	// Pick a swapchain format the runtime supports. Prefer 8-bit linear
	// RGBA / BGRA — these line up with what the runtime's vk_native
	// compositor and the CNSDK DP expect. SRGB variants kicked out so we
	// don't trip the gamma double-correction we just fixed in audit B2.
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
	const int64_t preferred[] = {
	    VK_FORMAT_B8G8R8A8_UNORM,
	    VK_FORMAT_R8G8B8A8_UNORM,
	};
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
		LOGE("Runtime didn't advertise a UNORM swapchain format; first supported = 0x%llx",
		     (long long)formats[0]);
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
			LOGW("Swapchain advertises %u images; capping at 8", img_count);
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

// One reference space for the app — projection layer poses are expressed
// relative to this. STAGE if the runtime supports it, else LOCAL.
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

// Standalone command pool for the test app's per-frame cmd buffers.
// Created lazily once the session + swapchains exist.
bool
create_cmd_pool()
{
	VkCommandPoolCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	ci.queueFamilyIndex = g_vk_queue_family;
	VkResult res = vkCreateCommandPool(g_vk_device, &ci, nullptr, &g_app_cmd_pool);
	if (res != VK_SUCCESS) {
		LOGE("vkCreateCommandPool failed: %d", res);
		return false;
	}
	return true;
}

void
destroy_swapchains()
{
	for (uint32_t i = 0; i < kViewCount; ++i) {
		if (g_views[i].swapchain != XR_NULL_HANDLE) {
			xrDestroySwapchain(g_views[i].swapchain);
			g_views[i].swapchain = XR_NULL_HANDLE;
		}
		g_views[i].image_count = 0;
		g_views[i].width = 0;
		g_views[i].height = 0;
	}
	g_swapchain_format = VK_FORMAT_UNDEFINED;
}

void
destroy_reference_space()
{
	if (g_app_space != XR_NULL_HANDLE) {
		xrDestroySpace(g_app_space);
		g_app_space = XR_NULL_HANDLE;
	}
}

void
destroy_cmd_pool()
{
	if (g_app_cmd_pool != VK_NULL_HANDLE && g_vk_device != VK_NULL_HANDLE) {
		vkDestroyCommandPool(g_vk_device, g_app_cmd_pool, nullptr);
		g_app_cmd_pool = VK_NULL_HANDLE;
	}
}

// Record + submit a clear-to-color cmd buffer on the swapchain image
// for view `view_idx` at swapchain index `image_idx`. Distinct colors
// per view so a one-eye-covered hardware test can verify left/right
// (see docs/vendors/leia/cnsdk-android-calibration.md § "Tile-to-eye
// mapping"). Layout-aware: arrives from xrAcquireSwapchainImage in
// COLOR_ATTACHMENT_OPTIMAL (per OpenXR spec), transitions to
// TRANSFER_DST for the clear, then back to COLOR_ATTACHMENT_OPTIMAL
// for xrReleaseSwapchainImage.
bool
record_clear(uint32_t view_idx, uint32_t image_idx)
{
	VkCommandBufferAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool = g_app_cmd_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(g_vk_device, &ai, &cmd) != VK_SUCCESS) {
		return false;
	}

	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	VkImage img = g_views[view_idx].images[image_idx].image;

	VkImageMemoryBarrier to_dst = {};
	to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	to_dst.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_dst.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_dst.image = img;
	to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, nullptr, 0, nullptr, 1, &to_dst);

	VkClearColorValue color = {};
	color.float32[0] = (view_idx == 0) ? 0.8f : 0.1f;  // left: red, right: blue
	color.float32[1] = 0.1f;
	color.float32[2] = (view_idx == 0) ? 0.1f : 0.8f;
	color.float32[3] = 1.0f;
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                     &color, 1, &range);

	VkImageMemoryBarrier to_color = to_dst;
	to_color.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	to_color.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    0, 0, nullptr, 0, nullptr, 1, &to_color);

	vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VkResult res = vkQueueSubmit(g_vk_queue, 1, &si, VK_NULL_HANDLE);
	if (res == VK_SUCCESS) {
		// vkQueueWaitIdle is a host stall but acceptable for this
		// skeleton — xrEndFrame needs the image to be ready and we
		// haven't wired a per-frame fence yet.
		vkQueueWaitIdle(g_vk_queue);
	}
	vkFreeCommandBuffers(g_vk_device, g_app_cmd_pool, 1, &cmd);
	return res == VK_SUCCESS;
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
			LOGW("instance loss pending — exiting");
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

	XrCompositionLayerProjectionView projection_views[kViewCount] = {};
	bool rendered = false;
	if (frame_state.shouldRender) {
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
		uint32_t located_view_count = 0;
		res = xrLocateViews(g_session, &locate_info, &view_state,
		                    kViewCount, &located_view_count, views);
		if (res == XR_SUCCESS && located_view_count == kViewCount) {
#ifdef XRT_DEBUG_ANDROID_VERBOSE
			// Throttle ~1Hz: dump per-view pose + FOV so calibration tests
			// (see android-bringup-checklist.md § B) can read them off
			// directly without instrumenting further.
			if ((g_frame_count % 60) == 0) {
				DXR_HW_DBG("views[L]: pos=(%.3f, %.3f, %.3f) quat=(%.3f, %.3f, %.3f, %.3f) "
				           "fov=(L=%.3f R=%.3f U=%.3f D=%.3f) rad",
				           views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
				           views[0].pose.orientation.x, views[0].pose.orientation.y,
				           views[0].pose.orientation.z, views[0].pose.orientation.w,
				           views[0].fov.angleLeft, views[0].fov.angleRight,
				           views[0].fov.angleUp, views[0].fov.angleDown);
				DXR_HW_DBG("views[R]: pos=(%.3f, %.3f, %.3f) quat=(%.3f, %.3f, %.3f, %.3f)",
				           views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z,
				           views[1].pose.orientation.x, views[1].pose.orientation.y,
				           views[1].pose.orientation.z, views[1].pose.orientation.w);
			}
#endif
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

				record_clear(i, img_idx);

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
	    reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection_layer),
	};

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
	if ((g_frame_count % 60) == 0) {
		LOGI("frame %llu", (unsigned long long)g_frame_count);
	}
	return true;
}

void
destroy_session()
{
	if (g_session != XR_NULL_HANDLE) {
		XrResult res = xrDestroySession(g_session);
		log_xr_result("xrDestroySession", res);
		g_session = XR_NULL_HANDLE;
	}
}

void
destroy_vulkan()
{
	if (g_vk_device != VK_NULL_HANDLE) {
		// Drain the queue before tearing down — same defensive idiom as
		// the runtime's DP destroy path (B6 audit fix).
		vkDeviceWaitIdle(g_vk_device);
		vkDestroyDevice(g_vk_device, nullptr);
		g_vk_device = VK_NULL_HANDLE;
		g_vk_queue = VK_NULL_HANDLE;
		g_vk_queue_family = UINT32_MAX;
	}
	if (g_vk_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(g_vk_instance, nullptr);
		g_vk_instance = VK_NULL_HANDLE;
	}
	g_vk_phys_device = VK_NULL_HANDLE;
}

void
destroy_instance()
{
	// Tear down in reverse-creation order: cmd_pool → space → swapchains
	// → session → Vulkan → instance. Doing it any other way invalidates
	// handles still referenced by the runtime / loader. Drain GPU
	// before per-view image destruction (audit B6).
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(g_vk_device);
	}
	destroy_cmd_pool();
	destroy_reference_space();
	destroy_swapchains();
	destroy_session();
	destroy_vulkan();
	if (g_instance != XR_NULL_HANDLE) {
		XrResult res = xrDestroyInstance(g_instance);
		log_xr_result("xrDestroyInstance", res);
		g_instance = XR_NULL_HANDLE;
		g_system_id = XR_NULL_SYSTEM_ID;
	}
}

void
handle_cmd(struct android_app *app, int32_t cmd)
{
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		LOGI("APP_CMD_INIT_WINDOW (window=%p)", app->window);
		// xrCreateInstance is deferred until the window exists because
		// some runtimes inspect the Activity surface state during create.
		// Idempotent — guarded by g_instance.
		if (g_instance == XR_NULL_HANDLE) {
			// Chain the bring-up steps — bail at the first failure
			// because each step depends on the previous one's outputs.
			if (create_instance(app) &&
			    query_system_and_graphics_reqs() &&
			    create_vulkan_instance() &&
			    pick_physical_device() &&
			    create_vulkan_device() &&
			    create_session() &&
			    create_swapchains() &&
			    create_reference_space() &&
			    create_cmd_pool()) {
				LOGI("Bring-up chain complete; awaiting session state events.");
			} else {
				LOGW("Bring-up chain failed; see logs above.");
			}
		}
		break;
	case APP_CMD_TERM_WINDOW:
		LOGI("APP_CMD_TERM_WINDOW");
		break;
	case APP_CMD_GAINED_FOCUS:
		LOGI("APP_CMD_GAINED_FOCUS");
		break;
	case APP_CMD_LOST_FOCUS:
		LOGI("APP_CMD_LOST_FOCUS");
		break;
	case APP_CMD_DESTROY:
		LOGI("APP_CMD_DESTROY");
		destroy_instance();
		break;
	default:
		break;
	}
}

} // namespace

extern "C" void
android_main(struct android_app *app)
{
	LOGI("cube_handle_vk_android: android_main entered");
	DXR_HW_DBG("android_main: activity=%p vm=%p", (void *)app->activity->clazz,
	           (void *)app->activity->vm);
	app->onAppCmd = handle_cmd;

	if (!initialize_loader(app)) {
		LOGE("OpenXR loader init failed; the loop continues but no XR calls will work");
	}

	while (true) {
		// Drain Android lifecycle events. Block only when the OpenXR
		// session isn't actively rendering yet — once we hit
		// SYNCHRONIZED+, we want to spin the frame loop and only
		// non-blocking poll Android.
		const int poll_timeout_ms = g_session_running ? 0 : 250;
		int events;
		struct android_poll_source *source;
		while (ALooper_pollAll(poll_timeout_ms, nullptr, &events, (void **)&source) >= 0) {
			if (source != nullptr) {
				source->process(app, source);
			}
			if (app->destroyRequested != 0) {
				LOGI("destroyRequested — exiting android_main");
				destroy_instance();
				return;
			}
		}

		// OpenXR side: pump session state events and render a frame
		// when the session is in a rendering state.
		if (g_instance != XR_NULL_HANDLE) {
			poll_xr_events();
			if (g_exit_requested) {
				LOGI("XR exit requested — exiting android_main");
				destroy_instance();
				return;
			}
			if (g_session_running &&
			    (g_session_state == XR_SESSION_STATE_SYNCHRONIZED ||
			     g_session_state == XR_SESSION_STATE_VISIBLE ||
			     g_session_state == XR_SESSION_STATE_FOCUSED)) {
				render_frame();
			}
		}
	}
}
