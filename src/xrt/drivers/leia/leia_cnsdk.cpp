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
#include <leia/device/config.h>

#ifdef XRT_OS_ANDROID
#include "android/android_globals.h"
#endif

#include <atomic>
#include <chrono>
#include <thread>


/*
 *
 * Internal struct.
 *
 */

struct leia_cnsdk
{
	struct leia_core *core{nullptr};
	struct leia_interlacer *interlacer{nullptr};

	// Face-tracking startup is offloaded to a worker thread because
	// leia_core_enable_face_tracking is heavy (CNSDK docs explicitly warn
	// against the main thread). Worker pattern:
	//   - Spawn in leia_cnsdk_create.
	//   - Worker polls leia_core_is_initialized until ready, then snapshots
	//     the camera center from leia_device_config (needed to convert
	//     CNSDK's camera-relative face positions into display-relative),
	//     calls enable + start face tracking, then sets
	//     face_tracking_started and exits.
	//   - Destroy sets shutting_down to ask the worker to bail if it's
	//     still in the polling phase, then joins.
	//
	// camera_center_{x,y,z}_m: cached at worker init. The `_m` suffix
	// reminds the reader they're already mm→m converted before storage.
	// These are read by leia_cnsdk_get_primary_face on the render thread
	// only after face_tracking_started.load(acquire) returns true — the
	// happens-before ordering of the atomic gives the read visibility on
	// the worker's writes.
	std::atomic<bool> face_tracking_started{false};
	std::atomic<bool> shutting_down{false};
	std::thread worker;
	float camera_center_x_m{0.0f};
	float camera_center_y_m{0.0f};
	float camera_center_z_m{0.0f};
};


/*
 *
 * Private helpers.
 *
 */

namespace {

void
face_tracking_worker(struct leia_cnsdk *cnsdk)
{
	using namespace std::chrono_literals;

	// Phase 1: wait for the async core init to complete. Poll every 50 ms;
	// honor shutdown promptly.
	while (!cnsdk->shutting_down.load(std::memory_order_acquire)) {
		if (cnsdk->core != nullptr && leia_core_is_initialized(cnsdk->core)) {
			break;
		}
		std::this_thread::sleep_for(50ms);
	}
	if (cnsdk->shutting_down.load(std::memory_order_acquire)) {
		return;
	}

	// Phase 2a: snapshot the camera center from the device config so the
	// main-thread fast path can translate face positions without
	// re-acquiring the (probably non-thread-safe) device config every
	// frame. CNSDK's coordinate system has the camera at the origin and
	// uses millimeters — we cache the offset in meters.
	struct leia_device_config *cfg = leia_core_get_device_config(cnsdk->core);
	if (cfg != NULL) {
		cnsdk->camera_center_x_m = cfg->cameraCenterX / 1000.0f;
		cnsdk->camera_center_y_m = cfg->cameraCenterY / 1000.0f;
		cnsdk->camera_center_z_m = cfg->cameraCenterZ / 1000.0f;
		leia_core_release_device_config(cnsdk->core, cfg);
	} else {
		U_LOG_W("leia_core_get_device_config failed in worker; camera center stays 0");
	}

	// Phase 2b: heavy enable + start. Single call, can't be interrupted —
	// destroy will block on the join until this returns.
	if (!leia_core_enable_face_tracking(cnsdk->core, true)) {
		U_LOG_W("leia_core_enable_face_tracking failed (worker)");
		return;
	}
	leia_core_start_face_tracking(cnsdk->core, true);

	cnsdk->face_tracking_started.store(true, std::memory_order_release);
	U_LOG_W("CNSDK face tracking started (worker)");
}

} // namespace


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

	auto *cnsdk = new struct leia_cnsdk();
	cnsdk->core = core;
	cnsdk->worker = std::thread(face_tracking_worker, cnsdk);

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

	// Signal the worker, then join. If the worker is mid-enable_face_tracking
	// we have to wait it out — CNSDK provides no interruption hook.
	cnsdk->shutting_down.store(true, std::memory_order_release);
	if (cnsdk->worker.joinable()) {
		cnsdk->worker.join();
	}

	if (cnsdk->interlacer != NULL) {
		// CNSDK 0.7.28 renamed the per-interlacer release; the core owns the
		// interlacer lifetime, so the shutdown call needs both handles.
		leia_interlacer_shutdown(cnsdk->core, cnsdk->interlacer);
		cnsdk->interlacer = NULL;
	}

	if (cnsdk->core != NULL) {
		// CNSDK 0.7.28 renamed leia_core_release → leia_core_shutdown.
		leia_core_shutdown(cnsdk->core);
		cnsdk->core = NULL;
	}

	leia_platform_on_library_unload();

	delete cnsdk;
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

extern "C" bool
leia_cnsdk_get_display_metrics(struct leia_cnsdk *cnsdk,
                               float *out_width_m,
                               float *out_height_m,
                               uint32_t *out_pixel_w,
                               uint32_t *out_pixel_h)
{
	if (cnsdk == NULL || cnsdk->core == NULL) {
		return false;
	}
	if (!leia_core_is_initialized(cnsdk->core)) {
		return false;
	}

	struct leia_device_config *cfg = leia_core_get_device_config(cnsdk->core);
	if (cfg == NULL) {
		return false;
	}

	if (out_width_m != NULL) {
		*out_width_m = (float)cfg->displaySizeInMm[0] / 1000.0f;
	}
	if (out_height_m != NULL) {
		*out_height_m = (float)cfg->displaySizeInMm[1] / 1000.0f;
	}
	if (out_pixel_w != NULL) {
		*out_pixel_w = (uint32_t)cfg->panelResolution[0];
	}
	if (out_pixel_h != NULL) {
		*out_pixel_h = (uint32_t)cfg->panelResolution[1];
	}

	leia_core_release_device_config(cnsdk->core, cfg);
	return true;
}

extern "C" bool
leia_cnsdk_ensure_face_tracking_started(struct leia_cnsdk *cnsdk)
{
	// Worker thread handles enable + start; this is now a non-blocking
	// status check.
	if (cnsdk == NULL) {
		return false;
	}
	return cnsdk->face_tracking_started.load(std::memory_order_acquire);
}

extern "C" bool
leia_cnsdk_ensure_interlacer(struct leia_cnsdk *cnsdk,
                              VkDevice device,
                              VkPhysicalDevice physDev,
                              VkFormat targetFmt)
{
	if (cnsdk == NULL || cnsdk->core == NULL) {
		return false;
	}
	if (cnsdk->interlacer != NULL) {
		return true;
	}
	if (!leia_core_is_initialized(cnsdk->core)) {
		return false;
	}

	struct leia_interlacer_init_configuration *ic = leia_interlacer_init_configuration_alloc();
	leia_interlacer_init_configuration_set_use_atlas_for_views(ic, false);
	cnsdk->interlacer = leia_interlacer_vulkan_initialize(
	    cnsdk->core, ic, device, physDev, VK_FORMAT_B8G8R8A8_SRGB,
	    targetFmt, VK_FORMAT_D32_SFLOAT, 3);
	leia_interlacer_init_configuration_free(ic);

	return cnsdk->interlacer != NULL;
}

extern "C" bool
leia_cnsdk_get_primary_face(struct leia_cnsdk *cnsdk,
                            float *out_x,
                            float *out_y,
                            float *out_z)
{
	if (cnsdk == NULL || cnsdk->core == NULL ||
	    !cnsdk->face_tracking_started.load(std::memory_order_acquire)) {
		return false;
	}

	float position[3] = {0, 0, 0};
	struct leia_float_slice slice = {position, 3};
	if (!leia_core_get_primary_face(cnsdk->core, slice)) {
		return false;
	}

	// CNSDK returns millimeters relative to the camera. xrt_eye_position
	// wants meters relative to the display center, so divide by 1000 then
	// subtract the cached camera center (also already in meters).
	const float pos_x_m = position[0] / 1000.0f - cnsdk->camera_center_x_m;
	const float pos_y_m = position[1] / 1000.0f - cnsdk->camera_center_y_m;
	const float pos_z_m = position[2] / 1000.0f - cnsdk->camera_center_z_m;

	if (out_x != NULL) { *out_x = pos_x_m; }
	if (out_y != NULL) { *out_y = pos_y_m; }
	if (out_z != NULL) { *out_z = pos_z_m; }
	return true;
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
                 VkImage targetImage,
                 VkSemaphore waitSemaphore)
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
	// CNSDK 0.7.28: set_view_for_texture_array dropped its trailing arg
	// (was used to disambiguate per-array-layer; now layer is implicit).
	leia_interlacer_vulkan_set_view_for_texture_array(cnsdk->interlacer, 0, left);
	leia_interlacer_vulkan_set_view_for_texture_array(cnsdk->interlacer, 1, right);
	leia_interlacer_set_shader_debug_mode(cnsdk->interlacer, LEIA_SHADER_DEBUG_MODE_NONE);
	leia_interlacer_vulkan_do_post_process(
	    cnsdk->interlacer, w, h, false, fb, targetImage, NULL,
	    waitSemaphore, NULL, 0);
}
