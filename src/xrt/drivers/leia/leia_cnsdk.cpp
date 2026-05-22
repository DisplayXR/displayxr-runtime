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


// Hardware-bring-up debug logging. Gated by XRT_DEBUG_ANDROID_VERBOSE
// which is passed via cppFlags from the Android Debug build variant
// (src/xrt/targets/openxr_android/build.gradle::debug). Compiles to
// nothing in release. Tag "HW_DBG_CNSDK:" is greppable in logcat.
#ifdef XRT_DEBUG_ANDROID_VERBOSE
#define DXR_HW_DBG(...)       U_LOG_I("HW_DBG_CNSDK: " __VA_ARGS__)
#define DXR_HW_DBG_ONCE(...)  do {                                                                 \
		static bool _logged = false;                                                                \
		if (!_logged) { U_LOG_I("HW_DBG_CNSDK[once]: " __VA_ARGS__); _logged = true; }              \
	} while (0)
#else
#define DXR_HW_DBG(...)       ((void)0)
#define DXR_HW_DBG_ONCE(...)  ((void)0)
#endif


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

	// Cached display metrics. Populated by the worker thread alongside
	// the camera-center snapshot; the atomic flag gives the render
	// thread happens-before visibility on the float/int writes. Once
	// set, leia_cnsdk_get_display_metrics returns the cached values
	// instead of calling get_device_config / release_device_config per
	// frame — eliminates a per-frame allocation churn AND the
	// concurrent-device-config-access concern (audit B9).
	std::atomic<bool> display_metrics_cached{false};
	float display_width_m_cached{0.0f};
	float display_height_m_cached{0.0f};
	uint32_t display_pixel_w_cached{0};
	uint32_t display_pixel_h_cached{0};

	// One-shot flag: once leia_interlacer_vulkan_initialize fails, give
	// up rather than retrying every frame. Read + written only by the
	// render thread (no concurrent access; no atomic needed).
	bool interlacer_init_failed{false};
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

	DXR_HW_DBG("worker: entered, waiting for leia_core_is_initialized");

	// Phase 1: wait for the async core init to complete. Poll every 50 ms;
	// honor shutdown promptly.
	int poll_count = 0;
	while (!cnsdk->shutting_down.load(std::memory_order_acquire)) {
		if (cnsdk->core != nullptr && leia_core_is_initialized(cnsdk->core)) {
			break;
		}
		if ((++poll_count % 20) == 0) {
			DXR_HW_DBG("worker: still polling for core init (~%d s elapsed)",
			           poll_count / 20);
		}
		std::this_thread::sleep_for(50ms);
	}
	if (cnsdk->shutting_down.load(std::memory_order_acquire)) {
		DXR_HW_DBG("worker: shutdown requested before core ready, exiting");
		return;
	}
	DXR_HW_DBG("worker: core initialized after %d polls", poll_count);

	// Phase 2a: snapshot all device-config values we need on the render
	// thread (camera center for face-position translation; display
	// metrics for Kooima projection). CNSDK doesn't annotate device
	// config thread safety, so we keep it on this one worker thread and
	// expose only cached values to the render thread via atomics. mm→m
	// conversion happens at storage time so render-thread reads are
	// branch-free.
	struct leia_device_config *cfg = leia_core_get_device_config(cnsdk->core);
	if (cfg != NULL) {
		cnsdk->camera_center_x_m = cfg->cameraCenterX / 1000.0f;
		cnsdk->camera_center_y_m = cfg->cameraCenterY / 1000.0f;
		cnsdk->camera_center_z_m = cfg->cameraCenterZ / 1000.0f;
		cnsdk->display_width_m_cached = (float)cfg->displaySizeInMm[0] / 1000.0f;
		cnsdk->display_height_m_cached = (float)cfg->displaySizeInMm[1] / 1000.0f;
		cnsdk->display_pixel_w_cached = (uint32_t)cfg->panelResolution[0];
		cnsdk->display_pixel_h_cached = (uint32_t)cfg->panelResolution[1];
		leia_core_release_device_config(cnsdk->core, cfg);
		cnsdk->display_metrics_cached.store(true, std::memory_order_release);
		DXR_HW_DBG("worker: cached metrics: %ux%u px, %.3fx%.3f m; cam=(%.3f, %.3f, %.3f) m",
		           cnsdk->display_pixel_w_cached, cnsdk->display_pixel_h_cached,
		           cnsdk->display_width_m_cached, cnsdk->display_height_m_cached,
		           cnsdk->camera_center_x_m, cnsdk->camera_center_y_m,
		           cnsdk->camera_center_z_m);
	} else {
		U_LOG_W("leia_core_get_device_config failed in worker; camera center + metrics stay default");
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
	DXR_HW_DBG("leia_cnsdk_create: entering");
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

	DXR_HW_DBG("leia_cnsdk_create: core=%p, worker thread spawned", (void *)core);
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
	DXR_HW_DBG("leia_cnsdk_destroy: entering, core=%p", (void *)cnsdk->core);

	// Signal the worker, then join with a watchdog: if it doesn't finish
	// within kWorkerJoinTimeoutMs, detach instead so destroy can return.
	// The worker might be mid-leia_core_enable_face_tracking with no
	// interruption hook — without the timeout, destroy can hang
	// indefinitely on a CNSDK deadlock (audit B10).
	//
	// Detaching leaks the std::thread but is the only option short of
	// CNSDK exposing a cancel API.
	cnsdk->shutting_down.store(true, std::memory_order_release);
	if (cnsdk->worker.joinable()) {
		constexpr auto kWorkerJoinTimeoutMs = std::chrono::milliseconds(2000);
		// std::thread::join doesn't take a timeout, so use a side thread
		// that does the join and a condition variable to wait on with a
		// deadline. Cheap on the happy path (the worker is usually
		// already finished by destroy time, so join returns instantly).
		std::atomic<bool> joined{false};
		std::thread joiner([&]() {
			cnsdk->worker.join();
			joined.store(true, std::memory_order_release);
		});
		const auto deadline = std::chrono::steady_clock::now() + kWorkerJoinTimeoutMs;
		while (!joined.load(std::memory_order_acquire) &&
		       std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (joined.load(std::memory_order_acquire)) {
			joiner.join();
		} else {
			U_LOG_W("CNSDK worker did not exit within %lld ms; detaching",
			        (long long)kWorkerJoinTimeoutMs.count());
			cnsdk->worker.detach();
			joiner.detach();
		}
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

extern "C" void
leia_cnsdk_on_pause(struct leia_cnsdk *cnsdk)
{
	if (cnsdk == NULL || cnsdk->core == NULL) {
		return;
	}
	if (!leia_core_is_initialized(cnsdk->core)) {
		DXR_HW_DBG("on_pause: skipped (core not initialized yet)");
		return;
	}
	DXR_HW_DBG("on_pause: forwarding to leia_core_on_pause");
	leia_core_on_pause(cnsdk->core);
}

extern "C" void
leia_cnsdk_on_resume(struct leia_cnsdk *cnsdk)
{
	if (cnsdk == NULL || cnsdk->core == NULL) {
		return;
	}
	if (!leia_core_is_initialized(cnsdk->core)) {
		DXR_HW_DBG("on_resume: skipped (core not initialized yet)");
		return;
	}
	DXR_HW_DBG("on_resume: forwarding to leia_core_on_resume");
	leia_core_on_resume(cnsdk->core);
}

extern "C" bool
leia_cnsdk_get_display_metrics(struct leia_cnsdk *cnsdk,
                               float *out_width_m,
                               float *out_height_m,
                               uint32_t *out_pixel_w,
                               uint32_t *out_pixel_h)
{
	// Worker thread snapshots all four values from the device config
	// once, then sets the atomic. Render thread polls the atomic and
	// reads the cached float/int fields. No per-frame get/release.
	if (cnsdk == NULL ||
	    !cnsdk->display_metrics_cached.load(std::memory_order_acquire)) {
		return false;
	}

	if (out_width_m != NULL) {
		*out_width_m = cnsdk->display_width_m_cached;
	}
	if (out_height_m != NULL) {
		*out_height_m = cnsdk->display_height_m_cached;
	}
	if (out_pixel_w != NULL) {
		*out_pixel_w = cnsdk->display_pixel_w_cached;
	}
	if (out_pixel_h != NULL) {
		*out_pixel_h = cnsdk->display_pixel_h_cached;
	}
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
	// One-shot give-up: once leia_interlacer_vulkan_initialize fails
	// (typically permanently — wrong VkDevice format, no GPU memory,
	// CNSDK lib mismatch), don't keep retrying every frame.
	if (cnsdk->interlacer_init_failed) {
		return false;
	}
	if (!leia_core_is_initialized(cnsdk->core)) {
		return false;
	}

	struct leia_interlacer_init_configuration *ic = leia_interlacer_init_configuration_alloc();
	// Atlas mode: CNSDK accepts the SBS atlas VkImage+View directly per
	// frame via set_interlace_view_texture_atlas, and splits internally.
	// No per-view image management on our side; the DP shrinks
	// substantially. See feature/android-cnsdk-ci for the prior art
	// (CNSDK 0.10.56 used a different API but same architectural idea).
	leia_interlacer_init_configuration_set_use_atlas_for_views(ic, true);
	// Views format = atlas format. Atlas is rendered to UNORM by
	// comp_vk_native_renderer.c, so use UNORM here (audit B2).
	cnsdk->interlacer = leia_interlacer_vulkan_initialize(
	    cnsdk->core, ic, device, physDev, VK_FORMAT_B8G8R8A8_UNORM,
	    targetFmt, VK_FORMAT_D32_SFLOAT, 3);
	leia_interlacer_init_configuration_free(ic);

	if (cnsdk->interlacer == NULL) {
		U_LOG_W("leia_interlacer_vulkan_initialize returned NULL; giving up (no retries)");
		cnsdk->interlacer_init_failed = true;
		return false;
	}

	// Tell CNSDK the atlas is laid out 2x1 SBS horizontal. This is the
	// default but we set it explicitly so future layout changes
	// (multi-view modes) only have to touch one place.
	leia_interlacer_set_num_tiles(cnsdk->interlacer, 2, 1);
	DXR_HW_DBG("ensure_interlacer: created interlacer=%p (atlas mode, 2x1, targetFmt=%d)",
	           (void *)cnsdk->interlacer, (int)targetFmt);
	return true;
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

#ifdef XRT_DEBUG_ANDROID_VERBOSE
	// Throttle to once per ~second at 60 Hz so logcat stays readable.
	static int dbg_face_counter = 0;
	if ((dbg_face_counter++ % 60) == 0) {
		DXR_HW_DBG("face: raw_mm=(%.1f, %.1f, %.1f) → out_m=(%.4f, %.4f, %.4f)",
		           position[0], position[1], position[2], pos_x_m, pos_y_m, pos_z_m);
	}
#endif

	if (out_x != NULL) { *out_x = pos_x_m; }
	if (out_y != NULL) { *out_y = pos_y_m; }
	if (out_z != NULL) { *out_z = pos_z_m; }
	return true;
}

extern "C" void
leia_cnsdk_weave(struct leia_cnsdk *cnsdk,
                 VkDevice device,
                 VkPhysicalDevice physDev,
                 VkImage atlas_image,
                 VkImageView atlas_view,
                 uint32_t atlas_width,
                 uint32_t atlas_height,
                 VkFormat targetFmt,
                 uint32_t w,
                 uint32_t h,
                 VkFramebuffer fb,
                 VkImage targetImage)
{
	(void)device; (void)physDev; (void)targetFmt;

	if (cnsdk == NULL || cnsdk->interlacer == NULL) {
		return;
	}

	leia_interlacer_set_flip_input_uv_vertical(cnsdk->interlacer, true);

	// Atlas mode: hand CNSDK the SBS atlas VkImage+View each frame; it
	// splits internally per the 2x1 layout set in ensure_interlacer.
	// (Previously this function blitted tiles into per-view images and
	// passed those — see git log for the per-tile-blit history.)
	leia_interlacer_vulkan_set_interlace_view_texture_atlas(
	    cnsdk->interlacer, atlas_image, atlas_view);
	leia_interlacer_set_source_views_size(
	    cnsdk->interlacer, (int32_t)atlas_width, (int32_t)atlas_height,
	    /*isHorizontalViews=*/true);

	leia_interlacer_set_shader_debug_mode(cnsdk->interlacer, LEIA_SHADER_DEBUG_MODE_NONE);
	DXR_HW_DBG_ONCE("weave: first do_post_process atlas=%ux%u target=%ux%u",
	                atlas_width, atlas_height, w, h);
	leia_interlacer_vulkan_do_post_process(
	    cnsdk->interlacer, w, h, false, fb, targetImage, NULL,
	    NULL, NULL, 0);
}
