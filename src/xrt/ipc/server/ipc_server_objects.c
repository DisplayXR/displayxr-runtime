// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Tracking objects to IDs.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup ipc_server
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_tracking.h"

#include "shared/ipc_protocol.h"
#include "server/ipc_server.h"
#include "server/ipc_server_objects.h"

#include <assert.h>
#include <string.h>


/*
 *
 * Helper functions.
 *
 */

xrt_result_t
allocate_fill_in_xdev_and_state(volatile struct ipc_client_state *ics, uint32_t index, struct xrt_device *xdev)
{
	assert(index < XRT_SYSTEM_MAX_DEVICES);
	assert(xdev != NULL);

	// Convenience variables.
	struct ipc_server *s = ics->server;
	struct ipc_shared_memory *ism = get_ism(ics);

	// Setup the tracking origin ID.
	uint32_t tracking_origin_id = UINT32_MAX;
	xrt_result_t xret = ipc_server_objects_get_xtrack_id_or_add(ics, xdev->tracking_origin, &tracking_origin_id);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(s, "Failed to get/add tracking origin ID for device: '%s'", xdev->str);
		// Clear the device we just added since setup failed.
		ics->objects.xdevs[index] = NULL;
		return xret;
	}

	// Store the device in the objects array.
	ics->objects.xdevs[index] = xdev;

	// Holds internal state.
	volatile struct ipc_client_device_state *state = &ics->objects.states[index];

	// Initial update.
	xrt_device_update_inputs(xdev);

	// Copy the initial state and also count the number of inputs.
	const uint32_t input_count = xdev->input_count;
	const uint32_t input_start = ics->objects.input_index;
	for (size_t k = 0; k < input_count; k++) {
		ism->inputs[ics->objects.input_index++] = xdev->inputs[k];
	}

	// Setup the 'offsets' for the inputs.
	state->first_input_index = input_count > 0 ? input_start : 0;

	// Copy the initial state and also count the number of outputs.
	const uint32_t output_count = xdev->output_count;
	const uint32_t output_start = ics->objects.output_index;
	for (size_t k = 0; k < output_count; k++) {
		ism->outputs[ics->objects.output_index++] = xdev->outputs[k];
	}

	// Setup the 'offsets' for the outputs.
	state->first_output_index = output_count > 0 ? output_start : 0;

	// Increment the device count.
	ics->objects.isdev_count++;

	return XRT_SUCCESS;
}


/*
 *
 * Device functions.
 *
 */

xrt_result_t
ipc_server_objects_get_xdev_and_validate(volatile struct ipc_client_state *ics,
                                         uint32_t id,
                                         struct xrt_device **out_xdev)
{
	if (id >= XRT_SYSTEM_MAX_DEVICES) {
		IPC_ERROR(ics->server, "Invalid device ID %u (>= XRT_SYSTEM_MAX_DEVICES)", id);
		return XRT_ERROR_IPC_FAILURE;
	}

	struct xrt_device *xdev = ics->objects.xdevs[id];
	if (xdev == NULL) {
		IPC_ERROR(ics->server, "Device ID %u not found (NULL)", id);
		return XRT_ERROR_IPC_FAILURE;
	}

	*out_xdev = xdev;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_objects_get_icdev_state_and_validate(volatile struct ipc_client_state *ics,
                                                uint32_t id,
                                                volatile struct ipc_client_device_state **out_state)
{
	struct xrt_device *xdev = NULL; // Not used, just to satisfy the function signature.
	xrt_result_t xret = ipc_server_objects_get_xdev_and_validate(ics, id, &xdev);
	IPC_CHK_AND_RET(ics->server, xret, "ipc_server_objects_get_xdev_and_validate");

	// Validation passed, get the shared device.
	*out_state = &ics->objects.states[id];

	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_objects_get_xdev_id_or_add(volatile struct ipc_client_state *ics, struct xrt_device *xdev, uint32_t *out_id)
{
	assert(out_id != NULL);
	assert(xdev != NULL);

	// Check if device already exists and return its ID.
	uint32_t index = 0;
	for (; index < XRT_SYSTEM_MAX_DEVICES; index++) {
		if (ics->objects.xdevs[index] == NULL) {
			xrt_result_t xret = allocate_fill_in_xdev_and_state(ics, index, xdev);
			IPC_CHK_AND_RET(ics->server, xret, "allocate_fill_in_xdev_and_state");

			*out_id = index;

			return XRT_SUCCESS;
		}
		if (ics->objects.xdevs[index] == xdev) {
			*out_id = index;
			return XRT_SUCCESS;
		}
	}


	if (index >= XRT_SYSTEM_MAX_DEVICES) {
		IPC_ERROR(ics->server, "Failed to find available slot for device: '%s'", xdev->str);
		return XRT_ERROR_IPC_FAILURE;
	}

	*out_id = index;

	return XRT_SUCCESS;
}


/*
 *
 * Tracking origin functions.
 *
 */

xrt_result_t
ipc_server_objects_get_xtrack_and_validate(volatile struct ipc_client_state *ics,
                                           uint32_t id,
                                           struct xrt_tracking_origin **out_xtrack)
{
	if (id >= XRT_SYSTEM_MAX_DEVICES) {
		IPC_ERROR(ics->server, "Invalid tracking origin ID %u (>= XRT_SYSTEM_MAX_DEVICES)", id);
		return XRT_ERROR_IPC_FAILURE;
	}

	struct xrt_tracking_origin *xtrack = ics->objects.xtracks[id];
	if (xtrack == NULL) {
		IPC_ERROR(ics->server, "Tracking origin ID %u not found (NULL)", id);
		return XRT_ERROR_IPC_FAILURE;
	}

	*out_xtrack = xtrack;

	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_objects_get_xtrack_id_or_add(volatile struct ipc_client_state *ics,
                                        struct xrt_tracking_origin *xtrack,
                                        uint32_t *out_id)
{
	assert(out_id != NULL);

	// Find the next available slot in xtracks array and assign an ID, or if we find the xtrack return it.
	for (uint32_t index = 0; index < XRT_SYSTEM_MAX_DEVICES; index++) {
		if (ics->objects.xtracks[index] == NULL) {
			ics->objects.xtracks[index] = xtrack;
			*out_id = index;
			return XRT_SUCCESS;
		}
		if (ics->objects.xtracks[index] == xtrack) {
			*out_id = index;
			return XRT_SUCCESS;
		}
	}

	// No available slot or xtrack found
	IPC_ERROR(ics->server, "Failed to find available slot for tracking origin: '%s'", xtrack->name);

	return XRT_ERROR_IPC_FAILURE;
}
