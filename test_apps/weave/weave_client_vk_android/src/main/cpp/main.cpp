// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// weave_client_vk_android — the reference XR_DXR_weave PRESENT-OWNER on Android
// (#1036, epic #1031 / ADR-036 D3).
//
// This is what `displayxr-browser`'s GPU process will do, minus Chromium: the
// app owns its SurfaceView and its own Vulkan swapchain, renders pre-weave
// side-by-side stereo content into an AHardwareBuffer, hands that plus its
// window-relative rects to the runtime, gets a woven AHardwareBuffer back, blits
// it into its own swapchain and presents. It NEVER weaves (ADR-007 / ADR-019).
//
// Deliberately shader-free. The pre-weave content is written by the CPU into a
// staging buffer and copied into the input image, and the woven output is
// blitted straight into the swapchain — so the whole app is clears, copies and
// blits. That keeps the file about the weave contract rather than about a
// renderer, and makes the on-screen result trivially checkable by eye:
//
//   LEFT view  = RED  field, WHITE bar at 30% of the rect width
//   RIGHT view = BLUE field, WHITE bar at 60% of the rect width
//   both       = GREEN 2 px rect border, and a YELLOW block that walks
//                left-to-right once a second (liveness)
//
// On a weaving panel each eye therefore sees a different colour AND a bar at a
// different place — "is it woven, and is the phase right?" is answerable without
// a capture. On sim_display (anaglyph) the same pair reads as red/cyan.
//
// Window geometry is published explicitly (spec v7): Android has no HWND, and a
// SurfaceView's on-screen position is known only to the app. MainActivity samples
// View.getLocationOnScreen() from a Choreographer callback and pushes it here;
// we forward it with xrWeaveBindWindow2DXR so the runtime can anchor the
// interlace phase where the window physically sits (#1033).

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_weave.h>

#define LOG_TAG "weave_client_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define VK_CHECK(expr, what)                                                                                           \
	do {                                                                                                           \
		VkResult vkr_ = (expr);                                                                                \
		if (vkr_ != VK_SUCCESS) {                                                                              \
			LOGE("%s failed: %d", what, (int)vkr_);                                                        \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)

//! Set by JNI_OnLoad / nativeOnCreate; the loader + instance creation need both.
//! In the #1056 fd-handoff shape g_activity is the SERVICE object, not an
//! Activity — android_instance_base_init only NewGlobalRef's it, and the
//! Khronos loader only needs a Context for the runtime broker query.
JavaVM *g_jvm = nullptr;
jobject g_activity = nullptr;

//! #1056: an already-connected runtime socket handed to us by another process,
//! or -1 for the ordinary Java connect. Read once, in xr_create_instance().
int g_adopt_fd = -1;
//! When the fd arrived, so we can report connect-to-first-weave. Reported once.
uint64_t g_adopt_ns = 0;
bool g_adopt_reported = false;

namespace {

//! How many rects the frame is split into. 2 exercises the batch path's
//! multi-rect layout (the whole point of spec v3) while staying eyeballable.
constexpr uint32_t kRectCount = 2;

//! Swapchain images we ask for. Two is enough for a synchronous client.
constexpr uint32_t kSwapchainImages = 3;

struct WeaveApp
{
	// ---- Window / geometry, written by the UI thread ----
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	ANativeWindow *window = nullptr;
	bool window_changed = false;
	int32_t geom_x = 0, geom_y = 0;
	uint32_t geom_w = 0, geom_h = 0;
	int32_t geom_display_id = 0;
	bool geom_valid = false;
	uint64_t geom_generation = 0;
	bool quit = false;

	// ---- OpenXR ----
	XrInstance xr_instance = XR_NULL_HANDLE;
	XrSystemId xr_system = XR_NULL_SYSTEM_ID;
	XrSession xr_session = XR_NULL_HANDLE;
	XrVersion required_vk_version = 0;
	PFN_xrWeaveBindWindow2DXR weave_bind2 = nullptr;
	PFN_xrWeaveSubmitDXR weave_submit = nullptr;

	// ---- Vulkan core ----
	VkInstance vk_instance = VK_NULL_HANDLE;
	VkPhysicalDevice vk_phys = VK_NULL_HANDLE;
	VkDevice vk_device = VK_NULL_HANDLE;
	uint32_t vk_queue_family = UINT32_MAX;
	VkQueue vk_queue = VK_NULL_HANDLE;
	VkCommandPool cmd_pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkSemaphore acquire_sem = VK_NULL_HANDLE;
	VkSemaphore present_sem = VK_NULL_HANDLE;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_props = nullptr;

	// ---- Presentation (ours, not the runtime's) ----
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
	uint32_t swapchain_w = 0, swapchain_h = 0;
	VkImage swapchain_images[8] = {};
	uint32_t swapchain_image_count = 0;

	// ---- Pre-weave input (ours) ----
	AHardwareBuffer *in_ahb = nullptr;
	VkImage in_image = VK_NULL_HANDLE;
	VkDeviceMemory in_memory = VK_NULL_HANDLE;
	uint32_t in_w = 0, in_h = 0;
	bool in_first_use = true;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_memory = VK_NULL_HANDLE;
	uint8_t *staging_ptr = nullptr;
	size_t staging_size = 0;

	// ---- Woven output (the runtime's, handed to us once per allocation) ----
	AHardwareBuffer *out_ahb = nullptr;
	VkImage out_image = VK_NULL_HANDLE;
	VkDeviceMemory out_memory = VK_NULL_HANDLE;
	uint32_t out_w = 0, out_h = 0;
	bool out_first_use = true;

	// ---- Stats ----
	uint64_t frame = 0;
	uint64_t submit_ns_total = 0;
	uint64_t submit_ns_max = 0;
	uint32_t submit_samples = 0;
};

WeaveApp g_app;
bool g_xr_ready = false;

/*!
 * `adb shell setprop debug.dxr.weave.passthrough 1` blits the app's OWN
 * pre-weave input to the swapchain instead of the runtime's woven output. It is
 * the A/B that separates "my present path is broken" from "the weave is broken"
 * without a rebuild — the first question anyone asks when the panel is black.
 */
bool
passthrough_enabled()
{
	char value[PROP_VALUE_MAX] = {};
	if (__system_property_get("debug.dxr.weave.passthrough", value) <= 0) {
		return false;
	}
	return value[0] == '1' || value[0] == 't' || value[0] == 'y';
}

uint64_t
now_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 *
 * OpenXR bring-up.
 *
 */

/*!
 * #1056: publish an adopted runtime socket to the runtime's IPC client, so the
 * next xrCreateInstance skips the Java connect entirely.
 *
 * Two routes, in preference order, because an embedder may only have one:
 *
 *  1. `ipc_client_connection_adopt_fd()` — exported from the runtime library
 *     (libopenxr.version). It is only resolvable once the loader has actually
 *     loaded the runtime .so into this process, which a cheap
 *     xrEnumerateInstanceExtensionProperties() forces; before that
 *     dlsym(RTLD_DEFAULT) sees nothing.
 *  2. `DXR_IPC_FD=<n>` in the environment. No symbol resolution at all, which
 *     is what a process handed the fd in its descriptor table at launch wants
 *     (Chromium's FileDescriptorInfo / base::GlobalDescriptors shape). setenv()
 *     is visible to the runtime because Android has ONE bionic libc per process
 *     — the Windows static-CRT-environment caveat does not apply here.
 *
 * Both are tried so the spike proves both work.
 */
void
publish_adopted_fd()
{
	if (g_adopt_fd < 0) {
		return;
	}

	// Force the loader to load the runtime .so so its exports are visible.
	uint32_t count = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);

	// `setprop debug.dxr.fdhandoff.env 1` forces the environment route, so both
	// mechanisms can be exercised on the same build.
	char force_env[PROP_VALUE_MAX] = {};
	const bool env_only = __system_property_get("debug.dxr.fdhandoff.env", force_env) > 0 && force_env[0] == '1';

	using adopt_fn = void (*)(int);
	adopt_fn adopt = env_only ? nullptr
	                          : reinterpret_cast<adopt_fn>(dlsym(RTLD_DEFAULT, "ipc_client_connection_adopt_fd"));
	if (adopt == nullptr && !env_only) {
		// The Khronos loader dlopen()s the runtime .so RTLD_LOCAL, so its
		// exports are NOT in the global scope. RTLD_NOLOAD gives a handle to
		// the already-loaded copy without loading it twice — an embedder that
		// wants the explicit entry has to name the .so this way.
		void *h = dlopen("openxr_displayxr.so", RTLD_NOLOAD | RTLD_NOW);
		if (h != nullptr) {
			adopt = reinterpret_cast<adopt_fn>(dlsym(h, "ipc_client_connection_adopt_fd"));
			LOGI("HANDOFF: dlopen(RTLD_NOLOAD) got the runtime handle, adopt symbol %s",
			     adopt != nullptr ? "FOUND" : "missing");
		} else {
			LOGI("HANDOFF: dlopen(openxr_displayxr.so, RTLD_NOLOAD) failed: %s", dlerror());
		}
	}
	if (adopt != nullptr) {
		adopt(g_adopt_fd);
		LOGW("HANDOFF: published fd %d via ipc_client_connection_adopt_fd() (dlsym route)", g_adopt_fd);
		return;
	}

	char buf[32];
	snprintf(buf, sizeof(buf), "%d", g_adopt_fd);
	setenv("DXR_IPC_FD", buf, 1);
	LOGW("HANDOFF: adopt symbol not reachable (loader dlopens the runtime RTLD_LOCAL) — using DXR_IPC_FD=%s", buf);
}

bool
xr_create_instance()
{
	PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
	if (xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	                          reinterpret_cast<PFN_xrVoidFunction *>(&initialize_loader)) == XR_SUCCESS &&
	    initialize_loader != nullptr) {
		XrLoaderInitInfoAndroidKHR init = {};
		init.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
		init.applicationVM = g_jvm;
		init.applicationContext = g_activity;
		initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&init));
	}

	publish_adopted_fd();

	const char *extensions[] = {
	    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
	    XR_DXR_WEAVE_EXTENSION_NAME,
	    XR_DXR_DISPLAY_INFO_EXTENSION_NAME,
	    XR_DXR_VIEW_RIG_EXTENSION_NAME,
	};

	XrInstanceCreateInfoAndroidKHR android_info = {};
	android_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	android_info.applicationVM = g_jvm;
	android_info.applicationActivity = g_activity;

	XrInstanceCreateInfo ci = {};
	ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
	ci.next = &android_info;
	ci.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
	ci.enabledExtensionNames = extensions;
	strcpy(ci.applicationInfo.applicationName, "weave_client_vk_android");
	ci.applicationInfo.applicationVersion = 1;
	strcpy(ci.applicationInfo.engineName, "displayxr");
	ci.applicationInfo.engineVersion = 1;
	ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

	// The runtime service may still be starting after a cold launch; the cube
	// app retries the same way.
	XrResult res = XR_ERROR_RUNTIME_UNAVAILABLE;
	for (int attempt = 0; attempt < 5; ++attempt) {
		res = xrCreateInstance(&ci, &g_app.xr_instance);
		if (res != XR_ERROR_RUNTIME_UNAVAILABLE) {
			break;
		}
		LOGW("xrCreateInstance: runtime unavailable (attempt %d/5)", attempt + 1);
		struct timespec ts = {1, 0};
		nanosleep(&ts, nullptr);
	}
	if (res != XR_SUCCESS) {
		LOGE("xrCreateInstance failed: %d — XR_DXR_weave must be advertised (Android needs a runtime "
		     "with #1036)",
		     (int)res);
		return false;
	}
	LOGI("WEAVE_CLIENT: xrCreateInstance=XR_SUCCESS (XR_DXR_weave enabled)");
	return true;
}

bool
xr_get_system_and_requirements()
{
	XrSystemGetInfo sys_info = {};
	sys_info.type = XR_TYPE_SYSTEM_GET_INFO;
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult res = xrGetSystem(g_app.xr_instance, &sys_info, &g_app.xr_system);
	if (res != XR_SUCCESS) {
		sys_info.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
		res = xrGetSystem(g_app.xr_instance, &sys_info, &g_app.xr_system);
	}
	if (res != XR_SUCCESS) {
		LOGE("xrGetSystem failed: %d", (int)res);
		return false;
	}

	PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = nullptr;
	if (xrGetInstanceProcAddr(g_app.xr_instance, "xrGetVulkanGraphicsRequirements2KHR",
	                          reinterpret_cast<PFN_xrVoidFunction *>(&get_reqs)) != XR_SUCCESS ||
	    get_reqs == nullptr) {
		LOGE("xrGetVulkanGraphicsRequirements2KHR unavailable");
		return false;
	}
	XrGraphicsRequirementsVulkanKHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
	if (get_reqs(g_app.xr_instance, g_app.xr_system, &reqs) != XR_SUCCESS) {
		LOGE("xrGetVulkanGraphicsRequirements2KHR failed");
		return false;
	}
	g_app.required_vk_version = reqs.minApiVersionSupported;
	return true;
}

bool
vk_create_instance_and_device()
{
	// Instance. The runtime injects whatever it needs on top; we add the two
	// surface extensions, because unlike a normal OpenXR app WE present.
	const char *inst_exts[] = {
	    VK_KHR_SURFACE_EXTENSION_NAME,
	    VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
	};
	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "weave_client_vk_android";
	app_info.apiVersion = VK_MAKE_VERSION(XR_VERSION_MAJOR(g_app.required_vk_version),
	                                      XR_VERSION_MINOR(g_app.required_vk_version), 0);
	VkInstanceCreateInfo vk_ci = {};
	vk_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	vk_ci.pApplicationInfo = &app_info;
	vk_ci.enabledExtensionCount = sizeof(inst_exts) / sizeof(inst_exts[0]);
	vk_ci.ppEnabledExtensionNames = inst_exts;

	PFN_xrCreateVulkanInstanceKHR create_vk_instance = nullptr;
	if (xrGetInstanceProcAddr(g_app.xr_instance, "xrCreateVulkanInstanceKHR",
	                          reinterpret_cast<PFN_xrVoidFunction *>(&create_vk_instance)) != XR_SUCCESS) {
		LOGE("xrCreateVulkanInstanceKHR unavailable");
		return false;
	}
	XrVulkanInstanceCreateInfoKHR xr_inst_ci = {};
	xr_inst_ci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	xr_inst_ci.systemId = g_app.xr_system;
	xr_inst_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_inst_ci.vulkanCreateInfo = &vk_ci;
	VkResult vk_result = VK_SUCCESS;
	if (create_vk_instance(g_app.xr_instance, &xr_inst_ci, &g_app.vk_instance, &vk_result) != XR_SUCCESS ||
	    vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanInstanceKHR failed (vk=%d)", (int)vk_result);
		return false;
	}

	// Physical device — the one backing our XrSystemId.
	PFN_xrGetVulkanGraphicsDevice2KHR get_phys = nullptr;
	if (xrGetInstanceProcAddr(g_app.xr_instance, "xrGetVulkanGraphicsDevice2KHR",
	                          reinterpret_cast<PFN_xrVoidFunction *>(&get_phys)) != XR_SUCCESS) {
		LOGE("xrGetVulkanGraphicsDevice2KHR unavailable");
		return false;
	}
	XrVulkanGraphicsDeviceGetInfoKHR dev_get = {};
	dev_get.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	dev_get.systemId = g_app.xr_system;
	dev_get.vulkanInstance = g_app.vk_instance;
	if (get_phys(g_app.xr_instance, &dev_get, &g_app.vk_phys) != XR_SUCCESS) {
		LOGE("xrGetVulkanGraphicsDevice2KHR failed");
		return false;
	}

	// Queue family.
	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_app.vk_phys, &qf_count, nullptr);
	VkQueueFamilyProperties qf_props[16] = {};
	if (qf_count > 16) {
		qf_count = 16;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(g_app.vk_phys, &qf_count, qf_props);
	for (uint32_t i = 0; i < qf_count; ++i) {
		if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g_app.vk_queue_family = i;
			break;
		}
	}
	if (g_app.vk_queue_family == UINT32_MAX) {
		LOGE("no graphics queue family");
		return false;
	}

	// Device. VK_ANDROID_external_memory_android_hardware_buffer is what makes
	// the whole handoff possible in both directions; VK_KHR_swapchain is because
	// we present ourselves.
	const char *dev_exts[] = {
	    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
	    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
	    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
	    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
	    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
	    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
	    VK_KHR_MAINTENANCE1_EXTENSION_NAME,
	};
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = g_app.vk_queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkPhysicalDeviceFeatures features = {};
	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.pEnabledFeatures = &features;
	dci.enabledExtensionCount = sizeof(dev_exts) / sizeof(dev_exts[0]);
	dci.ppEnabledExtensionNames = dev_exts;

	PFN_xrCreateVulkanDeviceKHR create_vk_device = nullptr;
	if (xrGetInstanceProcAddr(g_app.xr_instance, "xrCreateVulkanDeviceKHR",
	                          reinterpret_cast<PFN_xrVoidFunction *>(&create_vk_device)) != XR_SUCCESS) {
		LOGE("xrCreateVulkanDeviceKHR unavailable");
		return false;
	}
	XrVulkanDeviceCreateInfoKHR xr_dev_ci = {};
	xr_dev_ci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	xr_dev_ci.systemId = g_app.xr_system;
	xr_dev_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_dev_ci.vulkanPhysicalDevice = g_app.vk_phys;
	xr_dev_ci.vulkanCreateInfo = &dci;
	if (create_vk_device(g_app.xr_instance, &xr_dev_ci, &g_app.vk_device, &vk_result) != XR_SUCCESS ||
	    vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanDeviceKHR failed (vk=%d)", (int)vk_result);
		return false;
	}
	vkGetDeviceQueue(g_app.vk_device, g_app.vk_queue_family, 0, &g_app.vk_queue);

	g_app.get_ahb_props = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
	    vkGetDeviceProcAddr(g_app.vk_device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
	if (g_app.get_ahb_props == nullptr) {
		LOGE("vkGetAndroidHardwareBufferPropertiesANDROID unavailable");
		return false;
	}

	// Command pool / buffer / sync.
	VkCommandPoolCreateInfo pool_ci = {};
	pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_ci.queueFamilyIndex = g_app.vk_queue_family;
	pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VK_CHECK(vkCreateCommandPool(g_app.vk_device, &pool_ci, nullptr, &g_app.cmd_pool), "vkCreateCommandPool");
	VkCommandBufferAllocateInfo cb_ai = {};
	cb_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cb_ai.commandPool = g_app.cmd_pool;
	cb_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cb_ai.commandBufferCount = 1;
	VK_CHECK(vkAllocateCommandBuffers(g_app.vk_device, &cb_ai, &g_app.cmd), "vkAllocateCommandBuffers");
	VkFenceCreateInfo fence_ci = {};
	fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VK_CHECK(vkCreateFence(g_app.vk_device, &fence_ci, nullptr, &g_app.fence), "vkCreateFence");
	VkSemaphoreCreateInfo sem_ci = {};
	sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VK_CHECK(vkCreateSemaphore(g_app.vk_device, &sem_ci, nullptr, &g_app.acquire_sem), "vkCreateSemaphore");
	VK_CHECK(vkCreateSemaphore(g_app.vk_device, &sem_ci, nullptr, &g_app.present_sem), "vkCreateSemaphore");
	return true;
}

bool
xr_create_session()
{
	XrGraphicsBindingVulkanKHR binding = {};
	binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
	binding.instance = g_app.vk_instance;
	binding.physicalDevice = g_app.vk_phys;
	binding.device = g_app.vk_device;
	binding.queueFamilyIndex = g_app.vk_queue_family;
	binding.queueIndex = 0;

	XrSessionCreateInfo ci = {};
	ci.type = XR_TYPE_SESSION_CREATE_INFO;
	ci.next = &binding;
	ci.systemId = g_app.xr_system;
	XrResult res = xrCreateSession(g_app.xr_instance, &ci, &g_app.xr_session);
	if (res != XR_SUCCESS) {
		LOGE("xrCreateSession failed: %d", (int)res);
		return false;
	}

	// A present-owner never calls xrBeginSession / xrWaitFrame / xrEndFrame: it
	// owns its present loop and drives the weave service directly.
	if (xrGetInstanceProcAddr(g_app.xr_instance, "xrWeaveBindWindow2DXR",
	                          reinterpret_cast<PFN_xrVoidFunction *>(&g_app.weave_bind2)) != XR_SUCCESS ||
	    g_app.weave_bind2 == nullptr) {
		LOGE("xrWeaveBindWindow2DXR unavailable — runtime predates spec v7 (#1036)");
		return false;
	}
	if (xrGetInstanceProcAddr(g_app.xr_instance, "xrWeaveSubmitDXR",
	                          reinterpret_cast<PFN_xrVoidFunction *>(&g_app.weave_submit)) != XR_SUCCESS ||
	    g_app.weave_submit == nullptr) {
		LOGE("xrWeaveSubmitDXR unavailable");
		return false;
	}
	LOGI("WEAVE_CLIENT: session ready, weave entry points resolved");
	return true;
}

/*
 *
 * Vulkan presentation (ours).
 *
 */

bool
create_swapchain(ANativeWindow *window)
{
	VkAndroidSurfaceCreateInfoKHR sci = {};
	sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
	sci.window = window;
	VK_CHECK(vkCreateAndroidSurfaceKHR(g_app.vk_instance, &sci, nullptr, &g_app.surface),
	         "vkCreateAndroidSurfaceKHR");

	VkSurfaceCapabilitiesKHR caps = {};
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_app.vk_phys, g_app.surface, &caps),
	         "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

	uint32_t format_count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(g_app.vk_phys, g_app.surface, &format_count, nullptr);
	VkSurfaceFormatKHR formats[32] = {};
	if (format_count > 32) {
		format_count = 32;
	}
	vkGetPhysicalDeviceSurfaceFormatsKHR(g_app.vk_phys, g_app.surface, &format_count, formats);
	VkSurfaceFormatKHR chosen = formats[0];
	for (uint32_t i = 0; i < format_count; ++i) {
		if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
			chosen = formats[i];
			break;
		}
	}

	VkSwapchainCreateInfoKHR ci = {};
	ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	ci.surface = g_app.surface;
	ci.minImageCount = caps.minImageCount < kSwapchainImages ? kSwapchainImages : caps.minImageCount;
	if (caps.maxImageCount != 0 && ci.minImageCount > caps.maxImageCount) {
		ci.minImageCount = caps.maxImageCount;
	}
	ci.imageFormat = chosen.format;
	ci.imageColorSpace = chosen.colorSpace;
	ci.imageExtent = caps.currentExtent;
	ci.imageArrayLayers = 1;
	// TRANSFER_DST because the woven output is blitted straight in — no shaders.
	ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	// Hand SurfaceFlinger an IDENTITY-transform swapchain when it will take one.
	// Passing currentTransform while sizing imageExtent from currentExtent is the
	// classic Android mismatch: on a rotated panel the two disagree about which
	// axis is which and SF silently rescales the layer (or drops it) instead of
	// erroring.
	ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0
	                      ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
	                      : caps.currentTransform;
	ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
	                        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
	                        : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	ci.clipped = VK_TRUE;
	VK_CHECK(vkCreateSwapchainKHR(g_app.vk_device, &ci, nullptr, &g_app.swapchain), "vkCreateSwapchainKHR");

	g_app.swapchain_format = ci.imageFormat;
	g_app.swapchain_w = ci.imageExtent.width;
	g_app.swapchain_h = ci.imageExtent.height;
	g_app.swapchain_image_count = 8;
	VK_CHECK(vkGetSwapchainImagesKHR(g_app.vk_device, g_app.swapchain, &g_app.swapchain_image_count,
	                                 g_app.swapchain_images),
	         "vkGetSwapchainImagesKHR");
	LOGI("WEAVE_CLIENT: swapchain %ux%u, %u images, format %d, preTransform 0x%x (current 0x%x, min/max "
	     "extent %ux%u..%ux%u, alpha 0x%x)",
	     g_app.swapchain_w, g_app.swapchain_h, g_app.swapchain_image_count, (int)g_app.swapchain_format,
	     (unsigned)ci.preTransform, (unsigned)caps.currentTransform, caps.minImageExtent.width,
	     caps.minImageExtent.height, caps.maxImageExtent.width, caps.maxImageExtent.height,
	     (unsigned)caps.supportedCompositeAlpha);
	return true;
}

void
destroy_swapchain()
{
	if (g_app.swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(g_app.vk_device, g_app.swapchain, nullptr);
		g_app.swapchain = VK_NULL_HANDLE;
	}
	if (g_app.surface != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(g_app.vk_instance, g_app.surface, nullptr);
		g_app.surface = VK_NULL_HANDLE;
	}
	g_app.swapchain_image_count = 0;
}

/*
 *
 * AHardwareBuffer <-> VkImage.
 *
 */

bool
import_ahb(AHardwareBuffer *ahb, VkImageUsageFlags usage, VkImage *out_image, VkDeviceMemory *out_memory)
{
	VkAndroidHardwareBufferFormatPropertiesANDROID fmt_props = {};
	fmt_props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
	VkAndroidHardwareBufferPropertiesANDROID props = {};
	props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
	props.pNext = &fmt_props;
	VK_CHECK(g_app.get_ahb_props(g_app.vk_device, ahb, &props),
	         "vkGetAndroidHardwareBufferPropertiesANDROID");

	AHardwareBuffer_Desc desc = {};
	AHardwareBuffer_describe(ahb, &desc);

	VkExternalMemoryImageCreateInfo emi = {};
	emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &emi;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = {desc.width, desc.height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK(vkCreateImage(g_app.vk_device, &ici, nullptr, out_image), "vkCreateImage(AHB)");

	VkImportAndroidHardwareBufferInfoANDROID import_info = {};
	import_info.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
	import_info.buffer = ahb;
	VkMemoryDedicatedAllocateInfo dedicated = {};
	dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated.pNext = &import_info;
	dedicated.image = *out_image;

	VkPhysicalDeviceMemoryProperties mem_props = {};
	vkGetPhysicalDeviceMemoryProperties(g_app.vk_phys, &mem_props);
	uint32_t type_index = UINT32_MAX;
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
		if (props.memoryTypeBits & (1u << i)) {
			type_index = i;
			break;
		}
	}
	if (type_index == UINT32_MAX) {
		LOGE("no memory type for AHardwareBuffer import");
		return false;
	}

	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.pNext = &dedicated;
	mai.allocationSize = props.allocationSize;
	mai.memoryTypeIndex = type_index;
	VK_CHECK(vkAllocateMemory(g_app.vk_device, &mai, nullptr, out_memory), "vkAllocateMemory(AHB)");
	VK_CHECK(vkBindImageMemory(g_app.vk_device, *out_image, *out_memory, 0), "vkBindImageMemory(AHB)");
	return true;
}

bool
create_input(uint32_t w, uint32_t h)
{
	AHardwareBuffer_Desc desc = {};
	desc.width = w;
	desc.height = h;
	desc.layers = 1;
	desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
	// SAMPLED so the runtime can read it; COLOR_OUTPUT so the driver gives us a
	// layout the compositor's blit source likes.
	desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
	if (AHardwareBuffer_allocate(&desc, &g_app.in_ahb) != 0) {
		LOGE("AHardwareBuffer_allocate(input %ux%u) failed", w, h);
		return false;
	}
	if (!import_ahb(g_app.in_ahb, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                                  VK_IMAGE_USAGE_SAMPLED_BIT,
	                &g_app.in_image, &g_app.in_memory)) {
		return false;
	}
	g_app.in_w = w;
	g_app.in_h = h;
	g_app.in_first_use = true;

	// Host-visible staging buffer the CPU paints the pre-weave pattern into.
	g_app.staging_size = (size_t)w * h * 4u;
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = g_app.staging_size;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK(vkCreateBuffer(g_app.vk_device, &bci, nullptr, &g_app.staging), "vkCreateBuffer(staging)");

	VkMemoryRequirements reqs = {};
	vkGetBufferMemoryRequirements(g_app.vk_device, g_app.staging, &reqs);
	VkPhysicalDeviceMemoryProperties mem_props = {};
	vkGetPhysicalDeviceMemoryProperties(g_app.vk_phys, &mem_props);
	uint32_t type_index = UINT32_MAX;
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
		if ((reqs.memoryTypeBits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want) {
			type_index = i;
			break;
		}
	}
	if (type_index == UINT32_MAX) {
		LOGE("no host-visible memory type for staging");
		return false;
	}
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = reqs.size;
	mai.memoryTypeIndex = type_index;
	VK_CHECK(vkAllocateMemory(g_app.vk_device, &mai, nullptr, &g_app.staging_memory), "vkAllocateMemory(staging)");
	VK_CHECK(vkBindBufferMemory(g_app.vk_device, g_app.staging, g_app.staging_memory, 0), "vkBindBufferMemory");
	VK_CHECK(vkMapMemory(g_app.vk_device, g_app.staging_memory, 0, VK_WHOLE_SIZE, 0,
	                     reinterpret_cast<void **>(&g_app.staging_ptr)),
	         "vkMapMemory");
	LOGI("WEAVE_CLIENT: pre-weave input AHardwareBuffer %ux%u ready", w, h);
	return true;
}

void
destroy_input()
{
	if (g_app.staging_ptr != nullptr) {
		vkUnmapMemory(g_app.vk_device, g_app.staging_memory);
		g_app.staging_ptr = nullptr;
	}
	if (g_app.staging != VK_NULL_HANDLE) {
		vkDestroyBuffer(g_app.vk_device, g_app.staging, nullptr);
		g_app.staging = VK_NULL_HANDLE;
	}
	if (g_app.staging_memory != VK_NULL_HANDLE) {
		vkFreeMemory(g_app.vk_device, g_app.staging_memory, nullptr);
		g_app.staging_memory = VK_NULL_HANDLE;
	}
	if (g_app.in_image != VK_NULL_HANDLE) {
		vkDestroyImage(g_app.vk_device, g_app.in_image, nullptr);
		g_app.in_image = VK_NULL_HANDLE;
	}
	if (g_app.in_memory != VK_NULL_HANDLE) {
		vkFreeMemory(g_app.vk_device, g_app.in_memory, nullptr);
		g_app.in_memory = VK_NULL_HANDLE;
	}
	if (g_app.in_ahb != nullptr) {
		AHardwareBuffer_release(g_app.in_ahb);
		g_app.in_ahb = nullptr;
	}
	g_app.in_w = g_app.in_h = 0;
}

void
destroy_output()
{
	if (g_app.out_image != VK_NULL_HANDLE) {
		vkDestroyImage(g_app.vk_device, g_app.out_image, nullptr);
		g_app.out_image = VK_NULL_HANDLE;
	}
	if (g_app.out_memory != VK_NULL_HANDLE) {
		vkFreeMemory(g_app.vk_device, g_app.out_memory, nullptr);
		g_app.out_memory = VK_NULL_HANDLE;
	}
	if (g_app.out_ahb != nullptr) {
		AHardwareBuffer_release(g_app.out_ahb);
		g_app.out_ahb = nullptr;
	}
	g_app.out_w = g_app.out_h = 0;
	g_app.out_first_use = true;
}

/*
 *
 * The pre-weave pattern.
 *
 */

inline void
put_px(uint8_t *base, uint32_t stride, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	uint8_t *p = base + (size_t)y * stride + (size_t)x * 4u;
	p[0] = r;
	p[1] = g;
	p[2] = b;
	p[3] = a;
}

/*!
 * Paint one rect's SQUEEZED side-by-side content at that rect's own window
 * position — the spec-v3 batch layout: the input is window-sized and each rect
 * region holds its own SBS pair, left view in the rect's left half.
 */
void
paint_rect(uint8_t *base, uint32_t stride, int32_t rx, int32_t ry, int32_t rw, int32_t rh, uint64_t frame)
{
	const int32_t half = rw / 2;
	if (half <= 0 || rh <= 0) {
		return;
	}
	// The liveness block walks across the rect once per ~1 s at 60 Hz.
	const int32_t walk = (int32_t)((frame % 60) * (uint64_t)half / 60);

	for (int32_t y = 0; y < rh; ++y) {
		for (int32_t hx = 0; hx < half; ++hx) {
			// Bars sit at different fractions per eye — the disparity that
			// makes "which eye am I seeing?" answerable by eye.
			const bool left_bar = hx > (half * 28) / 100 && hx < (half * 32) / 100;
			const bool right_bar = hx > (half * 58) / 100 && hx < (half * 62) / 100;
			const bool live = hx >= walk && hx < walk + half / 24 && y > rh / 2 - rh / 16 &&
			                  y < rh / 2 + rh / 16;
			const bool border = y < 2 || y >= rh - 2 || hx < 2 || hx >= half - 2;

			uint8_t lr = 200, lg = 24, lb = 24;   // left view: red field
			uint8_t rr = 24, rg = 48, rb = 220;   // right view: blue field
			if (left_bar) {
				lr = lg = lb = 245;
			}
			if (right_bar) {
				rr = rg = rb = 245;
			}
			if (live) {
				lr = 250;
				lg = 220;
				lb = 40;
				rr = 250;
				rg = 220;
				rb = 40;
			}
			if (border) {
				lr = rr = 30;
				lg = rg = 220;
				lb = rb = 30;
			}
			put_px(base, stride, (uint32_t)(rx + hx), (uint32_t)(ry + y), lr, lg, lb, 255);
			put_px(base, stride, (uint32_t)(rx + half + hx), (uint32_t)(ry + y), rr, rg, rb, 255);
		}
	}
}

void
paint_frame(uint64_t frame, const XrRect2Di *rects, uint32_t rect_count)
{
	const uint32_t stride = g_app.in_w * 4u;
	memset(g_app.staging_ptr, 0, g_app.staging_size);
	for (uint32_t i = 0; i < rect_count; ++i) {
		paint_rect(g_app.staging_ptr, stride, rects[i].offset.x, rects[i].offset.y,
		           rects[i].extent.width, rects[i].extent.height, frame);
	}
}

/*
 *
 * Frame.
 *
 */

void
barrier(VkCommandBuffer cmd,
        VkImage image,
        VkImageLayout old_layout,
        VkImageLayout new_layout,
        VkAccessFlags src_access,
        VkAccessFlags dst_access,
        VkPipelineStageFlags src_stage,
        VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.oldLayout = old_layout;
	b.newLayout = new_layout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

//! Upload the CPU pattern into the input AHardwareBuffer and WAIT for it. The
//! weave contract is "caller's writes are complete before xrWeaveSubmitDXR"
//! (there is no acquire fence on this platform yet, #1036 follow-up), so this
//! wait is the contract, not laziness.
bool
upload_input()
{
	vkResetCommandBuffer(g_app.cmd, 0);
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkBeginCommandBuffer(g_app.cmd, &bi), "vkBeginCommandBuffer(upload)");

	barrier(g_app.cmd, g_app.in_image,
	        g_app.in_first_use ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	g_app.in_first_use = false;

	VkBufferImageCopy copy = {};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {g_app.in_w, g_app.in_h, 1};
	vkCmdCopyBufferToImage(g_app.cmd, g_app.staging, g_app.in_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	                       &copy);

	// GENERAL is what the runtime's importer expects to find across frames.
	barrier(g_app.cmd, g_app.in_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

	VK_CHECK(vkEndCommandBuffer(g_app.cmd), "vkEndCommandBuffer(upload)");
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &g_app.cmd;
	VK_CHECK(vkQueueSubmit(g_app.vk_queue, 1, &si, g_app.fence), "vkQueueSubmit(upload)");
	VK_CHECK(vkWaitForFences(g_app.vk_device, 1, &g_app.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(upload)");
	vkResetFences(g_app.vk_device, 1, &g_app.fence);
	return true;
}

//! Blit the woven output into the swapchain and present it. This is the
//! present-owner's own composite step — the runtime never touches our surface.
bool
present_woven()
{
	uint32_t index = 0;
	VkResult res = vkAcquireNextImageKHR(g_app.vk_device, g_app.swapchain, UINT64_MAX, g_app.acquire_sem,
	                                     VK_NULL_HANDLE, &index);
	if (res == VK_ERROR_OUT_OF_DATE_KHR) {
		LOGW("vkAcquireNextImageKHR: OUT_OF_DATE — skipping the frame");
		return true;
	}
	// SUBOPTIMAL still hands back a usable image; skipping it would leave the
	// surface black forever on a device that always reports it.
	if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
		LOGE("vkAcquireNextImageKHR: %d", (int)res);
		return false;
	}
	static bool logged_suboptimal = false;
	if (res == VK_SUBOPTIMAL_KHR && !logged_suboptimal) {
		logged_suboptimal = true;
		LOGW("vkAcquireNextImageKHR: SUBOPTIMAL (presenting anyway)");
	}

	vkResetCommandBuffer(g_app.cmd, 0);
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkBeginCommandBuffer(g_app.cmd, &bi), "vkBeginCommandBuffer(present)");

	// Passthrough (debug.dxr.weave.passthrough) blits the pre-weave input, which
	// is the A/B for "is my present path or the weave at fault?".
	const bool passthrough = passthrough_enabled();
	VkImage src_image = passthrough ? g_app.in_image : g_app.out_image;
	const uint32_t src_w = passthrough ? g_app.in_w : g_app.out_w;
	const uint32_t src_h = passthrough ? g_app.in_h : g_app.out_h;

	barrier(g_app.cmd, src_image, g_app.out_first_use && !passthrough ? VK_IMAGE_LAYOUT_UNDEFINED
	                                                                 : VK_IMAGE_LAYOUT_GENERAL,
	        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	g_app.out_first_use = false;
	barrier(g_app.cmd, g_app.swapchain_images[index], VK_IMAGE_LAYOUT_UNDEFINED,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageBlit blit = {};
	blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.srcOffsets[1] = {(int32_t)src_w, (int32_t)src_h, 1};
	blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.dstOffsets[1] = {(int32_t)g_app.swapchain_w, (int32_t)g_app.swapchain_h, 1};
	// NEAREST: the woven output is an interlaced lattice — any filtering here
	// would smear neighbouring views into each other and destroy the 3D.
	vkCmdBlitImage(g_app.cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               g_app.swapchain_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
	               VK_FILTER_NEAREST);

	barrier(g_app.cmd, g_app.swapchain_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	barrier(g_app.cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
	        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

	VK_CHECK(vkEndCommandBuffer(g_app.cmd), "vkEndCommandBuffer(present)");

	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.waitSemaphoreCount = 1;
	si.pWaitSemaphores = &g_app.acquire_sem;
	si.pWaitDstStageMask = &wait_stage;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &g_app.cmd;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &g_app.present_sem;
	VK_CHECK(vkQueueSubmit(g_app.vk_queue, 1, &si, g_app.fence), "vkQueueSubmit(present)");

	VkPresentInfoKHR pi = {};
	pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &g_app.present_sem;
	pi.swapchainCount = 1;
	pi.pSwapchains = &g_app.swapchain;
	pi.pImageIndices = &index;
	VkResult present_res = vkQueuePresentKHR(g_app.vk_queue, &pi);
	static bool logged_present = false;
	if (present_res != VK_SUCCESS && !logged_present) {
		logged_present = true;
		LOGW("vkQueuePresentKHR: %d", (int)present_res);
	}

	VK_CHECK(vkWaitForFences(g_app.vk_device, 1, &g_app.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(present)");
	vkResetFences(g_app.vk_device, 1, &g_app.fence);
	return true;
}

/*!
 * `adb shell setprop debug.dxr.weave.dump 1` writes the pre-weave input and the
 * woven output to PPMs next to the app, once, then clears the flag.
 *
 * This is the load-bearing verification for #1036 and it deliberately does not
 * go through the display: `adb screencap` returns black for this app's layer on
 * the NP02J, so "is the panel showing the weave?" cannot be answered from the
 * host. Reading the woven buffer back on the client proves the whole chain —
 * AHardwareBuffer out, DP weave, AHardwareBuffer back — independently of
 * whatever SurfaceFlinger does with our surface afterwards.
 */
void
dump_image(VkImage image, uint32_t w, uint32_t h, const char *path)
{
	const VkDeviceSize size = (VkDeviceSize)w * h * 4u;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;

	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = size;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (vkCreateBuffer(g_app.vk_device, &bci, nullptr, &buffer) != VK_SUCCESS) {
		return;
	}
	VkMemoryRequirements reqs = {};
	vkGetBufferMemoryRequirements(g_app.vk_device, buffer, &reqs);
	VkPhysicalDeviceMemoryProperties mem_props = {};
	vkGetPhysicalDeviceMemoryProperties(g_app.vk_phys, &mem_props);
	uint32_t type_index = UINT32_MAX;
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
		if ((reqs.memoryTypeBits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want) {
			type_index = i;
			break;
		}
	}
	if (type_index == UINT32_MAX) {
		vkDestroyBuffer(g_app.vk_device, buffer, nullptr);
		return;
	}
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = reqs.size;
	mai.memoryTypeIndex = type_index;
	vkAllocateMemory(g_app.vk_device, &mai, nullptr, &memory);
	vkBindBufferMemory(g_app.vk_device, buffer, memory, 0);

	vkResetCommandBuffer(g_app.cmd, 0);
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(g_app.cmd, &bi);
	barrier(g_app.cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy copy = {};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {w, h, 1};
	vkCmdCopyImageToBuffer(g_app.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
	barrier(g_app.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
	        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	vkEndCommandBuffer(g_app.cmd);
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &g_app.cmd;
	vkQueueSubmit(g_app.vk_queue, 1, &si, g_app.fence);
	vkWaitForFences(g_app.vk_device, 1, &g_app.fence, VK_TRUE, UINT64_MAX);
	vkResetFences(g_app.vk_device, 1, &g_app.fence);

	uint8_t *mapped = nullptr;
	if (vkMapMemory(g_app.vk_device, memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void **>(&mapped)) ==
	    VK_SUCCESS) {
		FILE *f = fopen(path, "wb");
		if (f != nullptr) {
			fprintf(f, "P6\n%u %u\n255\n", w, h);
			for (uint32_t i = 0; i < w * h; ++i) {
				fwrite(mapped + (size_t)i * 4u, 1, 3, f);
			}
			fclose(f);
			LOGI("WEAVE_CLIENT: dumped %ux%u to %s", w, h, path);
		} else {
			LOGE("WEAVE_CLIENT: could not open %s", path);
		}
		vkUnmapMemory(g_app.vk_device, memory);
	}
	vkDestroyBuffer(g_app.vk_device, buffer, nullptr);
	vkFreeMemory(g_app.vk_device, memory, nullptr);
}

void
maybe_dump()
{
	char value[PROP_VALUE_MAX] = {};
	if (__system_property_get("debug.dxr.weave.dump", value) <= 0 || value[0] != '1') {
		return;
	}
	__system_property_set("debug.dxr.weave.dump", "0");
	dump_image(g_app.in_image, g_app.in_w, g_app.in_h,
	           "/sdcard/Android/data/com.displayxr.weave_client_vk_android/files/weave_in.ppm");
	if (g_app.out_image != VK_NULL_HANDLE) {
		dump_image(g_app.out_image, g_app.out_w, g_app.out_h,
		           "/sdcard/Android/data/com.displayxr.weave_client_vk_android/files/weave_out.ppm");
	}
}

//! One present-owner frame: paint -> upload -> weave -> composite -> present.
bool
render_frame()
{
	// The rects: kRectCount horizontal bands, inset so the gaps between them are
	// visibly NOT woven (they stay whatever the caller drew).
	XrRect2Di rects[kRectCount] = {};
	const int32_t inset = (int32_t)(g_app.in_w / 20u);
	const int32_t band_h = (int32_t)(g_app.in_h / (kRectCount + 1));
	for (uint32_t i = 0; i < kRectCount; ++i) {
		rects[i].offset.x = inset;
		rects[i].offset.y = (int32_t)(band_h / 2 + i * band_h);
		rects[i].extent.width = (int32_t)g_app.in_w - 2 * inset;
		rects[i].extent.height = band_h - band_h / 8;
	}

	paint_frame(g_app.frame, rects, kRectCount);
	if (!upload_input()) {
		return false;
	}

	// Spec v7: say what the handle IS, so a wrong-platform handle is a clean
	// validation error rather than a dereference in the service.
	XrWeaveSubmitHandlesDXR kinds = {};
	kinds.type = XR_TYPE_WEAVE_SUBMIT_HANDLES_DXR;
	kinds.inputKind = XR_WEAVE_HANDLE_KIND_AHARDWAREBUFFER_DXR;
	kinds.overlayKind = XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR;

	// Spec v3 batch: window-sized input, each rect's SBS at its own position.
	XrWeaveSubmitRectsDXR batch = {};
	batch.type = XR_TYPE_WEAVE_SUBMIT_RECTS_DXR;
	batch.next = &kinds;
	batch.rectCount = kRectCount;
	batch.rects = rects;

	XrWeaveSubmitInfoDXR info = {};
	info.type = XR_TYPE_WEAVE_SUBMIT_INFO_DXR;
	info.next = &batch;
	info.inputTexture = g_app.in_ahb;
	info.firstChunk = XR_TRUE; // one submit per frame — clear the gaps

	XrWeaveOutputDXR out = {};
	out.type = XR_TYPE_WEAVE_OUTPUT_DXR;

	const uint64_t t0 = now_ns();
	XrResult res = g_app.weave_submit(g_app.xr_session, &info, &out);
	const uint64_t dt = now_ns() - t0;
	if (res != XR_SUCCESS) {
		LOGE("xrWeaveSubmitDXR failed: %d", (int)res);
		return false;
	}
	g_app.submit_ns_total += dt;
	g_app.submit_samples++;
	if (dt > g_app.submit_ns_max) {
		g_app.submit_ns_max = dt;
	}

	// #1056: the headline number for the fd-handoff spike — how long from "this
	// process was handed a connected socket" to "this process got woven pixels
	// back", with no Java connect of its own anywhere in between.
	if (!g_adopt_reported && g_adopt_ns != 0) {
		g_adopt_reported = true;
		LOGW("HANDOFF: adopt -> first woven frame = %.1f ms (first submit %.2f ms)",
		     (double)(now_ns() - g_adopt_ns) / 1e6, (double)dt / 1e6);
	}

	// The woven buffer arrives on the first submit and again on every
	// re-allocation (resize). On steady-state frames it is NULL and we reuse it.
	if (out.weavedTexture != nullptr) {
		destroy_output();
		g_app.out_ahb = static_cast<AHardwareBuffer *>(out.weavedTexture);
		if (!import_ahb(g_app.out_ahb,
		                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &g_app.out_image,
		                &g_app.out_memory)) {
			return false;
		}
		g_app.out_w = out.width;
		g_app.out_h = out.height;
		LOGI("WEAVE_CLIENT: woven output AHardwareBuffer %ux%u adopted (eyes valid=%d tracking=%d count=%u)",
		     out.width, out.height, (int)out.eyesValid, (int)out.eyesTracking, out.eyeCount);
	}
	if (g_app.out_image == VK_NULL_HANDLE) {
		LOGE("no woven output to composite");
		return false;
	}

	maybe_dump();

	if (!present_woven()) {
		return false;
	}

	if (++g_app.frame % 60 == 0 && g_app.submit_samples > 0) {
		LOGI("WEAVE_CLIENT: frame %llu — xrWeaveSubmitDXR avg %.2f ms, max %.2f ms over %u submits "
		     "(%ux%u output, %u rects)",
		     (unsigned long long)g_app.frame,
		     (double)g_app.submit_ns_total / (double)g_app.submit_samples / 1.0e6,
		     (double)g_app.submit_ns_max / 1.0e6, g_app.submit_samples, g_app.out_w, g_app.out_h,
		     kRectCount);
		g_app.submit_ns_total = 0;
		g_app.submit_ns_max = 0;
		g_app.submit_samples = 0;
	}
	return true;
}

//! Publish the window's client rect whenever it moves or resizes. On Android
//! this is the ONLY way the runtime can know where to anchor the interlace
//! phase — there is no window handle to read it off (#1033 / spec v7).
void
publish_geometry_if_changed(uint64_t *last_generation)
{
	pthread_mutex_lock(&g_app.lock);
	const bool valid = g_app.geom_valid;
	const uint64_t generation = g_app.geom_generation;
	const int32_t x = g_app.geom_x, y = g_app.geom_y, display_id = g_app.geom_display_id;
	const uint32_t w = g_app.geom_w, h = g_app.geom_h;
	pthread_mutex_unlock(&g_app.lock);

	if (!valid || generation == *last_generation) {
		return;
	}
	*last_generation = generation;

	XrWeaveWindowGeometryDXR geom = {};
	geom.type = XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR;
	geom.windowOriginOnScreen = {x, y};
	geom.clientSize = {(int32_t)w, (int32_t)h};
	geom.displayId = display_id;

	XrWeaveBindWindowInfoDXR bind = {};
	bind.type = XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR;
	bind.next = &geom;
	bind.windowHandle = nullptr; // Android: the Surface is ours, the runtime never touches it

	XrResult res = g_app.weave_bind2(g_app.xr_session, &bind);
	LOGI("WEAVE_CLIENT: published window rect %d,%d %ux%u display %d → %d", x, y, w, h, display_id, (int)res);
}

/*!
 * Rebuild the swapchain + pre-weave input when the surface's size changed
 * (rotation, resize, a new Surface after a background/foreground cycle).
 */
bool
reconcile_size()
{
	pthread_mutex_lock(&g_app.lock);
	ANativeWindow *window = g_app.window;
	const bool changed = g_app.window_changed;
	g_app.window_changed = false;
	pthread_mutex_unlock(&g_app.lock);
	if (window == nullptr) {
		return true; // backgrounded — the next surfaceCreated brings us back
	}

	const uint32_t w = (uint32_t)ANativeWindow_getWidth(window);
	const uint32_t h = (uint32_t)ANativeWindow_getHeight(window);
	if (!changed && w == g_app.swapchain_w && h == g_app.swapchain_h) {
		return true;
	}
	if (w == 0 || h == 0) {
		return true;
	}

	LOGI("WEAVE_CLIENT: surface is now %ux%u (was %ux%u) — rebuilding", w, h, g_app.swapchain_w,
	     g_app.swapchain_h);
	vkDeviceWaitIdle(g_app.vk_device);
	destroy_input();
	destroy_swapchain();
	// The runtime's woven output is sized from our input, so it will be
	// re-allocated (and handed back) on the next submit.
	destroy_output();
	if (!create_swapchain(window)) {
		return false;
	}
	return create_input(g_app.swapchain_w, g_app.swapchain_h);
}

void *
render_thread(void *)
{
	// Wait for the Surface.
	ANativeWindow *window = nullptr;
	while (window == nullptr) {
		pthread_mutex_lock(&g_app.lock);
		window = g_app.window;
		const bool quit = g_app.quit;
		pthread_mutex_unlock(&g_app.lock);
		if (quit) {
			return nullptr;
		}
		if (window == nullptr) {
			struct timespec ts = {0, 50 * 1000 * 1000};
			nanosleep(&ts, nullptr);
		}
	}

	if (!xr_create_instance() || !xr_get_system_and_requirements() || !vk_create_instance_and_device() ||
	    !xr_create_session()) {
		LOGE("WEAVE_CLIENT: bring-up failed");
		return nullptr;
	}
	g_xr_ready = true;

	if (!create_swapchain(window)) {
		return nullptr;
	}
	if (!create_input(g_app.swapchain_w, g_app.swapchain_h)) {
		return nullptr;
	}

	uint64_t last_geometry_generation = 0;
	// Bind once even before the first geometry report, so a runtime with no
	// geometry still has a bound window (display-scoped phase).
	{
		XrWeaveBindWindowInfoDXR bind = {};
		bind.type = XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR;
		g_app.weave_bind2(g_app.xr_session, &bind);
	}

	for (;;) {
		pthread_mutex_lock(&g_app.lock);
		const bool quit = g_app.quit;
		pthread_mutex_unlock(&g_app.lock);
		if (quit) {
			break;
		}
		publish_geometry_if_changed(&last_geometry_generation);

		// A rotation / resize changes the client area, and both the swapchain and
		// the pre-weave input are sized to it — rebuild them, or every later frame
		// weaves the old geometry into a stale-sized buffer.
		if (!reconcile_size()) {
			break;
		}

		if (!render_frame()) {
			struct timespec ts = {0, 200 * 1000 * 1000};
			nanosleep(&ts, nullptr);
		}
	}

	vkDeviceWaitIdle(g_app.vk_device);
	destroy_output();
	destroy_input();
	destroy_swapchain();
	if (g_app.xr_session != XR_NULL_HANDLE) {
		xrDestroySession(g_app.xr_session);
	}
	if (g_app.xr_instance != XR_NULL_HANDLE) {
		xrDestroyInstance(g_app.xr_instance);
	}
	return nullptr;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *)
{
	g_jvm = vm;
	return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_displayxr_weave_1client_1vk_1android_MainActivity_nativeOnCreate(JNIEnv *env, jobject thiz)
{
	g_activity = env->NewGlobalRef(thiz);
	pthread_t tid;
	pthread_create(&tid, nullptr, render_thread, nullptr);
	pthread_detach(tid);
}

JNIEXPORT void JNICALL
Java_com_displayxr_weave_1client_1vk_1android_MainActivity_nativeSetSurface(JNIEnv *env, jobject, jobject surface)
{
	pthread_mutex_lock(&g_app.lock);
	if (g_app.window != nullptr) {
		ANativeWindow_release(g_app.window);
		g_app.window = nullptr;
	}
	if (surface != nullptr) {
		g_app.window = ANativeWindow_fromSurface(env, surface);
	}
	g_app.window_changed = true;
	pthread_mutex_unlock(&g_app.lock);
}

JNIEXPORT void JNICALL
Java_com_displayxr_weave_1client_1vk_1android_MainActivity_nativeSetWindowGeometry(
    JNIEnv *, jobject, jint x, jint y, jint w, jint h, jint display_id)
{
	if (w <= 0 || h <= 0) {
		return;
	}
	pthread_mutex_lock(&g_app.lock);
	if (!g_app.geom_valid || g_app.geom_x != x || g_app.geom_y != y || g_app.geom_w != (uint32_t)w ||
	    g_app.geom_h != (uint32_t)h || g_app.geom_display_id != display_id) {
		g_app.geom_x = x;
		g_app.geom_y = y;
		g_app.geom_w = (uint32_t)w;
		g_app.geom_h = (uint32_t)h;
		g_app.geom_display_id = display_id;
		g_app.geom_valid = true;
		g_app.geom_generation++;
	}
	pthread_mutex_unlock(&g_app.lock);
}

JNIEXPORT void JNICALL
Java_com_displayxr_weave_1client_1vk_1android_MainActivity_nativeOnDestroy(JNIEnv *, jobject)
{
	pthread_mutex_lock(&g_app.lock);
	g_app.quit = true;
	pthread_mutex_unlock(&g_app.lock);
}

JNIEXPORT jboolean JNICALL
Java_com_displayxr_weave_1client_1vk_1android_MainActivity_nativeXrReady(JNIEnv *, jobject)
{
	return g_xr_ready ? JNI_TRUE : JNI_FALSE;
}

/*
 *
 * #1056 — the :gpu process's entry points.
 *
 * Same native code, same render thread; the ONLY differences are that the
 * Surface and the runtime socket both arrive from another process and that the
 * Context we hand the loader is a Service, not an Activity.
 *
 */

JNIEXPORT void JNICALL
Java_com_displayxr_weave_1client_1vk_1android_WeaveGpuService_nativeAdoptAndStart(JNIEnv *env,
                                                                                 jobject thiz,
                                                                                 jint fd,
                                                                                 jobject surface)
{
	g_activity = env->NewGlobalRef(thiz);

	// Dup: the Java ParcelFileDescriptor keeps owning what it gave us.
	g_adopt_fd = fd >= 0 ? dup(fd) : -1;
	g_adopt_ns = now_ns();
	LOGW("HANDOFF: :gpu pid %d adopting runtime socket fd %d (dup %d) — no bindService, no "
	     "org.freedesktop.monado.ipc.* in this process (#1056)",
	     (int)getpid(), (int)fd, g_adopt_fd);

	pthread_mutex_lock(&g_app.lock);
	if (g_app.window != nullptr) {
		ANativeWindow_release(g_app.window);
	}
	g_app.window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
	g_app.window_changed = true;
	pthread_mutex_unlock(&g_app.lock);

	pthread_t tid;
	pthread_create(&tid, nullptr, render_thread, nullptr);
	pthread_detach(tid);
}

JNIEXPORT void JNICALL
Java_com_displayxr_weave_1client_1vk_1android_WeaveGpuService_nativeSetWindowGeometry(
    JNIEnv *env, jobject thiz, jint x, jint y, jint w, jint h, jint display_id)
{
	Java_com_displayxr_weave_1client_1vk_1android_MainActivity_nativeSetWindowGeometry(env, thiz, x, y, w, h,
	                                                                                  display_id);
}

JNIEXPORT void JNICALL
Java_com_displayxr_weave_1client_1vk_1android_WeaveGpuService_nativeOnDestroy(JNIEnv *env, jobject thiz)
{
	Java_com_displayxr_weave_1client_1vk_1android_MainActivity_nativeOnDestroy(env, thiz);
}

} // extern "C"
