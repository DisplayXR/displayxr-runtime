// Copyright 2020-2024, Collabora, Ltd.
// Copyright 2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared functions for IPC client @ref xrt_device.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup ipc_client
 */

#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "math/m_api.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_device.h"

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client_xdev.h"
#include "ipc_client_generated.h"


/*
 *
 * Functions from xrt_device.
 *
 */

static xrt_result_t
ipc_client_xdev_update_inputs(struct xrt_device *xdev)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	xrt_result_t xret = ipc_call_device_update_input(icx->ipc_c, icx->device_id);
	IPC_CHK_ALWAYS_RET(icx->ipc_c, xret, "ipc_call_device_update_input");
}

static xrt_result_t
ipc_client_xdev_get_tracked_pose(struct xrt_device *xdev,
                                 enum xrt_input_name name,
                                 int64_t at_timestamp_ns,
                                 struct xrt_space_relation *out_relation)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	xrt_result_t xret = ipc_call_device_get_tracked_pose( //
	    icx->ipc_c,                                       //
	    icx->device_id,                                   //
	    name,                                             //
	    at_timestamp_ns,                                  //
	    out_relation);                                    //
	IPC_CHK_ALWAYS_RET(icx->ipc_c, xret, "ipc_call_device_get_tracked_pose");
}

static xrt_result_t
ipc_client_xdev_get_hand_tracking(struct xrt_device *xdev,
                                  enum xrt_input_name name,
                                  int64_t at_timestamp_ns,
                                  struct xrt_hand_joint_set *out_value,
                                  int64_t *out_timestamp_ns)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	xrt_result_t xret = ipc_call_device_get_hand_tracking( //
	    icx->ipc_c,                                        //
	    icx->device_id,                                    //
	    name,                                              //
	    at_timestamp_ns,                                   //
	    out_value,                                         //
	    out_timestamp_ns);                                 //
	IPC_CHK_ALWAYS_RET(icx->ipc_c, xret, "ipc_call_device_get_hand_tracking");
}

static xrt_result_t
ipc_client_xdev_get_face_tracking(struct xrt_device *xdev,
                                  enum xrt_input_name facial_expression_type,
                                  int64_t at_timestamp_ns,
                                  struct xrt_facial_expression_set *out_value)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	xrt_result_t xret = ipc_call_device_get_face_tracking( //
	    icx->ipc_c,                                        //
	    icx->device_id,                                    //
	    facial_expression_type,                            //
	    at_timestamp_ns,                                   //
	    out_value);                                        //
	IPC_CHK_ALWAYS_RET(icx->ipc_c, xret, "ipc_call_device_get_face_tracking");
}

static xrt_result_t
ipc_client_xdev_get_body_skeleton(struct xrt_device *xdev,
                                  enum xrt_input_name body_tracking_type,
                                  struct xrt_body_skeleton *out_value)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	xrt_result_t xret = ipc_call_device_get_body_skeleton( //
	    icx->ipc_c,                                        //
	    icx->device_id,                                    //
	    body_tracking_type,                                //
	    out_value);                                        //
	IPC_CHK_ALWAYS_RET(icx->ipc_c, xret, "ipc_call_device_get_body_skeleton");
}

static xrt_result_t
ipc_client_xdev_get_body_joints(struct xrt_device *xdev,
                                enum xrt_input_name body_tracking_type,
                                int64_t desired_timestamp_ns,
                                struct xrt_body_joint_set *out_value)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	xrt_result_t xret = ipc_call_device_get_body_joints( //
	    icx->ipc_c,                                      //
	    icx->device_id,                                  //
	    body_tracking_type,                              //
	    desired_timestamp_ns,                            //
	    out_value);                                      //
	IPC_CHK_ALWAYS_RET(icx->ipc_c, xret, "ipc_call_device_get_body_joints");
}

static xrt_result_t
ipc_client_xdev_get_presence(struct xrt_device *xdev, bool *presence)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	xrt_result_t xret = ipc_call_device_get_presence( //
	    icx->ipc_c,                                   //
	    icx->device_id,                               //
	    presence);                                    //
	IPC_CHK_ALWAYS_RET(icx->ipc_c, xret, "ipc_call_device_get_presence");
}

static xrt_result_t
ipc_client_xdev_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);
	struct ipc_connection *ipc_c = icx->ipc_c;

	xrt_result_t xret;
	if (value->type == XRT_OUTPUT_VALUE_TYPE_PCM_VIBRATION) {
		uint32_t samples_sent = MIN(value->pcm_vibration.sample_rate, 4000);

		struct ipc_pcm_haptic_buffer samples = {
		    .append = value->pcm_vibration.append,
		    .num_samples = samples_sent,
		    .sample_rate = value->pcm_vibration.sample_rate,
		};

		ipc_client_connection_lock(ipc_c);

		xret = ipc_send_device_set_haptic_output_locked(ipc_c, icx->device_id, name, &samples);
		IPC_CHK_WITH_GOTO(ipc_c, xret, "ipc_send_device_set_haptic_output_locked", send_haptic_output_end);

		xrt_result_t alloc_xret;
		xret = ipc_receive(&ipc_c->imc, &alloc_xret, sizeof alloc_xret);
		if (xret != XRT_SUCCESS || alloc_xret != XRT_SUCCESS) {
			goto send_haptic_output_end;
		}

		xret = ipc_send(&ipc_c->imc, value->pcm_vibration.buffer, sizeof(float) * samples_sent);
		if (xret != XRT_SUCCESS) {
			goto send_haptic_output_end;
		}

		xret = ipc_receive(&ipc_c->imc, value->pcm_vibration.samples_consumed,
		                   sizeof(*value->pcm_vibration.samples_consumed));
		if (xret != XRT_SUCCESS) {
			goto send_haptic_output_end;
		}

	send_haptic_output_end:
		ipc_client_connection_unlock(ipc_c);
	} else {
		xret = ipc_call_device_set_output(ipc_c, icx->device_id, name, value);
		IPC_CHK_ONLY_PRINT(ipc_c, xret, "ipc_call_device_set_output");
	}

	return xret;
}

xrt_result_t
ipc_client_xdev_get_output_limits(struct xrt_device *xdev, struct xrt_output_limits *limits)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	xrt_result_t xret = ipc_call_device_get_output_limits(icx->ipc_c, icx->device_id, limits);
	IPC_CHK_ONLY_PRINT(icx->ipc_c, xret, "ipc_call_device_get_output_limits");

	return xret;
}


/*
 *
 * Plane detection functions.
 *
 */

static xrt_result_t
ipc_client_xdev_begin_plane_detection_ext(struct xrt_device *xdev,
                                          const struct xrt_plane_detector_begin_info_ext *begin_info,
                                          uint64_t plane_detection_id,
                                          uint64_t *out_plane_detection_id)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	icx->ipc_c->ism->plane_begin_info_ext = *begin_info;

	xrt_result_t r = ipc_call_device_begin_plane_detection_ext(icx->ipc_c, icx->device_id, plane_detection_id,
	                                                           out_plane_detection_id);
	if (r != XRT_SUCCESS) {
		IPC_ERROR(icx->ipc_c, "Error sending hmd_begin_plane_detection_ext!");
		return r;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
ipc_client_xdev_destroy_plane_detection_ext(struct xrt_device *xdev, uint64_t plane_detection_id)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	xrt_result_t r = ipc_call_device_destroy_plane_detection_ext(icx->ipc_c, icx->device_id, plane_detection_id);
	if (r != XRT_SUCCESS) {
		IPC_ERROR(icx->ipc_c, "Error sending destroy_plane_detection_ext!");
		return r;
	}

	return XRT_SUCCESS;
}

/*!
 * Helper function for @ref xrt_device::get_plane_detection_state.
 *
 * @public @memberof xrt_device
 */
static xrt_result_t
ipc_client_xdev_get_plane_detection_state_ext(struct xrt_device *xdev,
                                              uint64_t plane_detection_id,
                                              enum xrt_plane_detector_state_ext *out_state)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);

	xrt_result_t r =
	    ipc_call_device_get_plane_detection_state_ext(icx->ipc_c, icx->device_id, plane_detection_id, out_state);
	if (r != XRT_SUCCESS) {
		IPC_ERROR(icx->ipc_c, "Error sending get_plane_detection_state_ext!");
		return r;
	}

	return XRT_SUCCESS;
}

/*!
 * Helper function for @ref xrt_device::get_plane_detections.
 *
 * @public @memberof xrt_device
 */
static xrt_result_t
ipc_client_xdev_get_plane_detections_ext(struct xrt_device *xdev,
                                         uint64_t plane_detection_id,
                                         struct xrt_plane_detections_ext *out_detections)
{
	struct ipc_client_xdev *icx = ipc_client_xdev(xdev);
	struct ipc_connection *ipc_c = icx->ipc_c;

	ipc_client_connection_lock(ipc_c);

	xrt_result_t xret = ipc_send_device_get_plane_detections_ext_locked(ipc_c, icx->device_id, plane_detection_id);
	IPC_CHK_WITH_GOTO(icx->ipc_c, xret, "ipc_send_device_get_plane_detections_ext_locked", out);

	// in this case, size == count
	uint32_t location_size = 0;
	uint32_t polygon_size = 0;
	uint32_t vertex_size = 0;

	xret = ipc_receive_device_get_plane_detections_ext_locked(ipc_c, &location_size, &polygon_size, &vertex_size);
	IPC_CHK_WITH_GOTO(icx->ipc_c, xret, "ipc_receive_device_get_plane_detections_ext_locked", out);


	// With no locations, the service won't send anything else
	if (location_size < 1) {
		out_detections->location_count = 0;
		goto out;
	}

	// realloc arrays in out_detections if necessary, then receive contents

	out_detections->location_count = location_size;
	if (out_detections->location_size < location_size) {
		U_ARRAY_REALLOC_OR_FREE(out_detections->locations, struct xrt_plane_detector_location_ext,
		                        location_size);
		U_ARRAY_REALLOC_OR_FREE(out_detections->polygon_info_start_index, uint32_t, location_size);
		out_detections->location_size = location_size;
	}

	if (out_detections->polygon_info_size < polygon_size) {
		U_ARRAY_REALLOC_OR_FREE(out_detections->polygon_infos, struct xrt_plane_polygon_info_ext, polygon_size);
		out_detections->polygon_info_size = polygon_size;
	}

	if (out_detections->vertex_size < vertex_size) {
		U_ARRAY_REALLOC_OR_FREE(out_detections->vertices, struct xrt_vec2, vertex_size);
		out_detections->vertex_size = vertex_size;
	}

	if ((location_size > 0 &&
	     (out_detections->locations == NULL || out_detections->polygon_info_start_index == NULL)) ||
	    (polygon_size > 0 && out_detections->polygon_infos == NULL) ||
	    (vertex_size > 0 && out_detections->vertices == NULL)) {
		IPC_ERROR(icx->ipc_c, "Error allocating memory for plane detections!");
		out_detections->location_size = 0;
		out_detections->polygon_info_size = 0;
		out_detections->vertex_size = 0;
		xret = XRT_ERROR_IPC_FAILURE;
		goto out;
	}

	if (location_size > 0) {
		// receive location_count * locations
		xret = ipc_receive(&ipc_c->imc, out_detections->locations,
		                   sizeof(struct xrt_plane_detector_location_ext) * location_size);
		IPC_CHK_WITH_GOTO(icx->ipc_c, xret, "ipc_receive(1)", out);

		// receive location_count * polygon_info_start_index
		xret = ipc_receive(&ipc_c->imc, out_detections->polygon_info_start_index,
		                   sizeof(uint32_t) * location_size);
		IPC_CHK_WITH_GOTO(icx->ipc_c, xret, "ipc_receive(2)", out);
	}


	if (polygon_size > 0) {
		// receive polygon_count * polygon_infos
		xret = ipc_receive(&ipc_c->imc, out_detections->polygon_infos,
		                   sizeof(struct xrt_plane_polygon_info_ext) * polygon_size);
		IPC_CHK_WITH_GOTO(icx->ipc_c, xret, "ipc_receive(3)", out);
	}

	if (vertex_size > 0) {
		// receive vertex_count * vertices
		xret = ipc_receive(&ipc_c->imc, out_detections->vertices, sizeof(struct xrt_vec2) * vertex_size);
		IPC_CHK_WITH_GOTO(icx->ipc_c, xret, "ipc_receive(4)", out);
	}

out:
	ipc_client_connection_unlock(ipc_c);
	return xret;
}


/*
 *
 * 'Exported' functions.
 *
 */

void
ipc_client_xdev_init(struct ipc_client_xdev *icx,
                     struct ipc_connection *ipc_c,
                     struct xrt_tracking_origin *xtrack,
                     uint32_t device_id)
{
	// Helpers.
	struct ipc_shared_memory *ism = ipc_c->ism;
	struct ipc_shared_device *isdev = &ism->isdevs[device_id];

	// Important fields.
	icx->ipc_c = ipc_c;
	icx->device_id = device_id;

	// Shared implemented functions.
	icx->base.update_inputs = ipc_client_xdev_update_inputs;
	icx->base.get_tracked_pose = ipc_client_xdev_get_tracked_pose;
	icx->base.get_hand_tracking = ipc_client_xdev_get_hand_tracking;
	icx->base.get_face_tracking = ipc_client_xdev_get_face_tracking;
	icx->base.get_body_skeleton = ipc_client_xdev_get_body_skeleton;
	icx->base.get_body_joints = ipc_client_xdev_get_body_joints;
	icx->base.get_presence = ipc_client_xdev_get_presence;
	icx->base.set_output = ipc_client_xdev_set_output;
	icx->base.get_output_limits = ipc_client_xdev_get_output_limits;

	// Plane detection EXT.
	icx->base.begin_plane_detection_ext = ipc_client_xdev_begin_plane_detection_ext;
	icx->base.destroy_plane_detection_ext = ipc_client_xdev_destroy_plane_detection_ext;
	icx->base.get_plane_detection_state_ext = ipc_client_xdev_get_plane_detection_state_ext;
	icx->base.get_plane_detections_ext = ipc_client_xdev_get_plane_detections_ext;

	// Not implemented functions, some get overridden.
	icx->base.get_view_poses = u_device_ni_get_view_poses;
	icx->base.compute_distortion = u_device_ni_compute_distortion;
	icx->base.get_visibility_mask = u_device_ni_get_visibility_mask;
	icx->base.is_form_factor_available = u_device_ni_is_form_factor_available;
	icx->base.get_battery_status = u_device_ni_get_battery_status;

	// Copying the information from the isdev.
	icx->base.device_type = isdev->device_type;
	icx->base.supported = isdev->supported;
	icx->base.tracking_origin = xtrack;
	icx->base.name = isdev->name;

	// Print name.
	snprintf(icx->base.str, XRT_DEVICE_NAME_LEN, "%s", isdev->str);
	snprintf(icx->base.serial, XRT_DEVICE_NAME_LEN, "%s", isdev->serial);

	// Setup inputs, by pointing directly to the shared memory.
	assert(isdev->input_count > 0);
	icx->base.inputs = &ism->inputs[isdev->first_input_index];
	icx->base.input_count = isdev->input_count;

	// Setup outputs, if any point directly into the shared memory.
	icx->base.output_count = isdev->output_count;
	if (isdev->output_count > 0) {
		icx->base.outputs = &ism->outputs[isdev->first_output_index];
	} else {
		icx->base.outputs = NULL;
	}

	// Setup binding profiles.
	icx->base.binding_profile_count = isdev->binding_profile_count;
	if (isdev->binding_profile_count > 0) {
		icx->base.binding_profiles =
		    U_TYPED_ARRAY_CALLOC(struct xrt_binding_profile, isdev->binding_profile_count);
	}

	for (size_t i = 0; i < isdev->binding_profile_count; i++) {
		struct xrt_binding_profile *xbp = &icx->base.binding_profiles[i];
		struct ipc_shared_binding_profile *isbp =
		    &ism->binding_profiles[isdev->first_binding_profile_index + i];

		xbp->name = isbp->name;
		if (isbp->input_count > 0) {
			xbp->inputs = &ism->input_pairs[isbp->first_input_index];
			xbp->input_count = isbp->input_count;
		}
		if (isbp->output_count > 0) {
			xbp->outputs = &ism->output_pairs[isbp->first_output_index];
			xbp->output_count = isbp->output_count;
		}
	}
}

void
ipc_client_xdev_fini(struct ipc_client_xdev *icx)
{
	// We do not own these, so don't free them.
	icx->base.inputs = NULL;
	icx->base.outputs = NULL;

	// We allocated the bindings profiles.
	if (icx->base.binding_profiles != NULL) {
		free(icx->base.binding_profiles);
		icx->base.binding_profiles = NULL;
	}
}
