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
#include "vk/vk_hud_blend.h"
#include "vk/vk_local2d_composite.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_hud.h"
#include "os/os_time.h"

#include "math/m_api.h"
#include "util/u_tiling.h"
#include "util/u_canvas.h"
#include "util/u_capture_intent.h"
#include "util/u_image_capture.h"
#include <displayxr_mcp/mcp_capture.h>

// STB_IMAGE_WRITE_STATIC scopes all stbi_write_* to this TU so linking
// alongside other compositors that also implement stb doesn't produce
// duplicate symbols.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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

	//! Generic Vulkan display processor (vendor-agnostic weaving).
	struct xrt_display_processor *display_processor;

	//! True when display is in 3D mode (weaver active). False = 2D passthrough.
	bool hardware_display_3d;

	//! Last known 3D rendering mode index (for V-key toggle restore).
	uint32_t last_3d_mode_index;

	//! True if app is legacy (no XR_EXT_display_info) — gates 1/2/3 key mode selection.
	bool legacy_app_tile_scaling;

	//! System devices (for qwerty driver).
	struct xrt_system_devices *xsysd;

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;

	//! System compositor info (display dimensions, nominal viewer position).
	bool sys_info_set;
	struct xrt_system_compositor_info sys_info;

	//! HUD overlay (shared u_hud system).
	struct u_hud *hud;

	//! HUD texture (VkImage, CPU-uploadable).
	VkImage hud_image;
	VkDeviceMemory hud_memory;
	uint32_t hud_width;
	uint32_t hud_height;

	//! Smoothed frame time for HUD FPS display.
	float smoothed_frame_time_ms;

	//! Last frame timestamp for dt calculation.
	uint64_t last_frame_ns;

	//! Lazily allocated intermediate image for cropping atlas to content dims.
	VkImage dp_input_image;
	VkImageView dp_input_view;
	VkDeviceMemory dp_input_memory;
	uint32_t dp_input_width, dp_input_height;

	//! Canvas output rect for shared-texture apps.
	struct u_canvas_rect canvas;

	//! 2D surround texture handle (Spec v6).
	struct u_surround_2d_handle surround_2d;

	//! Alpha-blend pipeline for window-space (HUD) layers rendered per-tile
	//! INTO the atlas pre-weave (matches d3d11/d3d12/metal/gl). Lazy-init
	//! with the atlas format on first window-space submission. The DP weaver
	//! then interlaces the layer along with the projection content, giving
	//! HUD per-eye disparity / parallax (#210).
	struct vk_hud_blend window_space_blend;
	bool window_space_blend_attempted;
	//! Cached framebuffer for atlas window-space pass (one per atlas view).
	VkFramebuffer atlas_ws_fb;
	VkImageView atlas_ws_fb_view;

	//! MCP capture_frame request box (serviced at end of layer_commit).
	//! Mirrors the pattern in comp_metal/gl/d3d11_compositor. See issue #210.
	struct mcp_capture_request mcp_capture;

	//! Per-frame capture intent (mode + path), populated by
	//! u_capture_intent_poll at the top of layer_commit and consumed
	//! at the projection-done boundary (PROJECTION_ONLY) or end of
	//! frame (POST_COMPOSE).
	struct u_capture_intent capture_intent;

	//! #439 Phase 3 — masked 2D-over-3D composite (post-weave). Pipelines +
	//! render passes; created eagerly at compositor init (formats are fixed
	//! B8G8R8A8_UNORM for both the target and the scratch — see the init).
	struct vk_local2d_composite local2d;
	bool local2d_initialized;

	//! twod flatten scratch (B8G8R8A8_UNORM, COLOR_ATTACHMENT|SAMPLED). The
	//! frame's Local2D layers are flattened here, then sampled as `twod` by the
	//! masked composite. Lazily (re)allocated to the window region dims.
	VkImage local2d_scratch;
	VkDeviceMemory local2d_scratch_mem;
	VkImageView local2d_scratch_view;
	VkFramebuffer local2d_scratch_fb;
	uint32_t local2d_scratch_w, local2d_scratch_h;

	//! Weave snapshot scratch (target format, TRANSFER_DST|SAMPLED). The DP
	//! wrote the woven 3D into the target (RT≠SRV), so the lerp reads this
	//! copy. Lazily (re)allocated to the window region dims.
	VkImage weave_scratch;
	VkDeviceMemory weave_scratch_mem;
	VkImageView weave_scratch_view;
	uint32_t weave_scratch_w, weave_scratch_h;

	//! Runtime-owned IMPLICIT zone mask (R8_UNORM, COLOR_ATTACHMENT|SAMPLED),
	//! rasterized each frame from the Local2D layer rects (M=1 keep weave, M=0
	//! inside the rects). Sampled by the composite when no explicit mask is
	//! submitted. No staged sibling needed — raster + sample share one cmd
	//! buffer with a barrier between (unlike the cross-call explicit mask).
	VkImage implicit_mask_tex;
	VkDeviceMemory implicit_mask_mem;
	VkImageView implicit_mask_view;
	VkFramebuffer implicit_mask_fb;
	uint32_t implicit_mask_w, implicit_mask_h;

	//! Composite-target framebuffer cache (over the rotating swapchain/shared
	//! image view, keyed by view — the DP target image rotates per frame).
	VkFramebuffer composite_target_fb;
	VkImageView composite_target_fb_view;
	uint32_t composite_fb_w, composite_fb_h;

	//! Active authored zone mask (#439 Phase 1, XR_EXT_local_3d_zone). Sticky
	//! last-submit-wins; cleared on destroy. NOT owned (the oxr handle owns it).
	struct comp_vk_native_zone_mask *active_zone_mask;

	//! True when the last committed frame carried Local2D layers — makes the
	//! implicit mask's canvas-supersede visible to vk_effective_canvas. Set once
	//! under the frame path at the top of layer_commit.
	bool local_2d_last_frame;
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

	// Notify the DP that the host activity is foregrounded. On Android
	// this propagates to the vendor SDK's on_resume hook so the plug-in
	// can re-enable face-tracking + restore backlight after a pause.
	xrt_display_processor_on_resume(c->display_processor);

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_end_session(struct xrt_compositor *xc)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	U_LOG_I("VK native compositor session end");

	// Counterpart of begin_session: notify the DP we're backgrounding
	// so the vendor SDK can stop face-tracking + dim the backlight for
	// power. Safe even if the DP doesn't implement on_pause — the
	// helper NULL-checks the vtable slot.
	xrt_display_processor_on_pause(c->display_processor);

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
				uint32_t tc, tr;
				comp_vk_native_renderer_get_tile_layout(c->renderer, &tc, &tr);
				comp_vk_native_renderer_resize(c->renderer, new_vw, new_vh,
				                                tc * new_vw, tr * new_vh);
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
			uint32_t tc, tr;
			comp_vk_native_renderer_get_tile_layout(c->renderer, &tc, &tr);
			comp_vk_native_renderer_resize(c->renderer, new_vw, new_vh,
			                                tc * new_vw, tr * new_vh);
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

/*!
 * Local-2D layer (XR_EXT_local_3d_zone v3, #439 Phase 3) — accumulate only;
 * the VK consumer is Windows leg 2 of
 * docs/roadmap/unified-2d-3d-phase3-impl.md §7 (un-parks the crossapi
 * §4 decision).
 */
static xrt_result_t
vk_compositor_layer_local_2d(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_local_2d(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * Composite window-space (HUD) layers per-tile INTO the atlas image,
 * pre-weave, with proper alpha blending. Mirrors the per-tile rendering
 * model used by d3d11 / d3d12 / metal / gl, so atlas-capture parity
 * shows HUD on every tile and the DP weaver gives the layer per-eye
 * disparity (parallax) just like a projection layer.
 *
 * Atlas must be in SHADER_READ_ONLY_OPTIMAL on entry (the renderer
 * leaves it there after the projection-blit pass); on exit it is
 * returned to SHADER_READ_ONLY_OPTIMAL for the DP. No-op when no
 * window-space layers are present.
 *
 * @param c             The compositor.
 * @param cmd           Active command buffer to record into.
 * @param atlas_image   Atlas VkImage.
 * @param atlas_view    Atlas VkImageView (framebuffer attachment).
 * @param atlas_w       Allocated atlas width in pixels.
 * @param atlas_h       Allocated atlas height in pixels.
 * @param view_w        Per-tile view width.
 * @param view_h        Per-tile view height.
 * @param tile_columns  Atlas tile columns.
 * @param tile_rows     Atlas tile rows.
 */
static void
vk_compositor_render_window_space_into_atlas(struct comp_vk_native_compositor *c,
                                               VkCommandBuffer cmd,
                                               VkImage atlas_image,
                                               VkImageView atlas_view,
                                               uint32_t atlas_w,
                                               uint32_t atlas_h,
                                               uint32_t view_w,
                                               uint32_t view_h,
                                               uint32_t tile_columns,
                                               uint32_t tile_rows)
{
	struct vk_bundle *vk = &c->vk;

	bool has_ws = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_WINDOW_SPACE) {
			has_ws = true;
			break;
		}
	}
	if (!has_ws || tile_columns == 0 || tile_rows == 0 || view_w == 0 || view_h == 0) {
		return;
	}

	if (!c->window_space_blend.initialized && !c->window_space_blend_attempted) {
		c->window_space_blend_attempted = true;
		VkFormat atlas_fmt = (VkFormat)comp_vk_native_renderer_get_format(c->renderer);
		if (!vk_hud_blend_init(&c->window_space_blend, vk, atlas_fmt)) {
			U_LOG_E("[VK native] window-space alpha-blend init failed; "
			        "layers will be skipped");
		}
	}
	if (!c->window_space_blend.initialized) {
		return;
	}

	if (c->atlas_ws_fb == VK_NULL_HANDLE || c->atlas_ws_fb_view != atlas_view) {
		if (c->atlas_ws_fb != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, c->atlas_ws_fb, NULL);
			c->atlas_ws_fb = VK_NULL_HANDLE;
		}
		VkFramebufferCreateInfo fb_ci = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = c->window_space_blend.render_pass,
		    .attachmentCount = 1,
		    .pAttachments = &atlas_view,
		    .width = atlas_w,
		    .height = atlas_h,
		    .layers = 1,
		};
		if (vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &c->atlas_ws_fb) != VK_SUCCESS) {
			U_LOG_E("[VK native] atlas framebuffer creation failed (%ux%u); "
			        "window-space layers will be skipped",
			        atlas_w, atlas_h);
			c->atlas_ws_fb = VK_NULL_HANDLE;
			return;
		}
		c->atlas_ws_fb_view = atlas_view;
	}

	// Atlas: SHADER_READ_ONLY_OPTIMAL → COLOR_ATTACHMENT_OPTIMAL.
	VkImageMemoryBarrier atlas_to_color = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .image = atlas_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    0, 0, NULL, 0, NULL, 1, &atlas_to_color);

	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_WINDOW_SPACE) {
			continue;
		}
		struct xrt_swapchain *xsc = layer->sc_array[0];
		if (xsc == NULL) {
			continue;
		}

		const struct xrt_layer_window_space_data *ws = &layer->data.window_space;
		uint32_t sc_index = ws->sub.image_index;

		VkImage src_image = (VkImage)(uintptr_t)
		    comp_vk_native_swapchain_get_image(xsc, sc_index);
		if (src_image == VK_NULL_HANDLE) {
			continue;
		}

		// Source: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL.
		VkImageMemoryBarrier src_to_sample = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = src_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
		                         ws->sub.array_index, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		    0, 0, NULL, 0, NULL, 1, &src_to_sample);

		// Per-view pass with disparity shift in tile-fraction → atlas px.
		// Mirrors the metal/GL compositors (#413): a 2D (non-3D) mode gets
		// ONE stamp over the full content region (tile grid × view dims);
		// 3D modes stamp every tile. The previous code looped the tile grid
		// unconditionally and hardcoded the 2-view disparity pair
		// (tile_index == 0 ? -half : +half), which mis-shifts every layout
		// other than 2 views.
		uint32_t effective_views = c->hardware_display_3d ? (tile_columns * tile_rows) : 1;
		float half_disp = ws->disparity / 2.0f;
		for (uint32_t eye = 0; eye < effective_views; eye++) {
			uint32_t tile_x = eye % tile_columns;
			uint32_t tile_y = eye / tile_columns;

			float tile_origin_x = c->hardware_display_3d ? (float)(tile_x * view_w) : 0.0f;
			float tile_origin_y = c->hardware_display_3d ? (float)(tile_y * view_h) : 0.0f;
			float tile_w = c->hardware_display_3d ? (float)view_w
			                                      : (float)(tile_columns * view_w);
			float tile_h = c->hardware_display_3d ? (float)view_h
			                                      : (float)(tile_rows * view_h);

			// Per-view horizontal disparity, graded across the view sweep
			// (view index = baseline order, same as the projection views):
			// first view = -half, last = +half. Degenerates to the classic
			// -/+ pair for 2-view modes and to 0 for a single view.
			float eye_shift = 0.0f;
			if (effective_views > 1) {
				float t = (float)eye / (float)(effective_views - 1);
				eye_shift = -half_disp + ws->disparity * t;
			}

			int32_t dx = (int32_t)(tile_origin_x + (ws->x + eye_shift) * tile_w);
			int32_t dy = (int32_t)(tile_origin_y + ws->y * tile_h);
			int32_t dw_i = (int32_t)(ws->width * tile_w);
			int32_t dh_i = (int32_t)(ws->height * tile_h);
			if (dw_i <= 0 || dh_i <= 0) {
				continue;
			}

			// One-shot per-geometry diagnostic (#413): logs the resolved
			// per-view placement whenever the mode/layout/dims change, so a
			// missing-HUD report can be pinned to placement vs crop without
			// a custom build. Not per-frame (see debug-logging conventions).
			{
				static uint32_t logged_key = 0;
				uint32_t key = (tile_columns << 28) ^ (tile_rows << 24) ^
				               (effective_views << 20) ^ (view_w << 8) ^ view_h ^
				               ((uint32_t)c->hardware_display_3d << 31);
				if (key != logged_key && eye == 0) {
					logged_key = key;
					U_LOG_W("[VK native] window-space placement: 3d=%d tiles=%ux%u "
					        "views=%u view=%ux%u atlas=%ux%u first stamp dst=(%d,%d "
					        "%dx%d) ws=(%.3f,%.3f %.3fx%.3f disp=%.3f)",
					        (int)c->hardware_display_3d, tile_columns, tile_rows,
					        effective_views, view_w, view_h, atlas_w, atlas_h,
					        dx, dy, dw_i, dh_i,
					        ws->x, ws->y, ws->width, ws->height, ws->disparity);
				}
			}

			vk_hud_blend_draw_no_layout(&c->window_space_blend, vk, cmd,
			    c->atlas_ws_fb, atlas_w, atlas_h,
			    src_image, dx, dy, (uint32_t)dw_i, (uint32_t)dh_i);
		}

		// Source back to COLOR_ATTACHMENT_OPTIMAL so the app can rerender.
		VkImageMemoryBarrier src_back = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .image = src_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
		                         ws->sub.array_index, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    0, 0, NULL, 0, NULL, 1, &src_back);
	}

	// Atlas: COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL for DP.
	VkImageMemoryBarrier atlas_back = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = atlas_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, NULL, 0, NULL, 1, &atlas_back);
}

/*
 *
 * HUD overlay (shared u_hud system)
 *
 */

/*!
 * Render the diagnostic HUD overlay onto the target image.
 * Uses vkCmdBlitImage for an opaque blit (no alpha blending).
 * The u_hud pixel buffer has pre-composited semi-transparent background.
 */
static void
vk_compositor_render_hud(struct comp_vk_native_compositor *c,
                          VkCommandBuffer cmd,
                          VkImage target_image,
                          uint32_t target_width,
                          uint32_t target_height)
{
	if (c->hud == NULL || !u_hud_is_visible()) {
		return;
	}

	struct vk_bundle *vk = &c->vk;

	// Frame timing
	uint64_t now_ns = os_monotonic_get_ns();
	float dt = (c->last_frame_ns > 0) ? (float)(now_ns - c->last_frame_ns) / 1e9f : 0.016f;
	c->last_frame_ns = now_ns;

	float dt_ms = dt * 1000.0f;
	if (dt_ms > 0.0f) {
		c->smoothed_frame_time_ms = c->smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	float fps = (c->smoothed_frame_time_ms > 0.0f) ? (1000.0f / c->smoothed_frame_time_ms) : 0.0f;

	// Display dimensions and nominal viewer position from sys_info (fallback from DP)
	float disp_w_mm = 0, disp_h_mm = 0;
	float nom_x = 0, nom_y = 0, nom_z = 600.0f;
	if (c->sys_info_set) {
		disp_w_mm = c->sys_info.display_width_m * 1000.0f;
		disp_h_mm = c->sys_info.display_height_m * 1000.0f;
		nom_y = c->sys_info.nominal_viewer_y_m * 1000.0f;
		nom_z = c->sys_info.nominal_viewer_z_m * 1000.0f;
	} else if (c->display_processor != NULL) {
		float dw_m = 0, dh_m = 0;
		if (xrt_display_processor_get_display_dimensions(c->display_processor, &dw_m, &dh_m)) {
			disp_w_mm = dw_m * 1000.0f;
			disp_h_mm = dh_m * 1000.0f;
		}
	}

	// Eye positions from display processor
	struct xrt_eye_positions eye_pos = {0};
	if (c->display_processor != NULL) {
		xrt_display_processor_get_predicted_eye_positions(c->display_processor, &eye_pos);
	}
	if (!eye_pos.valid) {
		eye_pos.count = 2;
		eye_pos.eyes[0] = (struct xrt_eye_position){-0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
		eye_pos.eyes[1] = (struct xrt_eye_position){ 0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
	}

	// Fill HUD data
	struct u_hud_data data = {0};
	data.device_name = (c->xdev != NULL) ? c->xdev->str : "Unknown";
	data.fps = fps;
	data.frame_time_ms = c->smoothed_frame_time_ms;
	data.mode_3d = c->hardware_display_3d;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			data.rendering_mode_name = c->xdev->rendering_modes[idx].mode_name;
		}
	}
	if (c->renderer != NULL) {
		uint32_t vw, vh;
		comp_vk_native_renderer_get_view_dimensions(c->renderer, &vw, &vh);
		data.render_width = vw;
		data.render_height = vh;
	}
	if (c->xdev != NULL && c->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(c->xdev->rendering_modes, c->xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = target_width;
	data.window_height = target_height;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	data.eye_count = eye_pos.count;
	for (uint32_t e = 0; e < eye_pos.count && e < 8; e++) {
		data.eyes[e].x = eye_pos.eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos.eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos.eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos.is_tracking;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(c->xsysd->xdevs, c->xsysd->xdev_count, &qwerty_pose)) {
			data.vdisp_x = qwerty_pose.position.x;
			data.vdisp_y = qwerty_pose.position.y;
			data.vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			data.forward_x = fwd_out.x;
			data.forward_y = fwd_out.y;
			data.forward_z = fwd_out.z;
		}

		struct qwerty_view_state ss;
		if (qwerty_get_view_state(c->xsysd->xdevs, c->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.cam_spread_factor = ss.cam_spread_factor;
			data.cam_parallax_factor = ss.cam_parallax_factor;
			data.cam_convergence = ss.cam_convergence;
			data.cam_half_tan_vfov = ss.cam_half_tan_vfov;
			data.disp_spread_factor = ss.disp_spread_factor;
			data.disp_parallax_factor = ss.disp_parallax_factor;
			data.disp_vHeight = ss.disp_vHeight;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	bool dirty = u_hud_update(c->hud, &data);

	uint32_t hud_w = u_hud_get_width(c->hud);
	uint32_t hud_h = u_hud_get_height(c->hud);

	// Lazy-create HUD VkImage (host-visible for CPU upload)
	if (c->hud_image == VK_NULL_HANDLE && hud_w > 0 && hud_h > 0) {
		VkImageCreateInfo image_ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType = VK_IMAGE_TYPE_2D,
		    .format = VK_FORMAT_R8G8B8A8_UNORM,
		    .extent = {hud_w, hud_h, 1},
		    .mipLevels = 1,
		    .arrayLayers = 1,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .tiling = VK_IMAGE_TILING_LINEAR,
		    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
		};
		VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &c->hud_image);
		if (ret != VK_SUCCESS) {
			U_LOG_E("Failed to create HUD VkImage: %d", ret);
			return;
		}

		VkMemoryRequirements mem_reqs;
		vk->vkGetImageMemoryRequirements(vk->device, c->hud_image, &mem_reqs);

		// Find host-visible memory type
		VkPhysicalDeviceMemoryProperties mem_props;
		vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
		uint32_t mem_type = UINT32_MAX;
		for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
			if ((mem_reqs.memoryTypeBits & (1u << i)) &&
			    (mem_props.memoryTypes[i].propertyFlags &
			     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
				mem_type = i;
				break;
			}
		}
		if (mem_type == UINT32_MAX) {
			U_LOG_E("No host-visible memory type for HUD image");
			vk->vkDestroyImage(vk->device, c->hud_image, NULL);
			c->hud_image = VK_NULL_HANDLE;
			return;
		}

		VkMemoryAllocateInfo alloc_info = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		    .allocationSize = mem_reqs.size,
		    .memoryTypeIndex = mem_type,
		};
		ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &c->hud_memory);
		if (ret != VK_SUCCESS) {
			U_LOG_E("Failed to allocate HUD memory: %d", ret);
			vk->vkDestroyImage(vk->device, c->hud_image, NULL);
			c->hud_image = VK_NULL_HANDLE;
			return;
		}
		vk->vkBindImageMemory(vk->device, c->hud_image, c->hud_memory, 0);

		c->hud_width = hud_w;
		c->hud_height = hud_h;
		dirty = true;
	}

	if (c->hud_image == VK_NULL_HANDLE) {
		return;
	}

	// Upload pixels if changed
	if (dirty) {
		const uint8_t *pixels = u_hud_get_pixels(c->hud);
		void *mapped = NULL;

		VkMemoryRequirements mem_reqs;
		vk->vkGetImageMemoryRequirements(vk->device, c->hud_image, &mem_reqs);

		VkResult ret = vk->vkMapMemory(vk->device, c->hud_memory, 0, mem_reqs.size, 0, &mapped);
		if (ret == VK_SUCCESS) {
			// Get subresource layout for proper row pitch
			VkImageSubresource subres = {
			    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			    .mipLevel = 0,
			    .arrayLayer = 0,
			};
			VkSubresourceLayout layout;
			vk->vkGetImageSubresourceLayout(vk->device, c->hud_image, &subres, &layout);

			uint32_t src_row_bytes = hud_w * 4;
			uint8_t *dst = (uint8_t *)mapped + layout.offset;
			for (uint32_t row = 0; row < hud_h; row++) {
				memcpy(dst + row * layout.rowPitch, pixels + row * src_row_bytes, src_row_bytes);
			}
			vk->vkUnmapMemory(vk->device, c->hud_memory);
		}
	}

	// Transition target from PRESENT_SRC_KHR to TRANSFER_DST
	VkImageMemoryBarrier to_dst = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .image = target_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 1, &to_dst);

	// Transition HUD image to TRANSFER_SRC
	VkImageMemoryBarrier hud_to_src = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .image = c->hud_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_HOST_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 1, &hud_to_src);

	// Blit HUD to bottom-left corner of target
	uint32_t margin = 10;
	VkImageBlit blit = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffsets = {{0, 0, 0}, {(int32_t)hud_w, (int32_t)hud_h, 1}},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffsets = {
	        {(int32_t)margin, (int32_t)(target_height - hud_h - margin), 0},
	        {(int32_t)(margin + hud_w), (int32_t)(target_height - margin), 1},
	    },
	};
	vk->vkCmdBlitImage(cmd,
	    c->hud_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    1, &blit, VK_FILTER_NEAREST);

	// Transition HUD image back to PREINITIALIZED for next upload
	VkImageMemoryBarrier hud_back = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_HOST_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
	    .image = c->hud_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_HOST_BIT,
	    0, 0, NULL, 0, NULL, 1, &hud_back);

	// Transition target back to PRESENT_SRC_KHR
	VkImageMemoryBarrier to_present = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = 0,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .image = target_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
	    0, 0, NULL, 0, NULL, 1, &to_present);
}

/*!
 * Lazily (re)create the DP input intermediate image at content dimensions.
 * Returns true on success.
 */
static bool
vk_ensure_dp_input_image(struct comp_vk_native_compositor *c,
                          uint32_t content_w, uint32_t content_h)
{
	struct vk_bundle *vk = &c->vk;

	if (c->dp_input_width == content_w && c->dp_input_height == content_h &&
	    c->dp_input_image != VK_NULL_HANDLE) {
		return true;
	}

	// Destroy old resources
	if (c->dp_input_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, c->dp_input_view, NULL);
		c->dp_input_view = VK_NULL_HANDLE;
	}
	if (c->dp_input_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->dp_input_image, NULL);
		c->dp_input_image = VK_NULL_HANDLE;
	}
	if (c->dp_input_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->dp_input_memory, NULL);
		c->dp_input_memory = VK_NULL_HANDLE;
	}

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .extent = {content_w, content_h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &c->dp_input_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create VK DP input image %ux%u: %d", content_w, content_h, res);
		return false;
	}

	VkMemoryRequirements mem_reqs;
	vk->vkGetImageMemoryRequirements(vk->device, c->dp_input_image, &mem_reqs);

	uint32_t mem_type_index = 0;
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((mem_reqs.memoryTypeBits & (1 << i)) &&
		    (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			mem_type_index = i;
			break;
		}
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mem_reqs.size,
	    .memoryTypeIndex = mem_type_index,
	};

	res = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &c->dp_input_memory);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to allocate VK DP input memory: %d", res);
		return false;
	}

	res = vk->vkBindImageMemory(vk->device, c->dp_input_image, c->dp_input_memory, 0);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to bind VK DP input memory: %d", res);
		return false;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = c->dp_input_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .subresourceRange = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel = 0,
	        .levelCount = 1,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	};

	res = vk->vkCreateImageView(vk->device, &view_ci, NULL, &c->dp_input_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create VK DP input view: %d", res);
		return false;
	}

	c->dp_input_width = content_w;
	c->dp_input_height = content_h;
	U_LOG_I("VK crop: created DP input image %ux%u", content_w, content_h);

	return true;
}

/*!
 * Record crop-blit commands to copy the content region from an oversized atlas
 * into the DP input intermediate image. Updates *src_image_u64 and *src_view_u64
 * to point to the intermediate if cropping was performed.
 */
static void
vk_crop_atlas_for_dp(struct comp_vk_native_compositor *c,
                      VkCommandBuffer cmd,
                      uint64_t *src_image_u64,
                      uint64_t *src_view_u64,
                      uint32_t content_w,
                      uint32_t content_h,
                      uint32_t atlas_w,
                      uint32_t atlas_h)
{
	if (content_w == atlas_w && content_h == atlas_h) {
		return;
	}

	if (!vk_ensure_dp_input_image(c, content_w, content_h)) {
		return; // fallback: pass oversized atlas
	}

	struct vk_bundle *vk = &c->vk;
	VkImage src_image = (VkImage)(uintptr_t)*src_image_u64;

	// Transition src → TRANSFER_SRC
	VkImageMemoryBarrier src_to_transfer = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .image = src_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 1, &src_to_transfer);

	// Transition intermediate → TRANSFER_DST
	VkImageMemoryBarrier dst_to_transfer = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .image = c->dp_input_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 1, &dst_to_transfer);

	// Copy content region
	VkImageCopy region = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffset = {0, 0, 0},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffset = {0, 0, 0},
	    .extent = {content_w, content_h, 1},
	};
	vk->vkCmdCopyImage(cmd,
	    src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    c->dp_input_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    1, &region);

	// Transition src back → SHADER_READ_ONLY
	VkImageMemoryBarrier src_back = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = src_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};

	// Transition intermediate → SHADER_READ_ONLY (for DP sampling)
	VkImageMemoryBarrier dst_to_read = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = c->dp_input_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};

	VkImageMemoryBarrier barriers[2] = {src_back, dst_to_read};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, NULL, 0, NULL, 2, barriers);

	// Update output pointers to intermediate
	*src_image_u64 = (uint64_t)(uintptr_t)c->dp_input_image;
	*src_view_u64 = (uint64_t)(uintptr_t)c->dp_input_view;
}

/*
 *
 * MCP capture helpers
 *
 */

// Helper: find a host-visible memory type.
static bool
vk_native_find_host_visible_memory_type(struct vk_bundle *vk, uint32_t type_bits, uint32_t *out_index)
{
	VkPhysicalDeviceMemoryProperties props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &props);
	const VkMemoryPropertyFlags want =
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
			*out_index = i;
			return true;
		}
	}
	return false;
}

// Read the content region of the renderer's atlas (tile_columns × view_width
// by tile_rows × view_height — what actually got composited, matching what
// the compositor crops and sends to the DP) into a host-visible staging
// buffer, swap channels if format is BGRA, and write @p path as PNG. Uses
// the renderer's command pool and the main queue.
static bool
vk_native_capture_atlas_to_png(struct comp_vk_native_compositor *c, const char *path)
{
	struct vk_bundle *vk = &c->vk;

	uint64_t atlas_image_u64 = comp_vk_native_renderer_get_atlas_image(c->renderer);
	VkImage atlas_image = (VkImage)(uintptr_t)atlas_image_u64;
	if (atlas_image == VK_NULL_HANDLE) {
		return false;
	}

	uint32_t tile_columns = 1, tile_rows = 1;
	comp_vk_native_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);
	uint32_t view_w = 0, view_h = 0;
	comp_vk_native_renderer_get_view_dimensions(c->renderer, &view_w, &view_h);
	if (tile_columns == 0 || tile_rows == 0 || view_w == 0 || view_h == 0) {
		return false;
	}
	uint32_t atlas_w = 0, atlas_h = 0;
	comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &atlas_w, &atlas_h);

	uint32_t content_w = tile_columns * view_w;
	uint32_t content_h = tile_rows * view_h;
	if (atlas_w > 0 && content_w > atlas_w) content_w = atlas_w;
	if (atlas_h > 0 && content_h > atlas_h) content_h = atlas_h;

	VkFormat atlas_format = (VkFormat)comp_vk_native_renderer_get_format(c->renderer);
	bool swap_bgra =
	    (atlas_format == VK_FORMAT_B8G8R8A8_UNORM || atlas_format == VK_FORMAT_B8G8R8A8_SRGB);

	VkCommandPool cmd_pool = (VkCommandPool)(uintptr_t)comp_vk_native_renderer_get_cmd_pool(c->renderer);
	if (cmd_pool == VK_NULL_HANDLE) {
		return false;
	}

	// Allocate host-visible staging buffer (content_w × content_h × 4).
	VkDeviceSize bytes = (VkDeviceSize)content_w * (VkDeviceSize)content_h * 4;
	VkBuffer staging_buf = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	uint8_t *mapped = NULL;

	VkBufferCreateInfo bi = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = bytes,
	    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (vk->vkCreateBuffer(vk->device, &bi, NULL, &staging_buf) != VK_SUCCESS) {
		return false;
	}

	VkMemoryRequirements mreq;
	vk->vkGetBufferMemoryRequirements(vk->device, staging_buf, &mreq);
	uint32_t mem_type = 0;
	if (!vk_native_find_host_visible_memory_type(vk, mreq.memoryTypeBits, &mem_type)) {
		vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
		return false;
	}
	VkMemoryAllocateInfo ai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mreq.size,
	    .memoryTypeIndex = mem_type,
	};
	if (vk->vkAllocateMemory(vk->device, &ai, NULL, &staging_mem) != VK_SUCCESS ||
	    vk->vkBindBufferMemory(vk->device, staging_buf, staging_mem, 0) != VK_SUCCESS) {
		if (staging_mem != VK_NULL_HANDLE) vk->vkFreeMemory(vk->device, staging_mem, NULL);
		vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
		return false;
	}

	// One-shot command buffer to copy atlas -> buffer.
	VkCommandBufferAllocateInfo cbai = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vk->vkAllocateCommandBuffers(vk->device, &cbai, &cmd) != VK_SUCCESS) {
		vk->vkFreeMemory(vk->device, staging_mem, NULL);
		vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
		return false;
	}

	VkCommandBufferBeginInfo cbi = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &cbi);

	// Atlas was sampled by the DP/weaver this frame, so its layout is
	// SHADER_READ_ONLY_OPTIMAL by the time we're called from layer_commit.
	// Transition: SHADER_READ_ONLY_OPTIMAL → TRANSFER_SRC_OPTIMAL → SHADER_READ_ONLY_OPTIMAL.
	VkImageMemoryBarrier b = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = atlas_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	};
	vk->vkCmdPipelineBarrier(cmd,
	                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                         VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         0, 0, NULL, 0, NULL, 1, &b);

	VkBufferImageCopy region = {
	    .bufferOffset = 0,
	    .bufferRowLength = 0,
	    .bufferImageHeight = 0,
	    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .imageOffset = {0, 0, 0},
	    .imageExtent = {content_w, content_h, 1},
	};
	vk->vkCmdCopyImageToBuffer(cmd, atlas_image,
	                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                           staging_buf, 1, &region);

	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vk->vkCmdPipelineBarrier(cmd,
	                         VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                         0, 0, NULL, 0, NULL, 1, &b);

	vk->vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};
	vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, VK_NULL_HANDLE);
	vk->vkQueueWaitIdle(vk->main_queue->queue);
	vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);

	// Read pixels and encode PNG.
	bool ok = false;
	if (vk->vkMapMemory(vk->device, staging_mem, 0, bytes, 0, (void **)&mapped) == VK_SUCCESS) {
		uint8_t *pixels = mapped;
		uint8_t *swapped = NULL;
		if (swap_bgra) {
			swapped = malloc((size_t)bytes);
			if (swapped != NULL) {
				for (VkDeviceSize i = 0; i < bytes; i += 4) {
					swapped[i + 0] = mapped[i + 2];
					swapped[i + 1] = mapped[i + 1];
					swapped[i + 2] = mapped[i + 0];
					swapped[i + 3] = mapped[i + 3];
				}
				pixels = swapped;
			}
		}
		// Force opaque: swapchain alpha is undefined for display output, and
		// left as-is the PNG renders transparent/black (issue #425). Covers
		// both the BGRA-swapped copy and the direct (mapped) path.
		u_image_force_opaque_rgba8(pixels, content_w, content_h, (size_t)content_w * 4);
		ok = stbi_write_png(path, (int)content_w, (int)content_h, 4,
		                    pixels, (int)content_w * 4) != 0;
		free(swapped);
		vk->vkUnmapMemory(vk->device, staging_mem);
	}

	vk->vkFreeMemory(vk->device, staging_mem, NULL);
	vk->vkDestroyBuffer(vk->device, staging_buf, NULL);
	return ok;
}

// Run the capture readback if the per-frame intent matches @p mode_filter.
// Caller polls intent at top of layer_commit; this fires at each boundary.
static void
vk_native_dispatch_capture(struct comp_vk_native_compositor *c, uint32_t mode_filter)
{
	if (!u_capture_intent_should_capture(&c->capture_intent, mode_filter)) {
		return;
	}
	bool ok = vk_native_capture_atlas_to_png(c, c->capture_intent.path);
	if (ok) {
		U_LOG_I("Atlas captured (mode=%u) to %s",
		        c->capture_intent.mode, c->capture_intent.path);
	} else {
		U_LOG_W("Atlas capture failed (mode=%u path=%s)",
		        c->capture_intent.mode, c->capture_intent.path);
	}
	u_capture_intent_complete(&c->capture_intent, &c->mcp_capture, ok);
}


// #439 Phase 3 — masked 2D-over-3D composite, defined below near the zone-mask
// API. Called from the weave path in layer_commit; release from destroy.
static bool
vk_composite_local_2d(struct comp_vk_native_compositor *c,
                      VkCommandBuffer cmd,
                      VkImage dst_image,
                      VkImageView dst_view,
                      uint32_t dst_w,
                      uint32_t dst_h,
                      VkImageLayout dst_incoming,
                      VkImageLayout dst_outgoing);
static void
vk_release_local2d_state(struct comp_vk_native_compositor *c);

static xrt_result_t
vk_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;

	// Capture-intent poll — checks MCP request + trigger files once per
	// frame; consumed at the projection-done boundary or end of frame
	// depending on requested mode. See u_capture_intent.h.
	u_capture_intent_poll(&c->capture_intent, &c->mcp_capture);

	// #439 Phase 3 — per-frame Local2D accumulator flag (read by
	// vk_effective_canvas + vk_composite_local_2d). Set once here so it reflects
	// this frame's committed layers.
	c->local_2d_last_frame = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			c->local_2d_last_frame = true;
			break;
		}
	}

	// Phase 1 diagnostic — env-gated per-client commit interval. Mirrors
	// the same `[CLIENT_FRAME_NS]` line emitted by the D3D11 in-process and
	// service compositors so per-app frame rates are directly comparable
	// across graphics APIs in standalone runs. One client per process here
	// so a static cache is fine; the compositor pointer tags the line.
	{
		static int log_client_frame_ns = -1;
		if (log_client_frame_ns < 0) {
			const char *e = getenv("DISPLAYXR_LOG_PRESENT_NS");
			log_client_frame_ns = (e != NULL && e[0] == '1') ? 1 : 0;
		}
		if (log_client_frame_ns) {
			static int64_t last_commit_ns = 0;
			int64_t now_ns = (int64_t)os_monotonic_get_ns();
			if (last_commit_ns != 0) {
				U_LOG_W("[CLIENT_FRAME_NS] client=%p dt_ns=%lld",
				        (void *)c,
				        (long long)(now_ns - last_commit_ns));
			}
			last_commit_ns = now_ns;
		}
	}

	// Get predicted eye positions
	struct xrt_vec3 left_eye = {-0.032f, 0.0f, 0.6f};
	struct xrt_vec3 right_eye = {0.032f, 0.0f, 0.6f};

	if (c->display_processor != NULL) {
		struct xrt_eye_positions eyes;
		if (xrt_display_processor_get_predicted_eye_positions(c->display_processor, &eyes) &&
		    eyes.valid) {
			left_eye.x = eyes.eyes[0].x;
			left_eye.y = eyes.eyes[0].y;
			left_eye.z = eyes.eyes[0].z;
			right_eye.x = eyes.eyes[1].x;
			right_eye.y = eyes.eyes[1].y;
			right_eye.z = eyes.eyes[1].z;
		}
	}

	// Sync hardware_display_3d, tile layout, and per-view dimensions
	// from device's active rendering mode.
	// Legacy apps: view dims are fixed at compromise scale, only update tile layout.
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			c->hardware_display_3d = mode->hardware_display_3d;
			if (mode->tile_columns > 0 && c->renderer != NULL) {
				if (!c->legacy_app_tile_scaling) {
					// Extension app: sync view dimensions from active mode
					uint32_t new_vw = mode->view_width_pixels;
					uint32_t new_vh = mode->view_height_pixels;
					uint32_t new_aw = mode->atlas_width_pixels;
					uint32_t new_ah = mode->atlas_height_pixels;
					if (c->canvas.valid) {
						u_tiling_compute_canvas_view(mode, c->canvas.w, c->canvas.h,
						                             &new_vw, &new_vh);
					}
#if defined(XRT_OS_WINDOWS) || defined(__APPLE__)
					else if (!c->owns_window && c->settings.preferred.width > 0 &&
					         c->settings.preferred.height > 0) {
						// Handle app: window may be smaller than the display,
						// so scale view dims to the actual window client area
						// (matches the D3D11/D3D12 path) — keeps the atlas
						// content region, DP input, and atlas capture at window
						// resolution.
						u_tiling_compute_canvas_view(mode, c->settings.preferred.width,
						                             c->settings.preferred.height,
						                             &new_vw, &new_vh);
					}
#endif
					if (new_vw > 0 && new_vh > 0) {
						comp_vk_native_renderer_resize(
						    c->renderer, new_vw, new_vh, new_aw, new_ah);
					}
				}
				// Always update tile layout (both legacy and extension apps)
				comp_vk_native_renderer_set_tile_layout(
				    c->renderer, mode->tile_columns, mode->tile_rows);
			}
		}
	}

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(c->xsysd->xdevs, c->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = c->xsysd->static_roles.head;
			if (head != NULL && head->hmd != NULL) {
				if (force_2d) {
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						c->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					head->hmd->active_rendering_mode_index = c->last_3d_mode_index;
				}
			}
			comp_vk_native_compositor_request_display_mode(&c->base.base, !force_2d);
		}

		// Rendering mode change from qwerty 0/1/2/3/4 keys.
		// Legacy apps only support V toggle — skip direct mode selection.
		if (!c->legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(c->xsysd->xdevs, c->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = c->xsysd->static_roles.head;
				if (head != NULL) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

	// Get target dimensions
	uint32_t tgt_width = c->settings.preferred.width;
	uint32_t tgt_height = c->settings.preferred.height;
	if (c->target != NULL) {
		comp_vk_native_target_get_dimensions(c->target, &tgt_width, &tgt_height);
	}

	// Zero-copy check: can we pass the app's swapchain directly to the DP?
	bool zero_copy = false;
	uint64_t zc_image_u64 = 0;
	uint64_t zc_view_u64 = 0;
	int32_t zc_format = 0;
	uint32_t zc_width = 0, zc_height = 0;

	{
		const struct xrt_rendering_mode *mode = NULL;
		if (c->xdev != NULL && c->xdev->hmd != NULL) {
			uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
			if (idx < c->xdev->rendering_mode_count)
				mode = &c->xdev->rendering_modes[idx];
		}
		if (mode != NULL && c->layer_accum.layer_count == 1) {
			struct comp_layer *layer = &c->layer_accum.layers[0];
			if (layer->data.type == XRT_LAYER_PROJECTION ||
			    layer->data.type == XRT_LAYER_PROJECTION_DEPTH) {
				uint32_t vc = mode->view_count;
				bool same_sc = (vc > 0 && vc <= XRT_MAX_VIEWS && layer->sc_array[0] != NULL);
				for (uint32_t v = 1; v < vc && same_sc; v++) {
					if (layer->sc_array[v] != layer->sc_array[0])
						same_sc = false;
				}
				if (same_sc) {
					uint32_t img_idx = layer->data.proj.v[0].sub.image_index;
					bool same_idx = true;
					for (uint32_t v = 1; v < vc; v++) {
						if (layer->data.proj.v[v].sub.image_index != img_idx) {
							same_idx = false;
							break;
						}
					}
					bool all_array_zero = same_idx;
					for (uint32_t v = 0; v < vc && all_array_zero; v++) {
						if (layer->data.proj.v[v].sub.array_index != 0)
							all_array_zero = false;
					}
					if (all_array_zero) {
						uint32_t sw, sh;
						comp_vk_native_swapchain_get_dimensions(layer->sc_array[0], &sw, &sh);
						int32_t rxs[XRT_MAX_VIEWS], rys[XRT_MAX_VIEWS];
						uint32_t rws[XRT_MAX_VIEWS], rhs_arr[XRT_MAX_VIEWS];
						for (uint32_t v = 0; v < vc; v++) {
							rxs[v] = layer->data.proj.v[v].sub.rect.offset.w;
							rys[v] = layer->data.proj.v[v].sub.rect.offset.h;
							rws[v] = layer->data.proj.v[v].sub.rect.extent.w;
							rhs_arr[v] = layer->data.proj.v[v].sub.rect.extent.h;
						}
						if (u_tiling_can_zero_copy(vc, rxs, rys, rws, rhs_arr, sw, sh, mode)) {
							zc_image_u64 = comp_vk_native_swapchain_get_image(layer->sc_array[0], img_idx);
							zc_view_u64 = comp_vk_native_swapchain_get_image_view(layer->sc_array[0], img_idx);
							if (zc_image_u64 != 0 && zc_view_u64 != 0) {
								zero_copy = true;
								zc_format = comp_vk_native_renderer_get_format(c->renderer);
								zc_width = sw;
								zc_height = sh;
							}
						}
					}
				}
			}
		}
	}

	// Render projection layers to atlas texture (skip if zero-copy)
	xrt_result_t xret = XRT_SUCCESS;
	if (!zero_copy) {
		xret = comp_vk_native_renderer_draw(
		    c->renderer, &c->layer_accum, &left_eye, &right_eye, tgt_width, tgt_height, c->hardware_display_3d);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to render layers");
			return xret;
		}

		// Projection-only capture point — atlas now contains projection
		// layers across all tiles; window-space layers haven't been
		// composed in yet. Atlas is in SHADER_READ_ONLY_OPTIMAL after
		// renderer's terminating barrier (see comp_vk_native_renderer.c).
		// Skipped in zero-copy mode (no renderer atlas to read).
		vk_native_dispatch_capture(c, MCP_CAPTURE_MODE_PROJECTION_ONLY);
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

		// Display processor path (weaving for 3D, passthrough for 2D)
		if (c->display_processor != NULL) {
			uint64_t src_image_u64, src_view_u64;
			int32_t view_format;
			uint32_t view_width, view_height, tc, tr;

			if (zero_copy) {
				src_image_u64 = zc_image_u64;
				src_view_u64 = zc_view_u64;
				view_format = zc_format;
			} else {
				src_image_u64 = comp_vk_native_renderer_get_atlas_image(c->renderer);
				src_view_u64 = comp_vk_native_renderer_get_atlas_view(c->renderer);
				view_format = comp_vk_native_renderer_get_format(c->renderer);
			}

			comp_vk_native_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);
			comp_vk_native_renderer_get_tile_layout(c->renderer, &tc, &tr);

			// Pre-weave: composite window-space layers per-tile INTO the atlas
			// so the DP weaves them along with the projection content. Skipped
			// in zero-copy (no atlas) and when no window-space layers exist.
			if (!zero_copy) {
				uint32_t atlas_w_pre, atlas_h_pre;
				comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &atlas_w_pre, &atlas_h_pre);
				vk_compositor_render_window_space_into_atlas(c, cmd,
				    (VkImage)(uintptr_t)src_image_u64,
				    (VkImageView)(uintptr_t)src_view_u64,
				    atlas_w_pre, atlas_h_pre,
				    view_width, view_height, tc, tr);
			}

			// Crop atlas to content dimensions before passing to DP
			{
				uint32_t content_w = tc * view_width;
				uint32_t content_h = tr * view_height;
				uint32_t atlas_w, atlas_h;
				if (zero_copy) {
					atlas_w = zc_width;
					atlas_h = zc_height;
				} else {
					comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &atlas_w, &atlas_h);
				}
				vk_crop_atlas_for_dp(c, cmd, &src_image_u64, &src_view_u64,
				                      content_w, content_h, atlas_w, atlas_h);
			}

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

			// DP target: use canvas dims for texture apps
			uint32_t dp_target_w = tgt_width;
			uint32_t dp_target_h = tgt_height;
			if (c->canvas.valid && c->canvas.w > 0 && c->canvas.h > 0) {
				dp_target_w = c->canvas.w;
				dp_target_h = c->canvas.h;
			}

			bool dp_self_submits =
			    xrt_display_processor_is_self_submitting(c->display_processor);

			if (dp_self_submits) {
				// Flush pre-DP work (window-space composite, atlas crop)
				// so the DP's internal submit sees a coherent atlas.
				vk->vkEndCommandBuffer(cmd);
				VkSubmitInfo pre_si = {
				    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				    .commandBufferCount = 1,
				    .pCommandBuffers = &cmd,
				};
				res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &pre_si, VK_NULL_HANDLE);
				if (res == VK_SUCCESS) {
					vk->vkQueueWaitIdle(vk->main_queue->queue);
				}
			}

			xrt_display_processor_process_atlas(
			    c->display_processor,
			    dp_self_submits ? VK_NULL_HANDLE : cmd,
			    (VkImage_XDP)(uintptr_t)src_image_u64,
			    (VkImageView)(uintptr_t)src_view_u64,
			    view_width, view_height,
			    tc, tr,
			    (VkFormat_XDP)view_format,
			    shared_fb,
			    (VkImage_XDP)c->shared_image,
			    dp_target_w, dp_target_h,
			    (VkFormat_XDP)view_format,
			    c->canvas.valid ? c->canvas.x : 0,
			    c->canvas.valid ? c->canvas.y : 0,
			    c->canvas.valid ? c->canvas.w : 0,
			    c->canvas.valid ? c->canvas.h : 0);

			if (dp_self_submits) {
				// DP owns its own submit — nothing left for us to do this
				// frame except mark woven and clean up.
				weaving_done = true;
			} else {
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
			}

			if (shared_fb != VK_NULL_HANDLE) {
				vk->vkDestroyFramebuffer(vk->device, shared_fb, NULL);
			}
		}

		// Fallback: blit atlas texture to shared image
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
			VkRenderPass dp_render_pass = (c->display_processor != NULL)
			    ? xrt_display_processor_get_render_pass(c->display_processor)
			    : VK_NULL_HANDLE;

			// A self-submitting DP (e.g. the Leia CNSDK weaver) runs its
			// own render pass internally and exposes none to us, so
			// get_render_pass() is NULL. Route it through the DP path
			// anyway — gating solely on a non-NULL render pass would drop
			// it into the blit_to_target fallback below, which on Android
			// faults inside the Adreno driver (HW-3) and never weaves. Only
			// the explicit-cmd-buffer DP path needs our render pass/FB.
			bool dp_self_submits = (c->display_processor != NULL) &&
			    xrt_display_processor_is_self_submitting(c->display_processor);

			if (c->display_processor != NULL &&
			    (dp_render_pass != VK_NULL_HANDLE || dp_self_submits)) {
				static bool dp_logged = false;
				if (!dp_logged) {
					U_LOG_W("VK rendering via display processor (compositor-owned swapchain)");
					dp_logged = true;
				}

				uint64_t src_image_u64, src_view_u64;
				int32_t view_format;
				uint32_t view_width, view_height, tc, tr;

				if (zero_copy) {
					src_image_u64 = zc_image_u64;
					src_view_u64 = zc_view_u64;
					view_format = zc_format;
				} else {
					src_image_u64 = comp_vk_native_renderer_get_atlas_image(c->renderer);
					src_view_u64 = comp_vk_native_renderer_get_atlas_view(c->renderer);
					view_format = comp_vk_native_renderer_get_format(c->renderer);
				}

				comp_vk_native_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);
				comp_vk_native_renderer_get_tile_layout(c->renderer, &tc, &tr);

				// Pre-weave: composite window-space layers per-tile INTO the
				// atlas. Skipped in zero-copy (no atlas) and when no
				// window-space layers exist.
				if (!zero_copy) {
					uint32_t atlas_w_pre, atlas_h_pre;
					comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &atlas_w_pre, &atlas_h_pre);
					vk_compositor_render_window_space_into_atlas(c, cmd,
					    (VkImage)(uintptr_t)src_image_u64,
					    (VkImageView)(uintptr_t)src_view_u64,
					    atlas_w_pre, atlas_h_pre,
					    view_width, view_height, tc, tr);
				}

				// Crop atlas to content dimensions before passing to DP
				{
					uint32_t content_w = tc * view_width;
					uint32_t content_h = tr * view_height;
					uint32_t atlas_w, atlas_h;
					if (zero_copy) {
						atlas_w = zc_width;
						atlas_h = zc_height;
					} else {
						comp_vk_native_renderer_get_atlas_dimensions(c->renderer, &atlas_w, &atlas_h);
					}
					vk_crop_atlas_for_dp(c, cmd, &src_image_u64, &src_view_u64,
					                      content_w, content_h, atlas_w, atlas_h);
				}

				// Create temporary framebuffer from the target's swapchain image.
				// Must use the DP's render pass for compatibility with vkCmdBeginRenderPass.
				// A self-submitting DP exposes no render pass, so leave target_fb
				// NULL for it (it weaves internally and ignores the FB) — calling
				// vkCreateFramebuffer with a NULL render pass would be invalid.
				if (dp_render_pass != VK_NULL_HANDLE) {
					VkImageView fb_view = (VkImageView)(uintptr_t)target_view;
					VkFramebufferCreateInfo fb_ci = {
					    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
					    .renderPass = dp_render_pass,
					    .attachmentCount = 1,
					    .pAttachments = &fb_view,
					    .width = tgt_width,
					    .height = tgt_height,
					    .layers = 1,
					};
					vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &target_fb);
				}

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

				if (dp_self_submits) {
					// Flush pre-DP work (WS-layer composite, atlas crop,
					// target-image layout transition) so the DP's
					// internal submit sees the right state on the GPU.
					vk->vkEndCommandBuffer(cmd);
					VkSubmitInfo pre_si = {
					    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
					    .commandBufferCount = 1,
					    .pCommandBuffers = &cmd,
					};
					res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &pre_si, VK_NULL_HANDLE);
					if (res == VK_SUCCESS) {
						vk->vkQueueWaitIdle(vk->main_queue->queue);
					}
				}

				// Hand the DP our lifecycle-managed view for this target
				// image. A self-submitting DP with no render pass (Leia
				// CNSDK) builds its own destination framebuffer and would
				// otherwise have to create its own VkImageView on the
				// swapchain image — which Adreno faults on. No-op for DPs
				// that don't expose the slot.
				xrt_display_processor_set_target_color_view(
				    c->display_processor, (VkImageView)(uintptr_t)target_view);

				// Call display processor with atlas (or zero-copy swapchain) texture
				xrt_display_processor_process_atlas(
				    c->display_processor,
				    dp_self_submits ? VK_NULL_HANDLE : cmd,
				    (VkImage_XDP)(uintptr_t)src_image_u64,
				    (VkImageView)(uintptr_t)src_view_u64,
				    view_width, view_height,
				    tc, tr,
				    (VkFormat_XDP)view_format,
				    target_fb,
				    (VkImage_XDP)(uintptr_t)target_image,
				    tgt_width, tgt_height,
				    (VkFormat_XDP)comp_vk_native_target_get_format(c->target),
				    c->canvas.valid ? c->canvas.x : 0,
				    c->canvas.valid ? c->canvas.y : 0,
				    c->canvas.valid ? c->canvas.w : 0,
				    c->canvas.valid ? c->canvas.h : 0);

				if (dp_self_submits) {
					// DP owned the post-weave submit. The vendor weave's
					// queue submit may still be in flight writing to
					// target_image; the HUD cmd buffer we're about to
					// record writes to the same image, so drain the GPU
					// before recording the HUD to avoid a target_image
					// race (audit B7). On Android the HUD is c->hud==NULL
					// and this overlay is a no-op, so the wait costs
					// nothing in practice — but keep it for safety when
					// HUD eventually wires up.
					vk->vkDeviceWaitIdle(vk->device);

					// Allocate a fresh cmd buffer for any post-DP
					// overlays (HUD), then fall through to the shared
					// end+submit below by re-using `cmd`.
					VkCommandBufferAllocateInfo post_ai = {
					    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					    .commandPool = cmd_pool,
					    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
					    .commandBufferCount = 1,
					};
					vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);
					vk->vkAllocateCommandBuffers(vk->device, &post_ai, &cmd);
					VkCommandBufferBeginInfo post_bi = {
					    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
					};
					vk->vkBeginCommandBuffer(cmd, &post_bi);
				}

				// Render pass finalLayout handles transition to PRESENT_SRC_KHR
				// Window-space layers are composed pre-weave into the atlas
				// (see vk_compositor_render_window_space_into_atlas), so we
				// only need the diagnostic HUD on the target post-weave.

				// #439 Phase 3 — overlay the frame's flattened 2D where the
				// zone mask says "2D" (M*weave + (1-M)*twod). No-op unless the
				// frame carries Local2D layers. Target stays PRESENT_SRC so the
				// HUD's own PRESENT_SRC→COLOR→PRESENT transition still applies.
				vk_composite_local_2d(c, cmd, (VkImage)(uintptr_t)target_image,
				    (VkImageView)(uintptr_t)target_view, tgt_width, tgt_height,
				    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

				// Diagnostic HUD overlay (TAB key toggle)
				vk_compositor_render_hud(c, cmd,
				    (VkImage)(uintptr_t)target_image, tgt_width, tgt_height);

				// A self-submitting DP (Leia CNSDK) ran its own internal
				// render pass, whose finalLayout leaves the target in
				// COLOR_ATTACHMENT_OPTIMAL — not PRESENT_SRC_KHR. The
				// non-self-submit path above relies on the compositor render
				// pass's finalLayout for that transition, but here there's no
				// such pass, so transition explicitly before present.
				if (dp_self_submits) {
					VkImageMemoryBarrier to_present = {
					    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					    .dstAccessMask = 0,
					    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
					    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					    .image = (VkImage)(uintptr_t)target_image,
					    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
					};
					vk->vkCmdPipelineBarrier(cmd,
					    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
					    0, 0, NULL, 0, NULL, 1, &to_present);
				}
			} else {
				// No display processor (or mono/2D mode): blit atlas texture to target
				comp_vk_native_renderer_blit_to_target(c->renderer, cmd,
				                                        target_image, tgt_width, tgt_height);

				// Diagnostic HUD overlay (TAB key toggle)
				vk_compositor_render_hud(c, cmd,
				    (VkImage)(uintptr_t)target_image, tgt_width, tgt_height);
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

	// Post-compose capture (#210) — captures the fully composed atlas
	// (projection + window-space + quads) the DP just received. Skipped
	// when intent is for projection-only (consumed earlier) or empty.
	vk_native_dispatch_capture(c, MCP_CAPTURE_MODE_POST_COMPOSE);

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

	// Uninstall MCP capture hook before any GPU resources go away — the
	// MCP thread can no longer post requests against us after this returns.
	mcp_capture_uninstall();
	mcp_capture_fini(&c->mcp_capture);

	vk->vkDeviceWaitIdle(vk->device);

	// Destroy HUD resources
	if (c->hud_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->hud_image, NULL);
	}
	if (c->hud_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->hud_memory, NULL);
	}
	u_hud_destroy(&c->hud);

	// Destroy window-space (HUD) alpha-blend pipeline + cached atlas FB
	if (c->atlas_ws_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, c->atlas_ws_fb, NULL);
		c->atlas_ws_fb = VK_NULL_HANDLE;
	}
	vk_hud_blend_fini(&c->window_space_blend, vk);

	// #439 Phase 3 — masked composite pipelines + scratch images. (The active
	// zone mask is owned by the oxr handle, freed via zone_mask_destroy.)
	vk_release_local2d_state(c);

	// Destroy DP input crop image
	if (c->dp_input_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, c->dp_input_view, NULL);
	}
	if (c->dp_input_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->dp_input_image, NULL);
	}
	if (c->dp_input_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->dp_input_memory, NULL);
	}

	// Destroy display processor
	xrt_display_processor_destroy(&c->display_processor);

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

/*!
 * Sanity-check a display-processor's C vtable before the compositor trusts it.
 *
 * The DP is a plug-in-owned struct of function pointers. On Windows we have
 * observed (standalone VK + Leia SR) the DP's small heap block being reused by
 * the GPU driver's Vulkan shader compiler during pipeline creation, leaving the
 * vtable holding garbage (non-code) pointers — a subsequent call through it is a
 * hard ACCESS_VIOLATION. This validates that the load-bearing slots point into
 * committed, executable pages; NULL is left alone (it's a valid "unsupported"
 * sentinel that every xrt_display_processor_* wrapper already tolerates).
 *
 * Returns true if the vtable looks usable. On non-Windows this is a no-op
 * (returns true) — the failure mode is Windows-driver-specific.
 */
static bool
dp_vtable_looks_sane(struct xrt_display_processor *dp)
{
	if (dp == NULL) {
		return false;
	}
#ifdef XRT_OS_WINDOWS
	// Load-bearing slots the compositor calls. process_atlas + destroy are
	// mandatory; the rest are optional (may legitimately be NULL).
	void *fns[] = {
	    (void *)dp->process_atlas,        (void *)dp->destroy,
	    (void *)dp->get_display_pixel_info, (void *)dp->get_render_pass,
	    (void *)dp->get_display_dimensions, (void *)dp->set_chroma_key,
	};
	if (dp->process_atlas == NULL || dp->destroy == NULL) {
		return false; // mandatory slots
	}
	const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
		if (fns[i] == NULL) {
			continue; // valid "unsupported" sentinel
		}
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery(fns[i], &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
		    (mbi.Protect & exec) == 0) {
			return false;
		}
	}
#endif
	return true;
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
                                 bool transparent_background,
                                 uint32_t chroma_key_color,
                                 int32_t display_screen_left,
                                 int32_t display_screen_top,
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
	c->hardware_display_3d = true;
	c->last_3d_mode_index = 1;

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
		xrt_result_t xret = comp_d3d11_window_create(
		    win_w, win_h, display_screen_left, display_screen_top, &c->own_window);
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
		    hwnd, transparent_background, &c->macos_window);
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
		    win_w, win_h, transparent_background, &c->macos_window);
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
		                               (int32_t)VK_FORMAT_B8G8R8A8_UNORM,
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

	// Forward transparent_background + chroma_key_color to the display processor.
	// The DP owns the chroma-key trick (compositor stays vendor-agnostic).
	// Safe no-op if the DP doesn't implement set_chroma_key or display_processor is NULL.
	if (c->display_processor != NULL) {
		xrt_display_processor_set_chroma_key(
		    c->display_processor, chroma_key_color, transparent_background);
	}

	// Create output target (VkSwapchainKHR) for presentation.
	// The compositor owns the swapchain — the weaver (display processor)
	// records interlacing commands into a caller-provided command buffer +
	// framebuffer via setCommandBuffer / setOutputFrameBuffer. It does NOT
	// create its own swapchain. The HWND passed to CreateVulkanWeaver is
	// used only for monitor detection and draw-region calculation.
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
		                                                  transparent_background,
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

	// Guard: target/swapchain creation above can trigger GPU-driver Vulkan
	// shader-compiler heap churn that, on some configs (Windows + Leia SR),
	// reuses the display-processor's heap block and corrupts its vtable. A
	// call through a garbage vtable slot (e.g. get_display_pixel_info just
	// below) is a hard crash. Validate here and degrade to passthrough
	// (no weaving) instead of crashing. Do NOT call the DP's destroy — that
	// pointer may itself be garbage; leak the corrupt object on this path.
	if (c->display_processor != NULL && !dp_vtable_looks_sane(c->display_processor)) {
		U_LOG_E("VK display processor vtable corrupt after target creation — "
		        "disabling DP (output will not be woven). Likely a plug-in built "
		        "against a different runtime ABI major (ADR-020), or a driver "
		        "heap-reuse collision.");
		c->display_processor = NULL;
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

	// Compute atlas dimensions from active rendering mode
	uint32_t tile_cols = 2, tile_rows = 1;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			if (mode->tile_columns > 0) {
				tile_cols = mode->tile_columns;
				tile_rows = mode->tile_rows;
			}
		}
	}
	uint32_t atlas_width = tile_cols * view_width;
	uint32_t atlas_height = tile_rows * view_height;

	// Create renderer with active mode atlas
	xrt_result_t xret = comp_vk_native_renderer_create(c, view_width, view_height,
	                                                     atlas_width, atlas_height, &c->renderer);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create VK renderer");
		vk_compositor_destroy(&c->base.base);
		return xret;
	}

	// Clear the atlas transparent (alpha=0) instead of opaque black when a
	// transparent background was requested, so app alpha<1 regions survive
	// to the present (issue #392). Mirrors the DP weave-clear plumbing above.
	comp_vk_native_renderer_set_transparent(c->renderer, transparent_background);

	// Set tile layout from active rendering mode
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count && c->xdev->rendering_modes[idx].tile_columns > 0) {
			comp_vk_native_renderer_set_tile_layout(
			    c->renderer,
			    c->xdev->rendering_modes[idx].tile_columns,
			    c->xdev->rendering_modes[idx].tile_rows);
		}
	}

	// Create HUD overlay for runtime-owned windows
#if defined(XRT_OS_WINDOWS) || defined(XRT_OS_MACOS)
	if (c->owns_window) {
		u_hud_create(&c->hud, c->settings.preferred.width);
	}
#endif

	// Initialize layer accumulator
	memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// #439 Phase 3 — masked 2D-over-3D composite pipelines. The target and the
	// runtime-owned scratch are both B8G8R8A8_UNORM (the windowed target prefers
	// it, the shared IOSurface image is it, and the weave snapshot is a raw copy
	// of the target), so the render-pass formats are fixed. Created eagerly so
	// the zone-mask API (which can be called before the first frame) has its R8
	// raster render pass available.
	c->local2d_initialized = vk_local2d_composite_init(&c->local2d, &c->vk, VK_FORMAT_B8G8R8A8_UNORM,
	                                                   VK_FORMAT_B8G8R8A8_UNORM);
	if (!c->local2d_initialized) {
		U_LOG_W("VK Local2D composite init failed — 2D-over-3D masking disabled this session");
	}

	// Populate supported swapchain formats (Vulkan formats)
	uint32_t format_count = 0;
#ifndef XRT_OS_ANDROID
	// BGRA is the desktop swapchain-native order (DXGI back-buffers and
	// IOSurfaces are BGRA8), so list it first there. On Android it must NOT be
	// advertised: app swapchain images are AHardwareBuffer-backed, and Adreno's
	// gralloc cannot allocate B8G8R8A8 (gralloc rejects format 52 →
	// "isSupported(...,52,...) failed", VK_FORMAT_B8G8R8A8_UNORM probes false).
	// Advertising it makes an app that prefers BGRA (e.g. cube_handle_vk_android)
	// fail xrCreateSwapchain → no reference space → black screen. RGBA8 is the
	// Adreno-allocatable order and is advertised first below. (#496)
	c->base.base.info.formats[format_count++] = VK_FORMAT_B8G8R8A8_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_B8G8R8A8_SRGB;
#endif
	c->base.base.info.formats[format_count++] = VK_FORMAT_R8G8B8A8_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R8G8B8A8_SRGB;
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
	c->base.base.layer_local_2d = vk_compositor_layer_local_2d;
	c->base.base.layer_commit = vk_compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = vk_compositor_layer_commit_with_semaphore;
	c->base.base.destroy = vk_compositor_destroy;

	// Install MCP capture_frame hook + arm the trigger-file path (#210).
	mcp_capture_init(&c->mcp_capture);
	mcp_capture_install(&c->mcp_capture);

	*out_xc = &c->base;

	U_LOG_I("VK native compositor created successfully (%ux%u)",
	        c->settings.preferred.width, c->settings.preferred.height);

	return XRT_SUCCESS;
}

bool
comp_vk_native_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                      struct xrt_eye_positions *out_eye_pos)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		if (xrt_display_processor_get_predicted_eye_positions(c->display_processor, out_eye_pos) &&
		    out_eye_pos->valid) {
			return true;
		}
	}

	return false;
}

bool
comp_vk_native_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                                  float *out_width_m,
                                                  float *out_height_m)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		if (xrt_display_processor_get_display_dimensions(
		        c->display_processor, out_width_m, out_height_m)) {
			return true;
		}
	}

	// Fallback to system compositor info (sim_display DP doesn't implement get_display_dimensions)
	if (c->sys_info_set && c->sys_info.display_width_m > 0.0f && c->sys_info.display_height_m > 0.0f) {
		*out_width_m = c->sys_info.display_width_m;
		*out_height_m = c->sys_info.display_height_m;
		return true;
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
	u_canvas_apply_to_metrics(out_metrics, &c->canvas);
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
		u_canvas_apply_to_metrics(out_metrics, &c->canvas);
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

void
comp_vk_native_compositor_set_sys_info(struct xrt_compositor *xc,
                                        const struct xrt_system_compositor_info *info)
{
	if (xc == NULL || info == NULL) return;
	struct comp_vk_native_compositor *c = vk_comp(xc);
	c->sys_info = *info;
	c->sys_info_set = true;
	c->legacy_app_tile_scaling = info->legacy_app_tile_scaling;
	c->last_3d_mode_index = 1;

	// Legacy apps: fix view dims at the actual recommended size the app was told to render at.
	if (info->legacy_app_tile_scaling && c->renderer != NULL &&
	    info->legacy_view_width_pixels > 0 && info->legacy_view_height_pixels > 0) {
		uint32_t vw = info->legacy_view_width_pixels;
		uint32_t vh = info->legacy_view_height_pixels;
		uint32_t tc = 2, tr = 1;
		if (c->xdev != NULL && c->xdev->hmd != NULL) {
			uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
			if (idx < c->xdev->rendering_mode_count &&
			    c->xdev->rendering_modes[idx].tile_columns > 0) {
				tc = c->xdev->rendering_modes[idx].tile_columns;
				tr = c->xdev->rendering_modes[idx].tile_rows;
			}
		}
		comp_vk_native_renderer_resize(c->renderer, vw, vh, tc * vw, tr * vh);
	}
}

void
comp_vk_native_compositor_set_legacy_app_tile_scaling(struct xrt_compositor *xc, bool legacy)
{
	if (xc == NULL) return;
	struct comp_vk_native_compositor *c = vk_comp(xc);
	c->legacy_app_tile_scaling = legacy;
}

void
comp_vk_native_compositor_set_output_rect(struct xrt_compositor *xc,
                                           int32_t x, int32_t y,
                                           uint32_t w, uint32_t h)
{
	if (xc == NULL) return;
	struct comp_vk_native_compositor *c = vk_comp(xc);
	c->canvas = (struct u_canvas_rect){.valid = true, .x = x, .y = y, .w = w, .h = h};
}

void
comp_vk_native_compositor_set_surround_2d(struct xrt_compositor *xc,
                                           void *shared_handle,
                                           uint32_t w, uint32_t h)
{
	if (xc == NULL) return;
	struct comp_vk_native_compositor *c = vk_comp(xc);
	if (shared_handle == NULL) {
		c->surround_2d = (struct u_surround_2d_handle){0};
		U_LOG_I("VK surround 2D cleared");
		return;
	}
	c->surround_2d.valid = true;
	c->surround_2d.shared_handle = shared_handle;
	c->surround_2d.w = w;
	c->surround_2d.h = h;
	// Phase (post-Phase-C) TODO: VK_KHR_external_memory_win32 import on
	// Windows / VK_KHR_external_memory_fd on POSIX; create a VkImage view
	// for the per-frame surround blit pass.
	U_LOG_I("VK surround 2D registered: handle=%p %ux%u (open + blit pending)",
	        shared_handle, w, h);
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


/*
 *
 * XR_EXT_local_3d_zone — VK consumer leg (#439 Phase 3).
 *
 * Builds the masked-composite mechanism net-new in Vulkan (the D3D11 leg in
 * comp_d3d11_compositor.cpp is the line-by-line algorithm reference). The
 * authored R8 zone mask is rasterized GPU-side (one-shot cmd buffers, since
 * these run outside layer_commit) and snapshotted into a SAMPLED `staged`
 * sibling on submit; the per-frame composite (vk_composite_local_2d, defined
 * above near layer_commit) lerps M*weave + (1-M)*twod into the woven target.
 *
 */

//! Authored zone-mask handle (XR_EXT_local_3d_zone). `tex` is the R8 raster
//! target (COLOR_ATTACHMENT, also app-drawable for Tier-3); `staged` is the
//! SAMPLED snapshot the composite reads, decoupled so in-progress authoring
//! can't tear into a frame.
struct comp_vk_native_zone_mask
{
	uint32_t w, h;
	VkImage tex;
	VkDeviceMemory tex_mem;
	VkImageView tex_view;
	VkFramebuffer fb; //!< Over tex, with local2d.mask_rp.
	VkImage staged;
	VkDeviceMemory staged_mem;
	VkImageView staged_view;
	VkImageLayout tex_layout;
	bool submitted;
};

static const VkImageSubresourceRange k_color_sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

// One-shot cmd buffer for the cross-call zone-mask raster ops (create / set_* /
// submit run outside layer_commit). Record → submit → wait → free, matching the
// rest of this file's single-threaded GPU-op idiom.
static VkCommandBuffer
vk_oneshot_begin(struct comp_vk_native_compositor *c)
{
	struct vk_bundle *vk = &c->vk;
	VkCommandPool pool = (VkCommandPool)(uintptr_t)comp_vk_native_renderer_get_cmd_pool(c->renderer);
	VkCommandBufferAllocateInfo ai = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vk->vkAllocateCommandBuffers(vk->device, &ai, &cmd) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	VkCommandBufferBeginInfo bi = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &bi);
	return cmd;
}

static void
vk_oneshot_end(struct comp_vk_native_compositor *c, VkCommandBuffer cmd)
{
	struct vk_bundle *vk = &c->vk;
	VkCommandPool pool = (VkCommandPool)(uintptr_t)comp_vk_native_renderer_get_cmd_pool(c->renderer);
	vk->vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};
	if (vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS) {
		vk->vkQueueWaitIdle(vk->main_queue->queue);
	}
	vk->vkFreeCommandBuffers(vk->device, pool, 1, &cmd);
}

// Create/reuse an RT-or-sampled image (+view, +optional framebuffer), freeing
// the old one on a dims change. Returns true if usable.
static bool
vk_ensure_rt(struct comp_vk_native_compositor *c,
             VkImage *img,
             VkDeviceMemory *mem,
             VkImageView *view,
             VkFramebuffer *fb,
             uint32_t *cw,
             uint32_t *ch,
             uint32_t w,
             uint32_t h,
             VkFormat fmt,
             VkImageUsageFlags usage,
             VkRenderPass rp,
             const char *what)
{
	struct vk_bundle *vk = &c->vk;
	if (*img != VK_NULL_HANDLE && *cw == w && *ch == h) {
		return true;
	}
	if (fb != NULL && *fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, *fb, NULL);
		*fb = VK_NULL_HANDLE;
	}
	if (*view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, *view, NULL);
		*view = VK_NULL_HANDLE;
	}
	if (*img != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, *img, NULL);
		*img = VK_NULL_HANDLE;
	}
	if (*mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, *mem, NULL);
		*mem = VK_NULL_HANDLE;
	}
	*cw = 0;
	*ch = 0;

	VkExtent2D ext = {w, h};
	if (vk_create_image_simple(vk, ext, fmt, usage, mem, img) != VK_SUCCESS) {
		U_LOG_E("[local2d] %s: image alloc %ux%u failed", what, w, h);
		return false;
	}
	if (vk_create_view(vk, *img, VK_IMAGE_VIEW_TYPE_2D, fmt, k_color_sub, view) != VK_SUCCESS) {
		U_LOG_E("[local2d] %s: view failed", what);
		return false;
	}
	if (rp != VK_NULL_HANDLE && fb != NULL) {
		VkFramebufferCreateInfo fb_ci = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = rp,
		    .attachmentCount = 1,
		    .pAttachments = view,
		    .width = w,
		    .height = h,
		    .layers = 1,
		};
		if (vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, fb) != VK_SUCCESS) {
			U_LOG_E("[local2d] %s: framebuffer failed", what);
			return false;
		}
	}
	*cw = w;
	*ch = h;
	return true;
}

static void
vk_destroy_rt(struct comp_vk_native_compositor *c,
              VkImage *img,
              VkDeviceMemory *mem,
              VkImageView *view,
              VkFramebuffer *fb)
{
	struct vk_bundle *vk = &c->vk;
	if (fb != NULL && *fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, *fb, NULL);
		*fb = VK_NULL_HANDLE;
	}
	if (*view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, *view, NULL);
		*view = VK_NULL_HANDLE;
	}
	if (*img != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, *img, NULL);
		*img = VK_NULL_HANDLE;
	}
	if (*mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, *mem, NULL);
		*mem = VK_NULL_HANDLE;
	}
}

// #439 Phase 2/3 effective canvas: an active mask or a Local2D-carrying frame
// supersedes the canvas output rect with the client-window rect (the weave
// region, composite region, and view dims share one authority). With neither,
// returns c->canvas verbatim so the no-mask path is unchanged.
static struct u_canvas_rect
vk_effective_canvas(struct comp_vk_native_compositor *c)
{
	if (c->active_zone_mask == NULL && !c->local_2d_last_frame) {
		return c->canvas;
	}
	struct u_canvas_rect win = {0};
#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT r;
		if (GetClientRect((HWND)c->hwnd, &r) && r.right > 0 && r.bottom > 0) {
			win.valid = true;
			win.x = 0;
			win.y = 0;
			win.w = (uint32_t)r.right;
			win.h = (uint32_t)r.bottom;
			return win;
		}
	}
#endif
	return win; // invalid → existing full-target fallbacks
}

// Compute the window region inside the (worst-case-allocated) dst surface.
static void
vk_window_region(struct comp_vk_native_compositor *c, uint32_t dst_w, uint32_t dst_h, uint32_t *rw, uint32_t *rh)
{
	*rw = dst_w;
	*rh = dst_h;
#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT r;
		if (GetClientRect((HWND)c->hwnd, &r) && r.right > 0 && r.bottom > 0) {
			*rw = ((uint32_t)r.right < dst_w) ? (uint32_t)r.right : dst_w;
			*rh = ((uint32_t)r.bottom < dst_h) ? (uint32_t)r.bottom : dst_h;
		}
	}
#endif
}

// #439 Phase 3 — masked 2D-over-3D composite, POST-weave. The DP has woven the
// 3D into `dst`; this overlays the frame's 2D content where the zone mask says
// "2D". Runs only when the frame carries Local2D layers (the `twod` source); a
// pure explicit-mask-only frame has no 2D pixels on VK (no surround) and is
// skipped. Returns true if it composited (dst left in dst_outgoing) or false if
// it skipped (dst untouched, still in dst_incoming).
static bool
vk_composite_local_2d(struct comp_vk_native_compositor *c,
                      VkCommandBuffer cmd,
                      VkImage dst_image,
                      VkImageView dst_view,
                      uint32_t dst_w,
                      uint32_t dst_h,
                      VkImageLayout dst_incoming,
                      VkImageLayout dst_outgoing)
{
	struct vk_bundle *vk = &c->vk;

	if (!c->local2d_initialized || !c->local_2d_last_frame || dst_image == VK_NULL_HANDLE ||
	    dst_view == VK_NULL_HANDLE) {
		return false;
	}

	uint32_t region_w, region_h;
	vk_window_region(c, dst_w, dst_h, &region_w, &region_h);
	if (region_w == 0 || region_h == 0) {
		return false;
	}

	const VkFormat scratch_fmt = VK_FORMAT_B8G8R8A8_UNORM; // matches target (raw weave copy)
	const VkFormat mask_fmt = VK_FORMAT_R8_UNORM;

	// Collect this frame's Local2D layer rects (for the implicit mask) once.
	struct xrt_rect rects[XRT_MAX_LAYERS];
	uint32_t rect_count = 0;
	for (uint32_t i = 0; i < c->layer_accum.layer_count && rect_count < XRT_MAX_LAYERS; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			rects[rect_count++] = c->layer_accum.layers[i].data.local_2d.rect;
		}
	}
	if (rect_count == 0) {
		return false; // local_2d_last_frame stale vs accum — nothing to draw
	}

	// Resolve the `twod` source: flatten the Local2D layers into local2d_scratch.
	if (!vk_ensure_rt(c, &c->local2d_scratch, &c->local2d_scratch_mem, &c->local2d_scratch_view,
	                  &c->local2d_scratch_fb, &c->local2d_scratch_w, &c->local2d_scratch_h, region_w,
	                  region_h, scratch_fmt,
	                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                  c->local2d.flatten_rp, "local2d scratch")) {
		return false;
	}
	// Weave snapshot scratch (the lerp reads a copy; dst is RT≠SRV).
	if (!vk_ensure_rt(c, &c->weave_scratch, &c->weave_scratch_mem, &c->weave_scratch_view, NULL,
	                  &c->weave_scratch_w, &c->weave_scratch_h, region_w, region_h, scratch_fmt,
	                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_NULL_HANDLE,
	                  "local2d weave")) {
		return false;
	}

	vk_local2d_composite_begin_frame(&c->local2d, vk);

	// --- twod: clear local2d_scratch transparent, flatten layers, → SHADER_READ.
	vk_cmd_image_barrier_locked(vk, cmd, c->local2d_scratch, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            k_color_sub);
	VkClearColorValue transparent = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
	vk->vkCmdClearColorImage(cmd, c->local2d_scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent, 1,
	                         &k_color_sub);
	vk_cmd_image_barrier_locked(vk, cmd, c->local2d_scratch, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);

	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		struct xrt_swapchain *sc = layer->sc_array[0];
		if (sc == NULL) {
			continue;
		}
		uint32_t img_idx = layer->data.local_2d.sub.image_index;
		// sRGB-passthrough: sample the layer's own view (the projection path
		// samples the same). UNORM-sibling decode is a follow-up if a layer
		// ever uses an _SRGB swapchain.
		VkImageView src_view =
		    (VkImageView)(uintptr_t)comp_vk_native_swapchain_get_image_view(sc, img_idx);
		if (src_view == VK_NULL_HANDLE) {
			continue;
		}

		const struct xrt_rect *dr = &layer->data.local_2d.rect;
		int32_t dx = dr->offset.w, dy = dr->offset.h, dw = dr->extent.w, dh = dr->extent.h;
		if (dw <= 0 || dh <= 0) {
			continue;
		}
		int32_t x0 = dx < 0 ? 0 : dx;
		int32_t y0 = dy < 0 ? 0 : dy;
		int32_t x1 = (dx + dw) > (int32_t)region_w ? (int32_t)region_w : (dx + dw);
		int32_t y1 = (dy + dh) > (int32_t)region_h ? (int32_t)region_h : (dy + dh);
		if (x1 <= x0 || y1 <= y0) {
			continue;
		}
		float fx0 = (float)(x0 - dx) / (float)dw;
		float fy0 = (float)(y0 - dy) / (float)dh;
		float fx1 = (float)(x1 - dx) / (float)dw;
		float fy1 = (float)(y1 - dy) / (float)dh;

		struct xrt_normalized_rect nr = layer->data.local_2d.sub.norm_rect;
		if (nr.w <= 0.0f || nr.h <= 0.0f) {
			nr.x = 0.0f;
			nr.y = 0.0f;
			nr.w = 1.0f;
			nr.h = 1.0f;
		}
		float src_x = nr.x + nr.w * fx0;
		float src_w = nr.w * (fx1 - fx0);
		float src_y, src_h;
		if (layer->data.flip_y) {
			src_y = nr.y + nr.h * (1.0f - fy0);
			src_h = -(nr.h * (fy1 - fy0));
		} else {
			src_y = nr.y + nr.h * fy0;
			src_h = nr.h * (fy1 - fy0);
		}
		bool unpremult = (layer->data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) != 0;

		vk_local2d_composite_flatten_draw(&c->local2d, vk, cmd, c->local2d_scratch_fb, region_w,
		                                  region_h, src_view, x0, y0, (uint32_t)(x1 - x0),
		                                  (uint32_t)(y1 - y0), src_x, src_y, src_w, src_h, unpremult);
	}
	vk_cmd_image_barrier_locked(vk, cmd, c->local2d_scratch, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);

	// --- mask: explicit submitted mask wins; else rasterize the implicit one.
	VkImageView mask_view = VK_NULL_HANDLE;
	struct comp_vk_native_zone_mask *emask = c->active_zone_mask;
	if (emask != NULL && emask->submitted && emask->staged_view != VK_NULL_HANDLE) {
		mask_view = emask->staged_view; // already SHADER_READ from submit
	} else {
		if (!vk_ensure_rt(c, &c->implicit_mask_tex, &c->implicit_mask_mem, &c->implicit_mask_view,
		                  &c->implicit_mask_fb, &c->implicit_mask_w, &c->implicit_mask_h, region_w,
		                  region_h, mask_fmt,
		                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                  c->local2d.mask_rp, "implicit mask")) {
			return false;
		}
		vk_cmd_image_barrier_locked(vk, cmd, c->implicit_mask_tex, 0,
		                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
		// Inverse of set_rects: M=1 (keep weave) everywhere, M=0 inside the
		// Local2D rects (show the flattened 2D there).
		vk_local2d_composite_raster_mask(&c->local2d, vk, cmd, c->implicit_mask_fb, region_w, region_h,
		                                 1.0f, rects, rect_count, 0.0f);
		vk_cmd_image_barrier_locked(vk, cmd, c->implicit_mask_tex, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                            VK_ACCESS_SHADER_READ_BIT,
		                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);
		mask_view = c->implicit_mask_view;
	}

	// --- weave snapshot: dst → TRANSFER_SRC, copy region → weave_scratch, dst
	// → COLOR_ATTACHMENT for the composite render pass.
	vk_cmd_image_barrier_locked(vk, cmd, dst_image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_ACCESS_TRANSFER_READ_BIT, dst_incoming,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_TRANSFER_BIT, k_color_sub);
	vk_cmd_image_barrier_locked(vk, cmd, c->weave_scratch, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            k_color_sub);
	VkImageCopy copy = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffset = {0, 0, 0},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffset = {0, 0, 0},
	    .extent = {region_w, region_h, 1},
	};
	vk->vkCmdCopyImage(cmd, dst_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->weave_scratch,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	vk_cmd_image_barrier_locked(vk, cmd, c->weave_scratch, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);
	vk_cmd_image_barrier_locked(vk, cmd, dst_image, VK_ACCESS_TRANSFER_READ_BIT,
	                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);

	// --- composite target framebuffer (over the rotating dst view).
	if (c->composite_target_fb_view != dst_view || c->composite_fb_w != dst_w ||
	    c->composite_fb_h != dst_h) {
		if (c->composite_target_fb != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, c->composite_target_fb, NULL);
			c->composite_target_fb = VK_NULL_HANDLE;
		}
		VkFramebufferCreateInfo fb_ci = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = c->local2d.composite_rp,
		    .attachmentCount = 1,
		    .pAttachments = &dst_view,
		    .width = dst_w,
		    .height = dst_h,
		    .layers = 1,
		};
		if (vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &c->composite_target_fb) != VK_SUCCESS) {
			return false;
		}
		c->composite_target_fb_view = dst_view;
		c->composite_fb_w = dst_w;
		c->composite_fb_h = dst_h;
	}

	// Effective canvas (window rect while active) — carried into the CB; the
	// mask-lerp path ignores it, kept coherent anyway.
	struct u_canvas_rect ec = vk_effective_canvas(c);
	int32_t cx = ec.valid ? ec.x : 0;
	int32_t cy = ec.valid ? ec.y : 0;
	uint32_t cw = ec.valid ? ec.w : region_w;
	uint32_t ch = ec.valid ? ec.h : region_h;

	vk_local2d_composite_draw(&c->local2d, vk, cmd, c->composite_target_fb, dst_w, dst_h,
	                          c->local2d_scratch_view, mask_view, c->weave_scratch_view, region_w,
	                          region_h, cx, cy, cw, ch);

	// One-shot lifecycle log (NOT per-frame): proves the masked composite ran +
	// which mask source resolved. WARN so it survives the hot-path INFO filter.
	{
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("VK Local2D composite: %ux%u region, %u layer(s), %s mask", region_w,
			        region_h, rect_count,
			        (emask != NULL && emask->submitted) ? "explicit" : "implicit");
		}
	}

	// dst → outgoing for the downstream stage (HUD / present / readback).
	vk_cmd_image_barrier_locked(vk, cmd, dst_image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
	                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dst_outgoing,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, k_color_sub);
	return true;
}

// Release all #439 Phase 3 composite + zone state (called from destroy).
static void
vk_release_local2d_state(struct comp_vk_native_compositor *c)
{
	struct vk_bundle *vk = &c->vk;
	if (c->composite_target_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, c->composite_target_fb, NULL);
		c->composite_target_fb = VK_NULL_HANDLE;
		c->composite_target_fb_view = VK_NULL_HANDLE;
	}
	vk_destroy_rt(c, &c->local2d_scratch, &c->local2d_scratch_mem, &c->local2d_scratch_view,
	              &c->local2d_scratch_fb);
	vk_destroy_rt(c, &c->weave_scratch, &c->weave_scratch_mem, &c->weave_scratch_view, NULL);
	vk_destroy_rt(c, &c->implicit_mask_tex, &c->implicit_mask_mem, &c->implicit_mask_view,
	              &c->implicit_mask_fb);
	if (c->local2d_initialized) {
		vk_local2d_composite_fini(&c->local2d, vk);
		c->local2d_initialized = false;
	}
}

xrt_result_t
comp_vk_native_compositor_zone_mask_create(struct xrt_compositor *xc, uint32_t w, uint32_t h, void **out_mask)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	if (out_mask == NULL || !c->local2d_initialized) {
		return XRT_ERROR_ALLOCATION;
	}

	// 0 → runtime chooses the client-window dims (the mask is window-sized).
	if (w == 0 || h == 0) {
#ifdef XRT_OS_WINDOWS
		if (c->hwnd != NULL) {
			RECT r;
			if (GetClientRect((HWND)c->hwnd, &r) && r.right > 0 && r.bottom > 0) {
				w = (uint32_t)r.right;
				h = (uint32_t)r.bottom;
			}
		}
#endif
		if (w == 0 || h == 0) {
			w = c->settings.preferred.width;
			h = c->settings.preferred.height;
		}
	}
	if (w == 0 || h == 0) {
		U_LOG_E("zone_mask_create: no window/surface to derive mask dims from");
		return XRT_ERROR_ALLOCATION;
	}

	struct comp_vk_native_zone_mask *mask = U_TYPED_CALLOC(struct comp_vk_native_zone_mask);
	if (mask == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	mask->w = w;
	mask->h = h;
	mask->tex_layout = VK_IMAGE_LAYOUT_UNDEFINED;

	uint32_t tw = 0, th = 0, sw = 0, sh = 0;
	bool ok = vk_ensure_rt(c, &mask->tex, &mask->tex_mem, &mask->tex_view, &mask->fb, &tw, &th, w, h,
	                       VK_FORMAT_R8_UNORM,
	                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                       c->local2d.mask_rp, "zone_mask tex");
	VkFramebuffer no_fb = VK_NULL_HANDLE;
	ok = ok && vk_ensure_rt(c, &mask->staged, &mask->staged_mem, &mask->staged_view, &no_fb, &sw, &sh, w, h,
	                        VK_FORMAT_R8_UNORM,
	                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_NULL_HANDLE,
	                        "zone_mask staged");
	if (!ok) {
		vk_destroy_rt(c, &mask->staged, &mask->staged_mem, &mask->staged_view, NULL);
		vk_destroy_rt(c, &mask->tex, &mask->tex_mem, &mask->tex_view, &mask->fb);
		free(mask);
		return XRT_ERROR_ALLOCATION;
	}

	// Default all-3D (M=1): an unauthored-but-submitted mask degrades to the
	// full weave, never a blanked canvas.
	VkCommandBuffer cmd = vk_oneshot_begin(c);
	if (cmd != VK_NULL_HANDLE) {
		vk_cmd_image_barrier_locked(vk, cmd, mask->tex, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
		vk_local2d_composite_raster_mask(&c->local2d, vk, cmd, mask->fb, w, h, 1.0f, NULL, 0, 1.0f);
		vk_oneshot_end(c, cmd);
		mask->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	U_LOG_W("zone_mask_create: %ux%u (client-window px)", w, h);
	*out_mask = mask;
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_compositor_zone_mask_set_whole(struct xrt_compositor *xc, void *mask_ptr, bool enable_3d)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	struct comp_vk_native_zone_mask *mask = (struct comp_vk_native_zone_mask *)mask_ptr;
	if (mask == NULL || mask->fb == VK_NULL_HANDLE) {
		return XRT_ERROR_ALLOCATION;
	}
	VkCommandBuffer cmd = vk_oneshot_begin(c);
	if (cmd == VK_NULL_HANDLE) {
		return XRT_ERROR_VULKAN;
	}
	vk_cmd_image_barrier_locked(vk, cmd, mask->tex, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            mask->tex_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
	vk_local2d_composite_raster_mask(&c->local2d, vk, cmd, mask->fb, mask->w, mask->h,
	                                 enable_3d ? 1.0f : 0.0f, NULL, 0, 0.0f);
	mask->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	vk_oneshot_end(c, cmd);
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                              void *mask_ptr,
                                              uint32_t count,
                                              const struct xrt_rect *rects)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	struct comp_vk_native_zone_mask *mask = (struct comp_vk_native_zone_mask *)mask_ptr;
	if (mask == NULL || mask->fb == VK_NULL_HANDLE || (count > 0 && rects == NULL)) {
		return XRT_ERROR_ALLOCATION;
	}
	VkCommandBuffer cmd = vk_oneshot_begin(c);
	if (cmd == VK_NULL_HANDLE) {
		return XRT_ERROR_VULKAN;
	}
	vk_cmd_image_barrier_locked(vk, cmd, mask->tex, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            mask->tex_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
	// M=0 everywhere, then M=1 inside each rect.
	vk_local2d_composite_raster_mask(&c->local2d, vk, cmd, mask->fb, mask->w, mask->h, 0.0f, rects, count,
	                                 1.0f);
	mask->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	vk_oneshot_end(c, cmd);
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_compositor_zone_mask_acquire_rt(struct xrt_compositor *xc,
                                               void *mask_ptr,
                                               void **out_image,
                                               void **out_image_view,
                                               uint32_t *out_w,
                                               uint32_t *out_h)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	struct comp_vk_native_zone_mask *mask = (struct comp_vk_native_zone_mask *)mask_ptr;
	if (mask == NULL || mask->tex == VK_NULL_HANDLE || out_image == NULL || out_image_view == NULL ||
	    out_w == NULL || out_h == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	// Tier-3: hand the R8 raster image to the app to draw into. Put it in
	// COLOR_ATTACHMENT_OPTIMAL (where the app's own render pass expects it);
	// submit then snapshots it to staged.
	VkCommandBuffer cmd = vk_oneshot_begin(c);
	if (cmd != VK_NULL_HANDLE) {
		vk_cmd_image_barrier_locked(vk, cmd, mask->tex, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                            mask->tex_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
		vk_oneshot_end(c, cmd);
		mask->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	*out_image = (void *)(uintptr_t)mask->tex;
	*out_image_view = (void *)(uintptr_t)mask->tex_view;
	*out_w = mask->w;
	*out_h = mask->h;
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	struct comp_vk_native_zone_mask *mask = (struct comp_vk_native_zone_mask *)mask_ptr;
	if (mask == NULL || mask->tex == VK_NULL_HANDLE || mask->staged == VK_NULL_HANDLE) {
		return XRT_ERROR_ALLOCATION;
	}
	VkCommandBuffer cmd = vk_oneshot_begin(c);
	if (cmd == VK_NULL_HANDLE) {
		return XRT_ERROR_VULKAN;
	}
	// Snapshot tex → staged so in-progress authoring can't tear into a frame.
	vk_cmd_image_barrier_locked(vk, cmd, mask->tex, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_ACCESS_TRANSFER_READ_BIT, mask->tex_layout,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                            VK_PIPELINE_STAGE_TRANSFER_BIT, k_color_sub);
	vk_cmd_image_barrier_locked(vk, cmd, mask->staged, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            k_color_sub);
	VkImageCopy copy = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .extent = {mask->w, mask->h, 1},
	};
	vk->vkCmdCopyImage(cmd, mask->tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mask->staged,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	vk_cmd_image_barrier_locked(vk, cmd, mask->staged, VK_ACCESS_TRANSFER_WRITE_BIT,
	                            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, k_color_sub);
	// tex returns to COLOR_ATTACHMENT for the next authoring round.
	vk_cmd_image_barrier_locked(vk, cmd, mask->tex, VK_ACCESS_TRANSFER_READ_BIT,
	                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, k_color_sub);
	vk_oneshot_end(c, cmd);

	mask->tex_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	mask->submitted = true;
	c->active_zone_mask = mask; // sticky last-submit-wins
	return XRT_SUCCESS;
}

void
comp_vk_native_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;
	struct comp_vk_native_zone_mask *mask = (struct comp_vk_native_zone_mask *)mask_ptr;
	if (mask == NULL) {
		return;
	}
	vk->vkDeviceWaitIdle(vk->device); // mask may be in flight
	if (c->active_zone_mask == mask) {
		c->active_zone_mask = NULL; // revert to implicit / legacy behavior
	}
	vk_destroy_rt(c, &mask->staged, &mask->staged_mem, &mask->staged_view, NULL);
	vk_destroy_rt(c, &mask->tex, &mask->tex_mem, &mask->tex_view, &mask->fb);
	free(mask);
}

// #439 Phase 3 Q4 — current recommended per-view render size (the renderer's
// view dims, recomputed each frame from the effective canvas). oxr fires
// XrEventDataLocal3DZoneViewSizeChangedEXT when this changes.
bool
comp_vk_native_compositor_get_recommended_view_size(struct xrt_compositor *xc, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	if (out_w == NULL || out_h == NULL || c->renderer == NULL) {
		return false;
	}
	uint32_t vw = 0, vh = 0;
	comp_vk_native_renderer_get_view_dimensions(c->renderer, &vw, &vh);
	if (vw == 0 || vh == 0) {
		return false;
	}
	*out_w = vw;
	*out_h = vh;
	return true;
}
