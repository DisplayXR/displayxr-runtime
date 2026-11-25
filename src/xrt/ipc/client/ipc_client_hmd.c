// Copyright 2020-2024, Collabora, Ltd.
// Copyright 2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  IPC Client HMD device.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup ipc_client
 */


#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "math/m_api.h"

#include "util/u_rendering_modes.h"
#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"

#include "client/ipc_client.h"
#include "client/ipc_client_xdev.h"
#include "client/ipc_client_connection.h"
#include "ipc_client_generated.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>


/*
 *
 * Structs and defines.
 *
 */

/*!
 * An IPC client proxy for an HMD @ref xrt_device and @ref ipc_client_xdev.
 * Using a typedef reduce impact of refactor change.
 *
 * @implements ipc_client_xdev
 * @ingroup ipc_client
 */
typedef struct ipc_client_xdev ipc_client_hmd_t;


/*
 *
 * Helpers.
 *
 */

static inline ipc_client_hmd_t *
ipc_client_hmd(struct xrt_device *xdev)
{
	return (ipc_client_hmd_t *)xdev;
}

static xrt_result_t
call_get_view_poses_raw(ipc_client_hmd_t *ich,
                        const struct xrt_vec3 *default_eye_relation,
                        int64_t at_timestamp_ns,
                        uint32_t view_count,
                        struct xrt_space_relation *out_head_relation,
                        struct xrt_fov *out_fovs,
                        struct xrt_pose *out_poses)
{
	struct ipc_connection *ipc_c = ich->ipc_c;
	xrt_result_t xret;

	ipc_client_connection_lock(ipc_c);

	// Using the raw send helper is the only one that is required.
	xret = ipc_send_device_get_view_poses_locked( //
	    ipc_c,                                    //
	    ich->device_id,                           //
	    default_eye_relation,                     //
	    at_timestamp_ns,                          //
	    view_count);                              //
	IPC_CHK_WITH_GOTO(ich->ipc_c, xret, "ipc_send_device_get_view_poses_locked", out);

	// This is the data we get back in the provided reply.
	uint32_t returned_view_count = 0;
	struct xrt_space_relation head_relation = XRT_SPACE_RELATION_ZERO;

	// Get the reply, use the raw function helper.
	xret = ipc_receive_device_get_view_poses_locked( //
	    ipc_c,                                       //
	    &head_relation,                              //
	    &returned_view_count);                       //
	IPC_CHK_WITH_GOTO(ich->ipc_c, xret, "ipc_receive_device_get_view_poses_locked", out);

	if (view_count != returned_view_count) {
		IPC_ERROR(ich->ipc_c, "Wrong view counts (sent: %u != got: %u)", view_count, returned_view_count);
		assert(false);
	}

	// We can read directly to the output variables.
	xret = ipc_receive(&ipc_c->imc, out_fovs, sizeof(struct xrt_fov) * view_count);
	IPC_CHK_WITH_GOTO(ich->ipc_c, xret, "ipc_receive(1)", out);

	// We can read directly to the output variables.
	xret = ipc_receive(&ipc_c->imc, out_poses, sizeof(struct xrt_pose) * view_count);
	IPC_CHK_WITH_GOTO(ich->ipc_c, xret, "ipc_receive(2)", out);

	/*
	 * Finally set the head_relation that we got in the reply, mostly to
	 * demonstrate that you can use the reply struct in such a way.
	 */
	*out_head_relation = head_relation;

out:
	ipc_client_connection_unlock(ipc_c);
	return xret;
}


/*
 *
 * Member functions
 *
 */

static xrt_result_t
ipc_client_hmd_update_inputs(struct xrt_device *xdev)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);

	// Sync active rendering mode from shared memory (server updates on V key)
	if (xdev->hmd != NULL) {
		xdev->hmd->active_rendering_mode_index =
		    ich->ipc_c->ism->hmd.active_rendering_mode_index;
	}

	// Call the base update_inputs (IPC call to server)
	xrt_result_t xret = ipc_call_device_update_input(ich->ipc_c, ich->device_id);
	IPC_CHK_ALWAYS_RET(ich->ipc_c, xret, "ipc_call_device_update_input");
}

static xrt_result_t
ipc_client_hmd_get_view_poses(struct xrt_device *xdev,
                              const struct xrt_vec3 *default_eye_relation,
                              int64_t at_timestamp_ns,
                              uint32_t view_count,
                              struct xrt_space_relation *out_head_relation,
                              struct xrt_fov *out_fovs,
                              struct xrt_pose *out_poses)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	xrt_result_t xret;

	struct ipc_info_get_view_poses_2 info = {0};

	if (view_count == 2) {
		// Fast path.
		xret = ipc_call_device_get_view_poses_2( //
		    ich->ipc_c,                          //
		    ich->device_id,                      //
		    default_eye_relation,                //
		    at_timestamp_ns,                     //
		    view_count,                          //
		    &info);                              //
		IPC_CHK_AND_RET(ich->ipc_c, xret, "ipc_call_device_get_view_poses_2");

		*out_head_relation = info.head_relation;
		for (int i = 0; i < 2; i++) {
			out_fovs[i] = info.fovs[i];
			out_poses[i] = info.poses[i];
		}


	} else if (view_count <= IPC_MAX_RAW_VIEWS) {
		// Artificial limit.
		xret = call_get_view_poses_raw( //
		    ich,                        //
		    default_eye_relation,       //
		    at_timestamp_ns,            //
		    view_count,                 //
		    out_head_relation,          //
		    out_fovs,                   //
		    out_poses);                 //
	} else {
		IPC_ERROR(ich->ipc_c, "Cannot handle %u view_count, %u or less supported.", view_count,
		          (uint32_t)IPC_MAX_RAW_VIEWS);
		assert(false && !"Too large view_count!");
	}

	return xret;
}

static xrt_result_t
ipc_client_hmd_compute_distortion(
    struct xrt_device *xdev, uint32_t view, float u, float v, struct xrt_uv_triplet *out_result)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	xrt_result_t xret;

	xret = ipc_call_device_compute_distortion( //
	    ich->ipc_c,                            //
	    ich->device_id,                        //
	    view,                                  //
	    u,                                     //
	    v,                                     //
	    out_result);                           //

	IPC_CHK_ALWAYS_RET(ich->ipc_c, xret, "ipc_call_device_compute_distortion");
}

static bool
ipc_client_hmd_is_form_factor_available(struct xrt_device *xdev, enum xrt_form_factor form_factor)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	xrt_result_t xret;

	bool available = false;
	xret = ipc_call_device_is_form_factor_available( //
	    ich->ipc_c,                                  //
	    ich->device_id,                              //
	    form_factor,                                 //
	    &available);                                 //
	IPC_CHK_ONLY_PRINT(ich->ipc_c, xret, "ipc_call_device_is_form_factor_available");

	return available;
}

static xrt_result_t
ipc_client_hmd_get_visibility_mask(struct xrt_device *xdev,
                                   enum xrt_visibility_mask_type type,
                                   uint32_t view_index,
                                   struct xrt_visibility_mask **out_mask)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	struct ipc_connection *ipc_c = ich->ipc_c;
	struct xrt_visibility_mask *mask = NULL;
	xrt_result_t xret;

	ipc_client_connection_lock(ipc_c);

	xret = ipc_send_device_get_visibility_mask_locked(ipc_c, ich->device_id, type, view_index);
	IPC_CHK_WITH_GOTO(ipc_c, xret, "ipc_send_device_get_visibility_mask_locked", err_mask_unlock);

	uint32_t mask_size;
	xret = ipc_receive_device_get_visibility_mask_locked(ipc_c, &mask_size);
	IPC_CHK_WITH_GOTO(ipc_c, xret, "ipc_receive_device_get_visibility_mask_locked", err_mask_unlock);

	mask = U_CALLOC_WITH_CAST(struct xrt_visibility_mask, mask_size);
	if (mask == NULL) {
		IPC_ERROR(ich->ipc_c, "failed to allocate xrt_visibility_mask");
		goto err_mask_unlock;
	}

	xret = ipc_receive(&ipc_c->imc, mask, mask_size);
	IPC_CHK_WITH_GOTO(ipc_c, xret, "ipc_receive", err_mask_free);

	*out_mask = mask;
	ipc_client_connection_unlock(ipc_c);

	return XRT_SUCCESS;

err_mask_free:
	free(mask);
err_mask_unlock:
	ipc_client_connection_unlock(ipc_c);
	return XRT_ERROR_IPC_FAILURE;
}

/*!
 * ADR-066 C1: copy the head's slot table out of shared memory - the two
 * presented slots plus the envelope behind them - and publish the envelope
 * count and generation to u_rendering_modes. Returns the generation copied.
 * The server writes the generation last; a copy that straddles a rewrite is
 * caught by the re-read and retried at the next poll.
 */
static uint32_t
ipc_client_hmd_copy_rendering_modes(ipc_client_hmd_t *ich)
{
	struct ipc_shared_memory *ism = ich->ipc_c->ism;
	const uint32_t gen = ism->hmd.rendering_mode_generation;
	uint32_t env = ism->hmd.rendering_mode_envelope_count;
	uint32_t count = ism->hmd.rendering_mode_count;
	if (count > XRT_MAX_RENDERING_MODES) {
		count = XRT_MAX_RENDERING_MODES;
	}
	if (env < count || env > XRT_MAX_RENDERING_MODES) {
		env = count;
	}
	for (uint32_t i = 0; i < env; i++) {
		ich->base.rendering_modes[i] = ism->hmd.rendering_modes[i];
	}
	ich->base.rendering_mode_count = count;
	u_device_rendering_modes_publish(&ich->base, env, gen);
	return gen;
}

//! The u_rendering_modes sync hook: re-copy when the service's generation moved.
static uint32_t
ipc_client_hmd_sync_rendering_modes(struct xrt_device *xdev)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	const uint32_t have = u_device_rendering_modes_generation(xdev);
	const uint32_t gen = ich->ipc_c->ism->hmd.rendering_mode_generation;
	if (gen == have) {
		return have;
	}
	const uint32_t copied = ipc_client_hmd_copy_rendering_modes(ich);
	if (ich->ipc_c->ism->hmd.rendering_mode_generation != copied) {
		// The server rewrote it under us: report the old generation so the
		// next poll copies again.
		u_device_rendering_modes_publish(xdev, u_device_rendering_modes_envelope_count(xdev), have);
		return have;
	}
	U_LOG_W(
	    "IPC client: the head's rendering-mode table changed (generation %u -> %u, slot 1 now '%s') - ADR-066 C1",
	    have, copied, ich->base.rendering_mode_count > 1 ? ich->base.rendering_modes[1].mode_name : "-");
	return copied;
}

static void
ipc_client_hmd_destroy(struct xrt_device *xdev)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);

	u_device_rendering_modes_forget(xdev);

	// Remove the variable tracking.
	u_var_remove_root(ich);

	// Free and de-init the shared things.
	ipc_client_xdev_fini(ich);

	// Free this device with the helper.
	u_device_free(&ich->base);
}

static xrt_result_t
ipc_client_hmd_get_brightness(struct xrt_device *xdev, float *out_brightness)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	struct ipc_connection *ipc_c = ich->ipc_c;
	xrt_result_t xret;

	ipc_client_connection_lock(ipc_c);

	xret = ipc_call_device_get_brightness(ipc_c, ich->device_id, out_brightness);
	IPC_CHK_ONLY_PRINT(ipc_c, xret, "ipc_call_device_get_brightness");

	ipc_client_connection_unlock(ipc_c);

	return xret;
}

static xrt_result_t
ipc_client_hmd_set_brightness(struct xrt_device *xdev, float brightness, bool relative)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	struct ipc_connection *ipc_c = ich->ipc_c;
	xrt_result_t xret;

	ipc_client_connection_lock(ipc_c);

	xret = ipc_call_device_set_brightness(ipc_c, ich->device_id, brightness, relative);
	IPC_CHK_ONLY_PRINT(ipc_c, xret, "ipc_call_device_set_brightness");

	ipc_client_connection_unlock(ipc_c);

	return xret;
}

/*!
 * @public @memberof ipc_client_hmd
 */
struct xrt_device *
ipc_client_hmd_create(struct ipc_connection *ipc_c, struct xrt_tracking_origin *xtrack, uint32_t device_id)
{
	// Convenience helper.
	struct ipc_shared_memory *ism = ipc_c->ism;

	// Allocate a HMD device.
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD);
	ipc_client_hmd_t *ich = U_DEVICE_ALLOCATE(ipc_client_hmd_t, flags, 0, 0);

	// Fills in almost everything a regular device needs.
	ipc_client_xdev_init(ich, ipc_c, xtrack, device_id);

	// Fill in needed HMD functions, and destroy.
	ich->base.update_inputs = ipc_client_hmd_update_inputs;
	ich->base.get_view_poses = ipc_client_hmd_get_view_poses;
	ich->base.compute_distortion = ipc_client_hmd_compute_distortion;
	ich->base.is_form_factor_available = ipc_client_hmd_is_form_factor_available;
	ich->base.get_visibility_mask = ipc_client_hmd_get_visibility_mask;
	ich->base.destroy = ipc_client_hmd_destroy;
	ich->base.get_brightness = ipc_client_hmd_get_brightness;
	ich->base.set_brightness = ipc_client_hmd_set_brightness;

	// Setup blend-modes.
	ich->base.hmd->blend_mode_count = ipc_c->ism->hmd.blend_mode_count;
	for (int i = 0; i < XRT_MAX_DEVICE_BLEND_MODES; i++) {
		ich->base.hmd->blend_modes[i] = ipc_c->ism->hmd.blend_modes[i];
	}

	// Setup the views.
	ich->base.hmd->view_count = ism->hmd.view_count;
	for (uint32_t i = 0; i < ich->base.hmd->view_count; ++i) {
		ich->base.hmd->views[i].display.w_pixels = ipc_c->ism->hmd.views[i].display.w_pixels;
		ich->base.hmd->views[i].display.h_pixels = ipc_c->ism->hmd.views[i].display.h_pixels;
	}

	// Setup rendering modes (for workspace mode 2D/3D switching via V key):
	// the two slots plus the envelope behind them (ADR-066 C1), kept current
	// by the sync hook the state tracker polls.
	(void)ipc_client_hmd_copy_rendering_modes(ich);
	u_device_rendering_modes_set_sync_hook(&ich->base, ipc_client_hmd_sync_rendering_modes);
	ich->base.hmd->active_rendering_mode_index = ism->hmd.active_rendering_mode_index;

	// Distortion information, fills in xdev->compute_distortion().
	u_distortion_mesh_set_none(&ich->base);

	// Setup variable tracker.
	u_var_add_root(ich, ich->base.str, true);
	u_var_add_ro_u32(ich, &ich->device_id, "device_id");

	return &ich->base;
}
