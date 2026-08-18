// Copyright 2018-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds Vulkan related functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup oxr_main
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_string_list.h"

#include "vk/vk_helpers.h"

#include "xrt/xrt_gfx_vk.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_two_call.h"


/*
 *
 * Helpers
 *
 */

#define GET_PROC(name) PFN_##name name = (PFN_##name)getProc(vkInstance, #name)

#define UUID_STR_SIZE (XRT_UUID_SIZE * 3 + 1)

static void
snprint_uuid(char *str, size_t size, const xrt_uuid_t *uuid)
{
	for (size_t i = 0, offset = 0; i < ARRAY_SIZE(uuid->data) && offset < size; i++, offset += 3) {
		snprintf(str + offset, size - offset, "%02x ", uuid->data[i]);
	}
}

static void
snprint_luid(char *str, size_t size, xrt_luid_t *luid)
{
	for (size_t i = 0, offset = 0; i < ARRAY_SIZE(luid->data) && offset < size; i++, offset += 3) {
		snprintf(str + offset, size - offset, "%02x ", luid->data[i]);
	}
}


/*
 *
 * Misc functions (to be organized).
 *
 */

XrResult
oxr_vk_get_instance_exts(struct oxr_logger *log,
                         struct oxr_system *sys,
                         uint32_t namesCapacityInput,
                         uint32_t *namesCountOutput,
                         char *namesString)
{
	size_t length = strlen(xrt_gfx_vk_instance_extensions) + 1;

	OXR_TWO_CALL_HELPER(log, namesCapacityInput, namesCountOutput, namesString, length,
	                    xrt_gfx_vk_instance_extensions, XR_SUCCESS);
}

XrResult
oxr_vk_get_device_exts(struct oxr_logger *log,
                       struct oxr_system *sys,
                       uint32_t namesCapacityInput,
                       uint32_t *namesCountOutput,
                       char *namesString)
{
	size_t length = strlen(xrt_gfx_vk_device_extensions) + 1;

	OXR_TWO_CALL_HELPER(log, namesCapacityInput, namesCountOutput, namesString, length,
	                    xrt_gfx_vk_device_extensions, XR_SUCCESS);
}

XrResult
oxr_vk_get_requirements(struct oxr_logger *log,
                        struct oxr_system *sys,
                        XrGraphicsRequirementsVulkanKHR *graphicsRequirements)
{
	struct xrt_api_requirements ver;

	xrt_gfx_vk_get_versions(&ver);
	graphicsRequirements->minApiVersionSupported = XR_MAKE_VERSION(ver.min_major, ver.min_minor, ver.min_patch);
	graphicsRequirements->maxApiVersionSupported = XR_MAKE_VERSION(ver.max_major, ver.max_minor, ver.max_patch);

	sys->gotten_requirements = true;

	return XR_SUCCESS;
}

DEBUG_GET_ONCE_LOG_OPTION(compositor_log, "XRT_COMPOSITOR_LOG", U_LOGGING_WARN)

//! @todo extension lists are duplicated as long strings in comp_vk_glue.c
static const char *required_vk_instance_extensions[] = {
    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,      //
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,     //
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,  //
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, //
#if defined(VK_KHR_win32_surface) && defined(XRT_OS_WINDOWS)
    // VK native compositor on Windows needs VkSurfaceKHR via VK_KHR_win32_surface
    VK_KHR_SURFACE_EXTENSION_NAME,       //
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME, //
#elif defined(VK_EXT_metal_surface) && defined(XRT_OS_MACOS)
    // VK native compositor on macOS needs VkSurfaceKHR via VK_EXT_metal_surface
    VK_KHR_SURFACE_EXTENSION_NAME,        //
    VK_EXT_METAL_SURFACE_EXTENSION_NAME,  //
#elif defined(VK_KHR_android_surface) && defined(XRT_OS_ANDROID)
    // VK native compositor on Android needs VkSurfaceKHR via VK_KHR_android_surface
    // (ANativeWindow -> VkSurfaceKHR in comp_vk_native_target). Without this the
    // instance-level vkCreateAndroidSurfaceKHR PFN is never loaded and the
    // compositor fails target creation at xrCreateSession.
    VK_KHR_SURFACE_EXTENSION_NAME,         //
    VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, //
#elif defined(VK_KHR_xcb_surface) && defined(XRT_OS_LINUX)
    // VK native compositor on Linux needs VkSurfaceKHR via VK_KHR_xcb_surface
    // (xcb_connection_t + xcb_window_t -> VkSurfaceKHR in comp_vk_native_target).
    // Without this the instance-level vkCreateXcbSurfaceKHR PFN is never loaded
    // and the compositor fails target creation at xrCreateSession.
    VK_KHR_SURFACE_EXTENSION_NAME,      //
    VK_KHR_XCB_SURFACE_EXTENSION_NAME,  //
#endif
};

static const char *optional_vk_instance_extensions[] = {
#if defined(VK_EXT_debug_utils)
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#endif
};

// The device extensions do vary by platform, but in a very regular way.
// This should match the list in comp_compositor, except it shouldn't include
// VK_KHR_SWAPCHAIN_EXTENSION_NAME
static const char *required_vk_device_extensions[] = {
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,      //
    VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,            //
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,           //
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,        //
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, //

// Platform version of "external_memory"
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
#if defined(VK_KHR_xcb_surface) && defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)
    // VK native compositor on desktop Linux presents on the app's VkDevice via a
    // swapchain over the XCB surface (comp_vk_native_target)
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#endif

#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER)
    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    VK_KHR_SWAPCHAIN_EXTENSION_NAME, // VK native compositor presents to the ANativeWindow surface via a swapchain

#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
    VK_EXT_EXTERNAL_MEMORY_METAL_EXTENSION_NAME,
    VK_EXT_METAL_OBJECTS_EXTENSION_NAME,
    VK_KHR_SWAPCHAIN_EXTENSION_NAME, // VK native compositor needs swapchain for presentation

#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_WIN32_HANDLE)
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    // VK native compositor on Windows needs swapchain for direct presentation
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#else
#error "Need port!"
#endif

// Platform version of "external_fence" and "external_semaphore"
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD) // Optional

#elif defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE)
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_WIN32_EXTENSION_NAME,

#else
#error "Need port!"
#endif
};

static const char *optional_device_extensions[] = {
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,

#elif defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE) // Not optional

#else
#error "Need port!"
#endif

#ifdef VK_KHR_image_format_list
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
#endif
#ifdef VK_KHR_timeline_semaphore
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
#else
    NULL, // avoid zero sized array with UB
#endif

#if defined(VK_KHR_present_id) && defined(VK_KHR_present_wait)
    // Late-weave presentation pacing (default-on): the VK native compositor
    // vsync-locks the weave by waiting on the previous present hitting glass
    // (vkWaitForPresentKHR). The features are chained below when supported —
    // enable2 apps get this with zero app-side code (enable1 apps must chain
    // the features themselves; INV-5.9).
    VK_KHR_PRESENT_ID_EXTENSION_NAME,
    VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
#endif

#if defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID) && defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
    // Desktop-background capture (runtime#757): dma-buf import on the app's
    // VkDevice so the display processor can consume PipeWire screencast
    // buffers. Optional here (checked against the physical device); the
    // enable1 string in comp_vk_glue.c lists them unconditionally.
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
#endif
};

static bool
vk_check_extension(VkExtensionProperties *props, uint32_t prop_count, const char *ext)
{
	for (uint32_t i = 0; i < prop_count; i++) {
		if (strcmp(props[i].extensionName, ext) == 0) {
			return true;
		}
	}

	return false;
}

static XrResult
vk_get_instance_ext_props(struct oxr_logger *log,
                          VkInstance instance,
                          PFN_vkGetInstanceProcAddr GetInstanceProcAddr,
                          VkExtensionProperties **out_props,
                          uint32_t *out_prop_count)
{
	PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties =
	    (PFN_vkEnumerateInstanceExtensionProperties)vkGetInstanceProcAddr(NULL,
	                                                                      "vkEnumerateInstanceExtensionProperties");

	if (!EnumerateInstanceExtensionProperties) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 "Failed to get EnumerateInstanceExtensionProperties fp");
	}

	uint32_t prop_count = 0;
	VkResult res = EnumerateInstanceExtensionProperties(NULL, &prop_count, NULL);
	if (res != VK_SUCCESS) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 "Failed to enumerate instance extension properties count (%d)", res);
	}


	VkExtensionProperties *props = U_TYPED_ARRAY_CALLOC(VkExtensionProperties, prop_count);

	res = EnumerateInstanceExtensionProperties(NULL, &prop_count, props);
	if (res != VK_SUCCESS) {
		free(props);
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 "Failed to enumerate instance extension properties (%d)", res);
	}

	*out_props = props;
	*out_prop_count = prop_count;

	return XR_SUCCESS;
}

XrResult
oxr_vk_create_vulkan_instance(struct oxr_logger *log,
                              struct oxr_system *sys,
                              const XrVulkanInstanceCreateInfoKHR *createInfo,
                              VkInstance *vulkanInstance,
                              VkResult *vulkanResult)
{

	PFN_vkGetInstanceProcAddr GetInstanceProcAddr = createInfo->pfnGetInstanceProcAddr;

	PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)GetInstanceProcAddr(NULL, "vkCreateInstance");
	if (!CreateInstance) {
		//! @todo: clarify in spec
		*vulkanResult = VK_ERROR_INITIALIZATION_FAILED;
		return XR_SUCCESS;
	}

	VkExtensionProperties *props = NULL;
	uint32_t prop_count = 0;
	XrResult res = vk_get_instance_ext_props(log, sys->vulkan_enable2_instance, createInfo->pfnGetInstanceProcAddr,
	                                         &props, &prop_count);
	if (res != XR_SUCCESS) {
		return res;
	}

	struct u_string_list *instance_ext_list = u_string_list_create_from_array(
	    required_vk_instance_extensions, ARRAY_SIZE(required_vk_instance_extensions));

#if defined(VK_EXT_debug_utils)
	bool debug_utils_enabled = false;
#endif

	for (uint32_t i = 0; i < ARRAY_SIZE(optional_vk_instance_extensions); i++) {

		if (optional_vk_instance_extensions[i] == NULL ||
		    !vk_check_extension(props, prop_count, optional_vk_instance_extensions[i])) {
			continue;
		}

		u_string_list_append_unique(instance_ext_list, optional_vk_instance_extensions[i]);

#if defined(VK_EXT_debug_utils)
		if (strcmp(optional_vk_instance_extensions[i], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
			debug_utils_enabled = true;
		}
#endif
	}

	for (uint32_t i = 0; i < createInfo->vulkanCreateInfo->enabledExtensionCount; i++) {
		u_string_list_append_unique(instance_ext_list,
		                            createInfo->vulkanCreateInfo->ppEnabledExtensionNames[i]);
	}

	VkInstanceCreateInfo modified_info = *createInfo->vulkanCreateInfo;
	modified_info.ppEnabledExtensionNames = u_string_list_get_data(instance_ext_list);
	modified_info.enabledExtensionCount = u_string_list_get_size(instance_ext_list);

	/*
	 * #902: inject VK_LAYER_DXR_queue_lock — per-queue submit serialization so
	 * the #868 repaint can share the app's VkQueue on GPUs whose graphics
	 * family exposes a single queue (Intel iGPUs, AMD). enable2 puts the
	 * runtime in-path here, so the app never knows. The compositor engages the
	 * shared-queue tier only after resolving the layer's marker entry point on
	 * the created device (handshake, not hope) — so a failed injection
	 * degrades, never breaks. DXR_VK_QUEUE_MODE=off|queue skips injection.
	 */
	const char **merged_layers = NULL;
	{
		const char *mode = getenv("DXR_VK_QUEUE_MODE");
		bool inject = !(mode != NULL && (strcmp(mode, "off") == 0 || strcmp(mode, "queue") == 0));
		uint32_t app_layer_count = createInfo->vulkanCreateInfo->enabledLayerCount;
		const char *const *app_layers = createInfo->vulkanCreateInfo->ppEnabledLayerNames;
		for (uint32_t i = 0; inject && i < app_layer_count; i++) {
			if (strcmp(app_layers[i], "VK_LAYER_DXR_queue_lock") == 0) {
				inject = false; // already requested
			}
		}
		if (inject) {
			merged_layers = U_TYPED_ARRAY_CALLOC(const char *, app_layer_count + 1);
			for (uint32_t i = 0; i < app_layer_count; i++) {
				merged_layers[i] = app_layers[i];
			}
			merged_layers[app_layer_count] = "VK_LAYER_DXR_queue_lock";
			modified_info.ppEnabledLayerNames = merged_layers;
			modified_info.enabledLayerCount = app_layer_count + 1;
		}
	}

	*vulkanResult = CreateInstance(&modified_info, createInfo->vulkanAllocator, vulkanInstance);

	/*
	 * #902: the layer is an optimization, never a break. If the FIRST attempt
	 * failed for ANY reason while our layer was injected — the loader could not
	 * find it, or it was found and loaded but its presence broke creation for
	 * this app (the Vulkan media-player demo got VK_ERROR_OUT_OF_HOST_MEMORY)
	 * — strip our layer and retry exactly once with the app's own layer list,
	 * letting the compositor fall back (no marker → repaint stays off on
	 * single-queue GPUs). The retry cannot double-inject: merged_layers is only
	 * non-NULL when the app did NOT request the layer itself, and the retry
	 * restores the app's own array verbatim.
	 */
	if (merged_layers != NULL && *vulkanResult != VK_SUCCESS) {
		VkResult first_result = *vulkanResult;
		if (first_result == VK_ERROR_LAYER_NOT_PRESENT) {
			oxr_warn(log,
			         "#902: VK_LAYER_DXR_queue_lock not found by the Vulkan loader — retrying without "
			         "it (shared-queue late-weave repaint unavailable; check the layer manifest / "
			         "VK_LAYER_PATH)");
		} else {
			oxr_warn(log,
			         "#902: vkCreateInstance failed with %s while VK_LAYER_DXR_queue_lock was injected "
			         "— retrying without it (the layer is an optimization, never a break; shared-queue "
			         "late-weave repaint unavailable)",
			         vk_result_string(first_result));
		}
		modified_info.ppEnabledLayerNames = createInfo->vulkanCreateInfo->ppEnabledLayerNames;
		modified_info.enabledLayerCount = createInfo->vulkanCreateInfo->enabledLayerCount;
		*vulkanInstance = VK_NULL_HANDLE;
		*vulkanResult = CreateInstance(&modified_info, createInfo->vulkanAllocator, vulkanInstance);
		if (*vulkanResult != VK_SUCCESS) {
			oxr_warn(log,
			         "#902: the retry without VK_LAYER_DXR_queue_lock also failed (%s; the first, "
			         "injected attempt returned %s) — reporting the retry's result to the app",
			         vk_result_string(*vulkanResult), vk_result_string(first_result));
		}
	}
	free(merged_layers);
	merged_layers = NULL;


	// Logging
	{
		struct oxr_sink_logger slog = {0};

		oxr_slog(&slog, "Creation of VkInstance:");
		oxr_slog(&slog, "\n\tresult: %s", vk_result_string(*vulkanResult));
		oxr_slog(&slog, "\n\tvulkanInstance: 0x%" PRIx64, (uint64_t)(intptr_t)*vulkanInstance);
		oxr_slog(&slog, "\n\textensions:");
		for (uint32_t i = 0; i < modified_info.enabledExtensionCount; i++) {
			oxr_slog(&slog, "\n\t\t%s", modified_info.ppEnabledExtensionNames[i]);
		}

		oxr_log_slog(log, &slog);
	}

#if defined(VK_EXT_debug_utils)
	if (*vulkanResult == VK_SUCCESS) {
		sys->vk.debug_utils_enabled = debug_utils_enabled;
	}
#endif

	u_string_list_destroy(&instance_ext_list);

	return XR_SUCCESS;
}

static XrResult
vk_get_device_ext_props(struct oxr_logger *log,
                        VkInstance instance,
                        PFN_vkGetInstanceProcAddr GetInstanceProcAddr,
                        VkPhysicalDevice physical_device,
                        VkExtensionProperties **out_props,
                        uint32_t *out_prop_count)
{
	PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties =
	    (PFN_vkEnumerateDeviceExtensionProperties)GetInstanceProcAddr(instance,
	                                                                  "vkEnumerateDeviceExtensionProperties");

	if (!EnumerateDeviceExtensionProperties) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 "Failed to get vkEnumerateDeviceExtensionProperties fp");
	}

	uint32_t prop_count = 0;
	VkResult res = EnumerateDeviceExtensionProperties(physical_device, NULL, &prop_count, NULL);
	if (res != VK_SUCCESS) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 "Failed to enumerate device extension properties count (%d)", res);
	}


	VkExtensionProperties *props = U_TYPED_ARRAY_CALLOC(VkExtensionProperties, prop_count);

	res = EnumerateDeviceExtensionProperties(physical_device, NULL, &prop_count, props);
	if (res != VK_SUCCESS) {
		free(props);
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "Failed to enumerate device extension properties (%d)",
		                 res);
	}

	*out_props = props;
	*out_prop_count = prop_count;

	return XR_SUCCESS;
}

static XrResult
vk_get_device_features(struct oxr_logger *log,
                       VkInstance instance,
                       PFN_vkGetInstanceProcAddr GetInstanceProcAddr,
                       VkPhysicalDevice physical_device,
                       VkPhysicalDeviceFeatures2KHR *physical_device_features)
{
	PFN_vkGetPhysicalDeviceFeatures2KHR GetPhysicalDeviceFeatures2 =
	    (PFN_vkGetPhysicalDeviceFeatures2KHR)GetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR");

	if (!GetPhysicalDeviceFeatures2) {
		oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "Failed to get vkGetPhysicalDeviceFeatures2 fp");
	}

	GetPhysicalDeviceFeatures2(    //
	    physical_device,           // physicalDevice
	    physical_device_features); // pFeatures

	return XR_SUCCESS;
}

static inline VkBaseInStructure const *
vk_find_struct_in_chain(const VkBaseInStructure *base, VkStructureType type)
{
	while (base != NULL) {
		if (base->sType == type) {
			return base;
		}
		base = base->pNext;
	}
	return NULL;
}

XrResult
oxr_vk_create_vulkan_device(struct oxr_logger *log,
                            struct oxr_system *sys,
                            const XrVulkanDeviceCreateInfoKHR *createInfo,
                            VkDevice *vulkanDevice,
                            VkResult *vulkanResult)
{
	XrResult res;

	PFN_vkGetInstanceProcAddr GetInstanceProcAddr = createInfo->pfnGetInstanceProcAddr;

	PFN_vkCreateDevice CreateDevice =
	    (PFN_vkCreateDevice)GetInstanceProcAddr(sys->vulkan_enable2_instance, "vkCreateDevice");
	if (!CreateDevice) {
		//! @todo: clarify in spec
		*vulkanResult = VK_ERROR_INITIALIZATION_FAILED;
		return XR_SUCCESS;
	}

	VkPhysicalDevice physical_device = createInfo->vulkanPhysicalDevice;

	struct u_string_list *device_extension_list =
	    u_string_list_create_from_array(required_vk_device_extensions, ARRAY_SIZE(required_vk_device_extensions));

	for (uint32_t i = 0; i < createInfo->vulkanCreateInfo->enabledExtensionCount; i++) {
		u_string_list_append_unique(device_extension_list,
		                            createInfo->vulkanCreateInfo->ppEnabledExtensionNames[i]);
	}



	VkExtensionProperties *props = NULL;
	uint32_t prop_count = 0;
	res = vk_get_device_ext_props(log, sys->vulkan_enable2_instance, createInfo->pfnGetInstanceProcAddr,
	                              physical_device, &props, &prop_count);
	if (res != XR_SUCCESS) {
		return res;
	}

#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
	bool external_fence_fd_enabled = false;
	bool external_semaphore_fd_enabled = false;
#endif
	bool image_format_list_enabled = false;
#if defined(VK_KHR_present_id) && defined(VK_KHR_present_wait)
	bool present_id_in_list = false;
	bool present_wait_in_list = false;
#endif

	for (uint32_t i = 0; i < ARRAY_SIZE(optional_device_extensions); i++) {
		// Empty list or a not supported extension.
		if (optional_device_extensions[i] == NULL ||
		    !vk_check_extension(props, prop_count, optional_device_extensions[i])) {
			continue;
		}

		u_string_list_append_unique(device_extension_list, optional_device_extensions[i]);

#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
		if (strcmp(optional_device_extensions[i], VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME) == 0) {
			external_fence_fd_enabled = true;
		}
		if (strcmp(optional_device_extensions[i], VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == 0) {
			external_semaphore_fd_enabled = true;
		}
#endif

		if (strcmp(optional_device_extensions[i], VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME) == 0) {
			image_format_list_enabled = true;
		}

#if defined(VK_KHR_present_id) && defined(VK_KHR_present_wait)
		if (strcmp(optional_device_extensions[i], VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0) {
			present_id_in_list = true;
		}
		if (strcmp(optional_device_extensions[i], VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0) {
			present_wait_in_list = true;
		}
#endif
	}

	free(props);


	VkPhysicalDeviceFeatures2KHR physical_device_features = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
	    .pNext = NULL,
	};

#ifdef VK_KHR_timeline_semaphore
	VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timeline_semaphore_info = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
	    .pNext = NULL,
	    .timelineSemaphore = VK_FALSE,
	};

	if (u_string_list_contains(device_extension_list, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
		physical_device_features.pNext = &timeline_semaphore_info;
	}
#endif

#if defined(VK_KHR_present_id) && defined(VK_KHR_present_wait)
	VkPhysicalDevicePresentIdFeaturesKHR present_id_info = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
	    .pNext = NULL,
	    .presentId = VK_FALSE,
	};
	VkPhysicalDevicePresentWaitFeaturesKHR present_wait_info = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
	    .pNext = NULL,
	    .presentWait = VK_FALSE,
	};

	if (present_id_in_list && present_wait_in_list) {
		present_id_info.pNext = physical_device_features.pNext;
		present_wait_info.pNext = &present_id_info;
		physical_device_features.pNext = &present_wait_info;
	}
#endif

	res = vk_get_device_features(log, sys->vulkan_enable2_instance, createInfo->pfnGetInstanceProcAddr,
	                             physical_device, &physical_device_features);
	if (res != XR_SUCCESS) {
		return res;
	}


	VkDeviceCreateInfo modified_info = *createInfo->vulkanCreateInfo;
	modified_info.ppEnabledExtensionNames = u_string_list_get_data(device_extension_list);
	modified_info.enabledExtensionCount = u_string_list_get_size(device_extension_list);

	/*
	 * #868: ask for ONE extra queue on the app's graphics family, for the
	 * runtime's own use.
	 *
	 * A VkQueue is externally synchronised — the APPLICATION must serialise
	 * access to it — so the runtime must never submit from a repaint thread on
	 * a queue the app also submits to. Measured consequence of doing so:
	 * VK_ERROR_DEVICE_LOST, with validation reporting
	 * "UNASSIGNED-Threading-MultipleThreads-Write ... VkQueue is simultaneously
	 * used in current thread A and thread B".
	 *
	 * We only get this lever under vulkan_enable2, where the runtime creates
	 * the device and therefore owns VkDeviceCreateInfo. It is already rewritten
	 * above for extensions; one more queue is the same kind of edit. Under
	 * vulkan_enable1 the app creates the device and there is nothing to do —
	 * the repaint simply stays off there.
	 *
	 * Strictly best-effort: if the family has no spare queue we leave the
	 * request untouched rather than fail device creation over a nicety.
	 */
	VkDeviceQueueCreateInfo *mod_queues = NULL;
	float *mod_prios = NULL;
	sys->vulkan_runtime_queue_family = -1;
	sys->vulkan_runtime_queue_index = -1;
	{
		PFN_vkGetPhysicalDeviceQueueFamilyProperties GetQFP =
		    (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GetInstanceProcAddr(
		        sys->vulkan_enable2_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
		const uint32_t qci_count = createInfo->vulkanCreateInfo->queueCreateInfoCount;
		if (GetQFP != NULL && qci_count > 0) {
			uint32_t fam_count = 0;
			GetQFP(physical_device, &fam_count, NULL);
			VkQueueFamilyProperties *fams =
			    fam_count > 0 ? U_TYPED_ARRAY_CALLOC(VkQueueFamilyProperties, fam_count) : NULL;
			if (fams != NULL) {
				GetQFP(physical_device, &fam_count, fams);

				mod_queues = U_TYPED_ARRAY_CALLOC(VkDeviceQueueCreateInfo, qci_count);
				for (uint32_t i = 0; i < qci_count; i++) {
					mod_queues[i] = createInfo->vulkanCreateInfo->pQueueCreateInfos[i];
				}

				for (uint32_t i = 0; i < qci_count; i++) {
					const uint32_t fam = mod_queues[i].queueFamilyIndex;
					if (fam >= fam_count ||
					    (fams[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
						continue;
					}
					const uint32_t want = mod_queues[i].queueCount + 1;
					if (want > fams[fam].queueCount) {
						oxr_log(log,
						        "#868: graphics family %u is saturated (%u/%u) — no "
						        "runtime-owned queue, VK repaint stays disabled",
						        fam, mod_queues[i].queueCount, fams[fam].queueCount);
						break;
					}
					mod_prios = U_TYPED_ARRAY_CALLOC(float, want);
					for (uint32_t q = 0; q < mod_queues[i].queueCount; q++) {
						mod_prios[q] = mod_queues[i].pQueuePriorities != NULL
						                   ? mod_queues[i].pQueuePriorities[q]
						                   : 1.0f;
					}
					// Lower priority than the app's: the repaint must never
					// out-schedule the frame it stands in for.
					mod_prios[want - 1] = 0.5f;

					sys->vulkan_runtime_queue_family = (int32_t)fam;
					sys->vulkan_runtime_queue_index = (int32_t)mod_queues[i].queueCount;
					mod_queues[i].queueCount = want;
					mod_queues[i].pQueuePriorities = mod_prios;

					modified_info.pQueueCreateInfos = mod_queues;
					oxr_log(log,
					        "#868: requesting a runtime-owned queue (family %u index %u) so "
					        "the weave can run off the app's frame thread",
					        fam, (uint32_t)sys->vulkan_runtime_queue_index);
					break;
				}
				free(fams);
			}
		}
	}

#ifdef VK_KHR_timeline_semaphore
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
	    .pNext = NULL,
	    .timelineSemaphore = timeline_semaphore_info.timelineSemaphore,
	};

	if (timeline_semaphore_info.timelineSemaphore) {
		// Check if the user has already put the struct into the chain
		const VkBaseInStructure *existing = vk_find_struct_in_chain(
		    (VkBaseInStructure *)&modified_info, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
		if (existing != NULL) {
			VkPhysicalDeviceTimelineSemaphoreFeatures *existing_timeline_semaphore_info =
			    (VkPhysicalDeviceTimelineSemaphoreFeatures *)existing;
			if (!existing_timeline_semaphore_info->timelineSemaphore) {
				oxr_warn(log, "Timeline semaphores are explicitly disabled by application");
				timeline_semaphore_info.timelineSemaphore = VK_FALSE;
			}
			// Timeline semaphores are already enabled so we don't have to do anything
		} else {
			// Insert struct at the front of the chain
			// Have to cast away const.
			timeline_semaphore.pNext = (void *)modified_info.pNext;
			modified_info.pNext = &timeline_semaphore;
		}
	}
#endif

#if defined(VK_KHR_present_id) && defined(VK_KHR_present_wait)
	// Enable present_id + present_wait when the device supports them, so the
	// VK native compositor's late-weave pacing (default-on) works with zero
	// app-side code on the enable2 path. Skip either struct if the app
	// already chained its own (its choice wins, matching the timeline-
	// semaphore precedent above).
	VkPhysicalDevicePresentIdFeaturesKHR present_id_enable = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
	    .pNext = NULL,
	    .presentId = present_id_info.presentId,
	};
	VkPhysicalDevicePresentWaitFeaturesKHR present_wait_enable = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
	    .pNext = NULL,
	    .presentWait = present_wait_info.presentWait,
	};

	if (present_id_info.presentId && present_wait_info.presentWait) {
		if (vk_find_struct_in_chain((VkBaseInStructure *)&modified_info,
		                            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR) == NULL) {
			present_id_enable.pNext = (void *)modified_info.pNext;
			modified_info.pNext = &present_id_enable;
		}
		if (vk_find_struct_in_chain((VkBaseInStructure *)&modified_info,
		                            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR) == NULL) {
			present_wait_enable.pNext = (void *)modified_info.pNext;
			modified_info.pNext = &present_wait_enable;
		}
	}
#endif

	*vulkanResult = CreateDevice(physical_device, &modified_info, createInfo->vulkanAllocator, vulkanDevice);

	// #868: the rewritten queue arrays are only read by CreateDevice above.
	// If creation failed there is no runtime-owned queue to speak of.
	if (*vulkanResult != VK_SUCCESS) {
		sys->vulkan_runtime_queue_family = -1;
		sys->vulkan_runtime_queue_index = -1;
	}
	free(mod_queues);
	free(mod_prios);

	// Logging
	{
		struct oxr_sink_logger slog = {0};

		oxr_slog(&slog, "Creation of VkDevice:");
		oxr_slog(&slog, "\n\tresult: %s", vk_result_string(*vulkanResult));
		oxr_slog(&slog, "\n\tvulkanDevice: 0x%" PRIx64, (uint64_t)(intptr_t)*vulkanDevice);
		oxr_slog(&slog, "\n\tvulkanInstance: 0x%" PRIx64, (uint64_t)(intptr_t)sys->vulkan_enable2_instance);
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
		oxr_slog(&slog, "\n\texternal_fence_fd: %s", external_fence_fd_enabled ? "true" : "false");
		oxr_slog(&slog, "\n\texternal_semaphore_fd: %s", external_semaphore_fd_enabled ? "true" : "false");
#endif
#ifdef VK_KHR_timeline_semaphore
		oxr_slog(&slog, "\n\ttimelineSemaphore: %s",
		         timeline_semaphore_info.timelineSemaphore ? "true" : "false");
#endif
#if defined(VK_KHR_present_id) && defined(VK_KHR_present_wait)
		oxr_slog(&slog, "\n\tpresentId/presentWait: %s (late-weave pacing)",
		         (present_id_info.presentId && present_wait_info.presentWait) ? "true" : "false");
#endif
		oxr_slog(&slog, "\n\textensions:");
		for (uint32_t i = 0; i < modified_info.enabledExtensionCount; i++) {
			oxr_slog(&slog, "\n\t\t%s", modified_info.ppEnabledExtensionNames[i]);
		}

		oxr_log_slog(log, &slog);
	}

#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
	if (*vulkanResult == VK_SUCCESS) {
		sys->vk.external_fence_fd_enabled = external_fence_fd_enabled;
		sys->vk.external_semaphore_fd_enabled = external_semaphore_fd_enabled;
	}
#endif

#ifdef VK_KHR_timeline_semaphore
	// Have timeline semaphores added and as such enabled.
	if (*vulkanResult == VK_SUCCESS) {
		sys->vk.timeline_semaphore_enabled = timeline_semaphore_info.timelineSemaphore;
		U_LOG_D("timeline semaphores enabled: %d", timeline_semaphore_info.timelineSemaphore);
	}
#endif

#ifdef VK_KHR_image_format_list
	if (*vulkanResult == VK_SUCCESS) {
		sys->vk.image_format_list_enabled = image_format_list_enabled;
	}
#endif

	u_string_list_destroy(&device_extension_list);

	return XR_SUCCESS;
}


XrResult
oxr_vk_get_physical_device(struct oxr_logger *log,
                           struct oxr_instance *inst,
                           struct oxr_system *sys,
                           VkInstance vkInstance,
                           PFN_vkGetInstanceProcAddr getProc,
                           VkPhysicalDevice *vkPhysicalDevice)
{
	GET_PROC(vkEnumeratePhysicalDevices);
	GET_PROC(vkGetPhysicalDeviceProperties2KHR);
	VkResult vk_ret;
	uint32_t count;

	if (sys->xsysc == NULL) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, " sys->xsysc == NULL");
	}

	vk_ret = vkEnumeratePhysicalDevices(vkInstance, &count, NULL);
	if (vk_ret != VK_SUCCESS) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "Call to vkEnumeratePhysicalDevices returned %u",
		                 vk_ret);
	}
	if (count == 0) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 "Call to vkEnumeratePhysicalDevices returned zero VkPhysicalDevices");
	}

	VkPhysicalDevice *phys = U_TYPED_ARRAY_CALLOC(VkPhysicalDevice, count);
	vk_ret = vkEnumeratePhysicalDevices(vkInstance, &count, phys);
	if (vk_ret != VK_SUCCESS) {
		free(phys);
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "Call to vkEnumeratePhysicalDevices returned %u",
		                 vk_ret);
	}
	if (count == 0) {
		free(phys);
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 "Call to vkEnumeratePhysicalDevices returned zero VkPhysicalDevices");
	}

	char suggested_uuid_str[UUID_STR_SIZE] = {0};
	snprint_uuid(suggested_uuid_str, ARRAY_SIZE(suggested_uuid_str), &sys->xsysc->info.client_vk_deviceUUID);

	enum u_logging_level log_level = debug_get_log_option_compositor_log();
	int gpu_index = -1;
	for (uint32_t i = 0; i < count; i++) {
		VkPhysicalDeviceIDProperties pdidp = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
		};

		VkPhysicalDeviceProperties2 pdp2 = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		    .pNext = &pdidp,
		};

		vkGetPhysicalDeviceProperties2KHR(phys[i], &pdp2);

		// These should always be true
		static_assert(VK_UUID_SIZE == XRT_UUID_SIZE, "uuid sizes mismatch");
		static_assert(ARRAY_SIZE(pdidp.deviceUUID) == XRT_UUID_SIZE, "array size mismatch");

		char buffer[UUID_STR_SIZE] = {0};
		if (log_level <= U_LOGGING_DEBUG) {
			snprint_uuid(buffer, ARRAY_SIZE(buffer), (xrt_uuid_t *)pdidp.deviceUUID);
			oxr_log(log, "GPU: #%d, uuid: %s", i, buffer);
			if (pdidp.deviceLUIDValid == VK_TRUE) {
				snprint_luid(buffer, ARRAY_SIZE(buffer), (xrt_luid_t *)pdidp.deviceLUID);
				oxr_log(log, "  LUID: %s", buffer);
			}
		}

		if (memcmp(pdidp.deviceUUID, sys->xsysc->info.client_vk_deviceUUID.data, XRT_UUID_SIZE) == 0) {
			gpu_index = i;
			if (log_level <= U_LOGGING_DEBUG) {
				oxr_log(log, "Using GPU #%d with uuid %s suggested by runtime", gpu_index, buffer);
			}
			break;
		}
	}

	if (gpu_index == -1) {
		oxr_warn(log, "Did not find runtime suggested GPU, fall back to GPU 0\n\tuuid: %s", suggested_uuid_str);
		gpu_index = 0;
	}

	*vkPhysicalDevice = phys[gpu_index];

	// vulkan_enable2 needs the physical device in xrCreateVulkanDeviceKHR
	if (inst->extensions.KHR_vulkan_enable2) {
		sys->vulkan_enable2_instance = vkInstance;
	}
	sys->suggested_vulkan_physical_device = *vkPhysicalDevice;
	if (log_level <= U_LOGGING_DEBUG) {
		oxr_log(log, "Suggesting vulkan physical device %p", (void *)*vkPhysicalDevice);
	}

	free(phys);

	return XR_SUCCESS;
}
