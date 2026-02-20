// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2024-2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Compositor rendering code.
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Moshi Turner <moshiturner@protonmail.com>
 * @ingroup comp_main
 */

#include "render/render_interface.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_frame.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_results.h"

#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_matrix_2x2.h"
#include "math/m_space.h"

#include "util/u_misc.h"
#include "util/u_trace_marker.h"
#include "util/u_distortion_mesh.h"
#include "util/u_sink.h"
#include "util/u_var.h"
#include "util/u_frame_times_widget.h"
#include "util/u_debug.h"
#include "util/comp_render_helpers.h"
#include "util/comp_high_level_render.h"

#include "main/comp_frame.h"
#include "main/comp_mirror_to_debug_gui.h"
#include "main/comp_window.h"

#ifdef XRT_OS_ANDROID
#include "android/android_globals.h"
#endif

#ifdef XRT_FEATURE_WINDOW_PEEK
#include "main/comp_window_peek.h"
#endif

#include "vk/vk_helpers.h"
#include "vk/vk_cmd.h"
#include "vk/vk_hud_blend.h"
#include "vk/vk_image_readback_to_xf_pool.h"

#ifdef XRT_HAVE_CNSDK
#include "leia/leia_cnsdk.h"
#endif

#ifdef XRT_HAVE_LEIA_SR_VULKAN
#include "leia/leia_sr.h"
#include "leia/leia_display_processor.h"
#endif

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_display_processor.h"
#include "sim_display/sim_display_interface.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty/qwerty_interface.h"
#endif

#include "xrt/xrt_system.h"
#include "util/u_hud.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>

DEBUG_GET_ONCE_LOG_OPTION(comp_frame_lag_level, "XRT_COMP_FRAME_LAG_LOG_AS_LEVEL", U_LOGGING_INFO)
#define LOG_FRAME_LAG(...) U_LOG_IFL(debug_get_log_option_comp_frame_lag_level(), u_log_get_global_level(), __VA_ARGS__)

/*
 *
 * Small internal helpers.
 *
 */

#define CHAIN(STRUCT, NEXT)                                                                                            \
	do {                                                                                                           \
		(STRUCT).pNext = NEXT;                                                                                 \
		NEXT = (VkBaseInStructure *)&(STRUCT);                                                                 \
	} while (false)


/*
 *
 * Private struct(s).
 *
 */

/*!
 * What is the source of the FoV values used for the final image that the
 * compositor produces and is sent to the hardware (or software).
 */
enum comp_target_fov_source
{
	/*!
	 * The FoV values used for the final target is taken from the
	 * distortion information on the @ref xrt_hmd_parts struct.
	 */
	COMP_TARGET_FOV_SOURCE_DISTORTION,

	/*!
	 * The FoV values used for the final target is taken from the
	 * those returned from the device's get_views.
	 */
	COMP_TARGET_FOV_SOURCE_DEVICE_VIEWS,
};

/*!
 * Holds associated vulkan objects and state to render with a distortion.
 *
 * @ingroup comp_main
 */
struct comp_renderer
{
	//! @name Durable members
	//! @brief These don't require the images to be created and don't depend on it.
	//! @{

	//! The compositor we were created by
	struct comp_compositor *c;
	struct comp_settings *settings;

	struct comp_mirror_to_debug_gui mirror_to_debug_gui;

	//! @}

	//! @name Image-dependent members
	//! @{

	//! Index of the current buffer/image
	int32_t acquired_buffer;

	//! Which buffer was last submitted and has a fence pending.
	int32_t fenced_buffer;

	/*!
	 * The render pass used to render to the target, it depends on the
	 * target's format so will be recreated each time the target changes.
	 */
	struct render_gfx_render_pass target_render_pass;

	/*!
	 * Array of "rendering" target resources equal in size to the number of
	 * comp_target images. Each target resources holds all of the resources
	 * needed to render to that target and its views.
	 */
	struct render_gfx_target_resources *rtr_array;

	/*!
	 * Array of fences equal in size to the number of comp_target images.
	 */
	VkFence *fences;

	/*!
	 * The number of renderings/fences we've created: set from comp_target when we use that data.
	 */
	uint32_t buffer_count;

	//! @}

	//! Generic display output processor (interlacing, SBS, etc.).
	//! If NULL, standard Monado distortion is used.
	struct xrt_display_processor *display_processor;

	struct leia_cnsdk *cnsdk;

#ifdef XRT_HAVE_LEIA_SR_VULKAN
	struct leiasr* leiasr;
#endif // XRT_HAVE_LEIA_SR_VULKAN

	//! Intermediate images for Y-flipping GL textures before display processing
	struct
	{
		VkImage images[2];
		VkDeviceMemory memories[2];
		VkImageView views[2];
		int width;
		int height;
		VkFormat format;
		bool initialized;
	} flip;

	//! Intermediate images for crop-blitting per-eye sub-rects before display processing.
	//! Needed when both eyes reference the same swapchain image (single-swapchain SBS layout).
	struct
	{
		VkImage images[2];
		VkDeviceMemory memories[2];
		VkImageView views[2];
		int width;
		int height;
		VkFormat format;
		bool initialized;
	} crop;

	//! Runtime HUD overlay (post-weave diagnostic text)
	struct
	{
		struct u_hud *hud;
		VkImage image;
		VkDeviceMemory memory;
		VkBuffer staging_buffer;
		VkDeviceMemory staging_memory;
		void *staging_mapped;
		struct vk_hud_blend hud_blend; //!< Alpha-blended HUD overlay pipeline
		bool gpu_initialized;
		uint64_t last_frame_time_ns;
		float smoothed_frame_time_ms;
	} hud;
};


/*
 *
 * Functions.
 *
 */

static void
renderer_wait_queue_idle(struct comp_renderer *r)
{
	COMP_TRACE_MARKER();
	struct vk_bundle *vk = &r->c->base.vk;

	vk_queue_lock(vk->main_queue);
	vk->vkQueueWaitIdle(vk->main_queue->queue);
	vk_queue_unlock(vk->main_queue);
}

static void
calc_viewport_data(struct comp_renderer *r,
                   struct render_viewport_data out_viewport_data[XRT_MAX_VIEWS],
                   size_t view_count)
{
	struct comp_compositor *c = r->c;

	bool pre_rotate = false;
	if (r->c->target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
	    r->c->target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
		COMP_SPEW(c, "Swapping width and height, since we are pre rotating");
		pre_rotate = true;
	}

	int w_i32 = pre_rotate ? r->c->xdev->hmd->screens[0].h_pixels : r->c->xdev->hmd->screens[0].w_pixels;
	int h_i32 = pre_rotate ? r->c->xdev->hmd->screens[0].w_pixels : r->c->xdev->hmd->screens[0].h_pixels;

	float scale_x = (float)r->c->target->width / (float)w_i32;
	float scale_y = (float)r->c->target->height / (float)h_i32;

	for (uint32_t i = 0; i < view_count; ++i) {
		struct xrt_view *v = &r->c->xdev->hmd->views[i];
		if (pre_rotate) {
			out_viewport_data[i] = (struct render_viewport_data){
			    .x = (uint32_t)(v->viewport.y_pixels * scale_x),
			    .y = (uint32_t)(v->viewport.x_pixels * scale_y),
			    .w = (uint32_t)(v->viewport.h_pixels * scale_x),
			    .h = (uint32_t)(v->viewport.w_pixels * scale_y),
			};
		} else {
			out_viewport_data[i] = (struct render_viewport_data){
			    .x = (uint32_t)(v->viewport.x_pixels * scale_x),
			    .y = (uint32_t)(v->viewport.y_pixels * scale_y),
			    .w = (uint32_t)(v->viewport.w_pixels * scale_x),
			    .h = (uint32_t)(v->viewport.h_pixels * scale_y),
			};
		}
	}
}

static void
calc_vertex_rot_data(struct comp_renderer *r, struct xrt_matrix_2x2 out_vertex_rots[XRT_MAX_VIEWS], size_t view_count)
{
	bool pre_rotate = false;
	if (r->c->target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
	    r->c->target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
		COMP_SPEW(r->c, "Swapping width and height, since we are pre rotating");
		pre_rotate = true;
	}

	const struct xrt_matrix_2x2 rotation_90_cw = {{
	    .vecs =
	        {
	            {0, 1},
	            {-1, 0},
	        },
	}};

	for (uint32_t i = 0; i < view_count; i++) {
		// Get the view.
		struct xrt_view *v = &r->c->xdev->hmd->views[i];

		// Copy data.
		struct xrt_matrix_2x2 rot = v->rot;

		// Should we rotate.
		if (pre_rotate) {
			m_mat2x2_multiply(&rot, &rotation_90_cw, &rot);
		}

		out_vertex_rots[i] = rot;
	}
}

static void
calc_pose_data(struct comp_renderer *r,
               enum comp_target_fov_source fov_source,
               struct xrt_fov out_fovs[XRT_MAX_VIEWS],
               struct xrt_pose out_world[XRT_MAX_VIEWS],
               struct xrt_pose out_eye[XRT_MAX_VIEWS],
               uint32_t view_count)
{
	COMP_TRACE_MARKER();

	struct xrt_vec3 default_eye_relation = {
	    0.063000f, /*! @todo get actual ipd_meters */
	    0.0f,
	    0.0f,
	};

	struct xrt_space_relation head_relation = XRT_SPACE_RELATION_ZERO;
	struct xrt_fov xdev_fovs[XRT_MAX_VIEWS] = XRT_STRUCT_INIT;
	struct xrt_pose xdev_poses[XRT_MAX_VIEWS] = XRT_STRUCT_INIT;

	xrt_result_t xret = xrt_device_get_view_poses(       //
	    r->c->xdev,                                      //
	    &default_eye_relation,                           //
	    r->c->frame.rendering.predicted_display_time_ns, // at_timestamp_ns
	    view_count,                                      //
	    &head_relation,                                  // out_head_relation
	    xdev_fovs,                                       // out_fovs
	    xdev_poses);                                     // out_poses
	if (xret != XRT_SUCCESS) {
		struct u_pp_sink_stack_only sink;
		u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);
		u_pp_xrt_result(dg, xret);
		U_LOG_E("xrt_device_get_view_poses failed: %s", sink.buffer);
		return;
	}

	struct xrt_fov dist_fov[XRT_MAX_VIEWS] = XRT_STRUCT_INIT;
	for (uint32_t i = 0; i < view_count; i++) {
		dist_fov[i] = r->c->xdev->hmd->distortion.fov[i];
	}

	bool use_xdev = false; // Probably what we want.

	switch (fov_source) {
	case COMP_TARGET_FOV_SOURCE_DISTORTION: use_xdev = false; break;
	case COMP_TARGET_FOV_SOURCE_DEVICE_VIEWS: use_xdev = true; break;
	}

	for (uint32_t i = 0; i < view_count; i++) {
		const struct xrt_fov fov = use_xdev ? xdev_fovs[i] : dist_fov[i];
		const struct xrt_pose eye_pose = xdev_poses[i];

		struct xrt_space_relation result = {0};
		struct xrt_relation_chain xrc = {0};
		m_relation_chain_push_pose_if_not_identity(&xrc, &eye_pose);
		m_relation_chain_push_relation(&xrc, &head_relation);
		m_relation_chain_resolve(&xrc, &result);

		// Results to callers.
		out_fovs[i] = fov;
		out_world[i] = result.pose;
		out_eye[i] = eye_pose;

		// For remote rendering targets.
		r->c->base.frame_params.fovs[i] = fov;
		r->c->base.frame_params.poses[i] = result.pose;
	}
}

//! @pre comp_target_has_images(r->c->target)
static void
renderer_build_rendering_target_resources(struct comp_renderer *r,
                                          struct render_gfx_target_resources *rtr,
                                          uint32_t index)
{
	COMP_TRACE_MARKER();

	struct comp_compositor *c = r->c;

	VkImageView image_view = r->c->target->images[index].view;
	VkExtent2D extent = {r->c->target->width, r->c->target->height};

	render_gfx_target_resources_init( //
	    rtr,                          //
	    &c->nr,                       //
	    &r->target_render_pass,       //
	    image_view,                   //
	    extent);                      //
}

/*!
 * @pre comp_target_has_images(r->c->target)
 * Update r->buffer_count before calling.
 */
static void
renderer_create_renderings_and_fences(struct comp_renderer *r)
{
	assert(r->fences == NULL);
	if (r->buffer_count == 0) {
		COMP_ERROR(r->c, "Requested 0 command buffers.");
		return;
	}

	COMP_DEBUG(r->c, "Allocating %d Command Buffers.", r->buffer_count);

	struct vk_bundle *vk = &r->c->base.vk;

	bool use_compute = r->settings->use_compute;
	if (!use_compute) {
		r->rtr_array = U_TYPED_ARRAY_CALLOC(struct render_gfx_target_resources, r->buffer_count);

		render_gfx_render_pass_init(     //
		    &r->target_render_pass,      // rgrp
		    &r->c->nr,                   // struct render_resources
		    r->c->target->format,        //
		    VK_ATTACHMENT_LOAD_OP_CLEAR, // load_op
		    r->c->target->final_layout); // final_layout

		for (uint32_t i = 0; i < r->buffer_count; ++i) {
			renderer_build_rendering_target_resources(r, &r->rtr_array[i], i);
		}
	}

	r->fences = U_TYPED_ARRAY_CALLOC(VkFence, r->buffer_count);

	for (uint32_t i = 0; i < r->buffer_count; i++) {
		VkFenceCreateInfo fence_info = {
		    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
		};

		VkResult ret = vk->vkCreateFence( //
		    vk->device,                   //
		    &fence_info,                  //
		    NULL,                         //
		    &r->fences[i]);               //
		if (ret != VK_SUCCESS) {
			COMP_ERROR(r->c, "vkCreateFence: %s", vk_result_string(ret));
		}

		char buf[] = "Comp Renderer X_XXXX_XXXX";
		snprintf(buf, ARRAY_SIZE(buf), "Comp Renderer %u", i);
		VK_NAME_FENCE(vk, r->fences[i], buf);
	}
}

static void
renderer_close_renderings_and_fences(struct comp_renderer *r)
{
	struct vk_bundle *vk = &r->c->base.vk;
	// Renderings
	if (r->buffer_count > 0 && r->rtr_array != NULL) {
		for (uint32_t i = 0; i < r->buffer_count; i++) {
			render_gfx_target_resources_fini(&r->rtr_array[i]);
		}

		// Close the render pass used for rendering to the target.
		render_gfx_render_pass_fini(&r->target_render_pass);

		free(r->rtr_array);
		r->rtr_array = NULL;
	}

	// Fences
	if (r->buffer_count > 0 && r->fences != NULL) {
		for (uint32_t i = 0; i < r->buffer_count; i++) {
			vk->vkDestroyFence(vk->device, r->fences[i], NULL);
			r->fences[i] = VK_NULL_HANDLE;
		}
		free(r->fences);
		r->fences = NULL;
	}

	r->buffer_count = 0;
	r->acquired_buffer = -1;
	r->fenced_buffer = -1;
}

/*!
 * @brief Ensure that target images and renderings are created, if possible.
 *
 * @param r Self pointer
 * @param force_recreate If true, will tear down and re-create images and renderings, e.g. for a resize
 *
 * @returns true if images and renderings are ready and created.
 *
 * @private @memberof comp_renderer
 * @ingroup comp_main
 */
static bool
renderer_ensure_images_and_renderings(struct comp_renderer *r, bool force_recreate)
{
	struct comp_compositor *c = r->c;
	struct comp_target *target = c->target;

	if (!comp_target_check_ready(target)) {
		// Not ready, so can't render anything.
		return false;
	}

	// We will create images if we don't have any images or if we were told to recreate them.
	bool create = force_recreate || !comp_target_has_images(target) || (r->buffer_count == 0);
	if (!create) {
		return true;
	}

#ifdef XRT_OS_WINDOWS
	// Don't force-recreate swapchain during drag — causes stutter from
	// per-pixel texture reallocation. The existing swapchain stretches fine.
	if (force_recreate && comp_window_mswin_is_in_size_move(c->target)) {
		return comp_target_has_images(target) && (r->buffer_count > 0);
	}
#endif

	COMP_DEBUG(c, "Creating images and renderings (force_recreate: %s).", force_recreate ? "true" : "false");

	/*
	 * This makes sure that any pending command buffer has completed
	 * and all resources referred by it can now be manipulated. This
	 * make sure that validation doesn't complain. This is done
	 * during resize so isn't time critical.
	 */
	renderer_wait_queue_idle(r);

	// Make we sure we destroy all dependent things before creating new images.
	renderer_close_renderings_and_fences(r);

	VkImageUsageFlags image_usage = 0;
	if (r->settings->use_compute) {
		image_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	} else {
		image_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}

	// TRANSFER_DST for vkCmdClearColorImage (used by macOS diagnostic and future use).
	image_usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	if (c->peek) {
		image_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	struct comp_target_create_images_info info = {
	    .extent =
	        {
	            .width = r->c->settings.preferred.width,
	            .height = r->c->settings.preferred.height,
	        },
	    .image_usage = image_usage,
	    .color_space = r->settings->color_space,
	    .present_mode = r->settings->present_mode,
	};

	static_assert(ARRAY_SIZE(info.formats) == ARRAY_SIZE(r->c->settings.formats), "Miss-match format array sizes");
	for (uint32_t i = 0; i < r->c->settings.format_count; i++) {
		info.formats[info.format_count++] = r->c->settings.formats[i];
	}

	comp_target_create_images(r->c->target, &info);

	bool pre_rotate = false;
	if (r->c->target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
	    r->c->target->surface_transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
		pre_rotate = true;
	}

	// @todo: is it safe to fail here?
	if (!render_distortion_images_ensure(&r->c->nr, &r->c->base.vk, r->c->xdev, pre_rotate))
		return false;

	r->buffer_count = r->c->target->image_count;

	renderer_create_renderings_and_fences(r);

	assert(r->buffer_count != 0);

	return true;
}

//! Create renderer and initialize non-image-dependent members
static void
renderer_init(struct comp_renderer *r, struct comp_compositor *c, VkExtent2D scratch_extent)
{
	COMP_TRACE_MARKER();

	r->c = c;
	r->settings = &c->settings;

	r->acquired_buffer = -1;
	r->fenced_buffer = -1;
	r->rtr_array = NULL;

	// Setup the scratch images.
	bool bret = chl_scratch_ensure( //
	    &c->scratch,                // scratch
	    &c->nr,                     // struct render_resources
	    c->nr.view_count,           // view_count
	    scratch_extent,             // extent
	    VK_FORMAT_R8G8B8A8_SRGB);   // format
	if (!bret) {
		COMP_ERROR(c, "chl_scratch_ensure: false");
		assert(bret && "Whelp, can't return a error. But should never really fail.");
	}

	// Try to early-allocate these, in case we can.
	renderer_ensure_images_and_renderings(r, false);

#ifdef XRT_HAVE_CNSDK
	leia_cnsdk_create(&r->cnsdk);
#endif

	struct vk_bundle *vk = &r->c->base.vk;

#ifdef XRT_HAVE_LEIA_SR_VULKAN
	// Get external window handle from compositor (if provided via XR_EXT_win32_window_binding)
	// NULL = fullscreen mode, valid HWND = windowed mode
	void *window_handle = r->c->external_window_handle;
	xrt_result_t sr_ret = leiasr_create(1000.0, vk->device, vk->physical_device, vk->main_queue->queue, r->target_render_pass.r->cmd_pool, window_handle, &r->leiasr);
	if (sr_ret != XRT_SUCCESS) {
		COMP_WARN(c, "Failed to create SR Vulkan weaver, continuing without interlacing");
		r->leiasr = NULL;
	}

	// Wrap the SR weaver in a generic display processor
	if (r->leiasr != NULL) {
		xrt_result_t dp_ret = leia_display_processor_create(r->leiasr, &r->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			COMP_WARN(c, "Failed to create SR display processor wrapper");
			r->display_processor = NULL;
		}
	}
#endif // XRT_HAVE_LEIA_SR_VULKAN

	// Create sim_display processor if SIM_DISPLAY_ENABLE=1 and no display processor yet
	if (r->display_processor == NULL) {
		const char *sim_enable = getenv("SIM_DISPLAY_ENABLE");
		if (sim_enable != NULL && strcmp(sim_enable, "1") == 0) {
			enum sim_display_output_mode mode = SIM_DISPLAY_OUTPUT_SBS;
			const char *mode_str = getenv("SIM_DISPLAY_OUTPUT");
			if (mode_str != NULL) {
				if (strcmp(mode_str, "anaglyph") == 0) {
					mode = SIM_DISPLAY_OUTPUT_ANAGLYPH;
				} else if (strcmp(mode_str, "blend") == 0) {
					mode = SIM_DISPLAY_OUTPUT_BLEND;
				}
			}

			xrt_result_t dp_ret = sim_display_processor_create(
			    mode, vk, (int32_t)r->c->target->format, &r->display_processor);
			if (dp_ret != XRT_SUCCESS) {
				COMP_WARN(c, "Failed to create sim display processor");
				r->display_processor = NULL;
			}
		}
	}

	// Create HUD overlay (only for runtime-owned windows, not app-provided windows)
	if (r->c->external_window_handle == NULL) {
		u_hud_create(&r->hud.hud, 480, 304);
	}

	VkResult ret = comp_mirror_init( //
	    &r->mirror_to_debug_gui,     //
	    vk,                          //
	    &c->shaders,                 //
	    scratch_extent);             //
	if (ret != VK_SUCCESS) {
		COMP_ERROR(c, "comp_mirror_init: %s", vk_result_string(ret));
		assert(false && "Whelp, can't return a error. But should never really fail.");
	}
}

static void
renderer_wait_for_last_fence(struct comp_renderer *r)
{
	COMP_TRACE_MARKER();

	if (r->fenced_buffer < 0) {
		return;
	}

	struct vk_bundle *vk = &r->c->base.vk;
	VkResult ret;

	ret = vk->vkWaitForFences(vk->device, 1, &r->fences[r->fenced_buffer], VK_TRUE, UINT64_MAX);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(r->c, "vkWaitForFences: %s", vk_result_string(ret));
	}

	r->fenced_buffer = -1;
}

static XRT_CHECK_RESULT VkResult
renderer_submit_queue(struct comp_renderer *r, VkCommandBuffer cmd, VkPipelineStageFlags pipeline_stage_flag)
{
	COMP_TRACE_MARKER();

	struct vk_bundle *vk = &r->c->base.vk;
	int64_t frame_id = r->c->frame.rendering.id;
	VkResult ret;

	assert(frame_id >= 0);


	/*
	 * Wait for previous frame's work to complete.
	 */

	// Wait for the last fence, if any.
	renderer_wait_for_last_fence(r);
	assert(r->fenced_buffer < 0);

	assert(r->acquired_buffer >= 0);
	ret = vk->vkResetFences(vk->device, 1, &r->fences[r->acquired_buffer]);
	VK_CHK_AND_RET(ret, "vkResetFences");


	/*
	 * Regular semaphore setup.
	 */

	// Convenience.
	struct comp_target *ct = r->c->target;
#define WAIT_SEMAPHORE_COUNT 1

	VkSemaphore wait_sems[WAIT_SEMAPHORE_COUNT] = {ct->semaphores.present_complete};
	VkPipelineStageFlags stage_flags[WAIT_SEMAPHORE_COUNT] = {pipeline_stage_flag};

	VkSemaphore *wait_sems_ptr = NULL;
	VkPipelineStageFlags *stage_flags_ptr = NULL;
	uint32_t wait_sem_count = 0;
	if (wait_sems[0] != VK_NULL_HANDLE) {
		wait_sems_ptr = wait_sems;
		stage_flags_ptr = stage_flags;
		wait_sem_count = WAIT_SEMAPHORE_COUNT;
	}

#define SIGNAL_SEMAPHRE_COUNT 1
	VkSemaphore signal_sems[SIGNAL_SEMAPHRE_COUNT] = {ct->semaphores.render_complete};

	uint32_t signal_sem_count = 0;
	VkSemaphore *signal_sems_ptr = NULL;
	if (signal_sems[0] != VK_NULL_HANDLE) {
		signal_sems_ptr = signal_sems;
		signal_sem_count = SIGNAL_SEMAPHRE_COUNT;
	}

	// Next pointer for VkSubmitInfo
	const void *next = NULL;

#ifdef VK_KHR_timeline_semaphore
	assert(!comp_frame_is_invalid_locked(&r->c->frame.rendering));
	uint64_t render_complete_signal_values[SIGNAL_SEMAPHRE_COUNT] = {(uint64_t)frame_id};

	VkTimelineSemaphoreSubmitInfoKHR timeline_info = {
	    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR,
	};

	if (ct->semaphores.render_complete_is_timeline) {
		timeline_info = (VkTimelineSemaphoreSubmitInfoKHR){
		    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR,
		    .signalSemaphoreValueCount = signal_sem_count,
		    .pSignalSemaphoreValues = render_complete_signal_values,
		};

		CHAIN(timeline_info, next);
	}
#endif


	VkSubmitInfo comp_submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .pNext = next,
	    .pWaitDstStageMask = stage_flags_ptr,
	    .pWaitSemaphores = wait_sems_ptr,
	    .waitSemaphoreCount = wait_sem_count,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = signal_sem_count,
	    .pSignalSemaphores = signal_sems_ptr,
	};

	// Everything prepared, now we are submitting.
	comp_target_mark_submit_begin(ct, frame_id, os_monotonic_get_ns());

	/*
	 * The renderer command buffer pool is only accessed from one thread,
	 * this satisfies the `_locked` requirement of the function. This lets
	 * us avoid taking a lot of locks. The queue lock will be taken by
	 * @ref vk_cmd_submit_locked tho.
	 */
	ret = vk_cmd_submit_locked(vk, vk->main_queue, 1, &comp_submit_info, r->fences[r->acquired_buffer]);

	// We have now completed the submit, even if we failed.
	comp_target_mark_submit_end(ct, frame_id, os_monotonic_get_ns());

	// Check after marking as submit complete.
	VK_CHK_AND_RET(ret, "vk_cmd_submit_locked");

	// This buffer now have a pending fence.
	r->fenced_buffer = r->acquired_buffer;

	return ret;
}

static void
renderer_acquire_swapchain_image(struct comp_renderer *r)
{
	COMP_TRACE_MARKER();

	uint32_t buffer_index = 0;
	VkResult ret;

	assert(r->acquired_buffer < 0);

	if (!renderer_ensure_images_and_renderings(r, false)) {
		// Not ready yet.
		return;
	}
	ret = comp_target_acquire(r->c->target, &buffer_index);

	if ((ret == VK_ERROR_OUT_OF_DATE_KHR) || (ret == VK_SUBOPTIMAL_KHR)) {
		COMP_DEBUG(r->c, "Received %s.", vk_result_string(ret));

		if (!renderer_ensure_images_and_renderings(r, true)) {
			// Failed on force recreate.
			COMP_ERROR(r->c,
			           "renderer_acquire_swapchain_image: comp_target_acquire was out of date, force "
			           "re-create image and renderings failed. Probably the target disappeared.");
			return;
		}

		/* Acquire image again to silence validation error */
		ret = comp_target_acquire(r->c->target, &buffer_index);
		if (ret == VK_SUBOPTIMAL_KHR) {
			COMP_DEBUG(r->c, "Swapchain still suboptimal after recreate, will retry next frame.");
		} else if (ret != VK_SUCCESS) {
			COMP_ERROR(r->c, "comp_target_acquire: %s", vk_result_string(ret));
		}
	} else if (ret != VK_SUCCESS) {
		COMP_ERROR(r->c, "comp_target_acquire: %s", vk_result_string(ret));
	}

	r->acquired_buffer = buffer_index;
}

static void
renderer_resize(struct comp_renderer *r)
{
	if (!comp_target_check_ready(r->c->target)) {
		// Can't create images right now.
		// Just close any existing renderings.
		renderer_close_renderings_and_fences(r);
		return;
	}
	// Force recreate.
	renderer_ensure_images_and_renderings(r, true);
}

static void
renderer_present_swapchain_image(struct comp_renderer *r, uint64_t desired_present_time_ns, uint64_t present_slop_ns)
{
	COMP_TRACE_MARKER();

	VkResult ret;

	assert(!comp_frame_is_invalid_locked(&r->c->frame.rendering));
	uint64_t render_complete_signal_value = (uint64_t)r->c->frame.rendering.id;

	ret = comp_target_present(           //
	    r->c->target,                    //
	    r->c->base.vk.main_queue->queue, //
	    r->acquired_buffer,              //
	    render_complete_signal_value,    //
	    desired_present_time_ns,         //
	    present_slop_ns);                //
	r->acquired_buffer = -1;

	if (ret == VK_ERROR_OUT_OF_DATE_KHR || ret == VK_SUBOPTIMAL_KHR) {
		renderer_resize(r);
		return;
	}
	if (ret != VK_SUCCESS) {
		COMP_ERROR(r->c, "vk_swapchain_present: %s", vk_result_string(ret));
	}
}

static void
renderer_wait_for_present(struct comp_renderer *r, uint64_t desired_present_time_ns)
{
	struct comp_compositor *c = r->c;

	if (!comp_target_check_ready(c->target)) {
		return;
	}

	// For estimating frame misses.
	uint64_t before_ns = os_monotonic_get_ns();

	if (c->target->wait_for_present_supported) {
		// reasonable timeout
		time_duration_ns timeout_ns = c->frame_interval_ns * 2.5f;

		// @note we don't actually care about the return value, just swallow errors, anything *critical* that
		// may be returned will be handled quite soon by later calls
		VkResult result = comp_target_wait_for_present(c->target, timeout_ns);
		(void)result;

		assert(result != VK_ERROR_EXTENSION_NOT_PRESENT);
	} else {
		/*
		 * For direct mode this makes us wait until the last frame has been
		 * actually shown to the user, this avoids us missing that we have
		 * missed a frame and miss-predicting the next frame.
		 *
		 * Not all drivers follow this behaviour, so KHR_present_wait
		 * should be preferred in all circumstances.
		 *
		 * Only do this if we are ready.
		 */

		// Do the acquire
		renderer_acquire_swapchain_image(r);
	}

	// How long did it take?
	uint64_t after_ns = os_monotonic_get_ns();

	/*
	 * Make sure we at least waited 1ms before warning. Then check
	 * if we are more then 1ms behind when we wanted to present.
	 */
	if (before_ns + U_TIME_1MS_IN_NS < after_ns && //
	    desired_present_time_ns + U_TIME_1MS_IN_NS < after_ns) {
		uint64_t diff_ns = after_ns - desired_present_time_ns;
		double diff_ms_f = time_ns_to_ms_f(diff_ns);
		LOG_FRAME_LAG("Compositor probably missed frame by %.2fms", diff_ms_f);
	}
}

static void
renderer_fini(struct comp_renderer *r)
{
	struct vk_bundle *vk = &r->c->base.vk;

	// Command buffers
	renderer_close_renderings_and_fences(r);

	// Do before layer render just in case it holds any references.
	comp_mirror_fini(&r->mirror_to_debug_gui, vk);

	// Do this after the layer renderer.
	chl_scratch_free_resources(&r->c->scratch, &r->c->nr);

#ifdef XRT_HAVE_CNSDK
	leia_cnsdk_destroy(&r->cnsdk);
#endif // XRT_HAVE_CNSDK

	// Destroy generic display processor before vendor-specific resources
	xrt_display_processor_destroy(&r->display_processor);

	// Destroy HUD resources
	if (r->hud.gpu_initialized) {
		vk_hud_blend_fini(&r->hud.hud_blend, vk);
		if (r->hud.staging_mapped != NULL) {
			vk->vkUnmapMemory(vk->device, r->hud.staging_memory);
		}
		if (r->hud.staging_buffer != VK_NULL_HANDLE)
			vk->vkDestroyBuffer(vk->device, r->hud.staging_buffer, NULL);
		if (r->hud.staging_memory != VK_NULL_HANDLE)
			vk->vkFreeMemory(vk->device, r->hud.staging_memory, NULL);
		if (r->hud.image != VK_NULL_HANDLE)
			vk->vkDestroyImage(vk->device, r->hud.image, NULL);
		if (r->hud.memory != VK_NULL_HANDLE)
			vk->vkFreeMemory(vk->device, r->hud.memory, NULL);
		r->hud.gpu_initialized = false;
	}
	u_hud_destroy(&r->hud.hud);

	// Destroy flip images (used for GL texture Y-flip before display processing)
	if (r->flip.initialized) {
		for (int i = 0; i < 2; i++) {
			if (r->flip.views[i] != VK_NULL_HANDLE)
				vk->vkDestroyImageView(vk->device, r->flip.views[i], NULL);
			if (r->flip.images[i] != VK_NULL_HANDLE)
				vk->vkDestroyImage(vk->device, r->flip.images[i], NULL);
			if (r->flip.memories[i] != VK_NULL_HANDLE)
				vk->vkFreeMemory(vk->device, r->flip.memories[i], NULL);
		}
		r->flip.initialized = false;
	}

	// Destroy crop images (used for crop-blitting per-eye sub-rects before display processing)
	if (r->crop.initialized) {
		for (int i = 0; i < 2; i++) {
			if (r->crop.views[i] != VK_NULL_HANDLE)
				vk->vkDestroyImageView(vk->device, r->crop.views[i], NULL);
			if (r->crop.images[i] != VK_NULL_HANDLE)
				vk->vkDestroyImage(vk->device, r->crop.images[i], NULL);
			if (r->crop.memories[i] != VK_NULL_HANDLE)
				vk->vkFreeMemory(vk->device, r->crop.memories[i], NULL);
		}
		r->crop.initialized = false;
	}

#ifdef XRT_HAVE_LEIA_SR_VULKAN
	leiasr_destroy(r->leiasr);
#endif // XRT_HAVE_LEIA_SR_VULKAN
}

static bool getLayerInfo(struct comp_renderer *r, int view_index, int* width, int* height, VkFormat* format, VkImageView* imageView)
{
	const int layerCount = r->c->base.layer_accum.layer_count;
	if (layerCount < 1)
		return false;

	struct comp_layer *theLayer = &r->c->base.layer_accum.layers[0];

	const struct xrt_layer_data *layer_data = &theLayer->data;
	if (layer_data->type != XRT_LAYER_PROJECTION && layer_data->type != XRT_LAYER_PROJECTION_DEPTH)
		return false;

	// Bounds check: view_index must be within layer's view_count.
	if ((uint32_t)view_index >= layer_data->view_count)
		return false;

	const struct xrt_layer_projection_view_data *vd = NULL;
	view_index_to_projection_data(view_index, layer_data, &vd);

	const uint32_t sc_array_index = is_view_index_right(view_index) ? 1 : 0;
	const uint32_t array_index = vd->sub.array_index;
	const struct comp_swapchain *sc = (struct comp_swapchain *)(comp_layer_get_swapchain(theLayer, sc_array_index));
	const struct comp_swapchain_image *image = &sc->images[vd->sub.image_index];

	*width = layer_data->proj.v[view_index].sub.rect.extent.w;
	*height = layer_data->proj.v[view_index].sub.rect.extent.h;
	*format = (VkFormat)sc->vkic.info.format;
	*imageView = get_image_view(image, layer_data->flags, array_index);

	return true;
}

/*!
 * Lazily create intermediate images for crop-blitting per-eye sub-rects before display processing.
 * Needed when both eyes reference the same swapchain image (single-swapchain SBS layout),
 * so the display processor sees UV 0..1 covering only valid per-eye content.
 */
static bool
ensure_crop_images(struct comp_renderer *r, struct vk_bundle *vk, int width, int height, VkFormat format)
{
	// Already initialized with matching size?
	if (r->crop.initialized && r->crop.width == width && r->crop.height == height && r->crop.format == format) {
		return true;
	}

	// Destroy old if size changed
	if (r->crop.initialized) {
		for (int i = 0; i < 2; i++) {
			if (r->crop.views[i] != VK_NULL_HANDLE)
				vk->vkDestroyImageView(vk->device, r->crop.views[i], NULL);
			if (r->crop.images[i] != VK_NULL_HANDLE)
				vk->vkDestroyImage(vk->device, r->crop.images[i], NULL);
			if (r->crop.memories[i] != VK_NULL_HANDLE)
				vk->vkFreeMemory(vk->device, r->crop.memories[i], NULL);
		}
		r->crop.initialized = false;
	}

	VkExtent2D extent = {.width = (uint32_t)width, .height = (uint32_t)height};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};

	for (int i = 0; i < 2; i++) {
		VkResult ret = vk_create_image_simple(vk, extent, format, usage,
		                                      &r->crop.memories[i], &r->crop.images[i]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("Failed to create crop image %d: %s", i, vk_result_string(ret));
			return false;
		}

		ret = vk_create_view(vk, r->crop.images[i], VK_IMAGE_VIEW_TYPE_2D, format, range, &r->crop.views[i]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("Failed to create crop image view %d: %s", i, vk_result_string(ret));
			return false;
		}
	}

	r->crop.width = width;
	r->crop.height = height;
	r->crop.format = format;
	r->crop.initialized = true;
	U_LOG_W("[dp] Created crop images: %dx%d format=%d", width, height, format);
	return true;
}

/*!
 * Lazy-initialize HUD GPU resources (VkImage + staging buffer).
 */
static bool
renderer_hud_init_gpu(struct comp_renderer *r, struct vk_bundle *vk)
{
	if (r->hud.gpu_initialized) {
		return true;
	}

	uint32_t hud_w = u_hud_get_width(r->hud.hud);
	uint32_t hud_h = u_hud_get_height(r->hud.hud);
	uint32_t pixel_size = hud_w * hud_h * 4;
	VkResult ret;

	// Create HUD image (TRANSFER_DST for upload, SAMPLED for blend pipeline)
	VkImageCreateInfo image_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .extent = {hud_w, hud_h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	ret = vk->vkCreateImage(vk->device, &image_info, NULL, &r->hud.image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to create image: %s", vk_result_string(ret));
		return false;
	}

	VkMemoryRequirements mem_reqs;
	vk->vkGetImageMemoryRequirements(vk->device, r->hud.image, &mem_reqs);

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mem_reqs.size,
	};
	if (!vk_get_memory_type(vk, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	                        &alloc_info.memoryTypeIndex)) {
		U_LOG_E("[HUD] Failed to find device-local memory type");
		vk->vkDestroyImage(vk->device, r->hud.image, NULL);
		r->hud.image = VK_NULL_HANDLE;
		return false;
	}

	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &r->hud.memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to allocate image memory");
		vk->vkDestroyImage(vk->device, r->hud.image, NULL);
		r->hud.image = VK_NULL_HANDLE;
		return false;
	}
	vk->vkBindImageMemory(vk->device, r->hud.image, r->hud.memory, 0);

	// Create staging buffer
	VkBufferCreateInfo buf_info = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = pixel_size,
	    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	ret = vk->vkCreateBuffer(vk->device, &buf_info, NULL, &r->hud.staging_buffer);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to create staging buffer");
		return false;
	}

	VkMemoryRequirements buf_reqs;
	vk->vkGetBufferMemoryRequirements(vk->device, r->hud.staging_buffer, &buf_reqs);

	VkMemoryAllocateInfo buf_alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = buf_reqs.size,
	};
	if (!vk_get_memory_type(vk, buf_reqs.memoryTypeBits,
	                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                        &buf_alloc.memoryTypeIndex)) {
		U_LOG_E("[HUD] Failed to find host-visible memory type");
		return false;
	}

	ret = vk->vkAllocateMemory(vk->device, &buf_alloc, NULL, &r->hud.staging_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to allocate staging memory");
		return false;
	}
	vk->vkBindBufferMemory(vk->device, r->hud.staging_buffer, r->hud.staging_memory, 0);

	ret = vk->vkMapMemory(vk->device, r->hud.staging_memory, 0, pixel_size, 0, &r->hud.staging_mapped);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to map staging buffer");
		return false;
	}

	// Init alpha-blend pipeline for semi-transparent HUD overlay
	if (!vk_hud_blend_init(&r->hud.hud_blend, vk, r->c->target->format, r->hud.image, hud_w, hud_h)) {
		U_LOG_E("[HUD] Failed to init alpha-blend pipeline");
		return false;
	}

	r->hud.gpu_initialized = true;
	U_LOG_W("[HUD] Vulkan GPU resources initialized (%ux%u)", hud_w, hud_h);
	return true;
}

/*!
 * Alpha-blend HUD overlay onto swapchain image (post-weave).
 * Records into an already-open command buffer.
 * Transitions: PRESENT_SRC -> COLOR_ATTACHMENT -> PRESENT_SRC
 */
static void
renderer_blit_hud(struct comp_renderer *r,
                  VkCommandBuffer cmd,
                  VkImage swapchain_image,
                  VkImageView swapchain_view,
                  uint32_t fb_width,
                  uint32_t fb_height,
                  bool is_mono)
{
	if (r->hud.hud == NULL || !u_hud_is_visible()) {
		return;
	}

	struct vk_bundle *vk = &r->c->base.vk;

	// Compute FPS
	uint64_t now_ns = os_monotonic_get_ns();
	if (r->hud.last_frame_time_ns != 0) {
		float dt_ms = (float)(now_ns - r->hud.last_frame_time_ns) / 1e6f;
		r->hud.smoothed_frame_time_ms = r->hud.smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	r->hud.last_frame_time_ns = now_ns;

	float fps = 0.0f;
	if (r->hud.smoothed_frame_time_ms > 0.0f) {
		fps = 1000.0f / r->hud.smoothed_frame_time_ms;
	}

	// Get eye positions and display dims
	float left_x = -32.0f, left_y = 0.0f, left_z = 600.0f;
	float right_x = 32.0f, right_y = 0.0f, right_z = 600.0f;
	bool tracking_active = false;
	float disp_w_mm = 0.0f, disp_h_mm = 0.0f;
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 600.0f;

#ifdef XRT_HAVE_LEIA_SR_VULKAN
	if (r->leiasr != NULL) {
		struct leiasr_eye_pair eyes;
		if (leiasr_get_predicted_eye_positions(r->leiasr, &eyes) && eyes.valid) {
			left_x = eyes.left.x * 1000.0f;
			left_y = eyes.left.y * 1000.0f;
			left_z = eyes.left.z * 1000.0f;
			right_x = eyes.right.x * 1000.0f;
			right_y = eyes.right.y * 1000.0f;
			right_z = eyes.right.z * 1000.0f;
			tracking_active = true;
		}

		struct leiasr_display_dimensions dims;
		if (leiasr_get_display_dimensions(r->leiasr, &dims) && dims.valid) {
			disp_w_mm = dims.width_m * 1000.0f;
			disp_h_mm = dims.height_m * 1000.0f;
			nom_x = dims.nominal_x_m * 1000.0f;
			nom_y = dims.nominal_y_m * 1000.0f;
			nom_z = dims.nominal_z_m * 1000.0f;
		}
	}
#endif

	// Fallback: get display info from sim_display device
	float zoom_scale = 0.0f;
	if (disp_w_mm == 0.0f && disp_h_mm == 0.0f) {
		struct sim_display_info sd_info;
		if (sim_display_get_display_info(r->c->xdev, &sd_info)) {
			disp_w_mm = sd_info.display_width_m * 1000.0f;
			disp_h_mm = sd_info.display_height_m * 1000.0f;
			nom_y = sd_info.nominal_y_m * 1000.0f;
			nom_z = sd_info.nominal_z_m * 1000.0f;
			zoom_scale = sd_info.zoom_scale;
		}
	}

	// Determine output mode
	const char *output_mode = "Fallback";
	if (is_mono) {
		output_mode = "2D";
	} else if (r->display_processor != NULL) {
		output_mode = "Weaved";
	}
#ifdef XRT_HAVE_LEIA_SR_VULKAN
	else if (r->leiasr != NULL) {
		output_mode = "Weaved (direct)";
	}
#endif

	// Get render dimensions from layer data
	uint32_t render_w = 0, render_h = 0;
	const struct comp_layer *layers = r->c->base.layer_accum.layers;
	uint32_t layer_count = r->c->base.layer_accum.layer_count;
	for (uint32_t i = 0; i < layer_count; i++) {
		enum xrt_layer_type type = layers[i].data.type;
		if (type == XRT_LAYER_PROJECTION || type == XRT_LAYER_PROJECTION_DEPTH) {
			render_w = layers[i].data.proj.v[0].sub.rect.extent.w;
			render_h = layers[i].data.proj.v[0].sub.rect.extent.h;
			break;
		}
	}

	// Virtual display position + forward vector from qwerty device pose.
	float vdisp_x = 0.0f, vdisp_y = 0.0f, vdisp_z = 0.0f;
	float fwd_x = 0.0f, fwd_y = 0.0f, fwd_z = -1.0f;
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (r->c->xsysd != NULL) {
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(r->c->xsysd->xdevs, r->c->xsysd->xdev_count, &qwerty_pose)) {
			vdisp_x = qwerty_pose.position.x;
			vdisp_y = qwerty_pose.position.y;
			vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			fwd_x = fwd_out.x;
			fwd_y = fwd_out.y;
			fwd_z = fwd_out.z;
		}
	}
#endif

	// Build device name with render mode for sim_display
	char device_name_buf[128];
	const char *device_name = r->c->xdev->str;
	if (zoom_scale > 0.0f) { // sim_display was detected
		const char *mode_str = "SBS";
		enum sim_display_output_mode sd_mode = sim_display_get_output_mode();
		if (sd_mode == SIM_DISPLAY_OUTPUT_ANAGLYPH) mode_str = "Anaglyph";
		else if (sd_mode == SIM_DISPLAY_OUTPUT_BLEND) mode_str = "Blended";
		snprintf(device_name_buf, sizeof(device_name_buf), "%s (%s)", r->c->xdev->str, mode_str);
		device_name = device_name_buf;
	}

	// Fill HUD data
	struct u_hud_data data = {0};
	data.device_name = device_name;
	data.fps = fps;
	data.frame_time_ms = r->hud.smoothed_frame_time_ms;
	data.mode_3d = !is_mono;
	data.output_mode = output_mode;
	data.render_width = render_w;
	data.render_height = render_h;
	data.window_width = fb_width;
	data.window_height = fb_height;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	data.left_eye_x = left_x;
	data.left_eye_y = left_y;
	data.left_eye_z = left_z;
	data.right_eye_x = right_x;
	data.right_eye_y = right_y;
	data.right_eye_z = right_z;
	data.eye_tracking_active = tracking_active;
	data.zoom_scale = zoom_scale;
	data.vdisp_x = vdisp_x;
	data.vdisp_y = vdisp_y;
	data.vdisp_z = vdisp_z;
	data.forward_x = fwd_x;
	data.forward_y = fwd_y;
	data.forward_z = fwd_z;

	bool dirty = u_hud_update(r->hud.hud, &data);

	// Lazy-init GPU resources
	if (!renderer_hud_init_gpu(r, vk)) {
		return;
	}

	uint32_t hud_w = u_hud_get_width(r->hud.hud);
	uint32_t hud_h = u_hud_get_height(r->hud.hud);

	// Upload pixels to staging buffer if changed
	if (dirty) {
		memcpy(r->hud.staging_mapped, u_hud_get_pixels(r->hud.hud), hud_w * hud_h * 4);
	}

	// Transition HUD image: UNDEFINED -> TRANSFER_DST
	VkImageMemoryBarrier hud_to_dst = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .image = r->hud.image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
	                         0, NULL, 1, &hud_to_dst);

	// Copy staging buffer -> HUD image
	VkBufferImageCopy copy_region = {
	    .bufferOffset = 0,
	    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .imageExtent = {hud_w, hud_h, 1},
	};
	vk->vkCmdCopyBufferToImage(cmd, r->hud.staging_buffer, r->hud.image,
	                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

	// Transition HUD image: TRANSFER_DST -> SHADER_READ_ONLY (for sampling in blend pipeline)
	VkImageMemoryBarrier hud_to_src = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = r->hud.image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
	                         NULL, 0, NULL, 1, &hud_to_src);

	// Alpha-blend HUD onto swapchain (PRESENT_SRC -> COLOR_ATTACHMENT -> PRESENT_SRC)
	vk_hud_blend_draw(&r->hud.hud_blend, vk, cmd, swapchain_view, swapchain_image, fb_width, fb_height, hud_w,
	                   hud_h);
}

static void
do_weaving(struct comp_renderer *r,
           struct render_gfx *render,
           struct render_gfx_target_resources *rtr,
           const struct comp_layer *layer,
           struct chl_frame_state *frame_state)
{

#ifdef XRT_HAVE_CNSDK
	if (r->cnsdk != NULL) {
		int iw = 0, ih = 0;
		VkFormat ifmt = VK_FORMAT_UNDEFINED;
		VkImageView leftView = VK_NULL_HANDLE, rightView = VK_NULL_HANDLE;
		bool lok = getLayerInfo(r, 0, &iw, &ih, &ifmt, &leftView);
		bool rok = getLayerInfo(r, 1, &iw, &ih, &ifmt, &rightView);
		if (lok && rok) {
			leia_cnsdk_weave(r->cnsdk,
			                 r->c->base.vk.device,
			                 r->c->base.vk.physical_device,
			                 leftView, rightView,
			                 r->c->target->format,
			                 rtr->data.width, rtr->data.height,
			                 rtr->framebuffer,
			                 r->c->target->images[r->acquired_buffer].handle);
		}
	}
#endif

	// Generic display processor path — routes through xrt_display_processor
	// interface instead of calling vendor-specific weaving functions directly.
	if (r->display_processor != NULL) {
		// Get command buffer.
		VkCommandBuffer commandBuffer = render->r->cmd;

		// Get framebuffer.
		VkFramebuffer framebuffer = rtr->framebuffer;
		int framebufferWidth = rtr->extent.width;
		int framebufferHeight = rtr->extent.height;
		VkFormat framebufferFormat = rtr->rgrp->format;

		// Get views.
		int imageWidth = 0;
		int imageHeight = 0;
		VkFormat imageFormat = VK_FORMAT_UNDEFINED;
		VkImageView leftImageView = VK_NULL_HANDLE;
		VkImageView rightImageView = VK_NULL_HANDLE;
		bool leftViewOk = getLayerInfo(r, 0, &imageWidth, &imageHeight, &imageFormat, &leftImageView);
		bool rightViewOk = getLayerInfo(r, 1, &imageWidth, &imageHeight, &imageFormat, &rightImageView);

		{
			static bool dp_dims_logged = false;
			if (!dp_dims_logged && leftViewOk && rightViewOk) {
				U_LOG_W("[dp] source=%dx%d target=%dx%d",
				        imageWidth, imageHeight, framebufferWidth, framebufferHeight);
				dp_dims_logged = true;
			}
		}

		// Process views through the display processor.
		if (leftViewOk && rightViewOk) {
			struct vk_bundle *vk = &r->c->base.vk;
			const struct xrt_layer_projection_view_data *vd_left = &layer->data.proj.v[0];
			const struct xrt_layer_projection_view_data *vd_right = &layer->data.proj.v[1];
			struct comp_swapchain *sc_left =
			    (struct comp_swapchain *)comp_layer_get_swapchain(layer, 0);
			struct comp_swapchain *sc_right =
			    (struct comp_swapchain *)comp_layer_get_swapchain(layer, 1);
			VkImage img_left = sc_left->vkic.images[vd_left->sub.image_index].handle;
			VkImage img_right = sc_right->vkic.images[vd_right->sub.image_index].handle;
			bool same_image = (img_left == img_right);

			// Extract sub-rect offsets for each eye
			int leftOffsetX = layer->data.proj.v[0].sub.rect.offset.w;
			int leftOffsetY = layer->data.proj.v[0].sub.rect.offset.h;
			int rightOffsetX = layer->data.proj.v[1].sub.rect.offset.w;
			int rightOffsetY = layer->data.proj.v[1].sub.rect.offset.h;

			// Determine if crop-blit is needed: when eyes share the same image
			// or have non-zero offsets (sub-rect addressing).
			bool need_crop = same_image ||
			                 leftOffsetX != 0 || leftOffsetY != 0 ||
			                 rightOffsetX != 0 || rightOffsetY != 0;

			VkImageView weaveLeft = leftImageView;
			VkImageView weaveRight = rightImageView;

			// Reset command pool (discards chl_frame_state commands) and start fresh recording.
			render_gfx_begin(render);

			if (need_crop) {
				// Crop-blit: extract per-eye sub-rects into intermediate images
				// so the display processor sees UV 0..1 covering only valid content.
				if (!ensure_crop_images(r, vk, imageWidth, imageHeight, imageFormat)) {
					U_LOG_E("[dp] Failed to ensure crop images");
					render_gfx_end(render);
					return;
				}

				bool flip_y = layer->data.flip_y;
				int left_src_top = leftOffsetY + (flip_y ? imageHeight : 0);
				int left_src_bot = leftOffsetY + (flip_y ? 0 : imageHeight);
				int right_src_top = rightOffsetY + (flip_y ? imageHeight : 0);
				int right_src_bot = rightOffsetY + (flip_y ? 0 : imageHeight);

				// Pre-barriers: source COLOR_ATTACHMENT → TRANSFER_SRC,
				// crop images UNDEFINED → TRANSFER_DST.
				// When same_image, deduplicate source barrier (3 instead of 4).
				uint32_t pre_barrier_count = same_image ? 3 : 4;
				VkImageMemoryBarrier pre_barriers[4] = {
				    {
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .image = img_left,
				        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				    },
				    {
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask = 0,
				        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				        .image = r->crop.images[0],
				        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				    },
				    {
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask = 0,
				        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				        .image = r->crop.images[1],
				        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				    },
				};
				if (!same_image) {
					pre_barriers[3] = pre_barriers[2];
					pre_barriers[2] = pre_barriers[1];
					pre_barriers[1] = (VkImageMemoryBarrier){
					    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
					    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					    .image = img_right,
					    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
					};
				}
				vk->vkCmdPipelineBarrier(commandBuffer,
				                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				                         VK_PIPELINE_STAGE_TRANSFER_BIT,
				                         0, 0, NULL, 0, NULL, pre_barrier_count, pre_barriers);

				// Blit left eye sub-region into crop image 0
				VkImageBlit left_blit = {
				    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .srcOffsets = {{leftOffsetX, left_src_top, 0},
				                   {leftOffsetX + imageWidth, left_src_bot, 1}},
				    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .dstOffsets = {{0, 0, 0}, {imageWidth, imageHeight, 1}},
				};
				vk->vkCmdBlitImage(commandBuffer,
				                   img_left, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				                   r->crop.images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				                   1, &left_blit, VK_FILTER_NEAREST);

				// Blit right eye sub-region into crop image 1
				VkImageBlit right_blit = {
				    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .srcOffsets = {{rightOffsetX, right_src_top, 0},
				                   {rightOffsetX + imageWidth, right_src_bot, 1}},
				    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .dstOffsets = {{0, 0, 0}, {imageWidth, imageHeight, 1}},
				};
				vk->vkCmdBlitImage(commandBuffer,
				                   img_right, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				                   r->crop.images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				                   1, &right_blit, VK_FILTER_NEAREST);

				// Post-barriers: source → COLOR_ATTACHMENT (restore),
				// crop images → SHADER_READ_ONLY (for display processor sampling).
				uint32_t post_barrier_count = same_image ? 3 : 4;
				VkImageMemoryBarrier post_barriers[4] = {
				    {
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        .image = img_left,
				        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				    },
				    {
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				        .image = r->crop.images[0],
				        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				    },
				    {
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				        .image = r->crop.images[1],
				        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				    },
				};
				if (!same_image) {
					post_barriers[3] = post_barriers[2];
					post_barriers[2] = post_barriers[1];
					post_barriers[1] = (VkImageMemoryBarrier){
					    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
					    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					    .image = img_right,
					    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
					};
				}
				vk->vkCmdPipelineBarrier(commandBuffer,
				                         VK_PIPELINE_STAGE_TRANSFER_BIT,
				                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                         0, 0, NULL, 0, NULL, post_barrier_count, post_barriers);

				weaveLeft = r->crop.views[0];
				weaveRight = r->crop.views[1];
			} else {
				// No crop needed: separate swapchains with zero offsets.
				// Just transition source images directly to SHADER_READ_ONLY.
				VkImageMemoryBarrier barriers[2] = {
				    {
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				        .image = img_left,
				        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				    },
				    {
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				        .image = img_right,
				        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				    },
				};
				vk->vkCmdPipelineBarrier(commandBuffer,
				                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                         0, 0, NULL, 0, NULL, 2, barriers);
			}

			// Display processor render pass handles target layout:
			// initialLayout=UNDEFINED → finalLayout=PRESENT_SRC_KHR
			xrt_display_processor_process_views(
			    r->display_processor,
			    commandBuffer,
			    weaveLeft, weaveRight,
			    imageWidth, imageHeight,
			    (VkFormat_XDP)imageFormat,
			    framebuffer,
			    framebufferWidth, framebufferHeight,
			    (VkFormat_XDP)framebufferFormat);

			// HUD overlay (post-weave, crisp text not interlaced)
			renderer_blit_hud(r, commandBuffer,
			                  r->c->target->images[r->acquired_buffer].handle,
			                  r->c->target->images[r->acquired_buffer].view,
			                  framebufferWidth, framebufferHeight, false);

			render_gfx_end(render);
		}
	}
}
/*
 *
 * Graphics
 *
 */

/*!
 * @pre render_gfx_init(render, &c->nr)
 */
static XRT_CHECK_RESULT VkResult
dispatch_graphics(struct comp_renderer *r,
                  struct render_gfx *render,
                  struct chl_frame_state *frame_state,
                  enum comp_target_fov_source fov_source)
{
	COMP_TRACE_MARKER();

	struct comp_compositor *c = r->c;
	struct vk_bundle *vk = &c->base.vk;
	VkResult ret;

	// Basics
	const struct comp_layer *layers = c->base.layer_accum.layers;
	uint32_t layer_count = c->base.layer_accum.layer_count;

	// Resources for the distortion render target.
	struct render_gfx_target_resources *rtr = &r->rtr_array[r->acquired_buffer];

	// Detect mono from first projection layer's view_count.
	// Mono submission (view_count == 1) means 2D display mode — single view,
	// full-width viewport, no stereo interlacing/weaving.
	bool is_mono = false;
	for (uint32_t i = 0; i < layer_count; i++) {
		if (layers[i].data.type == XRT_LAYER_PROJECTION ||
		    layers[i].data.type == XRT_LAYER_PROJECTION_DEPTH) {
			is_mono = (layers[i].data.view_count == 1);
			break;
		}
	}

	// For mono: override frame state to single view so all downstream
	// loops (squash, distortion, target) iterate once for view 0 only.
	uint32_t effective_view_count = is_mono ? 1 : render->r->view_count;
	if (is_mono) {
		frame_state->view_count = 1;
	}

	// Viewport information.
	struct render_viewport_data viewport_datas[XRT_MAX_VIEWS];
	if (is_mono) {
		// Mono: single viewport covering the full render target.
		viewport_datas[0] = (struct render_viewport_data){
		    .x = 0,
		    .y = 0,
		    .w = r->c->target->width,
		    .h = r->c->target->height,
		};
	} else {
		calc_viewport_data(r, viewport_datas, render->r->view_count);
	}

	// Vertex rotation information.
	struct xrt_matrix_2x2 vertex_rots[XRT_MAX_VIEWS];
	calc_vertex_rot_data(r, vertex_rots, effective_view_count);

	// Device view information.
	struct xrt_fov fovs[XRT_MAX_VIEWS];
	struct xrt_pose world_poses[XRT_MAX_VIEWS];
	struct xrt_pose eye_poses[XRT_MAX_VIEWS];
	calc_pose_data(                //
	    r,                         //
	    fov_source,                //
	    fovs,                      //
	    world_poses,               //
	    eye_poses,                 //
	    effective_view_count);     //

	// Inline chl_frame_state_gfx_default_pipeline to keep command buffer
	// open for HUD overlay blit (in mono path; stereo uses do_weaving).
	chl_frame_state_gfx_set_views( //
	    frame_state,               //
	    world_poses,               //
	    eye_poses,                 //
	    fovs,                      //
	    layer_count);              //

	chl_frame_state_gfx_set_target( //
	    frame_state,                //
	    rtr,                        //
	    viewport_datas,             //
	    vertex_rots);               //

	render_gfx_begin(render);

	comp_render_gfx_dispatch( //
	    render,               //
	    layers,               //
	    layer_count,          //
	    &frame_state->data);  //

	if (!is_mono) {
		// End current cmd buffer before do_weaving (which resets the
		// cmd pool for display processor, or does nothing for fallback).
		render_gfx_end(render);
		do_weaving(r, render, rtr, layers, frame_state);
	} else {
		// Mono: blit HUD while cmd buffer is still open, then end.
		renderer_blit_hud(r, render->r->cmd,
		                  r->c->target->images[r->acquired_buffer].handle,
		                  r->c->target->images[r->acquired_buffer].view,
		                  rtr->extent.width, rtr->extent.height, true);
		render_gfx_end(render);
	}

	// Everything is ready, submit to the queue.
	ret = renderer_submit_queue(r, render->r->cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	VK_CHK_AND_RET(ret, "renderer_submit_queue");

	return ret;
}

/*
 *
 * Compute
 *
 */

/*!
 * @pre render_compute_init(render, &c->nr)
 */
static XRT_CHECK_RESULT VkResult
dispatch_compute(struct comp_renderer *r,
                 struct render_compute *render,
                 struct chl_frame_state *frame_state,
                 enum comp_target_fov_source fov_source)
{
	COMP_TRACE_MARKER();

	struct comp_compositor *c = r->c;
	struct vk_bundle *vk = &c->base.vk;
	VkResult ret;

	// Basics
	const struct comp_layer *layers = c->base.layer_accum.layers;
	uint32_t layer_count = c->base.layer_accum.layer_count;

	// Detect mono from first projection layer's view_count.
	bool is_mono = false;
	for (uint32_t i = 0; i < layer_count; i++) {
		if (layers[i].data.type == XRT_LAYER_PROJECTION ||
		    layers[i].data.type == XRT_LAYER_PROJECTION_DEPTH) {
			is_mono = (layers[i].data.view_count == 1);
			break;
		}
	}

	uint32_t effective_view_count = is_mono ? 1 : render->r->view_count;
	if (is_mono) {
		frame_state->view_count = 1;
	}

	// Device view information.
	struct xrt_fov fovs[XRT_MAX_VIEWS];
	struct xrt_pose world_poses[XRT_MAX_VIEWS];
	struct xrt_pose eye_poses[XRT_MAX_VIEWS];
	calc_pose_data(                //
	    r,                         //
	    fov_source,                //
	    fovs,                      //
	    world_poses,               //
	    eye_poses,                 //
	    effective_view_count);     //

	// Target Vulkan resources..
	VkImage target_image = r->c->target->images[r->acquired_buffer].handle;
	VkImageView target_storage_view = r->c->target->images[r->acquired_buffer].view;

	// Target view information.
	struct render_viewport_data target_viewport_datas[XRT_MAX_VIEWS];
	if (is_mono) {
		target_viewport_datas[0] = (struct render_viewport_data){
		    .x = 0,
		    .y = 0,
		    .w = r->c->target->width,
		    .h = r->c->target->height,
		};
	} else {
		calc_viewport_data(r, target_viewport_datas, render->r->view_count);
	}

	// Does everything.
	chl_frame_state_cs_default_pipeline( //
	    frame_state,                     //
	    render,                          //
	    layers,                          //
	    layer_count,                     //
	    world_poses,                     //
	    eye_poses,                       //
	    fovs,                            //
	    target_image,                    //
	    target_storage_view,             //
	    target_viewport_datas);          //

	// Everything is ready, submit to the queue.
	ret = renderer_submit_queue(r, render->r->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	VK_CHK_AND_RET(ret, "renderer_submit_queue");

	return ret;
}


/*
 *
 * Interface functions.
 *
 */

XRT_CHECK_RESULT xrt_result_t
comp_renderer_draw(struct comp_renderer *r)
{
	COMP_TRACE_MARKER();

	struct comp_target *ct = r->c->target;
	struct comp_compositor *c = r->c;

	// Check that we don't have any bad data.
	assert(!comp_frame_is_invalid_locked(&c->frame.waited));
	assert(comp_frame_is_invalid_locked(&c->frame.rendering));

	// Move waited frame to rendering frame, clear waited.
	comp_frame_move_and_clear_locked(&c->frame.rendering, &c->frame.waited);

	// Tell the target we are starting to render, for frame timing.
	comp_target_mark_begin(ct, c->frame.rendering.id, os_monotonic_get_ns());

#ifdef XRT_OS_WINDOWS
	// During window drag, synchronize with the window thread's WM_PAINT cycle.
	// This ensures the window position is stable between weave() and Present(),
	// keeping the SR interlacing pattern aligned with the display position.
	if (comp_window_mswin_is_in_size_move(ct)) {
		comp_window_mswin_wait_for_paint(ct);
	}
#endif

	// Are we ready to render? No - skip rendering.
	if (!comp_target_check_ready(r->c->target)) {
		// Need to emulate rendering for the timing.
		//! @todo This should be discard.
		comp_target_mark_submit_begin(ct, c->frame.rendering.id, os_monotonic_get_ns());
		comp_target_mark_submit_end(ct, c->frame.rendering.id, os_monotonic_get_ns());

		// Clear the rendering frame.
		comp_frame_clear_locked(&c->frame.rendering);
		return XRT_SUCCESS;
	}

	comp_target_flush(ct);

	comp_target_update_timings(ct);

	if (r->acquired_buffer < 0) {
		// Ensures that renderings are created.
		renderer_acquire_swapchain_image(r);
	}

	comp_target_update_timings(ct);

	// Hardcoded for now.
	const uint32_t view_count = c->nr.view_count;
	enum comp_target_fov_source fov_source = COMP_TARGET_FOV_SOURCE_DISTORTION;

	bool fast_path = c->base.frame_params.one_projection_layer_fast_path;
	bool do_timewarp = !c->debug.atw_off;

	// Consistency check.
	assert(!fast_path || c->base.layer_accum.layer_count >= 1);

	// For scratch image debugging.
	struct chl_frame_state frame_state;
	chl_frame_state_init( //
	    &frame_state,     //
	    &c->nr,           //
	    view_count,       //
	    do_timewarp,      //
	    fast_path,        //
	    &c->scratch);     //

	bool use_compute = r->settings->use_compute;
	struct render_gfx render_g = {0};
	struct render_compute render_c = {0};

	VkResult res = VK_SUCCESS;
	if (use_compute) {
		render_compute_init(&render_c, &c->nr);
		res = dispatch_compute(r, &render_c, &frame_state, fov_source);
	} else {
		render_gfx_init(&render_g, &c->nr);
		res = dispatch_graphics(r, &render_g, &frame_state, fov_source);
	}
	if (res != VK_SUCCESS) {
		return XRT_ERROR_VULKAN;
	}

#ifdef XRT_FEATURE_WINDOW_PEEK
	if (c->peek) {
		switch (comp_window_peek_get_eye(c->peek)) {
		case COMP_WINDOW_PEEK_EYE_LEFT: {
			uint32_t scratch_index = frame_state.scratch_state.views[0].index;
			struct comp_scratch_single_images *view = &c->scratch.views[0].cssi;

			comp_window_peek_blit(                 //
			    c->peek,                           //
			    view->images[scratch_index].image, //
			    view->info.width,                  //
			    view->info.height);                //
		} break;
		case COMP_WINDOW_PEEK_EYE_RIGHT: {
			uint32_t scratch_index = frame_state.scratch_state.views[1].index;
			struct comp_scratch_single_images *view = &c->scratch.views[1].cssi;

			comp_window_peek_blit(                 //
			    c->peek,                           //
			    view->images[scratch_index].image, //
			    view->info.width,                  //
			    view->info.height);                //
		} break;
		case COMP_WINDOW_PEEK_EYE_BOTH:
			/* TODO: display the undistorted image */
			comp_window_peek_blit(c->peek, c->target->images[r->acquired_buffer].handle, c->target->width,
			                      c->target->height);
			break;
		}
	}
#endif

	renderer_present_swapchain_image(r, c->frame.rendering.desired_present_time_ns,
	                                 c->frame.rendering.present_slop_ns);

#ifdef XRT_OS_WINDOWS
	// Signal the WM_PAINT handler that rendering is done, unblocking the modal drag loop.
	comp_window_mswin_signal_paint_done(ct);
#endif

	// macOS: CA flushing is handled by oxr_macos_pump_events on the main thread.

	// Save for timestamps below.
	uint64_t frame_id = c->frame.rendering.id;
	uint64_t desired_present_time_ns = c->frame.rendering.desired_present_time_ns;
	uint64_t predicted_display_time_ns = c->frame.rendering.predicted_display_time_ns;

	// Clear the rendered frame.
	comp_frame_clear_locked(&c->frame.rendering);

	xrt_result_t xret = XRT_SUCCESS;
	comp_mirror_fixup_ui_state(&r->mirror_to_debug_gui, c);
	if (comp_mirror_is_ready_and_active(&r->mirror_to_debug_gui, c, predicted_display_time_ns)) {

		uint32_t scratch_index = frame_state.scratch_state.views[0].index;
		struct comp_scratch_single_images *view = &c->scratch.views[0].cssi;
		struct render_scratch_color_image *rsci = &view->images[scratch_index];
		VkExtent2D extent = {view->info.width, view->info.width};

		// Used for both, want clamp to edge to no bring in black.
		VkSampler clamp_to_edge = c->nr.samplers.clamp_to_edge;

		// Covers the whole view.
		struct xrt_normalized_rect rect = {0, 0, 1.0f, 1.0f};

		xret = comp_mirror_do_blit(    //
		    &r->mirror_to_debug_gui,   //
		    &c->base.vk,               //
		    frame_id,                  //
		    predicted_display_time_ns, //
		    rsci->image,               //
		    rsci->srgb_view,           //
		    clamp_to_edge,             //
		    extent,                    //
		    rect);                     //
	}

	/*
	 * This fixes a lot of validation issues as it makes sure that the
	 * command buffer has completed and all resources referred by it can
	 * now be manipulated.
	 *
	 * This is done after a swap so isn't time critical.
	 */
	renderer_wait_queue_idle(r);

	/*
	 * Free any resources and finalize the scratch images,
	 * which sends them send to debug UI if it is active.
	 */
	chl_frame_state_fini(&frame_state);

	// Check timestamps.
	if (xret == XRT_SUCCESS) {
		/*
		 * Get timestamps of GPU work (if available).
		 */

		uint64_t gpu_start_ns, gpu_end_ns;
		if (render_resources_get_timestamps(&c->nr, &gpu_start_ns, &gpu_end_ns)) {
			uint64_t now_ns = os_monotonic_get_ns();
			comp_target_info_gpu(ct, frame_id, gpu_start_ns, gpu_end_ns, now_ns);
		}
	}


	/*
	 * Free resources.
	 */

	if (use_compute) {
		render_compute_fini(&render_c);
	} else {
		render_gfx_fini(&render_g);
	}

	renderer_wait_for_present(r, desired_present_time_ns);

	comp_target_update_timings(ct);

	return xret;
}

struct comp_renderer *
comp_renderer_create(struct comp_compositor *c, VkExtent2D scratch_extent)
{
	struct comp_renderer *r = U_TYPED_CALLOC(struct comp_renderer);

	renderer_init(r, c, scratch_extent);

	return r;
}

void
comp_renderer_destroy(struct comp_renderer **ptr_r)
{
	if (ptr_r == NULL) {
		return;
	}

	struct comp_renderer *r = *ptr_r;
	if (r == NULL) {
		return;
	}

	renderer_fini(r);

	free(r);
	*ptr_r = NULL;
}

void
comp_renderer_add_debug_vars(struct comp_renderer *self)
{
	struct comp_renderer *r = self;

	comp_mirror_add_debug_vars(&r->mirror_to_debug_gui, r->c);
}
