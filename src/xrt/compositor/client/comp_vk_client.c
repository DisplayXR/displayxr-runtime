// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan client side glue to compositor implementation.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup comp_client
 */

#include "util/u_misc.h"
#include "util/u_handles.h"
#include "util/u_trace_marker.h"
#include "util/u_debug.h"

#include "comp_vk_client.h"

#include "util/u_logging.h"
#include "os/os_time.h" // os_monotonic_get_ns — VK [COMMIT_PROFILE_CLIENT]

#include <stdlib.h> // getenv

// Phase 2 — forward decls of the IPC client compositor bridges. Mirrors the
// pair the D3D11 client uses (see comp_d3d11_client.cpp). Resolved at link
// time so this TU does not pull the IPC client headers.
extern xrt_result_t
comp_ipc_client_compositor_get_workspace_sync_fence(struct xrt_compositor *xc,
                                                    bool *out_have_fence,
                                                    xrt_graphics_sync_handle_t *out_handle);
extern void
comp_ipc_client_compositor_set_workspace_sync_fence_value(struct xrt_compositor *xc, uint64_t value);

// Prefixed with OXR since the only user right now is the OpenXR state tracker.
DEBUG_GET_ONCE_LOG_OPTION(vulkan_log, "OXR_VULKAN_LOG", U_LOGGING_INFO)

/*!
 * Down-cast helper.
 *
 * @private @memberof client_vk_swapchain
 */
static inline struct client_vk_swapchain *
client_vk_swapchain(struct xrt_swapchain *xsc)
{
	return (struct client_vk_swapchain *)xsc;
}

/*!
 * Helper to get the native swapchain.
 */
static inline struct xrt_swapchain *
to_native_swapchain(struct xrt_swapchain *xsc)
{
	return &client_vk_swapchain(xsc)->xscn->base;
}

/*!
 * Down-cast helper.
 *
 * @private @memberof client_vk_compositor
 */
static inline struct client_vk_compositor *
client_vk_compositor(struct xrt_compositor *xc)
{
	return (struct client_vk_compositor *)xc;
}

/*!
 * Helper to get the native swapchain.
 */
static inline struct xrt_compositor *
to_native_compositor(struct xrt_compositor *xc)
{
	return &client_vk_compositor(xc)->xcn->base;
}


/*
 *
 * Transition helpers.
 *
 */

static xrt_result_t
submit_image_barrier(struct client_vk_swapchain *sc, VkCommandBuffer cmd_buffer)
{
	COMP_TRACE_MARKER();

	struct client_vk_compositor *c = sc->c;
	struct vk_bundle *vk = &c->vk;
	VkResult ret;

	// Note we do not submit a fence here, it's not needed.
	ret = vk_cmd_pool_submit_cmd_buffer(vk, &c->pool, cmd_buffer);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_cmd_pool_submit_cmd_buffer: %s %u", vk_result_string(ret), ret);
		return XRT_ERROR_FAILED_TO_SUBMIT_VULKAN_COMMANDS;
	}

	return XRT_SUCCESS;
}


/*
 *
 * Semaphore helpers.
 *
 */

#ifdef VK_KHR_timeline_semaphore
static xrt_result_t
setup_semaphore(struct client_vk_compositor *c)
{
	xrt_graphics_sync_handle_t handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	struct xrt_compositor_semaphore *xcsem = NULL;
	struct vk_bundle *vk = &c->vk;
	xrt_result_t xret;
	VkResult ret;

	xret = xrt_comp_create_semaphore(&c->xcn->base, &handle, &xcsem);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create semaphore!");
		return xret;
	}

	VkSemaphore semaphore = VK_NULL_HANDLE;
	ret = vk_create_timeline_semaphore_from_native(vk, handle, &semaphore);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateSemaphore: %s", vk_result_string(ret));
		u_graphics_sync_unref(&handle);
		xrt_compositor_semaphore_reference(&xcsem, NULL);
		return XRT_ERROR_VULKAN;
	}

	VK_NAME_SEMAPHORE(vk, semaphore, "timeline semaphore");

	c->sync.semaphore = semaphore;
	c->sync.xcsem = xcsem; // No need to reference.

	return XRT_SUCCESS;
}
#endif


/*
 *
 * Frame submit helpers.
 *
 */

static bool
submit_handle(struct client_vk_compositor *c, xrt_graphics_sync_handle_t sync_handle, xrt_result_t *out_xret)
{
	// Did we get a ready made handle, assume it's in the command stream and call commit directly.
	if (!xrt_graphics_sync_handle_is_valid(sync_handle)) {
		return false;
	}

	// Commit consumes the sync_handle.
	*out_xret = xrt_comp_layer_commit(&c->xcn->base, sync_handle);
	return true;
}

static bool
submit_semaphore(struct client_vk_compositor *c, xrt_result_t *out_xret)
{
#ifdef VK_KHR_timeline_semaphore
	// The VK client's vk_bundle uses function pointers from the app's VK device.
	// In IPC/workspace mode, the IPC compositor creates a timeline semaphore, but
	// the app's VK dispatch table may not have the required timeline semaphore
	// VK functions loaded (vkQueueSubmit with VkTimelineSemaphoreSubmitInfo
	// reads from the dispatch table which may have null entries).
	// Fall through to submit_fence or submit_fallback instead.
	if (c->sync.xcsem == NULL) {
		return false;
	}

	// Check if the VK device actually supports timeline semaphores
	struct vk_bundle *vk = &c->vk;
	if (!vk->features.timeline_semaphore || vk->vkQueueSubmit == NULL) {
		return false;
	}
	VkResult ret;

	VkSemaphore semaphores[1] = {
	    c->sync.semaphore,
	};
	uint64_t values[1] = {
	    ++(c->sync.value),
	};

	VkTimelineSemaphoreSubmitInfo semaphore_submit_info = {
	    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
	    .waitSemaphoreValueCount = 0,
	    .pWaitSemaphoreValues = NULL,
	    .signalSemaphoreValueCount = ARRAY_SIZE(values),
	    .pSignalSemaphoreValues = values,
	};
	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .pNext = &semaphore_submit_info,
	    .signalSemaphoreCount = ARRAY_SIZE(semaphores),
	    .pSignalSemaphores = semaphores,
	};

	ret = vk->vkQueueSubmit(   //
	    vk->main_queue->queue, // queue
	    1,                     // submitCount
	    &submit_info,          // pSubmits
	    VK_NULL_HANDLE);       // fence
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkQueueSubmit: %s", vk_result_string(ret));
		*out_xret = XRT_ERROR_VULKAN;
		return true;
	}

	*out_xret = xrt_comp_layer_commit_with_semaphore( //
	    &c->xcn->base,                                // xc
	    c->sync.xcsem,                                // xcsem
	    values[0]);                                   // value

	return true;
#else
	return false;
#endif
}

static bool
submit_fence(struct client_vk_compositor *c, xrt_result_t *out_xret)
{
	xrt_graphics_sync_handle_t sync_handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	struct vk_bundle *vk = &c->vk;
	VkResult ret;

#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
	// Using sync fds currently to match OpenGL extension.
	bool sync_fence = vk->external.fence_sync_fd;
#elif defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE)
	bool sync_fence = vk->external.fence_win32_handle;
#else
#error "Need port to export fence sync handles"
#endif

	if (!sync_fence) {
		return false;
	}

	{
		COMP_TRACE_IDENT(create_and_submit_fence);

		ret = vk_create_and_submit_fence_native(vk, &sync_handle);
		if (ret != VK_SUCCESS) {
			// This is really bad, log again.
			U_LOG_E("Could not create and submit a native fence!");
			*out_xret = XRT_ERROR_VULKAN;
			return true; // If we fail we should still return.
		}
	}

	*out_xret = xrt_comp_layer_commit(&c->xcn->base, sync_handle);
	return true;
}

static bool
submit_fallback(struct client_vk_compositor *c, xrt_result_t *out_xret)
{
	struct vk_bundle *vk = &c->vk;

	// Per-stage timestamps for [COMMIT_PROFILE_CLIENT]. Same shape as the
	// d3d11 client emit so the bench script parser doesn't need a new branch.
	int64_t profile_t0 = os_monotonic_get_ns();

	// Wait for the app's GPU work to complete before notifying the compositor.
	// With cross-device external memory sharing (null compositor on Device A,
	// VK app on Device B), the compositor will read the shared images on Device A.
	// Without this wait, Device B's GPU writes may still be in-flight when
	// Device A reads, causing VK_ERROR_DEVICE_LOST on Intel Iris Xe (Gen12).
	// GL apps don't need this because GL has implicit driver-level sync.
	{
		COMP_TRACE_IDENT(device_wait_idle);

		vk_queue_lock(vk->main_queue);
		vk->vkQueueWaitIdle(vk->main_queue->queue);
		vk_queue_unlock(vk->main_queue);
	}

	int64_t profile_t1 = os_monotonic_get_ns();

	// Phase 2 — once the queue is idle, host-signal the workspace_sync
	// semaphore and ship the new value to the service before the IPC commit.
	// The service's per-view loop reads `last_signaled_fence_value` and
	// queues an `ID3D11DeviceContext4::Wait` on it; without this advance the
	// service treats every view as stale and skips the blit (manifests as
	// black gauss-splat windows in shell mode). Mirrors the D3D11 client's
	// signal in `client_d3d11_compositor_layer_commit`. No-op when
	// `workspace_sync_semaphore` is null (legacy KeyedMutex path stays in
	// effect — same fallback as the D3D11 client).
#ifdef VK_KHR_timeline_semaphore
	if (c->workspace_sync_semaphore != VK_NULL_HANDLE && vk->vkSignalSemaphore != NULL) {
		c->workspace_sync_fence_value++;
		VkSemaphoreSignalInfoKHR sig_info = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO_KHR,
		    .semaphore = c->workspace_sync_semaphore,
		    .value = c->workspace_sync_fence_value,
		};
		VkResult sig_ret = vk->vkSignalSemaphore(vk->device, &sig_info);
		if (sig_ret != VK_SUCCESS) {
			VK_WARN(vk,
			        "Phase 2: vkSignalSemaphore(workspace_sync_semaphore) failed: %s; "
			        "service will treat this frame as stale.",
			        vk_result_string(sig_ret));
			c->workspace_sync_fence_value--;
		} else {
			comp_ipc_client_compositor_set_workspace_sync_fence_value(
			    &c->xcn->base, c->workspace_sync_fence_value);
		}
	}
#endif

	int64_t profile_t2 = os_monotonic_get_ns();

	*out_xret = xrt_comp_layer_commit(&c->xcn->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID);

	int64_t profile_t3 = os_monotonic_get_ns();

	// Emit per-stage [COMMIT_PROFILE_CLIENT]. Env-gated by DISPLAYXR_LOG_PRESENT_NS=1
	// (same gate as the existing service-side and d3d11-client emits, so
	// the bench parser picks it up directly).
	// Field mapping for the VK fallback path:
	//   signal_ns    = vkQueueWaitIdle (the dominant cost; analogous to the
	//                  d3d11 client's fence Signal — both ensure GPU work
	//                  is visible before notifying the service)
	//   pre_ipc_ns   = workspace_sync semaphore signal + set-fence-value RPC
	//   ipc_ns       = the layer_commit RPC roundtrip
	//   post_ipc_ns  = 0 (nothing happens after layer_commit returns)
	{
		static int log_commit_profile = -1;
		if (log_commit_profile < 0) {
			const char *e = getenv("DISPLAYXR_LOG_PRESENT_NS");
			log_commit_profile = (e != NULL && e[0] == '1') ? 1 : 0;
		}
		if (log_commit_profile) {
			U_LOG_W("[COMMIT_PROFILE_CLIENT] client=%p signal_ns=%lld pre_ipc_ns=%lld ipc_ns=%lld post_ipc_ns=%lld total_ns=%lld",
			        (void *)c,
			        (long long)(profile_t1 - profile_t0),
			        (long long)(profile_t2 - profile_t1),
			        (long long)(profile_t3 - profile_t2),
			        (long long)0,
			        (long long)(profile_t3 - profile_t0));
		}
	}

	return true;
}


/*
 *
 * Swapchain function.
 *
 */

static void
client_vk_swapchain_destroy(struct xrt_swapchain *xsc)
{
	COMP_TRACE_MARKER();

	struct client_vk_swapchain *sc = client_vk_swapchain(xsc);
	struct client_vk_compositor *c = sc->c;
	struct vk_bundle *vk = &c->vk;

	for (uint32_t i = 0; i < sc->base.base.image_count; i++) {
		if (sc->base.images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, sc->base.images[i], NULL);
			sc->base.images[i] = VK_NULL_HANDLE;
		}

		if (sc->mems[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, sc->mems[i], NULL);
			sc->mems[i] = VK_NULL_HANDLE;
		}
	}

	// Drop our reference, does NULL checking.
	xrt_swapchain_native_reference(&sc->xscn, NULL);

	free(sc);
}

static xrt_result_t
client_vk_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	COMP_TRACE_MARKER();

	struct client_vk_swapchain *sc = client_vk_swapchain(xsc);
	xrt_result_t xret;
	uint32_t index = 0;

	// Pipe down call into native swapchain.
	xret = xrt_swapchain_acquire_image(&sc->xscn->base, &index);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	// Finally done.
	*out_index = index;

	return XRT_SUCCESS;
}

static xrt_result_t
client_vk_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	COMP_TRACE_MARKER();

	// Pipe down call into native swapchain.
	return xrt_swapchain_wait_image(to_native_swapchain(xsc), timeout_ns, index);
}

static xrt_result_t
client_vk_swapchain_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	COMP_TRACE_MARKER();

	struct client_vk_swapchain *sc = client_vk_swapchain(xsc);

	// When disable_fence_sync is true (null compositor with separate VkDevice),
	// skip ALL barriers (both acquire and release). Submitting any command buffer
	// that references cross-device imported images causes VK_ERROR_DEVICE_LOST
	// on NVIDIA and Intel drivers — even acquire barriers with VK_QUEUE_FAMILY_IGNORED
	// and oldLayout=UNDEFINED trigger deferred GPU faults detected by vkQueueWaitIdle.
	//
	// This is safe because the null compositor does not read the swapchain images
	// directly — it acts as a sync/timing bridge only. No GPU-side synchronization
	// is needed between app and compositor. The app handles its own layout
	// transitions via render pass initialLayout/finalLayout.
	if (sc->c->xcn->base.info.disable_fence_sync) {
		return XRT_SUCCESS;
	}

	VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;

	switch (direction) {
	case XRT_BARRIER_TO_APP: cmd_buffer = sc->acquire[index]; break;
	case XRT_BARRIER_TO_COMP: cmd_buffer = sc->release[index]; break;
	default: assert(false);
	}

	return submit_image_barrier(sc, cmd_buffer);
}

static xrt_result_t
client_vk_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	COMP_TRACE_MARKER();

	// Pipe down call into native swapchain.
	return xrt_swapchain_release_image(to_native_swapchain(xsc), index);
}

static xrt_result_t
client_vk_compositor_passthrough_create(struct xrt_compositor *xc, const struct xrt_passthrough_create_info *info)
{
	struct client_vk_compositor *c = client_vk_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_create_passthrough(&c->xcn->base, info);
}

static xrt_result_t
client_vk_compositor_passthrough_layer_create(struct xrt_compositor *xc,
                                              const struct xrt_passthrough_layer_create_info *info)
{
	struct client_vk_compositor *c = client_vk_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_create_passthrough_layer(&c->xcn->base, info);
}

static xrt_result_t
client_vk_compositor_passthrough_destroy(struct xrt_compositor *xc)
{
	struct client_vk_compositor *c = client_vk_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_destroy_passthrough(&c->xcn->base);
}

/*
 *
 * Compositor functions.
 *
 */

static void
client_vk_compositor_destroy(struct xrt_compositor *xc)
{
	COMP_TRACE_MARKER();

	struct client_vk_compositor *c = client_vk_compositor(xc);
	struct vk_bundle *vk = &c->vk;

	if (c->sync.semaphore != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, c->sync.semaphore, NULL);
		c->sync.semaphore = VK_NULL_HANDLE;
	}
	xrt_compositor_semaphore_reference(&c->sync.xcsem, NULL);

	if (c->workspace_sync_semaphore != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, c->workspace_sync_semaphore, NULL);
		c->workspace_sync_semaphore = VK_NULL_HANDLE;
	}

	// Now safe to free the pool.
	vk_cmd_pool_destroy(vk, &c->pool);

	vk_deinit_mutex(vk);

	free(c);
}

static xrt_result_t
client_vk_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	COMP_TRACE_MARKER();

	// Pipe down call into native compositor.
	return xrt_comp_begin_session(to_native_compositor(xc), info);
}

static xrt_result_t
client_vk_compositor_end_session(struct xrt_compositor *xc)
{
	COMP_TRACE_MARKER();

	// Pipe down call into native compositor.
	return xrt_comp_end_session(to_native_compositor(xc));
}

static xrt_result_t
client_vk_compositor_wait_frame(struct xrt_compositor *xc,
                                int64_t *out_frame_id,
                                int64_t *predicted_display_time,
                                int64_t *predicted_display_period)
{
	COMP_TRACE_MARKER();

	// Pipe down call into native compositor.
	return xrt_comp_wait_frame(    //
	    to_native_compositor(xc),  //
	    out_frame_id,              //
	    predicted_display_time,    //
	    predicted_display_period); //
}

static xrt_result_t
client_vk_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	// Pipe down call into native compositor.
	return xrt_comp_begin_frame(to_native_compositor(xc), frame_id);
}

static xrt_result_t
client_vk_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	// Pipe down call into native compositor.
	return xrt_comp_discard_frame(to_native_compositor(xc), frame_id);
}

static xrt_result_t
client_vk_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	COMP_TRACE_MARKER();

	struct xrt_compositor *xcn = to_native_compositor(xc);

	xrt_result_t xret = xrt_comp_layer_begin(xcn, data);
	return xret;
}

static xrt_result_t
client_vk_compositor_layer_projection(struct xrt_compositor *xc,
                                      struct xrt_device *xdev,
                                      struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                      const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscn[XRT_MAX_VIEWS];

	assert(data->type == XRT_LAYER_PROJECTION);
	for (uint32_t i = 0; i < data->view_count; ++i) {
		xscn[i] = &client_vk_swapchain(xsc[i])->xscn->base;
	}

	xcn = to_native_compositor(xc);

	return xrt_comp_layer_projection(xcn, xdev, xscn, data);
}


static xrt_result_t
client_vk_compositor_layer_stereo_projection_depth(struct xrt_compositor *xc,
                                                   struct xrt_device *xdev,
                                                   struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                                   struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                                   const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;

	struct xrt_swapchain *xscn[XRT_MAX_VIEWS];
	struct xrt_swapchain *d_xscn[XRT_MAX_VIEWS];

	assert(data->type == XRT_LAYER_PROJECTION_DEPTH);

	xcn = to_native_compositor(xc);
	for (uint32_t i = 0; i < data->view_count; ++i) {
		xscn[i] = to_native_swapchain(xsc[i]);
		d_xscn[i] = to_native_swapchain(d_xsc[i]);
	}

	return xrt_comp_layer_projection_depth(xcn, xdev, xscn, d_xscn, data);
}

static xrt_result_t
client_vk_compositor_layer_quad(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc,
                                const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_QUAD);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_quad(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_cube(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc,
                                const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_CUBE);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_cube(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_cylinder(struct xrt_compositor *xc,
                                    struct xrt_device *xdev,
                                    struct xrt_swapchain *xsc,
                                    const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_CYLINDER);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_cylinder(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_equirect1(struct xrt_compositor *xc,
                                     struct xrt_device *xdev,
                                     struct xrt_swapchain *xsc,
                                     const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_EQUIRECT1);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_equirect1(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_equirect2(struct xrt_compositor *xc,
                                     struct xrt_device *xdev,
                                     struct xrt_swapchain *xsc,
                                     const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_EQUIRECT2);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_equirect2(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_window_space(struct xrt_compositor *xc,
                                        struct xrt_device *xdev,
                                        struct xrt_swapchain *xsc,
                                        const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_WINDOW_SPACE);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_window_space(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_passthrough(struct xrt_compositor *xc,
                                       struct xrt_device *xdev,
                                       const struct xrt_layer_data *data)
{
	struct client_vk_compositor *c = client_vk_compositor(xc);

	assert(data->type == XRT_LAYER_PASSTHROUGH);

	return xrt_comp_layer_passthrough(&c->xcn->base, xdev, data);
}

static xrt_result_t
client_vk_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	COMP_TRACE_MARKER();

	struct client_vk_compositor *c = client_vk_compositor(xc);

	if (c->renderdoc_enabled) {
		struct vk_bundle *vk = &c->vk;
		VkResult ret = vk_cmd_pool_submit_cmd_buffer(vk, &c->pool, c->dcb);
		if (ret != VK_SUCCESS) {
			VK_ERROR(vk, "vk_cmd_pool_submit_cmd_buffer: %s %u", vk_result_string(ret), ret);
			return XRT_ERROR_FAILED_TO_SUBMIT_VULKAN_COMMANDS;
		}
	}

	xrt_result_t xret = XRT_SUCCESS;
	if (submit_handle(c, sync_handle, &xret)) {
		return xret;
	} else if (submit_fallback(c, &xret)) {
		// In IPC/workspace mode, skip semaphore and fence paths — the app's VK
		// device dispatch table may have null entries for extension functions
		// that the IPC compositor's VK bundle expects. The fallback path uses
		// only core VK functions (vkQueueWaitIdle) and is safe.
		return xret;
	} else if (submit_semaphore(c, &xret)) {
		return xret;
	} else if (submit_fence(c, &xret)) {
		return xret;
	} else {
		// Really bad state.
		return XRT_ERROR_VULKAN;
	}
}

static xrt_result_t
client_vk_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                                     const struct xrt_swapchain_create_info *info,
                                                     struct xrt_swapchain_create_properties *xsccp)
{
	return xrt_comp_get_swapchain_create_properties(to_native_compositor(xc), info, xsccp);
}

static xrt_result_t
client_vk_swapchain_create(struct xrt_compositor *xc,
                           const struct xrt_swapchain_create_info *info,
                           struct xrt_swapchain **out_xsc)
{
	COMP_TRACE_MARKER();

	struct client_vk_compositor *c = client_vk_compositor(xc);
	struct vk_bundle *vk = &c->vk;
	VkResult ret;
	xrt_result_t xret;

	struct xrt_swapchain_create_properties xsccp = XRT_STRUCT_INIT;
	xret = xrt_comp_get_swapchain_create_properties(&c->xcn->base, info, &xsccp);
	if (xret != XRT_SUCCESS) {
		VK_ERROR(vk, "Failed to get create properties: %u", xret);
		return xret;
	}

	// Update the create info.
	struct xrt_swapchain_create_info xinfo = *info;
	xinfo.bits |= xsccp.extra_bits;

	struct xrt_swapchain_native *xscn = NULL; // Has to be NULL.
	xret = xrt_comp_native_create_swapchain(c->xcn, &xinfo, &xscn);

	if (xret != XRT_SUCCESS) {
		return xret;
	}
	assert(xscn != NULL);

	struct xrt_swapchain *xsc = &xscn->base;

	VkAccessFlags barrier_access_mask = vk_csci_get_barrier_access_mask(xinfo.bits);
	VkImageLayout barrier_optimal_layout = vk_csci_get_barrier_optimal_layout(xinfo.format);
	VkImageAspectFlags barrier_aspect_mask = vk_csci_get_barrier_aspect_mask(xinfo.format);

	struct client_vk_swapchain *sc = U_TYPED_CALLOC(struct client_vk_swapchain);
	sc->base.base.destroy = client_vk_swapchain_destroy;
	sc->base.base.acquire_image = client_vk_swapchain_acquire_image;
	sc->base.base.wait_image = client_vk_swapchain_wait_image;
	sc->base.base.barrier_image = client_vk_swapchain_barrier_image;
	sc->base.base.release_image = client_vk_swapchain_release_image;
	sc->base.base.reference.count = 1;
	sc->base.base.image_count = xsc->image_count; // Fetch the number of images from the native swapchain.
	sc->c = c;
	sc->xscn = xscn;

	for (uint32_t i = 0; i < xsc->image_count; i++) {
		ret = vk_create_image_from_native(vk, &xinfo, &xscn->images[i], &sc->base.images[i], &sc->mems[i]);

		if (ret != VK_SUCCESS) {
			return XRT_ERROR_VULKAN;
		}

		VK_NAME_IMAGE(vk, sc->base.images[i], "vk_image_collection image");
		VK_NAME_DEVICE_MEMORY(vk, sc->mems[i], "vk_image_collection device_memory");
	}

	vk_cmd_pool_lock(&c->pool);
	const VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

	// Prerecord command buffers for swapchain image ownership/layout transitions
	for (uint32_t i = 0; i < xsc->image_count; i++) {
		ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, &c->pool, flags, &sc->acquire[i]);
		if (ret != VK_SUCCESS) {
			vk_cmd_pool_unlock(&c->pool);
			return XRT_ERROR_VULKAN;
		}
		VK_NAME_COMMAND_BUFFER(vk, sc->acquire[i], "client_vk_swapchain acquire command buffer");
		ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, &c->pool, flags, &sc->release[i]);
		if (ret != VK_SUCCESS) {
			vk_cmd_pool_unlock(&c->pool);
			return XRT_ERROR_VULKAN;
		}
		VK_NAME_COMMAND_BUFFER(vk, sc->release[i], "client_vk_swapchain release command buffer");

		VkImageSubresourceRange subresource_range = {
		    .aspectMask = barrier_aspect_mask,
		    .baseMipLevel = 0,
		    .levelCount = VK_REMAINING_MIP_LEVELS,
		    .baseArrayLayer = 0,
		    .layerCount = VK_REMAINING_ARRAY_LAYERS,
		};

		VkImageMemoryBarrier acquire = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = barrier_access_mask,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = barrier_optimal_layout,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image = sc->base.images[i],
		    .subresourceRange = subresource_range,
		};

		// When disable_fence_sync is true (null compositor with separate VkDevice),
		// skip VK_QUEUE_FAMILY_EXTERNAL transfer — it causes VK_ERROR_DEVICE_LOST
		// on some drivers (NVIDIA, Intel). CPU sync via vkQueueWaitIdle is used instead.
		uint32_t release_src_queue = c->xcn->base.info.disable_fence_sync
		                                 ? VK_QUEUE_FAMILY_IGNORED
		                                 : vk->main_queue->family_index;
		uint32_t release_dst_queue = c->xcn->base.info.disable_fence_sync
		                                 ? VK_QUEUE_FAMILY_IGNORED
		                                 : VK_QUEUE_FAMILY_EXTERNAL;

		VkImageMemoryBarrier release = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = barrier_access_mask,
		    .dstAccessMask = 0,
		    .oldLayout = barrier_optimal_layout,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .srcQueueFamilyIndex = release_src_queue,
		    .dstQueueFamilyIndex = release_dst_queue,
		    .image = sc->base.images[i],
		    .subresourceRange = subresource_range,
		};

		//! @todo less conservative pipeline stage masks based on usage
		vk->vkCmdPipelineBarrier(               //
		    sc->acquire[i],                     // commandBuffer
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,  // srcStageMask
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // dstStageMask
		    0,                                  // dependencyFlags
		    0,                                  // memoryBarrierCount
		    NULL,                               // pMemoryBarriers
		    0,                                  // bufferMemoryBarrierCount
		    NULL,                               // pBufferMemoryBarriers
		    1,                                  // imageMemoryBarrierCount
		    &acquire);                          // pImageMemoryBarriers

		vk->vkCmdPipelineBarrier(                 //
		    sc->release[i],                       // commandBuffer
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,   // srcStageMask
		    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dstStageMask
		    0,                                    // dependencyFlags
		    0,                                    // memoryBarrierCount
		    NULL,                                 // pMemoryBarriers
		    0,                                    // bufferMemoryBarrierCount
		    NULL,                                 // pBufferMemoryBarriers
		    1,                                    // imageMemoryBarrierCount
		    &release);                            // pImageMemoryBarriers

		ret = vk->vkEndCommandBuffer(sc->acquire[i]);
		if (ret != VK_SUCCESS) {
			VK_ERROR(vk, "vkEndCommandBuffer: %s", vk_result_string(ret));
			vk_cmd_pool_unlock(&c->pool);
			return XRT_ERROR_VULKAN;
		}
		ret = vk->vkEndCommandBuffer(sc->release[i]);
		if (ret != VK_SUCCESS) {
			VK_ERROR(vk, "vkEndCommandBuffer: %s", vk_result_string(ret));
			vk_cmd_pool_unlock(&c->pool);
			return XRT_ERROR_VULKAN;
		}
	}
	vk_cmd_pool_unlock(&c->pool);


	*out_xsc = &sc->base.base;

	return XRT_SUCCESS;
}

struct client_vk_compositor *
client_vk_compositor_create(struct xrt_compositor_native *xcn,
                            VkInstance instance,
                            PFN_vkGetInstanceProcAddr getProc,
                            VkPhysicalDevice physicalDevice,
                            VkDevice device,
                            bool external_fence_fd_enabled,
                            bool external_semaphore_fd_enabled,
                            bool timeline_semaphore_enabled,
                            bool image_format_list_enabled,
                            bool debug_utils_enabled,
                            bool renderdoc_enabled,
                            uint32_t queueFamilyIndex,
                            uint32_t queueIndex)
{
	COMP_TRACE_MARKER();

	xrt_result_t xret;
	VkResult ret;
	struct client_vk_compositor *c = U_TYPED_CALLOC(struct client_vk_compositor);

	c->base.base.get_swapchain_create_properties = client_vk_compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = client_vk_swapchain_create;
	c->base.base.create_passthrough = client_vk_compositor_passthrough_create;
	c->base.base.create_passthrough_layer = client_vk_compositor_passthrough_layer_create;
	c->base.base.destroy_passthrough = client_vk_compositor_passthrough_destroy;
	c->base.base.begin_session = client_vk_compositor_begin_session;
	c->base.base.end_session = client_vk_compositor_end_session;
	c->base.base.wait_frame = client_vk_compositor_wait_frame;
	c->base.base.begin_frame = client_vk_compositor_begin_frame;
	c->base.base.discard_frame = client_vk_compositor_discard_frame;
	c->base.base.layer_begin = client_vk_compositor_layer_begin;
	c->base.base.layer_projection = client_vk_compositor_layer_projection;
	c->base.base.layer_projection_depth = client_vk_compositor_layer_stereo_projection_depth;
	c->base.base.layer_quad = client_vk_compositor_layer_quad;
	c->base.base.layer_cube = client_vk_compositor_layer_cube;
	c->base.base.layer_cylinder = client_vk_compositor_layer_cylinder;
	c->base.base.layer_equirect1 = client_vk_compositor_layer_equirect1;
	c->base.base.layer_equirect2 = client_vk_compositor_layer_equirect2;
	c->base.base.layer_window_space = client_vk_compositor_layer_window_space;
	c->base.base.layer_passthrough = client_vk_compositor_layer_passthrough;
	c->base.base.layer_commit = client_vk_compositor_layer_commit;
	c->base.base.destroy = client_vk_compositor_destroy;

	c->xcn = xcn;
	// passthrough our formats from the native compositor to the client
	for (uint32_t i = 0; i < xcn->base.info.format_count; i++) {
		c->base.base.info.formats[i] = xcn->base.info.formats[i];
	}

	c->base.base.info.format_count = xcn->base.info.format_count;
	c->renderdoc_enabled = renderdoc_enabled;

	// Default to info.
	enum u_logging_level log_level = debug_get_log_option_vulkan_log();

	ret = vk_init_from_given(          //
	    &c->vk,                        // vk_bundle
	    getProc,                       // vkGetInstanceProcAddr
	    instance,                      // instance
	    physicalDevice,                // physical_device
	    device,                        // device
	    queueFamilyIndex,              // queue_family_index
	    queueIndex,                    // queue_index
	    external_fence_fd_enabled,     // external_fence_fd_enabled
	    external_semaphore_fd_enabled, // external_semaphore_fd_enabled
	    timeline_semaphore_enabled,    // timeline_semaphore_enabled
	    image_format_list_enabled,     // image_format_list_enabled
	    debug_utils_enabled,           // debug_utils_enabled
	    log_level);                    // log_level
	if (ret != VK_SUCCESS) {
		goto err_free;
	}

	// If the native compositor says no external fence sync, clear the flag
	// so submit_fence is skipped and submit_fallback (vkQueueWaitIdle) is used.
	if (xcn->base.info.disable_fence_sync) {
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
		c->vk.external.fence_sync_fd = false;
		c->vk.external.fence_opaque_fd = false;
#elif defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE)
		c->vk.external.fence_win32_handle = false;
#endif
	}

	ret = vk_init_mutex(&c->vk);
	if (ret != VK_SUCCESS) {
		goto err_free;
	}

	ret = vk_cmd_pool_init(&c->vk, &c->pool, 0);
	if (ret != VK_SUCCESS) {
		goto err_mutex;
	}

	VK_NAME_COMMAND_POOL(&c->vk, c->pool.pool, "client_vk_compositor command pool");

#ifdef VK_KHR_timeline_semaphore
	if (!xcn->base.info.disable_fence_sync && vk_can_import_and_export_timeline_semaphore(&c->vk)) {
		xret = setup_semaphore(c);
		if (xret != XRT_SUCCESS) {
			goto err_pool;
		}
	}
#endif

	// Phase 2 — fetch the per-IPC-client workspace_sync_fence from the
	// service and import it as a VK timeline semaphore. Mirrors the D3D11
	// client's setup in `client_d3d11_compositor_create`. Failure leaves
	// `workspace_sync_semaphore` null and the legacy vkQueueWaitIdle-only
	// path stays in effect for this client.
#ifdef VK_KHR_timeline_semaphore
	c->workspace_sync_semaphore = VK_NULL_HANDLE;
	c->workspace_sync_fence_value = 0;
	if (vk_can_import_and_export_timeline_semaphore(&c->vk)) {
		bool have_fence = false;
		xrt_graphics_sync_handle_t fence_handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		xrt_result_t fxret =
		    comp_ipc_client_compositor_get_workspace_sync_fence(&xcn->base, &have_fence, &fence_handle);
		if (fxret == XRT_SUCCESS && have_fence && xrt_graphics_sync_handle_is_valid(fence_handle)) {
			VkSemaphore wsf_sem = VK_NULL_HANDLE;
			VkResult vret = vk_create_timeline_semaphore_from_native(&c->vk, fence_handle, &wsf_sem);
			if (vret == VK_SUCCESS && wsf_sem != VK_NULL_HANDLE) {
				c->workspace_sync_semaphore = wsf_sem;
				U_LOG_W("[FENCE] client=%p workspace_sync_semaphore imported "
				        "(VK GPU-side wait path active)",
				        (void *)c);
			} else {
				U_LOG_W("Phase 2 (VK): vk_create_timeline_semaphore_from_native failed (%s); "
				        "falling back to vkQueueWaitIdle-only path.",
				        vk_result_string(vret));
				// Importing took ownership of the handle on success;
				// on failure the helper does NOT consume it, so close.
				u_graphics_sync_unref(&fence_handle);
			}
		} else if (fxret != XRT_SUCCESS) {
			U_LOG_W("Phase 2 (VK): get_workspace_sync_fence RPC failed (xret=%d); "
			        "vkQueueWaitIdle path stays in effect.",
			        (int)fxret);
		} else {
			U_LOG_W("Phase 2 (VK): server reported no workspace_sync_fence "
			        "(have=%d valid_handle=%d); vkQueueWaitIdle path stays in effect.",
			        (int)have_fence, (int)xrt_graphics_sync_handle_is_valid(fence_handle));
		}
	} else {
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE)
		U_LOG_W("Phase 2 (VK): VkDevice does not support timeline semaphore import/export "
		        "(d3d12_fence=%d opaque_win32=%d); vkQueueWaitIdle path stays in effect.",
		        (int)c->vk.external.timeline_semaphore_d3d12_fence,
		        (int)c->vk.external.timeline_semaphore_win32_handle);
#else
		U_LOG_W("Phase 2 (VK): VkDevice does not support timeline semaphore import/export "
		        "(sync_fd=%d opaque_fd=%d); vkQueueWaitIdle path stays in effect.",
		        (int)c->vk.external.timeline_semaphore_sync_fd,
		        (int)c->vk.external.timeline_semaphore_opaque_fd);
#endif
	}
#endif

	// Get max texture size.
	{
		struct vk_bundle *vk = &c->vk;
		VkPhysicalDeviceProperties pdp;
		vk->vkGetPhysicalDeviceProperties(vk->physical_device, &pdp);
		c->base.base.info.max_texture_size = pdp.limits.maxImageDimension2D;
	}

	if (!c->renderdoc_enabled) {
		return c;
	}

	struct vk_bundle *vk = &c->vk;
	if (!vk->has_EXT_debug_utils) {
		c->renderdoc_enabled = false;
		return c;
	}

	// Create a no-op VkCommandBuffer and submit it to the VkQueue, just for inserting a debug label into
	// RenderDoc for triggering the capture.
	ret = vk_cmd_pool_create_begin_insert_label_and_end_cmd_buffer_locked(
	    vk, &c->pool, "vr-marker,frame_end,type,application", &c->dcb);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_cmd_pool_create_insert_debug_label_and_end_cmd_buffer: %s", vk_result_string(ret));
		goto err_pool;
	}

	return c;

err_pool:
	vk_cmd_pool_destroy(&c->vk, &c->pool);
err_mutex:
	vk_deinit_mutex(&c->vk);
err_free:
	free(c);

	return NULL;
}
