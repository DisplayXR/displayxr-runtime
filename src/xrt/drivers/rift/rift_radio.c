// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Radio state machine functions for the Oculus Rift.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift
 */

#include "util/u_device.h"
#include "util/u_var.h"

#include "rift_radio.h"
#include "rift_bindings.h"


static void
rift_update_input(struct xrt_device *device, size_t index, union xrt_input_value value, int64_t now)
{
	device->inputs[index].value = value;
	device->inputs[index].timestamp = now;
}

/*
 * Rift Touch Controller device functions
 */

static xrt_result_t
rift_touch_controller_get_tracked_pose(struct xrt_device *xdev,
                                       const enum xrt_input_name name,
                                       int64_t at_timestamp_ns,
                                       struct xrt_space_relation *out_relation)
{
	switch (name) {
	case XRT_INPUT_TOUCH_GRIP_POSE:
	case XRT_INPUT_TOUCH_AIM_POSE:
		// @todo
		(*out_relation) = (struct xrt_space_relation)XRT_SPACE_RELATION_ZERO;
		break;
	default: return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	return XRT_SUCCESS;
}

static void
rift_touch_controller_destroy(struct xrt_device *xdev)
{
	struct rift_touch_controller *controller = (struct rift_touch_controller *)xdev;

	u_var_remove_root(controller);

	if (controller->input_mutex_created) {
		os_mutex_destroy(&controller->input_mutex);
	}

	u_device_free(&controller->base);
}

static xrt_result_t
rift_touch_controller_update_inputs(struct xrt_device *xdev)
{
	struct rift_touch_controller *controller = rift_touch_controller(xdev);

	os_mutex_lock(&controller->input_mutex);
	struct rift_touch_controller_input_state input_state = controller->input_state;
	os_mutex_unlock(&controller->input_mutex);

	uint64_t now = os_monotonic_get_ns();

	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_A_CLICK,
	                  (union xrt_input_value){.boolean = input_state.buttons & RIFT_TOUCH_CONTROLLER_BUTTON_A},
	                  now);
	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_B_CLICK,
	                  (union xrt_input_value){.boolean = input_state.buttons & RIFT_TOUCH_CONTROLLER_BUTTON_B},
	                  now);
	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_SYSTEM_CLICK,
	                  (union xrt_input_value){.boolean = input_state.buttons & RIFT_TOUCH_CONTROLLER_BUTTON_MENU},
	                  now);
	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_THUMBSTICK_CLICK,
	                  (union xrt_input_value){.boolean = input_state.buttons & RIFT_TOUCH_CONTROLLER_BUTTON_STICK},
	                  now);

	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_THUMBSTICK,
	                  (union xrt_input_value){.vec2 = {CLAMP(input_state.stick[0], -1.0f, 1.0f),
	                                                   CLAMP(input_state.stick[1], -1.0f, 1.0f)}},
	                  now);

	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_TRIGGER_VALUE,
	                  (union xrt_input_value){.vec1 = {CLAMP(input_state.trigger, 0.0f, 1.0f)}}, now);
	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_SQUEEZE_VALUE,
	                  (union xrt_input_value){.vec1 = {CLAMP(input_state.grip, 0.0f, 1.0f)}}, now);

	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_A_TOUCH,
	                  (union xrt_input_value){.boolean = input_state.cap_a_x >= 1}, now);
	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_B_TOUCH,
	                  (union xrt_input_value){.boolean = input_state.cap_b_y >= 1}, now);
	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_TRIGGER_TOUCH,
	                  (union xrt_input_value){.boolean = input_state.cap_trigger >= 1}, now);
	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_THUMBSTICK_TOUCH,
	                  (union xrt_input_value){.boolean = input_state.cap_stick >= 1}, now);
	rift_update_input(&controller->base, RIFT_TOUCH_CONTROLLER_INPUT_THUMBREST_TOUCH,
	                  (union xrt_input_value){.boolean = input_state.cap_thumbrest >= 1}, now);


	return XRT_SUCCESS;
}

/*
 * Rift Remote device functions
 */
static xrt_result_t
rift_remote_get_tracked_pose(struct xrt_device *xdev,
                             const enum xrt_input_name name,
                             int64_t at_timestamp_ns,
                             struct xrt_space_relation *out_relation)
{
	// The remote has no tracked poses
	return XRT_ERROR_INPUT_UNSUPPORTED;
}

static void
rift_remote_destroy(struct xrt_device *xdev)
{
	struct rift_remote *remote = (struct rift_remote *)xdev;

	u_var_remove_root(remote);

	u_device_free(&remote->base);
}

static xrt_result_t
rift_remote_update_inputs(struct xrt_device *xdev)
{
	struct rift_remote *remote = rift_remote(xdev);

	uint64_t now = os_monotonic_get_ns();

	uint16_t buttons = xrt_atomic_s32_load(&remote->buttons);

	rift_update_input(&remote->base, RIFT_REMOTE_INPUT_DPAD_UP,
	                  (union xrt_input_value){.boolean = buttons & RIFT_REMOTE_BUTTON_MASK_DPAD_UP}, now);
	rift_update_input(&remote->base, RIFT_REMOTE_INPUT_DPAD_DOWN,
	                  (union xrt_input_value){.boolean = buttons & RIFT_REMOTE_BUTTON_MASK_DPAD_DOWN}, now);
	rift_update_input(&remote->base, RIFT_REMOTE_INPUT_DPAD_LEFT,
	                  (union xrt_input_value){.boolean = buttons & RIFT_REMOTE_BUTTON_MASK_DPAD_LEFT}, now);
	rift_update_input(&remote->base, RIFT_REMOTE_INPUT_DPAD_RIGHT,
	                  (union xrt_input_value){.boolean = buttons & RIFT_REMOTE_BUTTON_MASK_DPAD_RIGHT}, now);
	rift_update_input(&remote->base, RIFT_REMOTE_INPUT_SELECT,
	                  (union xrt_input_value){.boolean = buttons & RIFT_REMOTE_BUTTON_MASK_SELECT}, now);
	rift_update_input(&remote->base, RIFT_REMOTE_INPUT_VOLUME_UP,
	                  (union xrt_input_value){.boolean = buttons & RIFT_REMOTE_BUTTON_MASK_VOLUME_UP}, now);
	rift_update_input(&remote->base, RIFT_REMOTE_INPUT_VOLUME_DOWN,
	                  (union xrt_input_value){.boolean = buttons & RIFT_REMOTE_BUTTON_MASK_VOLUME_DOWN}, now);
	rift_update_input(&remote->base, RIFT_REMOTE_INPUT_BACK,
	                  (union xrt_input_value){.boolean = buttons & RIFT_REMOTE_BUTTON_MASK_BACK}, now);
	rift_update_input(&remote->base, RIFT_REMOTE_INPUT_OCULUS,
	                  (union xrt_input_value){.boolean = buttons & RIFT_REMOTE_BUTTON_MASK_OCULUS}, now);

	return XRT_SUCCESS;
}

/*
 * Implementation functions
 */

static struct rift_remote *
rift_remote_create(struct rift_hmd *hmd)
{
	struct rift_remote *remote =
	    U_DEVICE_ALLOCATE(struct rift_remote, U_DEVICE_ALLOC_TRACKING_NONE, RIFT_REMOTE_INPUT_COUNT, 1);
	if (remote == NULL) {
		HMD_ERROR(hmd, "Failed to allocate remote");
		return NULL;
	}

	strcpy(remote->base.str, "Oculus Rift Remote");
	remote->base.device_type = XRT_DEVICE_TYPE_GAMEPAD;
	remote->base.name = XRT_DEVICE_RIFT_REMOTE;

#define SET_INPUT(NAME)                                                                                                \
	do {                                                                                                           \
		remote->base.inputs[RIFT_REMOTE_INPUT_##NAME].name = XRT_INPUT_RIFT_REMOTE_##NAME##_CLICK;             \
	} while (0)
	SET_INPUT(DPAD_UP);
	SET_INPUT(DPAD_DOWN);
	SET_INPUT(DPAD_LEFT);
	SET_INPUT(DPAD_RIGHT);
	SET_INPUT(SELECT);
	SET_INPUT(VOLUME_UP);
	SET_INPUT(VOLUME_DOWN);
	SET_INPUT(BACK);
	SET_INPUT(OCULUS);
#undef SET_INPUT

	u_device_populate_function_pointers(&remote->base, rift_remote_get_tracked_pose, rift_remote_destroy);
	remote->base.update_inputs = rift_remote_update_inputs;

	remote->base.binding_profiles = remote_profile_bindings;
	remote->base.binding_profile_count = remote_profile_bindings_count;

	u_var_add_root(remote, "Rift Remote", true);
	u_var_add_ro_u32(remote, (uint32_t *)&remote->buttons, "buttons");

	return remote;
}

static struct rift_touch_controller *
rift_touch_controller_create(struct rift_hmd *hmd, enum rift_radio_device_type device_type)
{
	int result;

	struct rift_touch_controller *controller = U_DEVICE_ALLOCATE(
	    struct rift_touch_controller, U_DEVICE_ALLOC_NO_FLAGS, RIFT_TOUCH_CONTROLLER_INPUT_COUNT, 1);
	if (controller == NULL) {
		HMD_ERROR(hmd, "Failed to allocate touch controller");
		return NULL;
	}

	controller->base.tracking_origin = hmd->base.tracking_origin;

	strcpy(controller->base.str, "Oculus Touch (Unknown)");
	switch (device_type) {
	case RIFT_RADIO_DEVICE_LEFT_TOUCH:
		strcpy(controller->base.str, "Oculus Touch (Left)");
		controller->base.device_type = XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER;
		break;
	case RIFT_RADIO_DEVICE_RIGHT_TOUCH:
		strcpy(controller->base.str, "Oculus Touch (Right)");
		controller->base.device_type = XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
		break;
	case RIFT_RADIO_DEVICE_TRACKED_OBJECT:
		strcpy(controller->base.str, "Oculus Touch (Tracked Object)");
		controller->base.device_type = XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER;
		break;
	default: break; return controller;
	}
	controller->base.name = XRT_DEVICE_TOUCH_CONTROLLER_RIFT_CV1;

#define SET_INPUT(NAME, ACTIVE)                                                                                        \
	do {                                                                                                           \
		if (ACTIVE) {                                                                                          \
			controller->base.inputs[RIFT_TOUCH_CONTROLLER_INPUT_##NAME].name = XRT_INPUT_TOUCH_##NAME;     \
			controller->base.inputs[RIFT_TOUCH_CONTROLLER_INPUT_##NAME].active = ACTIVE;                   \
		}                                                                                                      \
	} while (0)
	SET_INPUT(X_CLICK, device_type == RIFT_RADIO_DEVICE_LEFT_TOUCH);
	SET_INPUT(X_TOUCH, device_type == RIFT_RADIO_DEVICE_LEFT_TOUCH);
	SET_INPUT(Y_CLICK, device_type == RIFT_RADIO_DEVICE_LEFT_TOUCH);
	SET_INPUT(Y_TOUCH, device_type == RIFT_RADIO_DEVICE_LEFT_TOUCH);
	SET_INPUT(MENU_CLICK, device_type == RIFT_RADIO_DEVICE_LEFT_TOUCH);

	SET_INPUT(A_CLICK, device_type == RIFT_RADIO_DEVICE_RIGHT_TOUCH);
	SET_INPUT(A_TOUCH, device_type == RIFT_RADIO_DEVICE_RIGHT_TOUCH);
	SET_INPUT(B_CLICK, device_type == RIFT_RADIO_DEVICE_RIGHT_TOUCH);
	SET_INPUT(B_TOUCH, device_type == RIFT_RADIO_DEVICE_RIGHT_TOUCH);
	SET_INPUT(SYSTEM_CLICK, device_type == RIFT_RADIO_DEVICE_RIGHT_TOUCH);

	SET_INPUT(SQUEEZE_VALUE, true);
	SET_INPUT(TRIGGER_TOUCH, true);
	SET_INPUT(TRIGGER_VALUE, true);
	SET_INPUT(THUMBSTICK_CLICK, true);
	SET_INPUT(THUMBSTICK_TOUCH, true);
	SET_INPUT(THUMBSTICK, true);
	SET_INPUT(THUMBREST_TOUCH, true);
	SET_INPUT(GRIP_POSE, true);
	SET_INPUT(AIM_POSE, true);
	SET_INPUT(TRIGGER_PROXIMITY, true);
	SET_INPUT(THUMB_PROXIMITY, true);
#undef SET_INPUT

	controller->base.outputs[0].name = XRT_OUTPUT_NAME_TOUCH_HAPTIC;

	u_device_populate_function_pointers(&controller->base, rift_touch_controller_get_tracked_pose,
	                                    rift_touch_controller_destroy);
	controller->base.update_inputs = rift_touch_controller_update_inputs;

	controller->base.binding_profile_count = touch_profile_bindings_count;
	controller->base.binding_profiles = touch_profile_bindings;

	result = os_mutex_init(&controller->input_mutex);
	if (result < 0) {
		HMD_ERROR(hmd, "Failed to init touch controller input mutex");
		u_device_free(&controller->base);
		return NULL;
	}
	controller->input_mutex_created = true;

	u_var_add_root(controller, "Rift Touch Controller", true);
	u_var_add_u8(controller, &controller->input_state.buttons, "buttons");

	return controller;
}

int
rift_radio_handle_read(struct rift_hmd *hmd)
{
	// int result;
	uint8_t buf[REPORT_MAX_SIZE];
	int length;

	// 3x expected wait time (500hz, so 2ms)
	length = os_hid_read(hmd->radio_dev, buf, sizeof(buf), 6);

	// int64_t receive_ns = os_monotonic_get_ns();

	if (length < 0) {
		HMD_ERROR(hmd, "Got error reading from radio device, assuming fatal, reason %d", length);
		return length;
	}

	// non fatal, but nothing to do
	if (length == 0) {
		HMD_TRACE(hmd, "Timed out waiting for packet from radio, packets should come in at 500hz");
		return 0;
	}

	// do nothing with the keepalive
	if (buf[0] == IN_REPORT_CV1_RADIO_KEEPALIVE) {
		HMD_TRACE(hmd, "Got radio keepalive(?)");
		return 0;
	}

	if (buf[0] != IN_REPORT_RADIO_DATA) {
		HMD_WARN(hmd, "Got radio IN report with bad ID (got %d, expected %d, size %d), ignoring...", buf[0],
		         IN_REPORT_RADIO_DATA, length);
		return 0;
	}

	if (length != IN_REPORT_RADIO_DATA_SIZE) {
		HMD_WARN(hmd, "Got radio IN report with bad size (got %d, expected %d), ignoring...", length,
		         IN_REPORT_RADIO_DATA_SIZE);
		return 0;
	}

	struct rift_radio_report radio_report;
	memcpy(&radio_report, buf + 1, sizeof(radio_report));

	for (uint32_t i = 0; i < ARRAY_SIZE(radio_report.messages); i++) {
		const struct rift_radio_report_message message = radio_report.messages[i];

		if (message.flags != 0x1c && message.flags != 0x05) {
			// HMD_TRACE(hmd, "Got message with unknown radio flags %04x. skipping", message.flags);

			continue;
		}

		switch (message.device_type) {
		case RIFT_RADIO_DEVICE_LEFT_TOUCH:
		case RIFT_RADIO_DEVICE_RIGHT_TOUCH:
		case RIFT_RADIO_DEVICE_TRACKED_OBJECT: {
			size_t touch_index = rift_radio_device_type_to_touch_index(message.device_type);

			struct rift_touch_controller *controller = hmd->radio_state.touch_controllers[touch_index];

			if (controller == NULL) {
				controller = hmd->radio_state.touch_controllers[touch_index] =
				    rift_touch_controller_create(hmd, message.device_type);

				if (controller == NULL) {
					HMD_ERROR(hmd, "Failed to create touch controller for device type %d",
					          message.device_type);
					continue;
				}

				HMD_INFO(hmd, "Created touch controller for device type %d", message.device_type);

				os_mutex_lock(&hmd->device_mutex);
				hmd->devices[hmd->device_count++] = &controller->base;
				os_mutex_unlock(&hmd->device_mutex);
			}

			os_mutex_lock(&controller->input_mutex);
			controller->input_state.buttons = message.touch.buttons & 0x0F;
			os_mutex_unlock(&controller->input_mutex);

			break;
		}
		case RIFT_RADIO_DEVICE_REMOTE: {
			struct rift_remote *remote = hmd->radio_state.remote;

			if (remote == NULL) {
				remote = hmd->radio_state.remote = rift_remote_create(hmd);

				if (remote == NULL) {
					HMD_ERROR(hmd, "Failed to create remote");
					continue;
				}

				HMD_INFO(hmd, "Created remote");

				os_mutex_lock(&hmd->device_mutex);
				hmd->devices[hmd->device_count++] = &remote->base;
				os_mutex_unlock(&hmd->device_mutex);
			}

			xrt_atomic_s32_store(&remote->buttons, message.remote.buttons);

			break;
		}
		}
	}

	return 0;
}

int
rift_radio_handle_command(struct rift_hmd *hmd)
{
	return 0;
}
