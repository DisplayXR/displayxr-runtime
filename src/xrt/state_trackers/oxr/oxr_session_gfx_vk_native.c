// Copyright 2025, The DisplayXR Project
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

#ifdef XRT_OS_ANDROID
#include <android/looper.h>
#include "android/android_custom_surface.h"
#include "android/android_globals.h"
#endif

/*
 * Environment variable to enable/disable VK native compositor.
 * Default is TRUE — VK native compositor is enabled by default for in-process mode.
 * Set OXR_ENABLE_VK_NATIVE_COMPOSITOR=0 to force multi-compositor (for debugging).
 */
DEBUG_GET_ONCE_BOOL_OPTION(enable_vk_native_compositor, "OXR_ENABLE_VK_NATIVE_COMPOSITOR", true)

/*
 * Same override the client path already honours, reachable from the in-process
 * VK native path too. Only consumed by the VK-0 deposit (#1178) — an app that
 * enabled VK_KHR_timeline_semaphore without the runtime being able to observe it
 * (enable1, where the app builds its own device) can say so here.
 */
DEBUG_GET_ONCE_BOOL_OPTION(force_timeline_semaphores_vk_deposit, "OXR_DEBUG_FORCE_TIMELINE_SEMAPHORES", false)

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
                                bool window_is_wayland,
                                void *shared_texture_handle,
                                bool transparent_background,
                                struct oxr_session *sess)
{
	struct xrt_device *xdev = get_role_head(sess->sys);
	struct xrt_compositor_native *xcn = NULL;

	// Get VK display processor factory and display top-left from system compositor info
	void *dp_factory_vk = NULL;
	/*
	 * #918 VK-1 (#1178): the D3D11 factory travels beside the Vulkan one on
	 * Windows. It is NOT a second weaver — the compositor asks exactly one
	 * factory per session, and which one depends on where the weave lands.
	 * Under the output-device split the target is a DXGI swapchain on a
	 * runtime-owned scanout-adapter device, so the plug-in is asked for a
	 * D3D11 weaver; with the split off this stays unread.
	 */
	void *dp_factory_d3d11 = NULL;
	int32_t display_screen_left = 0;
	int32_t display_screen_top = 0;
	if (sys->xsysc != NULL) {
		dp_factory_vk = sys->xsysc->info.dp_factory_vk;
		dp_factory_d3d11 = sys->xsysc->info.dp_factory_d3d11;
		display_screen_left = sys->xsysc->info.display_screen_left;
		display_screen_top = sys->xsysc->info.display_screen_top;
	}

#ifdef XRT_OS_ANDROID
	/*
	 * Vendor display processors are created below, on THIS thread, and some
	 * vendor SDKs post their async init to the calling thread's Looper — with
	 * no Looper the init tears down immediately and the DP never comes up
	 * (#510 M2). `native_app_glue`'s android_main thread and a Java UI thread
	 * both have one; a bare pthread render thread (the engine shape) does not.
	 * In-process the runtime has no hook on the app's main thread to marshal
	 * onto (that hook, android_main_thread_dispatch_init(), is installed by the
	 * SERVICE's own JNI entry point), so name the condition instead of hanging.
	 */
	if (ALooper_forThread() == NULL) {
		U_LOG_W("Android: xrCreateSession is running on a thread with NO ALooper. A vendor display "
		        "processor whose init posts to the calling thread's Looper will not come up. Call "
		        "xrCreateSession from your Activity's main thread or from native_app_glue's "
		        "android_main thread.");
	}

	/*
	 * No XR_DXR_android_surface_binding chained ⟹ the `_hosted` fallback:
	 * spawn a SurfaceView on the activity via android_custom_surface and block
	 * briefly for its ANativeWindow.
	 *
	 * FULLSCREEN ONLY. This view is added straight to the WindowManager, so it
	 * has no ViewParent — and SurfaceView.onAttachedToWindow dereferences one on
	 * the freeform/translucent path, so the app dies with
	 * `NullPointerException … ViewParent.requestTransparentRegion` the moment
	 * the task lands in a multi-window container. An app that wants multi-window
	 * owns its own SurfaceView and chains the binding (#1037, ADR-036 D2).
	 */
	if (window_handle == NULL) {
		struct _JavaVM *vm = (struct _JavaVM *)android_globals_get_vm();
		void *activity = android_globals_get_activity();
		if (vm == NULL || activity == NULL) {
			return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
			                 "Android: VM=%p activity=%p — both must be set "
			                 "before xrCreateSession (call android_globals_store_vm_and_activity "
			                 "from JNI_OnLoad / Activity onCreate)",
			                 (void *)vm, activity);
		}
		struct android_custom_surface *cs = android_custom_surface_async_start(
		    vm, activity, /*display_id*/ 0, "DisplayXR", /*preferred_display_mode_id*/ 0, false);
		if (cs == NULL) {
			return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
			                 "Android: android_custom_surface_async_start failed");
		}
		ANativeWindow *win = android_custom_surface_wait_get_surface(cs, /*timeout_ms*/ 5000);
		if (win == NULL) {
			android_custom_surface_destroy(&cs);
			return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED,
			                 "Android: surfaceCreated callback never fired within 5 s");
		}
		android_globals_store_window((struct _ANativeWindow *)win);
		// Keep the custom surface alive + discoverable so oxr_session_poll can pull
		// the live surface each tick and republish it on background/resume — the
		// robust path that doesn't depend on JNI surface-callback registration
		// (unreliable under the in-process runtime's multiple MonadoView
		// classloaders). Last writer wins if the session is (re)created. #507
		android_globals_set_custom_surface(cs);
		window_handle = (void *)win;
		U_LOG_IFL_I(U_LOGGING_INFO,
		            "Android: ANativeWindow %p obtained via android_custom_surface", (void *)win);
	}
#endif

#ifdef XRT_OS_ANDROID
	else {
		// App-provided Surface (XR_DXR_android_surface_binding). The window is
		// already published in android_globals by the session-create parse, and
		// the session owns the reference — nothing to acquire or leak here.
		U_LOG_IFL_I(U_LOGGING_INFO, "Android: using the APP-provided ANativeWindow %p (#1037)",
		            window_handle);
	}
#endif

	// Create the VK native compositor
	xrt_result_t xret = comp_vk_native_compositor_create(
	    xdev, window_handle, window_is_wayland,
	    (void *)next->instance,
	    (void *)next->physicalDevice,
	    (void *)next->device,
	    next->queueFamilyIndex,
	    next->queueIndex,
	    // #868: a queue the runtime owns exclusively, when vulkan_enable2 let us
	    // ask for one. -1 = none, and the repaint stays off (see oxr_vulkan.c).
	    sess->sys->vulkan_runtime_queue_family,
	    sess->sys->vulkan_runtime_queue_index,
	    dp_factory_vk, dp_factory_d3d11, shared_texture_handle,
	    transparent_background,
	    display_screen_left, display_screen_top,
	    // VK-0 (#1178): does the APP's device carry VK_KHR_timeline_semaphore?
	    // Only the D3D11 deposit reads this, and only under DXR_VK_DEPOSIT=1 —
	    // its GPU-side sync is a D3D fence imported as a timeline semaphore,
	    // which cannot be created on a device that never enabled the feature.
	    sess->sys->vk.timeline_semaphore_enabled || debug_get_bool_option_force_timeline_semaphores_vk_deposit(),
	    // ADR-039: was VK_KHR_win32_keyed_mutex enabled on the app's device?
	    // The deposit's same-adapter sync rung needs the submit-time
	    // keyed-mutex handshake, which is invalid without the extension.
	    sess->sys->vk.win32_keyed_mutex_enabled,
	    &xcn);
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

	/*
	 * #886: the #868 repaint needs a queue the runtime owns, which only exists
	 * when vulkan_enable2 let the runtime create the VkDevice. Under enable1
	 * the app owns the device and the repaint silently stays off — name the
	 * consequence once so this is discoverable rather than silent. (The
	 * enable2-but-family-saturated case logs its own line in oxr_vulkan.c.)
	 */
	if (sess->sys->vulkan_runtime_queue_family < 0 && !sys->inst->extensions.KHR_vulkan_enable2) {
		U_LOG_W("#886: this Vulkan app uses XR_KHR_vulkan_enable1 — the app created the VkDevice, so "
		        "the runtime has no queue of its own and weave rate cannot be decoupled from render "
		        "rate (#868 repaint disabled). Port the app to XR_KHR_vulkan_enable2 "
		        "(xrCreateVulkanDeviceKHR).");
	}

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
