// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan session using native compositors for presentation.
 *
 * On macOS, Vulkan apps can use:
 *   1. VK native compositor (preferred): App (VK) → VK native → VkSurfaceKHR (Metal) → Display
 *   2. Metal native compositor (fallback): App (VK) → comp_vk_client → Metal → CAMetalLayer
 *
 * On Windows, Vulkan apps use the VK native compositor directly:
 *   App (Vulkan) → VK native compositor → Win32 surface → Display
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
                                           void *window_handle,
                                           void *shared_iosurface,
                                           bool transparent_background,
                                           struct oxr_session *sess)
{
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	// Get Metal display processor factory from system compositor info
	void *dp_factory_metal = NULL;
	if (sys->xsysc != NULL) {
		dp_factory_metal = sys->xsysc->info.dp_factory_metal;
	}

	bool offscreen = (window_handle == NULL && shared_iosurface != NULL);

	// Create the Metal native compositor
	// (it will create its own MTLDevice + MTLCommandQueue internally)
	xrt_result_t xret = comp_metal_compositor_create(
	    xdev,
	    window_handle,
	    NULL,  // command_queue — compositor creates its own Metal device
	    dp_factory_metal,
	    offscreen,
	    shared_iosurface,
	    transparent_background,
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

/*
 *
 * Windows: VK native compositor (direct Vulkan, no multi-compositor)
 *
 */

#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
#include "vk_native/comp_vk_native_compositor.h"
#endif

/*
 * Environment variable to enable/disable VK native compositor.
 * Default is TRUE — VK native compositor is enabled by default for in-process mode.
 * Set OXR_ENABLE_VK_NATIVE_COMPOSITOR=0 to force multi-compositor (for debugging).
 */
DEBUG_GET_ONCE_BOOL_OPTION(enable_vk_native_compositor, "OXR_ENABLE_VK_NATIVE_COMPOSITOR", true)

bool
oxr_vk_native_compositor_supported(struct oxr_system *sys, void *window_handle)
{
#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR
	(void)window_handle;

	bool is_service_mode = sys->xsysc != NULL && sys->xsysc->info.is_service_mode;
	bool env_enabled = debug_get_bool_option_enable_vk_native_compositor();

	U_LOG_IFL_I(U_LOGGING_INFO,
	            "VK native compositor check: XRT_HAVE_VK_NATIVE_COMPOSITOR=defined, "
	            "OXR_ENABLE_VK_NATIVE_COMPOSITOR=%s, window_handle=%p, is_service_mode=%s",
	            env_enabled ? "1 (enabled)" : "0 (disabled)", window_handle,
	            is_service_mode ? "true" : "false");

	if (is_service_mode) {
		U_LOG_IFL_I(U_LOGGING_INFO,
		            "VK native compositor DISABLED - running in service mode");
		return false;
	}

	if (!env_enabled) {
		U_LOG_IFL_I(U_LOGGING_INFO,
		            "VK native compositor DISABLED - falling back to multi-compositor");
		return false;
	}

	U_LOG_IFL_I(U_LOGGING_INFO, "VK native compositor ENABLED");
	return true;
#else
	U_LOG_IFL_I(U_LOGGING_INFO,
	            "VK native compositor check: XRT_HAVE_VK_NATIVE_COMPOSITOR=NOT defined");
	(void)sys;
	(void)window_handle;
	return false;
#endif
}

#ifdef XRT_HAVE_VK_NATIVE_COMPOSITOR

// Forward declaration — use VK native swapchain create
extern XrResult
oxr_swapchain_vk_create(struct oxr_logger *log,
                        struct oxr_session *sess,
                        const XrSwapchainCreateInfo *createInfo,
                        struct oxr_swapchain **out_swapchain);

XrResult
oxr_session_populate_vk_native(struct oxr_logger *log,
                                struct oxr_system *sys,
                                XrGraphicsBindingVulkanKHR const *next,
                                void *window_handle,
                                void *shared_texture_handle,
                                bool transparent_background,
                                struct oxr_session *sess)
{
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	// Get VK display processor factory from system compositor info
	void *dp_factory_vk = NULL;
	if (sys->xsysc != NULL) {
		dp_factory_vk = sys->xsysc->info.dp_factory_vk;
	}

	// Create the VK native compositor
	xrt_result_t xret = comp_vk_native_compositor_create(
	    xdev, window_handle,
	    (void *)next->instance,
	    (void *)next->physicalDevice,
	    (void *)next->device,
	    next->queueFamilyIndex,
	    next->queueIndex,
	    dp_factory_vk, shared_texture_handle, &xcn);
	if (xret != XRT_SUCCESS) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
		                 "Failed to create VK native compositor: %d", xret);
	}

	// Set system devices for qwerty driver support
	comp_vk_native_compositor_set_system_devices(&xcn->base, sess->sys->xsysd);

	// Set system compositor info (display dimensions, nominal viewer, legacy flags)
	if (sess->sys->xsysc != NULL) {
		comp_vk_native_compositor_set_sys_info(&xcn->base, &sess->sys->xsysc->info);
	}

	// Set the compositor directly — no client wrapper needed
	// The VK native compositor creates swapchains with real VkImages
	// that the app renders to directly (same VkDevice).
	sess->xcn = xcn;
	sess->compositor = &xcn->base;
	sess->create_swapchain = oxr_swapchain_vk_create;

	// D3D11 native compositor has is_d3d11_native_compositor flag;
	// we add is_vk_native_compositor for consistency
	sess->is_vk_native_compositor = true;

	// Native compositor is always visible and focused
	sess->compositor_visible = true;
	sess->compositor_focused = true;

	// Track external window / shared texture mode
	sess->has_external_window =
	    (window_handle != NULL || shared_texture_handle != NULL);
	if (sess->has_external_window) {
		struct xrt_device *head = GET_XDEV_BY_ROLE(sess->sys, head);
		if (head != NULL) {
			xrt_device_set_property(head, XRT_DEVICE_PROPERTY_EXT_APP_MODE, 1);
		}
	}

	U_LOG_IFL_I(U_LOGGING_INFO, "Using VK native compositor (direct Vulkan, no multi-compositor)%s",
	            shared_texture_handle ? " — shared texture mode" : "");

	return XR_SUCCESS;
}

#endif /* XRT_HAVE_VK_NATIVE_COMPOSITOR */
