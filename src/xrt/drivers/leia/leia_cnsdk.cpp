// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CNSDK wrapper implementation — isolates CNSDK headers
 *         from the rest of the compositor.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_cnsdk.h"

#include "util/u_logging.h"

#include <leia/sdk/core.h>
#include <leia/sdk/core.interlacer.vulkan.h>
#include <leia/common/version.h>

#ifdef XRT_OS_ANDROID
#include "android/android_globals.h"
#endif

#include <stdlib.h>


/*
 *
 * Internal struct.
 *
 */

struct leia_cnsdk
{
	struct leia_core *core;
	struct leia_interlacer *interlacer;       //!< Per-view interlacer (use_atlas=false).
	struct leia_interlacer *interlacer_atlas; //!< Atlas-mode interlacer (use_atlas=true).
};


/*
 *
 * Public API.
 *
 */

extern "C" xrt_result_t
leia_cnsdk_create(struct leia_cnsdk **out_cnsdk)
{
	leia_platform_on_library_load();

	struct leia_core_init_configuration *config = leia_core_init_configuration_alloc(CNSDK_VERSION);

#ifdef XRT_OS_ANDROID
	leia_core_init_configuration_set_platform_android_java_vm(config, (JavaVM *)android_globals_get_vm());
	leia_core_init_configuration_set_platform_android_handle(
	    config, LEIA_CORE_ANDROID_HANDLE_ACTIVITY, (jobject)android_globals_get_activity());
#endif

	leia_core_init_configuration_set_platform_log_level(config, kLeiaLogLevelTrace);
	leia_core_init_configuration_set_enable_validation(config, true);

	struct leia_core *core = leia_core_init_async(config);
	leia_core_init_configuration_free(config);

	if (core == NULL) {
		U_LOG_E("leia_core_init_async failed");
		*out_cnsdk = NULL;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	leia_core_set_backlight(core, true);

	struct leia_cnsdk *cnsdk = (struct leia_cnsdk *)calloc(1, sizeof(*cnsdk));
	cnsdk->core = core;
	cnsdk->interlacer = NULL;

	*out_cnsdk = cnsdk;
	return XRT_SUCCESS;
}

extern "C" void
leia_cnsdk_destroy(struct leia_cnsdk **cnsdk_ptr)
{
	if (cnsdk_ptr == NULL || *cnsdk_ptr == NULL) {
		return;
	}

	struct leia_cnsdk *cnsdk = *cnsdk_ptr;

	if (cnsdk->interlacer_atlas != NULL) {
		leia_interlacer_release(cnsdk->interlacer_atlas);
		cnsdk->interlacer_atlas = NULL;
	}

	if (cnsdk->interlacer != NULL) {
		leia_interlacer_release(cnsdk->interlacer);
		cnsdk->interlacer = NULL;
	}

	if (cnsdk->core != NULL) {
		leia_core_release(cnsdk->core);
		cnsdk->core = NULL;
	}

	leia_platform_on_library_unload();

	free(cnsdk);
	*cnsdk_ptr = NULL;
}

extern "C" bool
leia_cnsdk_is_initialized(struct leia_cnsdk *cnsdk)
{
	if (cnsdk == NULL || cnsdk->core == NULL) {
		return false;
	}
	return leia_core_is_initialized(cnsdk->core);
}

extern "C" void
leia_cnsdk_weave(struct leia_cnsdk *cnsdk,
                 VkDevice device,
                 VkPhysicalDevice physDev,
                 VkImageView left,
                 VkImageView right,
                 VkFormat targetFmt,
                 uint32_t w,
                 uint32_t h,
                 VkFramebuffer fb,
                 VkImage targetImage)
{
	if (cnsdk == NULL) {
		return;
	}

	// Lazy interlacer creation — wait until the core is ready.
	if (cnsdk->interlacer == NULL && leia_core_is_initialized(cnsdk->core)) {
		struct leia_interlacer_init_configuration *ic = leia_interlacer_init_configuration_alloc();
		leia_interlacer_init_configuration_set_use_atlas_for_views(ic, false);
		cnsdk->interlacer = leia_interlacer_vulkan_initialize(
		    cnsdk->core, ic, device, physDev, VK_FORMAT_B8G8R8A8_SRGB,
		    targetFmt, VK_FORMAT_D32_SFLOAT, 3);
		leia_interlacer_init_configuration_free(ic);
	}

	if (cnsdk->interlacer == NULL) {
		return;
	}

	leia_interlacer_set_flip_input_uv_vertical(cnsdk->interlacer, true);
	leia_interlacer_vulkan_set_view_for_texture_array(cnsdk->interlacer, 0, left, 0);
	leia_interlacer_vulkan_set_view_for_texture_array(cnsdk->interlacer, 1, right, 0);
	leia_interlacer_set_shader_debug_mode(cnsdk->interlacer, LEIA_SHADER_DEBUG_MODE_NONE);
	leia_interlacer_vulkan_do_post_process(
	    cnsdk->interlacer, w, h, false, fb, targetImage, NULL, NULL, NULL, 0);
}

extern "C" void
leia_cnsdk_weave_atlas(struct leia_cnsdk *cnsdk,
                       VkDevice device,
                       VkPhysicalDevice physDev,
                       VkImageView atlas_view,
                       VkFormat targetFmt,
                       uint32_t atlas_width,
                       uint32_t atlas_height,
                       uint32_t view_count,
                       VkFramebuffer fb,
                       VkImage targetImage)
{
	if (cnsdk == NULL) {
		return;
	}

	// Lazy atlas-mode interlacer creation — wait until the core is ready.
	// This is a separate interlacer from the per-view one because
	// use_atlas_for_views must be set at init time.
	if (cnsdk->interlacer_atlas == NULL && leia_core_is_initialized(cnsdk->core)) {
		struct leia_interlacer_init_configuration *ic = leia_interlacer_init_configuration_alloc();
		leia_interlacer_init_configuration_set_use_atlas_for_views(ic, true);
		cnsdk->interlacer_atlas = leia_interlacer_vulkan_initialize(
		    cnsdk->core, ic, device, physDev, VK_FORMAT_B8G8R8A8_SRGB,
		    targetFmt, VK_FORMAT_D32_SFLOAT, 3);
		leia_interlacer_init_configuration_free(ic);

		if (cnsdk->interlacer_atlas != NULL) {
			U_LOG_W("CNSDK: created atlas-mode Vulkan interlacer");
		}
	}

	if (cnsdk->interlacer_atlas == NULL) {
		return;
	}

	// Pass atlas view as source — CNSDK atlas mode handles the SBS split
	leia_interlacer_set_flip_input_uv_vertical(cnsdk->interlacer_atlas, true);
	leia_interlacer_vulkan_set_source_views(cnsdk->interlacer_atlas,
	    VK_NULL_HANDLE, atlas_view, 0, 0);
	leia_interlacer_set_shader_debug_mode(cnsdk->interlacer_atlas, LEIA_SHADER_DEBUG_MODE_NONE);
	leia_interlacer_vulkan_do_post_process(
	    cnsdk->interlacer_atlas, atlas_width, atlas_height, false,
	    fb, targetImage, NULL, NULL, NULL, 0);
}
