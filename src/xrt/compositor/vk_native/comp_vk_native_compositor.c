// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native Vulkan compositor implementation.
 * @author David Fattal
 * @ingroup comp_vk_native
 *
 * Follows the D3D11 native compositor pattern: direct Vulkan rendering +
 * display processor, no multi-compositor involvement. Uses the app's
 * VkDevice directly via vk_bundle (Monado's Vulkan wrapper).
 */

#include "comp_vk_native_compositor.h"
#include "comp_vk_native_swapchain.h"
#include "comp_vk_native_target.h"
#include "comp_vk_native_renderer.h"

#include "util/comp_layer_accum.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_vulkan_includes.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_display_processor.h"

#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "os/os_time.h"

#include "math/m_api.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "d3d11/comp_d3d11_window.h"
#endif

#ifdef XRT_OS_MACOS
#include "vk_native/comp_vk_native_window_macos.h"
#include <IOSurface/IOSurface.h>
#endif

#include <string.h>
#include <math.h>

/*!
 * Minimal settings struct for Vulkan compositor.
 */
struct comp_vk_settings
{
	struct
	{
		uint32_t width;
		uint32_t height;
	} preferred;

	int64_t nominal_frame_interval_ns;
};

/*!
 * The Vulkan native compositor structure.
 */
struct comp_vk_native_compositor
{
	//! Base type - must be first!
	struct xrt_compositor_native base;

	//! The device we are rendering for.
	struct xrt_device *xdev;

	//! Vulkan bundle (initialized from app's VkDevice via vk_init_from_given).
	struct vk_bundle vk;

	//! Queue family index.
	uint32_t queue_family_index;

	//! Output target (VkSwapchainKHR).
	struct comp_vk_native_target *target;

	//! Renderer for layer compositing.
	struct comp_vk_native_renderer *renderer;

	//! Accumulated layers for the current frame.
	struct comp_layer_accum layer_accum;

	//! Compositor settings.
	struct comp_vk_settings settings;

#ifdef XRT_OS_WINDOWS
	//! Window handle (either from app or self-created).
	void *hwnd;

	//! Self-created window (NULL if app provided window).
	struct comp_d3d11_window *own_window;

	//! True if we created the window ourselves.
	bool owns_window;
#endif

#ifdef XRT_OS_MACOS
	//! macOS window helper (self-owned or external view).
	struct comp_vk_native_window_macos *macos_window;

	//! True if we created the window ourselves.
	bool owns_window;
#endif

	//! Shared texture VkImage (imported from HANDLE).
	VkImage shared_image;

	//! Shared texture memory.
	VkDeviceMemory shared_memory;

	//! Shared texture image view.
	VkImageView shared_view;

	//! True if shared texture mode is active.
	bool has_shared_texture;

	//! Shared texture HANDLE (Win32).
	void *shared_texture_handle;

	//! Command pool for display processor factory.
	VkCommandPool cmd_pool;

	//! Simple render pass for display processor framebuffer creation.
	VkRenderPass dp_render_pass;

	//! Generic Vulkan display processor (vendor-agnostic weaving).
	struct xrt_display_processor *display_processor;

	//! System devices (for qwerty driver).
	struct xrt_system_devices *xsysd;

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;
};

/*
 *
 * Helper functions
 *
 */

#ifdef XRT_OS_MACOS
/*!
 * Import an IOSurface as a VkImage for shared texture rendering.
 *
 * Uses VK_EXT_metal_objects to import the IOSurface, export the MTLTexture,
 * and allocate memory via VK_EXT_external_memory_metal.
 */
static bool
import_shared_iosurface(struct comp_vk_native_compositor *c, void *iosurface_handle)
{
	struct vk_bundle *vk = &c->vk;

#if defined(VK_EXT_metal_objects) && defined(VK_EXT_external_memory_metal)
	if (!vk->has_EXT_metal_objects || !vk->has_EXT_external_memory_metal) {
		U_LOG_E("VK_EXT_metal_objects or VK_EXT_external_memory_metal not available");
		return false;
	}

	IOSurfaceRef surface = (IOSurfaceRef)iosurface_handle;
	uint32_t width = (uint32_t)IOSurfaceGetWidth(surface);
	uint32_t height = (uint32_t)IOSurfaceGetHeight(surface);

	if (width == 0 || height == 0) {
		U_LOG_E("IOSurface has zero dimensions");
		return false;
	}

	U_LOG_W("Importing IOSurface %ux%u as VkImage for shared texture", width, height);

	// Chain: VkImageCreateInfo -> VkImportMetalIOSurfaceInfoEXT -> VkExportMetalObjectCreateInfoEXT
	VkExportMetalObjectCreateInfoEXT export_metal_tex_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	    .pNext = NULL,
	    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
	};
	VkImportMetalIOSurfaceInfoEXT import_iosurface_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT,
	    .pNext = &export_metal_tex_info,
	    .ioSurface = surface,
	};

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &import_iosurface_info,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,  // IOSurface is BGRA8
	    .extent = {width, height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &c->shared_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkCreateImage for shared IOSurface failed: %d", ret);
		return false;
	}

	// Export MTLTexture from the VkImage (MoltenVK created it from the IOSurface)
	VkExportMetalTextureInfoEXT export_tex_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
	    .image = c->shared_image,
	    .imageView = VK_NULL_HANDLE,
	    .bufferView = VK_NULL_HANDLE,
	    .plane = VK_IMAGE_ASPECT_COLOR_BIT,
	    .mtlTexture = NULL,
	};
	VkExportMetalObjectsInfoEXT export_objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &export_tex_info,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &export_objects_info);

	void *metal_texture_handle = (void *)export_tex_info.mtlTexture;
	if (metal_texture_handle == NULL) {
		U_LOG_E("Failed to export MTLTexture from shared VkImage");
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	// Get memory requirements
	VkMemoryRequirements requirements = {0};
	vk->vkGetImageMemoryRequirements(vk->device, c->shared_image, &requirements);

	// Get valid memory type bits from the MTLTexture
	VkMemoryMetalHandlePropertiesEXT metal_props = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_METAL_HANDLE_PROPERTIES_EXT,
	};
	ret = vk->vkGetMemoryMetalHandlePropertiesEXT(
	    vk->device,
	    VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
	    metal_texture_handle,
	    &metal_props);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkGetMemoryMetalHandlePropertiesEXT failed: %d", ret);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	requirements.memoryTypeBits = metal_props.memoryTypeBits;

	// Import memory using the MTLTexture handle with dedicated allocation
	// (matches vk_helpers.c pattern for Metal texture import)
	VkImportMemoryMetalHandleInfoEXT import_memory_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT,
	    .pNext = NULL,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
	    .handle = metal_texture_handle,
	};

	VkMemoryDedicatedAllocateInfoKHR dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import_memory_info,
	    .image = c->shared_image,
	    .buffer = VK_NULL_HANDLE,
	};

	// Find a valid memory type
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);

	uint32_t memory_type_index = UINT32_MAX;
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) != 0) {
			memory_type_index = i;
			break;
		}
	}
	if (memory_type_index == UINT32_MAX) {
		U_LOG_E("No valid memory type for shared IOSurface");
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = memory_type_index,
	};

	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &c->shared_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkAllocateMemory for shared IOSurface failed: %d", ret);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	ret = vk->vkBindImageMemory(vk->device, c->shared_image, c->shared_memory, 0);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkBindImageMemory for shared IOSurface failed: %d", ret);
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		c->shared_memory = VK_NULL_HANDLE;
		return false;
	}

	// Create image view
	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = c->shared_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
	    .subresourceRange = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel = 0,
	        .levelCount = 1,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	};

	ret = vk->vkCreateImageView(vk->device, &view_ci, NULL, &c->shared_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkCreateImageView for shared IOSurface failed: %d", ret);
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		c->shared_memory = VK_NULL_HANDLE;
		return false;
	}

	c->has_shared_texture = true;
	c->settings.preferred.width = width;
	c->settings.preferred.height = height;

	U_LOG_W("Shared IOSurface imported: %ux%u, VkImage=%p, VkImageView=%p",
	        width, height, (void *)(uintptr_t)c->shared_image,
	        (void *)(uintptr_t)c->shared_view);
	return true;
#else
	U_LOG_E("VK_EXT_metal_objects not available at compile time");
	return false;
#endif
}
#endif // XRT_OS_MACOS

#ifdef XRT_OS_WINDOWS
/*!
 * Import a D3D11 shared texture HANDLE as a VkImage for shared texture rendering.
 *
 * Uses VK_KHR_external_memory_win32 to import the D3D11 MISC_SHARED handle
 * as VkDeviceMemory backed by the same GPU resource.
 */
static bool
import_shared_d3d11_texture(struct comp_vk_native_compositor *c, void *shared_handle)
{
	struct vk_bundle *vk = &c->vk;

	if (shared_handle == NULL) {
		U_LOG_E("shared_handle is NULL");
		return false;
	}

	// We need the display pixel dimensions from the device
	uint32_t width = c->settings.preferred.width;
	uint32_t height = c->settings.preferred.height;
	if (width == 0 || height == 0) {
		width = 1920;
		height = 1080;
	}

	U_LOG_W("Importing D3D11 shared texture as VkImage: %ux%u, handle=%p",
	        width, height, shared_handle);

	// Create VkImage with external memory info for D3D11 KMT handle
	VkExternalMemoryImageCreateInfo external_ci = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .pNext = NULL,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
	};

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &external_ci,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .extent = {width, height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &c->shared_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkCreateImage for shared D3D11 texture failed: %d", ret);
		return false;
	}

	// Get memory requirements
	VkMemoryRequirements requirements = {0};
	vk->vkGetImageMemoryRequirements(vk->device, c->shared_image, &requirements);

	// Import D3D11 KMT handle as Vulkan memory
	VkImportMemoryWin32HandleInfoKHR import_memory_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
	    .pNext = NULL,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
	    .handle = (HANDLE)shared_handle,
	};

	VkMemoryDedicatedAllocateInfoKHR dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import_memory_info,
	    .image = c->shared_image,
	    .buffer = VK_NULL_HANDLE,
	};

	// Find a valid memory type (device-local preferred)
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);

	uint32_t memory_type_index = UINT32_MAX;
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) != 0) {
			memory_type_index = i;
			break;
		}
	}
	if (memory_type_index == UINT32_MAX) {
		U_LOG_E("No valid memory type for shared D3D11 texture");
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = memory_type_index,
	};

	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &c->shared_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkAllocateMemory for shared D3D11 texture failed: %d", ret);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		return false;
	}

	ret = vk->vkBindImageMemory(vk->device, c->shared_image, c->shared_memory, 0);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkBindImageMemory for shared D3D11 texture failed: %d", ret);
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		c->shared_memory = VK_NULL_HANDLE;
		return false;
	}

	// Create image view
	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = c->shared_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
	    .subresourceRange = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel = 0,
	        .levelCount = 1,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	};

	ret = vk->vkCreateImageView(vk->device, &view_ci, NULL, &c->shared_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("vkCreateImageView for shared D3D11 texture failed: %d", ret);
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
		c->shared_image = VK_NULL_HANDLE;
		c->shared_memory = VK_NULL_HANDLE;
		return false;
	}

	c->has_shared_texture = true;

	U_LOG_W("Shared D3D11 texture imported: %ux%u, VkImage=%p, VkImageView=%p",
	        width, height, (void *)(uintptr_t)c->shared_image,
	        (void *)(uintptr_t)c->shared_view);
	return true;
}
#endif // XRT_OS_WINDOWS

static inline struct comp_vk_native_compositor *
vk_comp(struct xrt_compositor *xc)
{
	return (struct comp_vk_native_compositor *)xc;
}

/*
 *
 * xrt_compositor member functions
 *
 */

static xrt_result_t
vk_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                               const struct xrt_swapchain_create_info *info,
                                               struct xrt_swapchain_create_properties *xsccp)
{
	xsccp->image_count = 3;
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_create_swapchain(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_swapchain **out_xsc)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	return comp_vk_native_swapchain_create(c, info, out_xsc);
}

static xrt_result_t
vk_compositor_import_swapchain(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_image_native *native_images,
                                uint32_t image_count,
                                struct xrt_swapchain **out_xsc)
{
	return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}

static xrt_result_t
vk_compositor_import_fence(struct xrt_compositor *xc,
                            xrt_graphics_sync_handle_t handle,
                            struct xrt_compositor_fence **out_xcf)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
vk_compositor_create_semaphore(struct xrt_compositor *xc,
                                xrt_graphics_sync_handle_t *out_handle,
                                struct xrt_compositor_semaphore **out_xcsem)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
vk_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	U_LOG_I("VK native compositor session begin - target=%p, renderer=%p",
	        (void *)c->target, (void *)c->renderer);

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_end_session(struct xrt_compositor *xc)
{
	U_LOG_I("VK native compositor session end");
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_predict_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             int64_t *out_wake_time_ns,
                             int64_t *out_predicted_gpu_time_ns,
                             int64_t *out_predicted_display_time_ns,
                             int64_t *out_predicted_display_period_ns)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	int64_t period_ns = (int64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate);

	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = period_ns;

	c->last_display_time_ns = (uint64_t)*out_predicted_display_time_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_wait_frame(struct xrt_compositor *xc,
                          int64_t *out_frame_id,
                          int64_t *out_predicted_display_time_ns,
                          int64_t *out_predicted_display_period_ns)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

#ifdef XRT_OS_WINDOWS
	if (c->owns_window && c->own_window != NULL &&
	    !comp_d3d11_window_is_valid(c->own_window)) {
		U_LOG_I("Window closed - signaling session exit");
		return XRT_ERROR_IPC_FAILURE;
	}
#endif

#ifdef XRT_OS_MACOS
	if (c->owns_window && c->macos_window != NULL &&
	    !comp_vk_native_window_macos_is_valid(c->macos_window)) {
		U_LOG_I("Window closed - signaling session exit");
		return XRT_ERROR_IPC_FAILURE;
	}
#endif

	int64_t period_ns = (int64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;

	c->last_display_time_ns = (uint64_t)*out_predicted_display_time_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_mark_frame(struct xrt_compositor *xc,
                          int64_t frame_id,
                          enum xrt_compositor_frame_point point,
                          int64_t when_ns)
{
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT rect;
		if (GetClientRect((HWND)c->hwnd, &rect)) {
			uint32_t new_width = (uint32_t)(rect.right - rect.left);
			uint32_t new_height = (uint32_t)(rect.bottom - rect.top);

			if (new_width > 0 && new_height > 0 &&
			    (new_width != c->settings.preferred.width ||
			     new_height != c->settings.preferred.height)) {

				U_LOG_I("Window resized: %ux%u -> %ux%u",
				        c->settings.preferred.width, c->settings.preferred.height,
				        new_width, new_height);

				if (c->target != NULL) {
					comp_vk_native_target_resize(c->target, new_width, new_height);
				}
				c->settings.preferred.width = new_width;
				c->settings.preferred.height = new_height;

				uint32_t new_vw = new_width / 2;
				uint32_t new_vh = new_height;
				comp_vk_native_renderer_resize(c->renderer, new_vw, new_vh, new_height);
			}
		}
	}
#endif

#ifdef XRT_OS_MACOS
	if (c->macos_window != NULL) {
		uint32_t new_width = 0, new_height = 0;
		comp_vk_native_window_macos_get_dimensions(c->macos_window, &new_width, &new_height);

		if (new_width > 0 && new_height > 0 &&
		    (new_width != c->settings.preferred.width ||
		     new_height != c->settings.preferred.height)) {

			U_LOG_I("Window resized: %ux%u -> %ux%u",
			        c->settings.preferred.width, c->settings.preferred.height,
			        new_width, new_height);

			if (c->target != NULL) {
				comp_vk_native_target_resize(c->target, new_width, new_height);
			}
			c->settings.preferred.width = new_width;
			c->settings.preferred.height = new_height;

			uint32_t new_vw = new_width / 2;
			uint32_t new_vh = new_height;
			comp_vk_native_renderer_resize(c->renderer, new_vw, new_vh, new_height);
		}
	}
#endif

	c->layer_accum.layer_count = 0;
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	c->layer_accum.layer_count = 0;
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_begin(&c->layer_accum, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_projection(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_projection(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                      struct xrt_device *xdev,
                                      struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                      struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                      const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_quad(struct xrt_compositor *xc,
                          struct xrt_device *xdev,
                          struct xrt_swapchain *xsc,
                          const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_quad(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_cube(struct xrt_compositor *xc,
                          struct xrt_device *xdev,
                          struct xrt_swapchain *xsc,
                          const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_cube(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_cylinder(struct xrt_compositor *xc,
                              struct xrt_device *xdev,
                              struct xrt_swapchain *xsc,
                              const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_cylinder(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_equirect1(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc,
                               const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_equirect1(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_equirect2(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc,
                               const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_equirect2(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_passthrough(struct xrt_compositor *xc,
                                 struct xrt_device *xdev,
                                 const struct xrt_layer_data *data)
{
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_window_space(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_window_space(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;

	// Get predicted eye positions
	struct xrt_vec3 left_eye = {-0.032f, 0.0f, 0.6f};
	struct xrt_vec3 right_eye = {0.032f, 0.0f, 0.6f};

	if (c->display_processor != NULL) {
		struct xrt_eye_pair eyes;
		if (xrt_display_processor_get_predicted_eye_positions(c->display_processor, &eyes) &&
		    eyes.valid) {
			left_eye.x = eyes.left.x;
			left_eye.y = eyes.left.y;
			left_eye.z = eyes.left.z;
			right_eye.x = eyes.right.x;
			right_eye.y = eyes.right.y;
			right_eye.z = eyes.right.z;
		}
	}

	// Detect mono submission
	bool is_mono = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_PROJECTION ||
		    c->layer_accum.layers[i].data.type == XRT_LAYER_PROJECTION_DEPTH) {
			is_mono = (c->layer_accum.layers[i].data.view_count == 1);
			break;
		}
	}

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(c->xsysd->xdevs, c->xsysd->xdev_count, &force_2d);
		if (toggled) {
			comp_vk_native_compositor_request_display_mode(&c->base.base, !force_2d);
		}
		if (force_2d) {
			is_mono = true;
		}
	}
#endif

	// Get target dimensions
	uint32_t tgt_width = c->settings.preferred.width;
	uint32_t tgt_height = c->settings.preferred.height;
	if (c->target != NULL) {
		comp_vk_native_target_get_dimensions(c->target, &tgt_width, &tgt_height);
	}

	// Render layers to stereo texture
	xrt_result_t xret = comp_vk_native_renderer_draw(
	    c->renderer, &c->layer_accum, &left_eye, &right_eye, tgt_width, tgt_height, is_mono);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to render layers");
		return xret;
	}

	// Shared texture output path — render to IOSurface-backed VkImage
	if (c->has_shared_texture && c->shared_image != VK_NULL_HANDLE) {
		VkCommandPool cmd_pool = (VkCommandPool)(uintptr_t)
		    comp_vk_native_renderer_get_cmd_pool(c->renderer);

		VkCommandBufferAllocateInfo cmd_alloc = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		    .commandPool = cmd_pool,
		    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		    .commandBufferCount = 1,
		};

		VkCommandBuffer cmd;
		VkResult res = vk->vkAllocateCommandBuffers(vk->device, &cmd_alloc, &cmd);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to allocate command buffer for shared texture");
			return XRT_ERROR_VULKAN;
		}

		VkCommandBufferBeginInfo begin_info = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vk->vkBeginCommandBuffer(cmd, &begin_info);

		bool weaving_done = false;

		// Display processor weaving path
		if (!is_mono && c->display_processor != NULL) {
			uint64_t left_view, right_view;
			comp_vk_native_renderer_get_stereo_views(c->renderer, &left_view, &right_view);

			uint32_t view_width, view_height;
			comp_vk_native_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);

			int32_t view_format = comp_vk_native_renderer_get_format(c->renderer);

			VkRenderPass dp_render_pass = xrt_display_processor_get_render_pass(c->display_processor);
			VkFramebuffer shared_fb = VK_NULL_HANDLE;
			if (dp_render_pass != VK_NULL_HANDLE) {
				VkFramebufferCreateInfo fb_ci = {
				    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				    .renderPass = dp_render_pass,
				    .attachmentCount = 1,
				    .pAttachments = &c->shared_view,
				    .width = tgt_width,
				    .height = tgt_height,
				    .layers = 1,
				};
				vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &shared_fb);
			}

			xrt_display_processor_process_views(
			    c->display_processor,
			    cmd,
			    (VkImageView)(uintptr_t)left_view,
			    (VkImageView)(uintptr_t)right_view,
			    view_width, view_height,
			    (VkFormat_XDP)view_format,
			    shared_fb,
			    tgt_width, tgt_height,
			    (VkFormat_XDP)view_format);

			vk->vkEndCommandBuffer(cmd);

			VkSubmitInfo submit_info = {
			    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			    .commandBufferCount = 1,
			    .pCommandBuffers = &cmd,
			};
			res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, VK_NULL_HANDLE);
			if (res == VK_SUCCESS) {
				vk->vkQueueWaitIdle(vk->main_queue->queue);
				weaving_done = true;
			}

			if (shared_fb != VK_NULL_HANDLE) {
				vk->vkDestroyFramebuffer(vk->device, shared_fb, NULL);
			}
		}

		// Fallback: blit stereo texture to shared image
		if (!weaving_done) {
			comp_vk_native_renderer_blit_to_shared(c->renderer, cmd,
			    (uint64_t)(uintptr_t)c->shared_image, tgt_width, tgt_height);

			vk->vkEndCommandBuffer(cmd);

			VkSubmitInfo submit_info = {
			    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			    .commandBufferCount = 1,
			    .pCommandBuffers = &cmd,
			};
			res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, VK_NULL_HANDLE);
			if (res == VK_SUCCESS) {
				vk->vkQueueWaitIdle(vk->main_queue->queue);
			}
		}

		vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);
		return XRT_SUCCESS;
	}

	// If we have a target (window), present to it
	if (c->target != NULL) {
		uint32_t target_index;
		xret = comp_vk_native_target_acquire(c->target, &target_index);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to acquire target");
			return xret;
		}

		VkCommandPool cmd_pool = (VkCommandPool)(uintptr_t)
		    comp_vk_native_renderer_get_cmd_pool(c->renderer);

		VkCommandBufferAllocateInfo alloc_info = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		    .commandPool = cmd_pool,
		    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		    .commandBufferCount = 1,
		};

		VkCommandBuffer cmd;
		VkFramebuffer target_fb = VK_NULL_HANDLE;
		VkResult res = vk->vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd);
		if (res == VK_SUCCESS) {
			VkCommandBufferBeginInfo begin_info = {
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			vk->vkBeginCommandBuffer(cmd, &begin_info);

			uint64_t target_image, target_view;
			comp_vk_native_target_get_current_image(c->target, &target_image, &target_view);

			// Display processor weaving path: record interlacing commands
			// into our command buffer using a framebuffer from our target.
			// This matches the multi-compositor approach where the weaver is
			// a command recorder, not a standalone presenter.
			if (!is_mono && c->display_processor != NULL && c->dp_render_pass != VK_NULL_HANDLE) {
				static bool dp_logged = false;
				if (!dp_logged) {
					U_LOG_W("VK weaving via display processor (compositor-owned swapchain)");
					dp_logged = true;
				}

				uint64_t left_view, right_view;
				comp_vk_native_renderer_get_stereo_views(c->renderer, &left_view, &right_view);

				uint32_t view_width, view_height;
				comp_vk_native_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);

				int32_t view_format = comp_vk_native_renderer_get_format(c->renderer);

				// Create temporary framebuffer from the target's swapchain image
				VkImageView fb_view = (VkImageView)(uintptr_t)target_view;
				VkFramebufferCreateInfo fb_ci = {
				    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				    .renderPass = c->dp_render_pass,
				    .attachmentCount = 1,
				    .pAttachments = &fb_view,
				    .width = tgt_width,
				    .height = tgt_height,
				    .layers = 1,
				};
				vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &target_fb);

				// Pre-weave barrier: target → COLOR_ATTACHMENT_OPTIMAL
				VkImageMemoryBarrier pre_weave = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = 0,
				    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    .image = (VkImage)(uintptr_t)target_image,
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};
				vk->vkCmdPipelineBarrier(cmd,
				    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				    0, 0, NULL, 0, NULL, 1, &pre_weave);

				// Call display processor: records interlacing commands into cmd
				xrt_display_processor_process_views(
				    c->display_processor,
				    cmd,
				    (VkImageView)(uintptr_t)left_view,
				    (VkImageView)(uintptr_t)right_view,
				    view_width, view_height,
				    (VkFormat_XDP)view_format,
				    target_fb,
				    tgt_width, tgt_height,
				    (VkFormat_XDP)VK_FORMAT_B8G8R8A8_UNORM);

				// Render pass finalLayout handles transition to PRESENT_SRC_KHR
			} else {
				// No display processor: blit stereo texture to target
				comp_vk_native_renderer_blit_to_target(c->renderer, cmd,
				                                        target_image, tgt_width, tgt_height);
			}

			vk->vkEndCommandBuffer(cmd);

			VkSubmitInfo submit_info = {
			    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			    .commandBufferCount = 1,
			    .pCommandBuffers = &cmd,
			};

			res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, VK_NULL_HANDLE);
			if (res == VK_SUCCESS) {
				vk->vkQueueWaitIdle(vk->main_queue->queue);
			}

			vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);
		}

		// Destroy temporary framebuffer after GPU is done
		if (target_fb != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, target_fb, NULL);
		}

		// Present
		xret = comp_vk_native_target_present(c->target);

#ifdef XRT_OS_WINDOWS
		if (c->owns_window && c->own_window != NULL) {
			comp_d3d11_window_signal_paint_done(c->own_window);
		}
#endif

		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to present");
			return xret;
		}
	}

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                           struct xrt_compositor_semaphore *xcsem,
                                           uint64_t value)
{
	return vk_compositor_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
}

static void
vk_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;

	U_LOG_I("Destroying VK native compositor");

	vk->vkDeviceWaitIdle(vk->device);

	// Destroy display processor and its render pass
	xrt_display_processor_destroy(&c->display_processor);
	if (c->dp_render_pass != VK_NULL_HANDLE) {
		vk->vkDestroyRenderPass(vk->device, c->dp_render_pass, NULL);
	}

	// Destroy shared texture resources
	if (c->shared_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, c->shared_view, NULL);
	}
	if (c->shared_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
	}
	if (c->shared_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
	}

	if (c->renderer != NULL) {
		comp_vk_native_renderer_destroy(&c->renderer);
	}

	if (c->target != NULL) {
		comp_vk_native_target_destroy(&c->target);
	}

#ifdef XRT_OS_WINDOWS
	if (c->owns_window && c->own_window != NULL) {
		comp_d3d11_window_destroy(&c->own_window);
	}
#endif

#ifdef XRT_OS_MACOS
	if (c->macos_window != NULL) {
		comp_vk_native_window_macos_destroy(&c->macos_window);
	}
#endif

	// Destroy command pool (we created it for the display processor factory)
	if (c->cmd_pool != VK_NULL_HANDLE) {
		vk->vkDestroyCommandPool(vk->device, c->cmd_pool, NULL);
	}

	// Note: we do NOT destroy the VkDevice — it belongs to the app.
	// vk_bundle cleanup is minimal (just mutexes).

	free(c);
}

/*
 *
 * Exported functions
 *
 */

xrt_result_t
comp_vk_native_compositor_create(struct xrt_device *xdev,
                                 void *hwnd,
                                 void *vk_instance,
                                 void *vk_physical_device,
                                 void *vk_device,
                                 uint32_t queue_family_index,
                                 uint32_t queue_index,
                                 void *dp_factory_vk,
                                 void *shared_texture_handle,
                                 struct xrt_compositor_native **out_xc)
{
	if (vk_device == NULL) {
		U_LOG_E("VkDevice is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_I("Creating VK native compositor");

	struct comp_vk_native_compositor *c = U_TYPED_CALLOC(struct comp_vk_native_compositor);
	if (c == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	c->xdev = xdev;
	c->queue_family_index = queue_family_index;
	c->shared_texture_handle = shared_texture_handle;

	// Initialize vk_bundle from the app's existing VkDevice
	VkResult vk_ret = vk_init_from_given(
	    &c->vk,
	    vkGetInstanceProcAddr,
	    (VkInstance)vk_instance,
	    (VkPhysicalDevice)vk_physical_device,
	    (VkDevice)vk_device,
	    queue_family_index,
	    queue_index,
	    false,  // external_fence_fd_enabled
	    false,  // external_semaphore_fd_enabled
	    false,  // timeline_semaphore_enabled
	    false,  // image_format_list_enabled
	    false,  // debug_utils_enabled
	    U_LOGGING_INFO);
	if (vk_ret != VK_SUCCESS) {
		U_LOG_E("Failed to initialize vk_bundle from app device: %d", vk_ret);
		free(c);
		return XRT_ERROR_VULKAN;
	}

#ifdef XRT_OS_MACOS
	// Import shared IOSurface if provided
	if (shared_texture_handle != NULL) {
		if (!import_shared_iosurface(c, shared_texture_handle)) {
			U_LOG_E("Failed to import shared IOSurface");
			free(c);
			return XRT_ERROR_VULKAN;
		}
	}
#endif

#ifdef XRT_OS_WINDOWS
	// Import shared D3D11 texture if provided
	if (shared_texture_handle != NULL) {
		if (!import_shared_d3d11_texture(c, shared_texture_handle)) {
			U_LOG_E("Failed to import shared D3D11 texture");
			free(c);
			return XRT_ERROR_VULKAN;
		}
	}

	// Handle window
	if (hwnd != NULL) {
		c->hwnd = hwnd;
		U_LOG_I("Using app-provided window handle: %p", hwnd);
	} else if (shared_texture_handle != NULL) {
		c->hwnd = NULL;
		U_LOG_I("Offscreen mode — no window (shared texture handle: %p)", shared_texture_handle);
	} else {
		uint32_t win_w = xdev->hmd->screens[0].w_pixels;
		uint32_t win_h = xdev->hmd->screens[0].h_pixels;
		if (win_w == 0 || win_h == 0) {
			win_w = 1920;
			win_h = 1080;
		}
		U_LOG_I("Creating self-owned window (%ux%u)", win_w, win_h);
		xrt_result_t xret = comp_d3d11_window_create(win_w, win_h, &c->own_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create self-owned window");
			free(c);
			return xret;
		}
		c->hwnd = comp_d3d11_window_get_hwnd(c->own_window);
		c->owns_window = true;
		U_LOG_I("Created self-owned window: %p", c->hwnd);
	}
#endif

#ifdef XRT_OS_MACOS
	// Handle window on macOS
	if (hwnd != NULL) {
		// hwnd is an NSView* from cocoa_window_binding
		xrt_result_t xret = comp_vk_native_window_macos_setup_external(
		    hwnd, &c->macos_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to set up external view for VK native");
			free(c);
			return xret;
		}
		c->owns_window = false;
		U_LOG_I("Using app-provided NSView for VK native compositor");
	} else if (shared_texture_handle != NULL) {
		c->macos_window = NULL;
		U_LOG_I("Offscreen mode — no window (shared texture)");
	} else {
		uint32_t win_w = xdev->hmd->screens[0].w_pixels;
		uint32_t win_h = xdev->hmd->screens[0].h_pixels;
		if (win_w == 0 || win_h == 0) {
			win_w = 1920;
			win_h = 1080;
		}
		U_LOG_I("Creating self-owned macOS window (%ux%u)", win_w, win_h);
		xrt_result_t xret = comp_vk_native_window_macos_create(
		    win_w, win_h, &c->macos_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create self-owned macOS window");
			free(c);
			return xret;
		}
		c->owns_window = true;
	}
	// Set hwnd to the CAMetalLayer for target creation
	if (c->macos_window != NULL) {
		hwnd = comp_vk_native_window_macos_get_layer(c->macos_window);
	}
#endif

	// Initialize settings
	memset(&c->settings, 0, sizeof(c->settings));
	c->settings.preferred.width = xdev->hmd->screens[0].w_pixels;
	c->settings.preferred.height = xdev->hmd->screens[0].h_pixels;
	if (c->settings.preferred.width == 0 || c->settings.preferred.height == 0) {
		c->settings.preferred.width = 1920;
		c->settings.preferred.height = 1080;
	}
	c->settings.nominal_frame_interval_ns = xdev->hmd->screens[0].nominal_frame_interval_ns;
	if (c->settings.nominal_frame_interval_ns == 0) {
		c->settings.nominal_frame_interval_ns = (1000 * 1000 * 1000) / 60;
	}

#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT rect;
		if (GetClientRect((HWND)c->hwnd, &rect)) {
			c->settings.preferred.width = (uint32_t)(rect.right - rect.left);
			c->settings.preferred.height = (uint32_t)(rect.bottom - rect.top);
		}
	}
#endif

#ifdef XRT_OS_MACOS
	if (c->macos_window != NULL) {
		uint32_t mac_w = 0, mac_h = 0;
		comp_vk_native_window_macos_get_dimensions(c->macos_window, &mac_w, &mac_h);
		if (mac_w > 0 && mac_h > 0) {
			c->settings.preferred.width = mac_w;
			c->settings.preferred.height = mac_h;
		}
	}
	// Shared IOSurface dimensions take priority (import_shared_iosurface set them,
	// but settings init above may have overwritten with screen logical dimensions)
	if (c->has_shared_texture && shared_texture_handle != NULL) {
		IOSurfaceRef surface = (IOSurfaceRef)shared_texture_handle;
		c->settings.preferred.width = (uint32_t)IOSurfaceGetWidth(surface);
		c->settings.preferred.height = (uint32_t)IOSurfaceGetHeight(surface);
	}
#endif

	// Default refresh rate
	c->display_refresh_rate = 60.0f;

	// Create display processor via factory FIRST — the SR weaver creates
	// its own VkSwapchain on the HWND, so we must not also create one.
	if (dp_factory_vk != NULL) {
		xrt_dp_factory_vk_fn_t factory = (xrt_dp_factory_vk_fn_t)dp_factory_vk;

		// Create command pool for display processor (SR weaver needs it)
		VkCommandPoolCreateInfo pool_ci = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		    .queueFamilyIndex = c->queue_family_index,
		};
		VkResult pool_ret = c->vk.vkCreateCommandPool(
		    c->vk.device, &pool_ci, NULL, &c->cmd_pool);
		if (pool_ret != VK_SUCCESS) {
			U_LOG_E("Failed to create command pool for display processor: %d", pool_ret);
			vk_compositor_destroy(&c->base.base);
			return XRT_ERROR_VULKAN;
		}

		xrt_result_t dp_ret = factory(&c->vk, (void *)(uintptr_t)c->cmd_pool,
#ifdef XRT_OS_WINDOWS
		                               c->hwnd,
#else
		                               NULL,
#endif
		                               (int32_t)VK_FORMAT_R8G8B8A8_UNORM,
		                               &c->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("VK display processor factory failed (error %d), continuing without",
			        (int)dp_ret);
			c->display_processor = NULL;
		} else {
			U_LOG_W("VK display processor created via factory");
		}
	} else {
		U_LOG_W("No VK display processor factory provided");
	}

	// Create output target (VkSwapchainKHR) for presentation.
	// Even when a display processor (SR VK weaver) is present, we create
	// our own swapchain target. The weaver records interlacing commands
	// into a caller-provided command buffer + framebuffer — it does NOT
	// manage acquire/present internally. This matches the multi-compositor
	// approach where the compositor owns the swapchain and the weaver is
	// just a command recorder.
	if (hwnd != NULL
#ifdef XRT_OS_WINDOWS
	    || c->owns_window
#endif
#ifdef XRT_OS_MACOS
	    || c->owns_window
#endif
	) {
		void *target_hwnd = hwnd;
#ifdef XRT_OS_WINDOWS
		if (target_hwnd == NULL) target_hwnd = c->hwnd;
#endif
		xrt_result_t xret = comp_vk_native_target_create(c, target_hwnd,
		                                                  c->settings.preferred.width,
		                                                  c->settings.preferred.height,
		                                                  &c->target);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create VK target");
			vk_compositor_destroy(&c->base.base);
			return xret;
		}
	} else {
		c->target = NULL;
		U_LOG_I("No VK target — offscreen shared texture mode");
	}

	// Create a simple render pass for display processor framebuffer creation.
	// This must be compatible with the weaver's internal render pass
	// (single color attachment, B8G8R8A8_UNORM, store result).
	if (c->display_processor != NULL) {
		VkAttachmentDescription color_attachment = {
		    .format = VK_FORMAT_B8G8R8A8_UNORM,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		};
		VkAttachmentReference color_ref = {
		    .attachment = 0,
		    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};
		VkSubpassDescription subpass = {
		    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		    .colorAttachmentCount = 1,
		    .pColorAttachments = &color_ref,
		};
		VkRenderPassCreateInfo rp_ci = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		    .attachmentCount = 1,
		    .pAttachments = &color_attachment,
		    .subpassCount = 1,
		    .pSubpasses = &subpass,
		};
		c->vk.vkCreateRenderPass(c->vk.device, &rp_ci, NULL, &c->dp_render_pass);
	}

	// Determine view dimensions
	uint32_t view_width = c->settings.preferred.width / 2;
	uint32_t view_height = c->settings.preferred.height;

	// If display processor is available, query display info for view dimensions
	if (c->display_processor != NULL) {
		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		if (xrt_display_processor_get_display_pixel_info(
		        c->display_processor, &disp_px_w, &disp_px_h,
		        &disp_left, &disp_top) &&
		    disp_px_w > 0 && disp_px_h > 0) {
			uint32_t base_vw = disp_px_w / 2;
			uint32_t base_vh = disp_px_h;

			float ratio = fminf(
			    (float)c->settings.preferred.width / (float)disp_px_w,
			    (float)c->settings.preferred.height / (float)disp_px_h);
			if (ratio > 1.0f) ratio = 1.0f;

			view_width = (uint32_t)((float)base_vw * ratio);
			view_height = (uint32_t)((float)base_vh * ratio);
		}
	}

	// Create renderer
	uint32_t target_height = (c->display_processor != NULL) ? view_height : c->settings.preferred.height;
	xrt_result_t xret = comp_vk_native_renderer_create(c, view_width, view_height, target_height, &c->renderer);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create VK renderer");
		vk_compositor_destroy(&c->base.base);
		return xret;
	}

	// Initialize layer accumulator
	memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Populate supported swapchain formats (Vulkan formats)
	uint32_t format_count = 0;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R8G8B8A8_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R8G8B8A8_SRGB;
	c->base.base.info.formats[format_count++] = VK_FORMAT_B8G8R8A8_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_B8G8R8A8_SRGB;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R16G16B16A16_SFLOAT;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R16G16B16A16_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	c->base.base.info.formats[format_count++] = VK_FORMAT_D16_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_D32_SFLOAT;
	c->base.base.info.formats[format_count++] = VK_FORMAT_D24_UNORM_S8_UINT;
	c->base.base.info.format_count = format_count;

	// Native compositor is always visible and focused
	c->base.base.info.initial_visible = true;
	c->base.base.info.initial_focused = true;

	// Set up compositor interface
	c->base.base.get_swapchain_create_properties = vk_compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = vk_compositor_create_swapchain;
	c->base.base.import_swapchain = vk_compositor_import_swapchain;
	c->base.base.import_fence = vk_compositor_import_fence;
	c->base.base.create_semaphore = vk_compositor_create_semaphore;
	c->base.base.begin_session = vk_compositor_begin_session;
	c->base.base.end_session = vk_compositor_end_session;
	c->base.base.wait_frame = vk_compositor_wait_frame;
	c->base.base.predict_frame = vk_compositor_predict_frame;
	c->base.base.mark_frame = vk_compositor_mark_frame;
	c->base.base.begin_frame = vk_compositor_begin_frame;
	c->base.base.discard_frame = vk_compositor_discard_frame;
	c->base.base.layer_begin = vk_compositor_layer_begin;
	c->base.base.layer_projection = vk_compositor_layer_projection;
	c->base.base.layer_projection_depth = vk_compositor_layer_projection_depth;
	c->base.base.layer_quad = vk_compositor_layer_quad;
	c->base.base.layer_cube = vk_compositor_layer_cube;
	c->base.base.layer_cylinder = vk_compositor_layer_cylinder;
	c->base.base.layer_equirect1 = vk_compositor_layer_equirect1;
	c->base.base.layer_equirect2 = vk_compositor_layer_equirect2;
	c->base.base.layer_passthrough = vk_compositor_layer_passthrough;
	c->base.base.layer_window_space = vk_compositor_layer_window_space;
	c->base.base.layer_commit = vk_compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = vk_compositor_layer_commit_with_semaphore;
	c->base.base.destroy = vk_compositor_destroy;

	*out_xc = &c->base;

	U_LOG_I("VK native compositor created successfully (%ux%u)",
	        c->settings.preferred.width, c->settings.preferred.height);

	return XRT_SUCCESS;
}

bool
comp_vk_native_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                      struct xrt_vec3 *out_left_eye,
                                                      struct xrt_vec3 *out_right_eye)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		struct xrt_eye_pair eyes;
		if (xrt_display_processor_get_predicted_eye_positions(c->display_processor, &eyes) &&
		    eyes.valid) {
			out_left_eye->x = eyes.left.x;
			out_left_eye->y = eyes.left.y;
			out_left_eye->z = eyes.left.z;
			out_right_eye->x = eyes.right.x;
			out_right_eye->y = eyes.right.y;
			out_right_eye->z = eyes.right.z;
			return true;
		}
	}

	out_left_eye->x = -0.032f;
	out_left_eye->y = 0.0f;
	out_left_eye->z = 0.6f;
	out_right_eye->x = 0.032f;
	out_right_eye->y = 0.0f;
	out_right_eye->z = 0.6f;

	return false;
}

bool
comp_vk_native_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                                  float *out_width_m,
                                                  float *out_height_m)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		return xrt_display_processor_get_display_dimensions(
		    c->display_processor, out_width_m, out_height_m);
	}

	*out_width_m = 0.3f;
	*out_height_m = 0.2f;
	return false;
}

bool
comp_vk_native_compositor_get_window_metrics(struct xrt_compositor *xc,
                                              struct xrt_window_metrics *out_metrics)
{
	if (xc == NULL || out_metrics == NULL) {
		if (out_metrics != NULL) out_metrics->valid = false;
		return false;
	}

	struct comp_vk_native_compositor *c = vk_comp(xc);
	memset(out_metrics, 0, sizeof(*out_metrics));

#ifdef XRT_OS_WINDOWS
	if (c->display_processor == NULL || c->hwnd == NULL) {
		return false;
	}

	uint32_t disp_px_w = 0, disp_px_h = 0;
	int32_t disp_left = 0, disp_top = 0;
	if (!xrt_display_processor_get_display_pixel_info(
	        c->display_processor, &disp_px_w, &disp_px_h,
	        &disp_left, &disp_top)) {
		return false;
	}
	if (disp_px_w == 0 || disp_px_h == 0) return false;

	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	if (!xrt_display_processor_get_display_dimensions(
	        c->display_processor, &disp_w_m, &disp_h_m)) {
		return false;
	}

	RECT rect;
	if (!GetClientRect((HWND)c->hwnd, &rect)) return false;
	uint32_t win_px_w = (uint32_t)(rect.right - rect.left);
	uint32_t win_px_h = (uint32_t)(rect.bottom - rect.top);
	if (win_px_w == 0 || win_px_h == 0) return false;

	POINT client_origin = {0, 0};
	ClientToScreen((HWND)c->hwnd, &client_origin);

	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = disp_left;
	out_metrics->display_screen_top = disp_top;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->window_screen_left = (int32_t)client_origin.x;
	out_metrics->window_screen_top = (int32_t)client_origin.y;

	out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
	out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

	float win_center_px_x = (float)(client_origin.x - disp_left) + (float)win_px_w / 2.0f;
	float win_center_px_y = (float)(client_origin.y - disp_top) + (float)win_px_h / 2.0f;
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;

	out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	out_metrics->valid = true;
	return true;
#elif defined(XRT_OS_MACOS)
	// On macOS, delegate to display processor if available
	if (c->display_processor != NULL) {
		// Use xrt_display_processor_get_display_pixel_info + dimensions
		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		if (!xrt_display_processor_get_display_pixel_info(
		        c->display_processor, &disp_px_w, &disp_px_h,
		        &disp_left, &disp_top)) {
			return false;
		}
		if (disp_px_w == 0 || disp_px_h == 0) return false;

		float disp_w_m = 0.0f, disp_h_m = 0.0f;
		if (!xrt_display_processor_get_display_dimensions(
		        c->display_processor, &disp_w_m, &disp_h_m)) {
			return false;
		}

		uint32_t win_px_w = c->settings.preferred.width;
		uint32_t win_px_h = c->settings.preferred.height;
		if (win_px_w == 0 || win_px_h == 0) return false;

		float pixel_size_x = disp_w_m / (float)disp_px_w;
		float pixel_size_y = disp_h_m / (float)disp_px_h;

		out_metrics->display_width_m = disp_w_m;
		out_metrics->display_height_m = disp_h_m;
		out_metrics->display_pixel_width = disp_px_w;
		out_metrics->display_pixel_height = disp_px_h;
		out_metrics->display_screen_left = disp_left;
		out_metrics->display_screen_top = disp_top;

		out_metrics->window_pixel_width = win_px_w;
		out_metrics->window_pixel_height = win_px_h;
		out_metrics->window_screen_left = 0;
		out_metrics->window_screen_top = 0;

		out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
		out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

		// Center offset (assume centered for now)
		float win_center_px_x = (float)win_px_w / 2.0f;
		float win_center_px_y = (float)win_px_h / 2.0f;
		float disp_center_px_x = (float)disp_px_w / 2.0f;
		float disp_center_px_y = (float)disp_px_h / 2.0f;

		out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
		out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

		out_metrics->valid = true;
		return true;
	}
	return false;
#else
	(void)c;
	return false;
#endif
}

bool
comp_vk_native_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	if (xc == NULL) return false;
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		return xrt_display_processor_request_display_mode(c->display_processor, enable_3d);
	}
	return false;
}

void
comp_vk_native_compositor_set_system_devices(struct xrt_compositor *xc,
                                              struct xrt_system_devices *xsysd)
{
	if (xc == NULL) return;
	struct comp_vk_native_compositor *c = vk_comp(xc);

	c->xsysd = xsysd;

	if (xsysd != NULL) {
		U_LOG_I("VK native compositor: system devices set");
	}

	// macOS: no window-level input handling needed (uses oxr_macos event pump)

#ifdef XRT_OS_WINDOWS
	if (c->owns_window && c->own_window != NULL) {
		comp_d3d11_window_set_system_devices(c->own_window, xsysd);
	}
#endif
}

struct vk_bundle *
comp_vk_native_compositor_get_vk(struct comp_vk_native_compositor *c)
{
	return &c->vk;
}

uint32_t
comp_vk_native_compositor_get_queue_family(struct comp_vk_native_compositor *c)
{
	return c->queue_family_index;
}
