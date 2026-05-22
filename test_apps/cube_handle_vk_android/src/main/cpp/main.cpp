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
	// Tear down in reverse-creation order: session → Vulkan → instance.
	// Doing it any other way invalidates handles still referenced by the
	// runtime / loader.
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
			    create_session()) {
				LOGI("Bring-up chain complete; XrSession created.");
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
	app->onAppCmd = handle_cmd;

	if (!initialize_loader(app)) {
		LOGE("OpenXR loader init failed; the loop continues but no XR calls will work");
	}

	while (true) {
		int events;
		struct android_poll_source *source;
		while (ALooper_pollAll(0, nullptr, &events, (void **)&source) >= 0) {
			if (source != nullptr) {
				source->process(app, source);
			}
			if (app->destroyRequested != 0) {
				LOGI("destroyRequested — exiting android_main");
				destroy_instance();
				return;
			}
		}
	}
}
