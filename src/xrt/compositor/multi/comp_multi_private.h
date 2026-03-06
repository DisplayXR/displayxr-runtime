// Copyright 2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  System compositor capable of supporting multiple clients: internal structs.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_multi
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_processor.h"

#include "os/os_time.h"
#include "os/os_threading.h"

#include "util/u_hud.h"
#include "util/u_pacing.h"
#include "util/comp_target_service.h"
#include "multi/comp_multi_interface.h"

// Vulkan types needed for Y-flip SBS image and display processor support
// (comp_multi always links aux_vk, so Vulkan is always available)
#include "xrt/xrt_vulkan_includes.h"
#include "vk/vk_hud_blend.h"

#include "render/render_interface.h"

// Forward declarations for per-session rendering
struct comp_target;
struct xrt_eye_pair;
struct xrt_window_metrics;
struct xrt_system_devices;

#ifdef XRT_OS_WINDOWS
struct comp_d3d11_window;
#endif

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * Number of max active clients.
 *
 * @todo Move to `xrt_limits.h`, or make dynamic to remove limit.
 * @ingroup comp_multi
 */
#define MULTI_MAX_CLIENTS 64

/*!
 * Number of max active layers per @ref multi_compositor.
 *
 * @todo Move to `xrt_limits.h` and share.
 * @ingroup comp_multi
 */
#define MULTI_MAX_LAYERS XRT_MAX_LAYERS


/*
 *
 * Native compositor.
 *
 */

/*!
 * Data for a single composition layer.
 *
 * Similar in function to @ref comp_layer
 *
 * @ingroup comp_multi
 */
struct multi_layer_entry
{
	/*!
	 * Device to get pose from.
	 */
	struct xrt_device *xdev;

	/*!
	 * Pointers to swapchains.
	 *
	 * How many are actually used depends on the value of @p data.type
	 */
	struct xrt_swapchain *xscs[2 * XRT_MAX_VIEWS];

	/*!
	 * All basic (trivially-serializable) data associated with a layer,
	 * aside from which swapchain(s) are used.
	 */
	struct xrt_layer_data data;
};

/*!
 * Render state for a single client, including all layers.
 *
 * @ingroup comp_multi
 */
struct multi_layer_slot
{
	struct xrt_layer_frame_data data;
	uint32_t layer_count;
	struct multi_layer_entry layers[MULTI_MAX_LAYERS];
	bool active;
};

/*!
 * A single compositor for feeding the layers from one session/app into
 * the multi-client-capable system compositor.
 *
 * An instance (usually an IPC server instance) might have several of
 * these at once, feeding layers to a single multi-client-capable system
 * compositor.
 *
 * @ingroup comp_multi
 * @implements xrt_compositor_native
 */
struct multi_compositor
{
	struct xrt_compositor_native base;

	// Client info.
	struct xrt_session_info xsi;

	//! Where events for this compositor should go.
	struct xrt_session_event_sink *xses;

	//! Owning system compositor.
	struct multi_system_compositor *msc;

	//! System devices for qwerty input forwarding to self-owned window
	struct xrt_system_devices *xsysd;

	//! Used to implement wait frame, only used for in process.
	struct os_precise_sleeper frame_sleeper;

	//! Used when waiting for the scheduled frame to complete.
	struct os_precise_sleeper scheduled_sleeper;

	struct
	{
		bool visible;
		bool focused;

		int64_t z_order;

		bool session_active;
	} state;

	struct
	{
		//! Fence to wait for.
		struct xrt_compositor_fence *xcf;

		//! Timeline semaphore to wait for.
		struct xrt_compositor_semaphore *xcsem;

		//! Timeline semaphore value to wait for.
		uint64_t value;

		//! Frame id of frame being waited on.
		int64_t frame_id;

		//! The wait thread itself
		struct os_thread_helper oth;

		//! Have we gotten to the loop?
		bool alive;

		//! Is the thread waiting, if so the client should block.
		bool waiting;

		/*!
		 * Is the client thread blocked?
		 *
		 * Set to true by the client thread,
		 * cleared by the wait thread to release the client thread.
		 */
		bool blocked;
	} wait_thread;

	//! Lock for all of the slots.
	struct os_mutex slot_lock;

	/*!
	 * The next which the next frames to be picked up will be displayed.
	 */
	int64_t slot_next_frame_display;

	/*!
	 * Currently being transferred or waited on.
	 * Not protected by the slot lock as it is only touched by the client thread.
	 */
	struct multi_layer_slot progress;

	//! Scheduled frames for a future timepoint.
	struct multi_layer_slot scheduled;

	/*!
	 * Fully ready to be used.
	 * Not protected by the slot lock as it is only touched by the main render loop thread.
	 */
	struct multi_layer_slot delivered;

	struct u_pacing_app *upa;

	float current_refresh_rate_hz;

	/*!
	 * Per-session rendering resources for XR_EXT_win32_window_binding.
	 * When external_window_handle is set, this session renders to its own window.
	 */
	struct
	{
		//! External window handle (HWND on Windows), NULL for shared rendering
		void *external_window_handle;

		//! Readback callback for offscreen compositing (composited RGBA pixels)
		void (*readback_callback)(const uint8_t *, uint32_t, uint32_t, void *);
		void *readback_userdata;

		//! Shared GPU texture handle for zero-copy offscreen compositing
		//! (HANDLE on Win32, IOSurfaceRef on macOS). NULL if not used.
		void *shared_texture_handle;

		//! Per-session render target (VkSwapchain from external HWND)
		struct comp_target *target;

		//! Generic display output processor for this session.
		//! Wraps vendor-specific display processing (created via factory).
		struct xrt_display_processor *display_processor;

		//! @name Generic per-session Vulkan rendering resources
		//! Used by any display processor path.
		//! @{

		//! Command pool for per-session rendering
		VkCommandPool cmd_pool;

		//! Pre-allocated command buffers (one per swapchain image)
		VkCommandBuffer *cmd_buffers;

		//! Per-frame completion fences (one per swapchain image)
		VkFence *fences;

		//! Size of cmd_buffers and fences arrays
		uint32_t buffer_count;

		//! Index of buffer with pending fence (-1 = none)
		int32_t fenced_buffer;

		//! True if swapchain needs recreation (set on VK_SUBOPTIMAL_KHR)
		bool swapchain_needs_recreate;

		//! Render pass for display processor output (single color attachment, no depth)
		VkRenderPass render_pass;

		//! Framebuffers for display processor output (one per swapchain image)
		VkFramebuffer *framebuffers;

		//! @}

		//! @name Window-space layer compositing resources (vendor-neutral Vulkan)
		//! Used to composite overlay/HUD layers onto projection layers before
		//! display processing. Pure Vulkan — no vendor SDK dependencies.
		//! @{

		//! Per-eye composite images (one per eye, not side-by-side)
		VkImage composite_images[2];
		VkDeviceMemory composite_memories[2];
		VkImageView composite_eye_views[2];      //!< Per-eye image views for display processor input
		VkFramebuffer composite_framebuffers[2]; //!< Per-eye framebuffers for overlay rendering
		VkRenderPass composite_render_pass;      //!< LOAD_OP_LOAD for overlay compositing
		VkPipeline composite_pipeline;           //!< Alpha-blended quad pipeline
		VkPipelineLayout composite_pipe_layout;
		VkDescriptorSetLayout composite_desc_layout;
		VkDescriptorPool composite_desc_pool;
		VkDescriptorSet composite_desc_sets[XRT_MAX_LAYERS]; //!< One per possible window-space layer
		VkSampler composite_sampler;
		VkBuffer composite_ubo_buffer;           //!< Persistent UBO for window-space layer data
		VkDeviceMemory composite_ubo_memory;     //!< Memory backing composite_ubo_buffer
		void *composite_ubo_mapped;              //!< Persistently mapped UBO pointer
		uint32_t composite_width;                //!< Single eye width
		uint32_t composite_height;               //!< Eye height
		bool composite_initialized;              //!< True if composite resources are ready

		//! Pre-blit local copies of shared projection images (Intel CCS workaround).
		//! vkCmdBlitImage works for cross-device shared images on Intel; shader
		//! sampling does not. These compositor-owned copies are sampled instead.
		VkImage preblit_images[2];
		VkDeviceMemory preblit_memories[2];
		VkImageView preblit_views[2];

		//! Per-session shaders (loaded on demand, avoids invalid comp_compositor cast)
		struct render_shaders shaders;
		bool shaders_loaded;
		VkPipelineCache pipeline_cache;

		//! @}

		//! @name Display processor crop images (imageRect sub-region extraction)
		//! When the app renders to a sub-region of the swapchain (imageRect.extent
		//! < swapchain size), we must crop-blit into these intermediates before
		//! passing to the display processor, which samples UVs 0..1 on its input.
		//! @{
		VkImage dp_crop_images[2];          //!< Per-eye cropped images
		VkDeviceMemory dp_crop_memories[2];
		VkImageView dp_crop_views[2];       //!< Per-eye image views for display processor
		int dp_crop_width;
		int dp_crop_height;
		VkFormat dp_crop_format;
		bool dp_crop_initialized;
		//! @}

		//! @name SBS (side-by-side) flip image for GL textures (Y-flip + stereo packing)
		//! Used by any display processor, not Leia-specific.
		//! @{
		VkImage flip_sbs_image;          //!< Single SBS image (2*eye_width x eye_height)
		VkDeviceMemory flip_sbs_memory;
		VkImageView flip_sbs_view;       //!< Full-image view covering both L/R halves
		int flip_width;                  //!< Per-eye width
		int flip_height;
		VkFormat flip_format;
		bool flip_initialized;
		//! @}

#ifdef XRT_OS_WINDOWS
		//! Self-created window when no external HWND provided (Windows only)
		struct comp_d3d11_window *own_window;
#endif

		//! True if we created the window ourselves (must destroy on end_session)
		bool owns_window;

		//! @name HUD overlay (runtime-owned windows only)
		//! @{
		struct u_hud *hud;
		VkImage hud_image;
		VkDeviceMemory hud_memory;
		VkBuffer hud_staging_buffer;
		VkDeviceMemory hud_staging_memory;
		void *hud_staging_mapped;
		struct vk_hud_blend hud_blend; //!< Alpha-blended HUD overlay pipeline
		bool hud_gpu_initialized;
		uint64_t hud_last_frame_time_ns;
		float hud_smoothed_frame_time_ms;
		//! @}

		//! Saved device output mode before 2D switch (-1 = none saved)
		int saved_output_mode;

		//! True if window-close exit request has been pushed (avoids duplicates)
		bool window_close_exit_sent;

		//! True if per-session resources are initialized
		bool initialized;
	} session_render;
};

/*!
 * Small helper go from @ref xrt_compositor to @ref multi_compositor.
 *
 * @ingroup comp_multi
 */
static inline struct multi_compositor *
multi_compositor(struct xrt_compositor *xc)
{
	return (struct multi_compositor *)xc;
}

/*!
 * Create a multi client wrapper compositor.
 *
 * @ingroup comp_multi
 */
xrt_result_t
multi_compositor_create(struct multi_system_compositor *msc,
                        const struct xrt_session_info *xsi,
                        struct xrt_session_event_sink *xses,
                        struct xrt_compositor_native **out_xcn);

/*!
 * Push a event to be delivered to the session that corresponds
 * to the given @ref multi_compositor.
 *
 * @ingroup comp_multi
 * @private @memberof multi_compositor
 */
XRT_CHECK_RESULT xrt_result_t
multi_compositor_push_event(struct multi_compositor *mc, const union xrt_session_event *xse);

/*!
 * Deliver any scheduled frames at that is to be display at or after the given @p display_time_ns. Called by the render
 * thread and copies data from multi_compositor::scheduled to multi_compositor::delivered while holding the slot_lock.
 *
 * @ingroup comp_multi
 * @private @memberof multi_compositor
 */
void
multi_compositor_deliver_any_frames(struct multi_compositor *mc, int64_t display_time_ns);

/*!
 * Makes the current delivered frame as latched, called by the render thread.
 * The list_and_timing_lock is held when this function is called.
 *
 * @ingroup comp_multi
 * @private @memberof multi_compositor
 */
void
multi_compositor_latch_frame_locked(struct multi_compositor *mc, int64_t when_ns, int64_t system_frame_id);

/*!
 * Clears and retires the delivered frame, called by the render thread.
 * The list_and_timing_lock is held when this function is called.
 *
 * @ingroup comp_multi
 * @private @memberof multi_compositor
 */
void
multi_compositor_retire_delivered_locked(struct multi_compositor *mc, int64_t when_ns);


/*
 *
 * Multi-client-capable system compositor
 *
 */

/*!
 * State of the multi-client system compositor. Use to track the calling of native
 * compositor methods @ref xrt_comp_begin_session and @ref xrt_comp_end_session.
 *
 * It is driven by the number of active app sessions.
 *
 * @ingroup comp_multi
 */
enum multi_system_state
{
	/*!
	 * Invalid state, never used.
	 */
	MULTI_SYSTEM_STATE_INVALID,

	/*!
	 * One of the initial states, the multi-client system compositor will
	 * make sure that its @ref xrt_compositor_native submits one frame.
	 *
	 * The session hasn't been started yet.
	 */
	MULTI_SYSTEM_STATE_INIT_WARM_START,

	/*!
	 * One of the initial state and post stopping state.
	 *
	 * The multi-client system compositor has called @ref xrt_comp_end_session
	 * on its @ref xrt_compositor_native.
	 */
	MULTI_SYSTEM_STATE_STOPPED,

	/*!
	 * The main session is running.
	 *
	 * The multi-client system compositor has called @ref xrt_comp_begin_session
	 * on its @ref xrt_compositor_native.
	 */
	MULTI_SYSTEM_STATE_RUNNING,

	/*!
	 * There are no active sessions and the multi-client system compositor is
	 * instructing the native compositor to draw one or more clear frames.
	 *
	 * The multi-client system compositor has not yet called @ref xrt_comp_begin_session
	 * on its @ref xrt_compositor_native.
	 */
	MULTI_SYSTEM_STATE_STOPPING,
};

/*!
 * The multi-client module (aka multi compositor) is  system compositor that
 * multiplexes access to a single @ref xrt_compositor_native, merging layers
 * from one or more client apps/sessions. This object implements the
 * @ref xrt_system_compositor, and gives each session a @ref multi_compositor,
 * which implements @ref xrt_compositor_native.
 *
 * @ingroup comp_multi
 * @implements xrt_system_compositor
 */
struct multi_system_compositor
{
	//! Base interface.
	struct xrt_system_compositor base;

	//! Extra functions to handle multi client.
	struct xrt_multi_compositor_control xmcc;

	/*!
	 * Real native compositor, which this multi client module submits the
	 * combined layers of active @ref multi_compositor objects.
	 */
	struct xrt_compositor_native *xcn;

	/*!
	 * App pacer factory, when a new @ref multi_compositor is created a
	 * pacer is created from this factory.
	 */
	struct u_pacing_app_factory *upaf;

	//! Render loop thread.
	struct os_thread_helper oth;

	struct
	{
		/*!
		 * The state of the multi-client system compositor.
		 * This is updated on the multi_system_compositor::oth
		 * thread, aka multi-client system compositor main thread.
		 * It is driven by the active_count field.
		 */
		enum multi_system_state state;

		//! Number of active sessions, protected by oth.
		uint64_t active_count;
	} sessions;

	/*!
	 * This mutex protects the list of client compositor
	 * and the rendering timings on it.
	 */
	struct os_mutex list_and_timing_lock;

	struct
	{
		int64_t predicted_display_time_ns;
		int64_t predicted_display_period_ns;
		int64_t diff_ns;
	} last_timings;

	//! List of active clients.
	struct multi_compositor *clients[MULTI_MAX_CLIENTS];

	//! External window handle from first session with HWND (for windowed mode)
	void *external_window_handle;

	//! Service for creating per-session render targets (provided by comp_main)
	struct comp_target_service *target_service;
};

/*!
 * Cast helper
 *
 * @ingroup comp_multi
 * @private @memberof multi_system_compositor
 */
static inline struct multi_system_compositor *
multi_system_compositor(struct xrt_system_compositor *xsc)
{
	return (struct multi_system_compositor *)xsc;
}

/*!
 * The client compositor calls this function to update when its session is
 * started or stopped.
 *
 * @ingroup comp_multi
 * @private @memberof multi_system_compositor
 */
void
multi_system_compositor_update_session_status(struct multi_system_compositor *msc, bool active);

/*!
 * Initialize per-session render resources for a multi_compositor with external window.
 * Called lazily on first frame if session has external_window_handle set.
 *
 * @param mc The multi_compositor to initialize resources for
 * @return true on success, false on failure
 *
 * @ingroup comp_multi
 * @private @memberof multi_compositor
 */
bool
multi_compositor_init_session_render(struct multi_compositor *mc);

/*!
 * Check if a multi_compositor has per-session rendering enabled.
 *
 * @param mc The multi_compositor to check
 * @return true if session has its own render target
 *
 * @ingroup comp_multi
 * @private @memberof multi_compositor
 */
static inline bool
multi_compositor_has_session_render(struct multi_compositor *mc)
{
	return mc->session_render.external_window_handle != NULL || mc->session_render.readback_callback != NULL ||
	       mc->session_render.shared_texture_handle != NULL;
}

/*!
 * Get predicted eye positions from the session's display processor.
 * Uses the display processor's eye tracking (e.g. SR SDK LookaroundFilter).
 *
 * @param mc The multi_compositor (must have per-session rendering initialized)
 * @param[out] out_eye_pair Pointer to receive the eye positions (in meters)
 * @return true if valid eye positions are available, false otherwise
 *
 * @ingroup comp_multi
 * @private @memberof multi_compositor
 */
bool
multi_compositor_get_predicted_eye_positions(struct multi_compositor *mc, struct xrt_eye_pair *out_eye_pair);

/*!
 * Get window metrics for adaptive FOV and eye position adjustment.
 *
 * Vendor-neutral: prefers SR SDK path when available (precise display
 * screen position from SR::Display), falls back to generic Win32 path
 * using MonitorFromWindow + xrt_system_compositor_info fields.
 *
 * @param mc The multi_compositor (must have per-session rendering initialized)
 * @param[out] out_metrics Pointer to receive the window metrics.
 * @return true if valid metrics are available, false otherwise.
 *
 * @ingroup comp_multi
 * @private @memberof multi_compositor
 */
bool
multi_compositor_get_window_metrics(struct multi_compositor *mc, struct xrt_window_metrics *out_metrics);

/*!
 * Request display mode switch (2D/3D).
 *
 * Uses display processor vtable when available, falls back to
 * generic device property output mode switching.
 *
 * @param mc The multi_compositor (must have per-session rendering initialized)
 * @param enable_3d true to switch to 3D mode, false for 2D mode.
 * @return true on success.
 *
 * @ingroup comp_multi
 * @private @memberof multi_compositor
 */
bool
multi_compositor_request_display_mode(struct multi_compositor *mc, bool enable_3d);


#ifdef __cplusplus
}
#endif
