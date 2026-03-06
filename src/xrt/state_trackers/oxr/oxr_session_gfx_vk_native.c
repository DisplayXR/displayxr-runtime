// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan session using Metal native compositor for presentation (macOS).
 *
 * On macOS, Vulkan apps are routed through the Metal native compositor:
 *   App (Vulkan) → comp_vk_client → Metal native compositor → CAMetalLayer
 *
 * The Metal compositor creates its own MTLDevice when no command queue is
 * provided. comp_vk_client imports Metal textures as VkImages via
 * VK_EXT_external_memory_metal (MoltenVK).
 *
 * @author David Fattal
 * @ingroup oxr_main
 */

#include <stdlib.h>

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_debug.h"

#include "xrt/xrt_instance.h"
#include "xrt/xrt_config_have.h"
#include "xrt/xrt_gfx_vk.h"

#include "vk/vk_helpers.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_handle.h"

#if defined(XRT_HAVE_METAL_NATIVE_COMPOSITOR) && defined(XR_USE_GRAPHICS_API_VULKAN)

#include "metal/comp_metal_compositor.h"

DEBUG_GET_ONCE_BOOL_OPTION(force_timeline_semaphores_vk_native, "OXR_DEBUG_FORCE_TIMELINE_SEMAPHORES", false)

/*!
 * Convert Metal pixel format enum values to Vulkan format enum values.
 * Metal and Vulkan use different numbering; this is needed when routing
 * Vulkan apps through the Metal native compositor.
 */
static int64_t
metal_format_to_vulkan(int64_t metal_fmt)
{
	switch (metal_fmt) {
	case 70:  return 37;  // MTLPixelFormatRGBA8Unorm → VK_FORMAT_R8G8B8A8_UNORM
	case 71:  return 43;  // MTLPixelFormatRGBA8Unorm_sRGB → VK_FORMAT_R8G8B8A8_SRGB
	case 80:  return 44;  // MTLPixelFormatBGRA8Unorm → VK_FORMAT_B8G8R8A8_UNORM
	case 81:  return 50;  // MTLPixelFormatBGRA8Unorm_sRGB → VK_FORMAT_B8G8R8A8_SRGB
	case 115: return 97;  // MTLPixelFormatRGBA16Float → VK_FORMAT_R16G16B16A16_SFLOAT
	case 90:  return 64;  // MTLPixelFormatRGB10A2Unorm → VK_FORMAT_A2B10G10R10_UNORM_PACK32
	case 252: return 126; // MTLPixelFormatDepth32Float → VK_FORMAT_D32_SFLOAT
	default:  return metal_fmt; // pass through unknown formats
	}
}

XrResult
oxr_session_populate_vk_with_metal_native(struct oxr_logger *log,
                                           struct oxr_system *sys,
                                           XrGraphicsBindingVulkanKHR const *next,
                                           struct oxr_session *sess)
{
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	// Get Metal display processor factory from system compositor info
	void *dp_factory_metal = NULL;
	if (sys->xsysc != NULL) {
		dp_factory_metal = sys->xsysc->info.dp_factory_metal;
	}

	// Create the Metal native compositor with NULL command queue
	// (it will create its own MTLDevice + MTLCommandQueue internally)
	xrt_result_t xret = comp_metal_compositor_create(
	    xdev,
	    NULL,  // window_handle — compositor creates its own window
	    NULL,  // command_queue — compositor creates its own Metal device
	    dp_factory_metal,
	    false, // offscreen
	    NULL,  // shared_iosurface
	    &xcn);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create Metal native compositor for Vulkan app: %d", xret);
	}

	// Set system devices for qwerty driver support
	comp_metal_compositor_set_system_devices(&xcn->base, sess->sys->xsysd);

	// Set system compositor info for display dimensions
	if (sys->xsysc != NULL) {
		comp_metal_compositor_set_sys_info(&xcn->base, &sys->xsysc->info);
	}

	// Convert format list from Metal pixel format values to Vulkan format values.
	// Metal and Vulkan use different enum numbering (e.g., MTLPixelFormatRGBA8Unorm=70
	// vs VK_FORMAT_R8G8B8A8_UNORM=37), and comp_vk_client passes them to Vulkan directly.
	for (uint32_t i = 0; i < xcn->base.info.format_count; i++) {
		xcn->base.info.formats[i] = metal_format_to_vulkan(xcn->base.info.formats[i]);
	}

	// Now wrap the Metal native compositor with a Vulkan client compositor.
	// comp_vk_client will import Metal textures as VkImages via
	// VK_EXT_external_memory_metal (supported by MoltenVK).
	bool timeline_semaphore_enabled = sess->sys->vk.timeline_semaphore_enabled;
	bool external_fence_fd_enabled = sess->sys->vk.external_fence_fd_enabled;
	bool external_semaphore_fd_enabled = sess->sys->vk.external_semaphore_fd_enabled;
	bool image_format_list_enabled =
	    sys->inst->extensions.KHR_vulkan_enable || sess->sys->vk.image_format_list_enabled;
	bool debug_utils_enabled = false;
	bool renderdoc_enabled = false;

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
	if (sys->inst->extensions.KHR_vulkan_enable && sys->inst->extensions.KHR_vulkan_enable2 &&
	    !external_fence_fd_enabled && !external_semaphore_fd_enabled) {
		external_fence_fd_enabled = true;
		external_semaphore_fd_enabled = true;
	} else if (sys->inst->extensions.KHR_vulkan_enable) {
		external_fence_fd_enabled = true;
		external_semaphore_fd_enabled = true;
	}
#endif

	if (!timeline_semaphore_enabled && debug_get_bool_option_force_timeline_semaphores_vk_native()) {
		timeline_semaphore_enabled = true;
	}

#ifdef OXR_HAVE_KHR_vulkan_enable2
	if (sys->inst->extensions.KHR_vulkan_enable2) {
		debug_utils_enabled = sess->sys->vk.debug_utils_enabled;
	}
#endif

	struct xrt_compositor_vk *xcvk = xrt_gfx_vk_provider_create(
	    xcn,
	    next->instance,
	    vkGetInstanceProcAddr,
	    next->physicalDevice,
	    next->device,
	    external_fence_fd_enabled,
	    external_semaphore_fd_enabled,
	    timeline_semaphore_enabled,
	    image_format_list_enabled,
	    debug_utils_enabled,
	    renderdoc_enabled,
	    next->queueFamilyIndex,
	    next->queueIndex);

	if (xcvk == NULL) {
		xrt_comp_native_destroy(&xcn);
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create Vulkan client compositor wrapping Metal native");
	}

	sess->xcn = xcn;
	sess->compositor = &xcvk->base;
	sess->create_swapchain = oxr_swapchain_vk_create;

	// Propagate native compositor's visibility/focus flags to the client wrapper.
	// oxr_session_create_impl reads these from sess->compositor->info to drive
	// the SYNCHRONIZED → VISIBLE → FOCUSED state transitions.
	xcvk->base.info.initial_visible = xcn->base.info.initial_visible;
	xcvk->base.info.initial_focused = xcn->base.info.initial_focused;

	U_LOG_W("Using Metal native compositor for Vulkan app (VK → Metal via MoltenVK)");

	return XR_SUCCESS;
}

#endif /* XRT_HAVE_METAL_NATIVE_COMPOSITOR && XR_USE_GRAPHICS_API_VULKAN */
