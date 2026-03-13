// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan presentation target (Win32 surface + VkSwapchainKHR).
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_target.h"
#include "comp_vk_native_compositor.h"

#include "xrt/xrt_vulkan_includes.h"
#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#ifdef XRT_OS_WINDOWS
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>
#endif

#ifdef XRT_OS_MACOS
#include <vulkan/vulkan_metal.h>
#endif

#define MAX_TARGET_IMAGES 4

/*!
 * Vulkan target structure.
 */
struct comp_vk_native_target
{
	//! Vulkan bundle (borrowed).
	struct vk_bundle *vk;

	//! Win32 surface.
	VkSurfaceKHR surface;

	//! Swapchain.
	VkSwapchainKHR swapchain;

	//! Swapchain images.
	VkImage images[MAX_TARGET_IMAGES];

	//! Swapchain image views.
	VkImageView views[MAX_TARGET_IMAGES];

	//! Number of swapchain images.
	uint32_t image_count;

	//! Current acquired image index.
	uint32_t current_index;

	//! Semaphore signaled when image is available.
	VkSemaphore image_available;

	//! Semaphore signaled when rendering is done.
	VkSemaphore render_finished;

	//! Current dimensions.
	uint32_t width;
	uint32_t height;

	//! Surface format.
	VkFormat format;

	//! Window handle.
	void *hwnd;

	//! Queue family index for present support check.
	uint32_t queue_family_index;
};

static void
destroy_swapchain_views(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;
	for (uint32_t i = 0; i < target->image_count; i++) {
		if (target->views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, target->views[i], NULL);
			target->views[i] = VK_NULL_HANDLE;
		}
	}
}

static xrt_result_t
create_swapchain_views(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;
	for (uint32_t i = 0; i < target->image_count; i++) {
		VkImageViewCreateInfo ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image = target->images[i],
		    .viewType = VK_IMAGE_VIEW_TYPE_2D,
		    .format = target->format,
		    .subresourceRange = {
		        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel = 0,
		        .levelCount = 1,
		        .baseArrayLayer = 0,
		        .layerCount = 1,
		    },
		};

		VkResult res = vk->vkCreateImageView(vk->device, &ci, NULL, &target->views[i]);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to create target image view %u: %d", i, res);
			return XRT_ERROR_VULKAN;
		}
	}
	return XRT_SUCCESS;
}

static xrt_result_t
create_swapchain(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;

	// Query surface capabilities
	VkSurfaceCapabilitiesKHR caps;
	VkResult res = vk->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
	    vk->physical_device, target->surface, &caps);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to get surface capabilities: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Use requested dimensions or surface extent
	VkExtent2D extent = {target->width, target->height};
	if (caps.currentExtent.width != UINT32_MAX) {
		extent = caps.currentExtent;
	}
	target->width = extent.width;
	target->height = extent.height;

	uint32_t image_count = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
		image_count = caps.maxImageCount;
	}
	if (image_count > MAX_TARGET_IMAGES) {
		image_count = MAX_TARGET_IMAGES;
	}

	// Pick surface format
	uint32_t format_count = 0;
	vk->vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical_device, target->surface,
	                                          &format_count, NULL);
	VkSurfaceFormatKHR formats[32];
	if (format_count > 32) format_count = 32;
	vk->vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical_device, target->surface,
	                                          &format_count, formats);

	// Prefer BGRA8_UNORM, fall back to first available
	target->format = formats[0].format;
	VkColorSpaceKHR color_space = formats[0].colorSpace;
	for (uint32_t i = 0; i < format_count; i++) {
		if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
			target->format = formats[i].format;
			color_space = formats[i].colorSpace;
			break;
		}
	}

	// Pick present mode: FIFO (VSync) is always available
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;

	VkSwapchainCreateInfoKHR ci = {
	    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
	    .surface = target->surface,
	    .minImageCount = image_count,
	    .imageFormat = target->format,
	    .imageColorSpace = color_space,
	    .imageExtent = extent,
	    .imageArrayLayers = 1,
	    .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .preTransform = caps.currentTransform,
	    .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
	    .presentMode = present_mode,
	    .clipped = VK_TRUE,
	    .oldSwapchain = VK_NULL_HANDLE,
	};

	res = vk->vkCreateSwapchainKHR(vk->device, &ci, NULL, &target->swapchain);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create swapchain: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Get swapchain images
	target->image_count = MAX_TARGET_IMAGES;
	res = vk->vkGetSwapchainImagesKHR(vk->device, target->swapchain,
	                                    &target->image_count, target->images);
	if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
		U_LOG_E("Failed to get swapchain images: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Create image views
	return create_swapchain_views(target);
}

xrt_result_t
comp_vk_native_target_create(struct comp_vk_native_compositor *c,
                              void *hwnd,
                              uint32_t width,
                              uint32_t height,
                              struct comp_vk_native_target **out_target)
{
	struct vk_bundle *vk = comp_vk_native_compositor_get_vk(c);
	uint32_t queue_family_index = comp_vk_native_compositor_get_queue_family(c);

	struct comp_vk_native_target *target = U_TYPED_CALLOC(struct comp_vk_native_target);
	if (target == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	target->vk = vk;
	target->hwnd = hwnd;
	target->width = width;
	target->height = height;
	target->queue_family_index = queue_family_index;

#ifdef XRT_OS_WINDOWS
	// Create Win32 surface
	// Note: vkCreateWin32SurfaceKHR is an instance-level function loaded
	// into vk_bundle by vk_get_instance_functions(). Access via vk->vkCreateWin32SurfaceKHR.
	PFN_vkCreateWin32SurfaceKHR pvkCreateWin32SurfaceKHR =
	    (PFN_vkCreateWin32SurfaceKHR)vk->vkGetInstanceProcAddr(vk->instance, "vkCreateWin32SurfaceKHR");
	if (pvkCreateWin32SurfaceKHR == NULL) {
		U_LOG_E("Failed to load vkCreateWin32SurfaceKHR");
		free(target);
		return XRT_ERROR_VULKAN;
	}

	VkWin32SurfaceCreateInfoKHR surface_ci = {
	    .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
	    .hinstance = GetModuleHandle(NULL),
	    .hwnd = (HWND)hwnd,
	};

	VkResult res = pvkCreateWin32SurfaceKHR(vk->instance, &surface_ci, NULL, &target->surface);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create Win32 surface: %d", res);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	// Check present support
	VkBool32 present_support = VK_FALSE;
	vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device,
	                                          queue_family_index,
	                                          target->surface, &present_support);
	if (!present_support) {
		U_LOG_E("Queue family does not support presentation to Win32 surface");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
#elif defined(XRT_OS_MACOS)
	// Create Metal surface via VK_EXT_metal_surface
	// hwnd parameter is actually a CAMetalLayer* on macOS
	// Try vk_bundle's pre-loaded function pointer first,
	// fall back to runtime lookup via vkGetInstanceProcAddr
	PFN_vkCreateMetalSurfaceEXT pfnCreateMetalSurface = vk->vkCreateMetalSurfaceEXT;
	if (pfnCreateMetalSurface == NULL) {
		pfnCreateMetalSurface = (PFN_vkCreateMetalSurfaceEXT)
		    vk->vkGetInstanceProcAddr(vk->instance, "vkCreateMetalSurfaceEXT");
	}
	if (pfnCreateMetalSurface == NULL) {
		U_LOG_E("vkCreateMetalSurfaceEXT not available — VK_EXT_metal_surface must be enabled");
		free(target);
		return XRT_ERROR_VULKAN;
	}

	U_LOG_I("Creating Metal surface from CAMetalLayer %p", hwnd);

	VkMetalSurfaceCreateInfoEXT surface_ci = {
	    .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
	    .pLayer = (const CAMetalLayer *)hwnd,
	};

	VkResult res = pfnCreateMetalSurface(vk->instance, &surface_ci, NULL, &target->surface);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create Metal surface: %d", res);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	// Check present support
	VkBool32 present_support = VK_FALSE;
	vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device,
	                                          queue_family_index,
	                                          target->surface, &present_support);
	if (!present_support) {
		U_LOG_E("Queue family does not support presentation to Metal surface");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
#else
	U_LOG_E("VK native target: no supported surface type on this platform");
	free(target);
	return XRT_ERROR_DEVICE_CREATION_FAILED;
#endif

	// Create synchronization primitives
	VkSemaphoreCreateInfo sem_ci = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	VkResult vk_res;
	vk_res = vk->vkCreateSemaphore(vk->device, &sem_ci, NULL, &target->image_available);
	if (vk_res != VK_SUCCESS) {
		U_LOG_E("Failed to create image_available semaphore");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
	vk_res = vk->vkCreateSemaphore(vk->device, &sem_ci, NULL, &target->render_finished);
	if (vk_res != VK_SUCCESS) {
		U_LOG_E("Failed to create render_finished semaphore");
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	// Create swapchain
	xrt_result_t xret = create_swapchain(target);
	if (xret != XRT_SUCCESS) {
		vk->vkDestroySemaphore(vk->device, target->render_finished, NULL);
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return xret;
	}

	*out_target = target;

	U_LOG_I("Created VK native target: %ux%u, %u images, format %d",
	        target->width, target->height, target->image_count, target->format);

	return XRT_SUCCESS;
}

void
comp_vk_native_target_destroy(struct comp_vk_native_target **target_ptr)
{
	if (target_ptr == NULL || *target_ptr == NULL) {
		return;
	}

	struct comp_vk_native_target *target = *target_ptr;
	struct vk_bundle *vk = target->vk;

	vk->vkDeviceWaitIdle(vk->device);

	destroy_swapchain_views(target);

	if (target->swapchain != VK_NULL_HANDLE) {
		vk->vkDestroySwapchainKHR(vk->device, target->swapchain, NULL);
	}
	if (target->render_finished != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, target->render_finished, NULL);
	}
	if (target->image_available != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
	}
	if (target->surface != VK_NULL_HANDLE) {
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
	}

	free(target);
	*target_ptr = NULL;
}

xrt_result_t
comp_vk_native_target_acquire(struct comp_vk_native_target *target, uint32_t *out_index)
{
	struct vk_bundle *vk = target->vk;

	// Use the semaphore for acquire, then do a dummy submit that waits on it
	// to ensure the image is actually available before the compositor renders.
	VkResult res = vk->vkAcquireNextImageKHR(vk->device, target->swapchain,
	                                          UINT64_MAX, target->image_available,
	                                          VK_NULL_HANDLE, &target->current_index);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
		// Swapchain invalidated (window resize, minimize, etc.) — recreate and retry
		U_LOG_I("Swapchain out of date, recreating");

		vk->vkDeviceWaitIdle(vk->device);
		destroy_swapchain_views(target);

		// Destroy old swapchain BEFORE creating new one — MoltenVK requires
		// the native window to be free (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR).
		if (target->swapchain != VK_NULL_HANDLE) {
			vk->vkDestroySwapchainKHR(vk->device, target->swapchain, NULL);
			target->swapchain = VK_NULL_HANDLE;
		}

		xrt_result_t xret = create_swapchain(target);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to recreate swapchain");
			return XRT_ERROR_VULKAN;
		}

		// Retry acquire with new swapchain
		res = vk->vkAcquireNextImageKHR(vk->device, target->swapchain,
		                                 UINT64_MAX, target->image_available,
		                                 VK_NULL_HANDLE, &target->current_index);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to acquire after swapchain recreation: %d", res);
			return XRT_ERROR_VULKAN;
		}
	} else if (res != VK_SUCCESS) {
		U_LOG_E("Failed to acquire swapchain image: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Wait for the acquired image to be available by doing a dummy submit
	// that waits on the image_available semaphore.
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkSubmitInfo wait_submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &target->image_available,
	    .pWaitDstStageMask = &wait_stage,
	};
	vk->vkQueueSubmit(vk->main_queue->queue, 1, &wait_submit, VK_NULL_HANDLE);
	vk->vkQueueWaitIdle(vk->main_queue->queue);

	*out_index = target->current_index;
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_target_present(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;

	// No semaphore wait needed — the compositor calls vkQueueWaitIdle
	// after all rendering commands before presenting.
	VkPresentInfoKHR present_info = {
	    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
	    .waitSemaphoreCount = 0,
	    .pWaitSemaphores = NULL,
	    .swapchainCount = 1,
	    .pSwapchains = &target->swapchain,
	    .pImageIndices = &target->current_index,
	};

	VkResult res = vk->vkQueuePresentKHR(vk->main_queue->queue, &present_info);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
		return XRT_SUCCESS;
	}
	if (res != VK_SUCCESS) {
		U_LOG_E("Present failed: %d", res);
		return XRT_ERROR_VULKAN;
	}

	return XRT_SUCCESS;
}

void
comp_vk_native_target_get_dimensions(struct comp_vk_native_target *target,
                                      uint32_t *out_width,
                                      uint32_t *out_height)
{
	*out_width = target->width;
	*out_height = target->height;
}

void
comp_vk_native_target_get_current_image(struct comp_vk_native_target *target,
                                         uint64_t *out_image,
                                         uint64_t *out_view)
{
	*out_image = (uint64_t)(uintptr_t)target->images[target->current_index];
	*out_view = (uint64_t)(uintptr_t)target->views[target->current_index];
}

xrt_result_t
comp_vk_native_target_resize(struct comp_vk_native_target *target,
                               uint32_t width,
                               uint32_t height)
{
	struct vk_bundle *vk = target->vk;

	if (width == target->width && height == target->height) {
		return XRT_SUCCESS;
	}

	vk->vkDeviceWaitIdle(vk->device);

	destroy_swapchain_views(target);

	VkSwapchainKHR old_swapchain = target->swapchain;
	target->swapchain = VK_NULL_HANDLE;
	target->width = width;
	target->height = height;

	xrt_result_t xret = create_swapchain(target);

	if (old_swapchain != VK_NULL_HANDLE) {
		vk->vkDestroySwapchainKHR(vk->device, old_swapchain, NULL);
	}

	return xret;
}
