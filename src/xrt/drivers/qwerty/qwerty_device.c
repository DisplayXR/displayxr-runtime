// Copyright 2021, Mateo de Mayo.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Implementation of qwerty_device related methods.
 * @author Mateo de Mayo <mateodemayo@gmail.com>
 * @ingroup drv_qwerty
 */

#include "xrt/xrt_device.h"

#include "math/m_api.h"
#include "math/m_space.h"
#include "math/m_mathinclude.h"

#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_var.h"
#include "util/u_logging.h"
#include "util/u_time.h"
#include "util/u_misc.h"
#include "os/os_time.h"

#include "qwerty_device.h"
#include "qwerty_interface.h"

#include <stdio.h>
#include <assert.h>

#define QWERTY_HMD_INITIAL_MOVEMENT_SPEED 0.002f // in meters per frame
#define QWERTY_HMD_INITIAL_LOOK_SPEED 0.02f      // in radians per frame
#define QWERTY_CONTROLLER_INITIAL_MOVEMENT_SPEED 0.005f
#define QWERTY_CONTROLLER_INITIAL_LOOK_SPEED 0.05f
#define MOVEMENT_SPEED_STEP 1.25f // Multiplier for how fast will mov speed increase/decrease
#define SPRINT_STEPS 5            // Amount of MOVEMENT_SPEED_STEPs to increase when sprinting

// clang-format off
// Values copied from u_device_setup_tracking_origins. CONTROLLER relative to HMD.
#define QWERTY_HMD_CAMERA_POS (struct xrt_vec3){0, 1.6f, 0}
#define QWERTY_HMD_DISPLAY_POS (struct xrt_vec3){0, 1.6f, -2.0f}
#define QWERTY_CONTROLLER_INITIAL_POS(is_left) (struct xrt_vec3){(is_left) ? -0.2f : 0.2f, -0.3f, -0.5f}
// clang-format on

static void
reset_controller_for_mode(struct qwerty_system *qs, struct qwerty_controller *qc, bool is_left);

// Indices for fake controller input components
#define QWERTY_TRIGGER 0
#define QWERTY_MENU 1
#define QWERTY_SQUEEZE 2
#define QWERTY_SYSTEM 3
#define QWERTY_THUMBSTICK 4
#define QWERTY_THUMBSTICK_CLICK 5
#define QWERTY_TRACKPAD 6
#define QWERTY_TRACKPAD_TOUCH 7
#define QWERTY_TRACKPAD_CLICK 8
#define QWERTY_GRIP 9
#define QWERTY_AIM 10
#define QWERTY_VIBRATION 0

#define QWERTY_TRACE(qd, ...) U_LOG_XDEV_IFL_T(&qd->base, qd->sys->log_level, __VA_ARGS__)
#define QWERTY_DEBUG(qd, ...) U_LOG_XDEV_IFL_D(&qd->base, qd->sys->log_level, __VA_ARGS__)
#define QWERTY_INFO(qd, ...) U_LOG_XDEV_IFL_I(&qd->base, qd->sys->log_level, __VA_ARGS__)
#define QWERTY_WARN(qd, ...) U_LOG_XDEV_IFL_W(&qd->base, qd->sys->log_level, __VA_ARGS__)
#define QWERTY_ERROR(qd, ...) U_LOG_XDEV_IFL_E(&qd->base, qd->sys->log_level, __VA_ARGS__)

static struct xrt_binding_input_pair touch_inputs[19] = {
    {XRT_INPUT_TOUCH_X_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_TOUCH_X_TOUCH, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_TOUCH_Y_CLICK, XRT_INPUT_WMR_HOME_CLICK},
    {XRT_INPUT_TOUCH_Y_TOUCH, XRT_INPUT_WMR_HOME_CLICK},
    {XRT_INPUT_TOUCH_MENU_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_TOUCH_A_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_TOUCH_A_TOUCH, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_TOUCH_B_CLICK, XRT_INPUT_WMR_HOME_CLICK},
    {XRT_INPUT_TOUCH_B_TOUCH, XRT_INPUT_WMR_HOME_CLICK},
    {XRT_INPUT_TOUCH_SYSTEM_CLICK, XRT_INPUT_WMR_HOME_CLICK},
    {XRT_INPUT_TOUCH_SQUEEZE_VALUE, XRT_INPUT_WMR_SQUEEZE_CLICK},
    {XRT_INPUT_TOUCH_TRIGGER_TOUCH, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_TOUCH_TRIGGER_VALUE, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_TOUCH_THUMBSTICK_CLICK, XRT_INPUT_WMR_THUMBSTICK_CLICK},
    {XRT_INPUT_TOUCH_THUMBSTICK_TOUCH, XRT_INPUT_WMR_THUMBSTICK_CLICK},
    {XRT_INPUT_TOUCH_THUMBSTICK, XRT_INPUT_WMR_THUMBSTICK},
    {XRT_INPUT_TOUCH_THUMBREST_TOUCH, XRT_INPUT_WMR_TRACKPAD_TOUCH},
    {XRT_INPUT_TOUCH_GRIP_POSE, XRT_INPUT_WMR_GRIP_POSE},
    {XRT_INPUT_TOUCH_AIM_POSE, XRT_INPUT_WMR_AIM_POSE},
};

static struct xrt_binding_output_pair touch_outputs[1] = {
    {XRT_OUTPUT_NAME_TOUCH_HAPTIC, XRT_OUTPUT_NAME_WMR_HAPTIC},
};

static struct xrt_binding_input_pair index_inputs[19] = {
    {XRT_INPUT_INDEX_SYSTEM_CLICK, XRT_INPUT_WMR_HOME_CLICK},
    {XRT_INPUT_INDEX_SYSTEM_TOUCH, XRT_INPUT_WMR_HOME_CLICK},
    {XRT_INPUT_INDEX_A_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_INDEX_A_TOUCH, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_INDEX_B_CLICK, XRT_INPUT_WMR_HOME_CLICK},
    {XRT_INPUT_INDEX_B_TOUCH, XRT_INPUT_WMR_HOME_CLICK},
    {XRT_INPUT_INDEX_SQUEEZE_VALUE, XRT_INPUT_WMR_SQUEEZE_CLICK},
    {XRT_INPUT_INDEX_SQUEEZE_FORCE, XRT_INPUT_WMR_SQUEEZE_CLICK},
    {XRT_INPUT_INDEX_TRIGGER_CLICK, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_INDEX_TRIGGER_TOUCH, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_INDEX_TRIGGER_VALUE, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_INDEX_THUMBSTICK_CLICK, XRT_INPUT_WMR_THUMBSTICK_CLICK},
    {XRT_INPUT_INDEX_THUMBSTICK_TOUCH, XRT_INPUT_WMR_THUMBSTICK_CLICK},
    {XRT_INPUT_INDEX_THUMBSTICK, XRT_INPUT_WMR_THUMBSTICK},
    {XRT_INPUT_INDEX_TRACKPAD_FORCE, XRT_INPUT_WMR_TRACKPAD_CLICK},
    {XRT_INPUT_INDEX_TRACKPAD_TOUCH, XRT_INPUT_WMR_TRACKPAD_TOUCH},
    {XRT_INPUT_INDEX_TRACKPAD, XRT_INPUT_WMR_TRACKPAD},
    {XRT_INPUT_INDEX_GRIP_POSE, XRT_INPUT_WMR_GRIP_POSE},
    {XRT_INPUT_INDEX_AIM_POSE, XRT_INPUT_WMR_AIM_POSE},
};

static struct xrt_binding_output_pair index_outputs[1] = {
    {XRT_OUTPUT_NAME_INDEX_HAPTIC, XRT_OUTPUT_NAME_WMR_HAPTIC},
};

static struct xrt_binding_input_pair vive_inputs[10] = {
    {XRT_INPUT_VIVE_SYSTEM_CLICK, XRT_INPUT_WMR_HOME_CLICK},
    {XRT_INPUT_VIVE_SQUEEZE_CLICK, XRT_INPUT_WMR_SQUEEZE_CLICK},
    {XRT_INPUT_VIVE_MENU_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_VIVE_TRIGGER_CLICK, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_VIVE_TRIGGER_VALUE, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_VIVE_TRACKPAD, XRT_INPUT_WMR_TRACKPAD},
    {XRT_INPUT_VIVE_TRACKPAD_CLICK, XRT_INPUT_WMR_TRACKPAD_CLICK},
    {XRT_INPUT_VIVE_TRACKPAD_TOUCH, XRT_INPUT_WMR_TRACKPAD_TOUCH},
    {XRT_INPUT_VIVE_GRIP_POSE, XRT_INPUT_WMR_GRIP_POSE},
    {XRT_INPUT_VIVE_AIM_POSE, XRT_INPUT_WMR_AIM_POSE},
};

static struct xrt_binding_output_pair vive_outputs[1] = {
    {XRT_OUTPUT_NAME_VIVE_HAPTIC, XRT_OUTPUT_NAME_WMR_HAPTIC},
};

static struct xrt_binding_input_pair wmr_inputs[11] = {
    {XRT_INPUT_WMR_MENU_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_WMR_SQUEEZE_CLICK, XRT_INPUT_WMR_SQUEEZE_CLICK},
    {XRT_INPUT_WMR_TRIGGER_VALUE, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_WMR_THUMBSTICK_CLICK, XRT_INPUT_WMR_THUMBSTICK_CLICK},
    {XRT_INPUT_WMR_THUMBSTICK, XRT_INPUT_WMR_THUMBSTICK},
    {XRT_INPUT_WMR_TRACKPAD_CLICK, XRT_INPUT_WMR_TRACKPAD_CLICK},
    {XRT_INPUT_WMR_TRACKPAD_TOUCH, XRT_INPUT_WMR_TRACKPAD_TOUCH},
    {XRT_INPUT_WMR_TRACKPAD, XRT_INPUT_WMR_TRACKPAD},
    {XRT_INPUT_WMR_GRIP_POSE, XRT_INPUT_WMR_GRIP_POSE},
    {XRT_INPUT_WMR_AIM_POSE, XRT_INPUT_WMR_AIM_POSE},
    {XRT_INPUT_WMR_HOME_CLICK, XRT_INPUT_WMR_HOME_CLICK},
};

static struct xrt_binding_output_pair wmr_outputs[1] = {
    {XRT_OUTPUT_NAME_WMR_HAPTIC, XRT_OUTPUT_NAME_WMR_HAPTIC},
};

static struct xrt_binding_input_pair simple_inputs[4] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_WMR_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_WMR_AIM_POSE},
};

static struct xrt_binding_output_pair simple_outputs[1] = {
    {XRT_OUTPUT_NAME_SIMPLE_VIBRATION, XRT_OUTPUT_NAME_WMR_HAPTIC},
};

static struct xrt_binding_profile binding_profiles[5] = {
    {
        .name = XRT_DEVICE_WMR_CONTROLLER,
        .inputs = wmr_inputs,
        .input_count = ARRAY_SIZE(wmr_inputs),
        .outputs = wmr_outputs,
        .output_count = ARRAY_SIZE(wmr_outputs),
    },
    {
        .name = XRT_DEVICE_TOUCH_CONTROLLER,
        .inputs = touch_inputs,
        .input_count = ARRAY_SIZE(touch_inputs),
        .outputs = touch_outputs,
        .output_count = ARRAY_SIZE(touch_outputs),
    },
    {
        .name = XRT_DEVICE_INDEX_CONTROLLER,
        .inputs = index_inputs,
        .input_count = ARRAY_SIZE(index_inputs),
        .outputs = index_outputs,
        .output_count = ARRAY_SIZE(index_outputs),
    },
    {
        .name = XRT_DEVICE_VIVE_WAND,
        .inputs = vive_inputs,
        .input_count = ARRAY_SIZE(vive_inputs),
        .outputs = vive_outputs,
        .output_count = ARRAY_SIZE(vive_outputs),
    },
    {
        .name = XRT_DEVICE_SIMPLE_CONTROLLER,
        .inputs = simple_inputs,
        .input_count = ARRAY_SIZE(simple_inputs),
        .outputs = simple_outputs,
        .output_count = ARRAY_SIZE(simple_outputs),
    },
};

static void
qwerty_system_remove(struct qwerty_system *qs, struct qwerty_device *qd);

static void
qwerty_system_destroy(struct qwerty_system *qs);

static void
qwerty_destroy(struct xrt_device *xd);

// Compare any two pointers without verbose casts
static inline bool
eq(void *a, void *b)
{
	return a == b;
}

// xrt_device functions

struct qwerty_device *
qwerty_device(struct xrt_device *xd)
{
	// Precondition: must be a real qwerty device, not an IPC proxy.
	// Check destroy function pointer before casting — safe for any xrt_device.
	if (xd == NULL || xd->destroy != qwerty_destroy) {
		return NULL;
	}
	struct qwerty_device *qd = (struct qwerty_device *)xd;
	assert(eq(qd, qd->sys->hmd) || eq(qd, qd->sys->lctrl) || eq(qd, qd->sys->rctrl));
	return qd;
}

struct qwerty_hmd *
qwerty_hmd(struct xrt_device *xd)
{
	struct qwerty_hmd *qh = (struct qwerty_hmd *)xd;
	bool is_qwerty_hmd = eq(qh, qh->base.sys->hmd);
	assert(is_qwerty_hmd);
	if (!is_qwerty_hmd) {
		return NULL;
	}
	return qh;
}

struct qwerty_controller *
qwerty_controller(struct xrt_device *xd)
{
	struct qwerty_controller *qc = (struct qwerty_controller *)xd;
	bool is_qwerty_controller = eq(qc, qc->base.sys->lctrl) || eq(qc, qc->base.sys->rctrl);
	assert(is_qwerty_controller);
	if (!is_qwerty_controller) {
		return NULL;
	}
	return qc;
}

static xrt_result_t
qwerty_update_inputs(struct xrt_device *xd)
{
	assert(xd->name == XRT_DEVICE_WMR_CONTROLLER);

	struct qwerty_controller *qc = qwerty_controller(xd);
	struct qwerty_device *qd = &qc->base;

	// clang-format off
	QWERTY_TRACE(qd, "trigger: %f, menu: %u, squeeze: %u, system %u, thumbstick: %u %f %f, trackpad: %u %f %f",
	             1.0f * qc->trigger_clicked, qc->menu_clicked, qc->squeeze_clicked, qc->system_clicked,
	             qc->thumbstick_clicked, 1.0f * (qc->thumbstick_right_pressed - qc->thumbstick_left_pressed),
	             1.0f * (qc->thumbstick_up_pressed - qc->thumbstick_down_pressed),
	             qc->trackpad_clicked, 1.0f * (qc->trackpad_right_pressed - qc->trackpad_left_pressed),
	             1.0f * (qc->trackpad_up_pressed - qc->trackpad_down_pressed));
	// clang-format on

	xd->inputs[QWERTY_TRIGGER].value.vec1.x = 1.0f * qc->trigger_clicked;
	xd->inputs[QWERTY_TRIGGER].timestamp = qc->trigger_timestamp;
	xd->inputs[QWERTY_MENU].value.boolean = qc->menu_clicked;
	xd->inputs[QWERTY_MENU].timestamp = qc->menu_timestamp;
	xd->inputs[QWERTY_SQUEEZE].value.boolean = qc->squeeze_clicked;
	xd->inputs[QWERTY_SQUEEZE].timestamp = qc->squeeze_timestamp;
	xd->inputs[QWERTY_SYSTEM].value.boolean = qc->system_clicked;
	xd->inputs[QWERTY_SYSTEM].timestamp = qc->system_timestamp;

	xd->inputs[QWERTY_THUMBSTICK].value.vec2.x =
	    1.0f * (qc->thumbstick_right_pressed - qc->thumbstick_left_pressed);
	xd->inputs[QWERTY_THUMBSTICK].value.vec2.y = 1.0f * (qc->thumbstick_up_pressed - qc->thumbstick_down_pressed);
	xd->inputs[QWERTY_THUMBSTICK].timestamp = qc->thumbstick_timestamp;
	xd->inputs[QWERTY_THUMBSTICK_CLICK].value.boolean = qc->thumbstick_clicked;
	xd->inputs[QWERTY_THUMBSTICK_CLICK].timestamp = qc->thumbstick_click_timestamp;

	xd->inputs[QWERTY_TRACKPAD].value.vec2.x = 1.0f * (qc->trackpad_right_pressed - qc->trackpad_left_pressed);
	xd->inputs[QWERTY_TRACKPAD].value.vec2.y = 1.0f * (qc->trackpad_up_pressed - qc->trackpad_down_pressed);
	xd->inputs[QWERTY_TRACKPAD].timestamp = qc->trackpad_timestamp;
	xd->inputs[QWERTY_TRACKPAD_TOUCH].value.boolean = qc->trackpad_right_pressed || qc->trackpad_left_pressed ||
	                                                  qc->trackpad_up_pressed || qc->trackpad_down_pressed ||
	                                                  qc->trackpad_clicked;
	xd->inputs[QWERTY_TRACKPAD_TOUCH].timestamp = MAX(qc->trackpad_timestamp, qc->trackpad_click_timestamp);
	xd->inputs[QWERTY_TRACKPAD_CLICK].value.boolean = qc->trackpad_clicked;
	xd->inputs[QWERTY_TRACKPAD_CLICK].timestamp = qc->trackpad_click_timestamp;

	return XRT_SUCCESS;
}

static xrt_result_t
qwerty_set_output(struct xrt_device *xd, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct qwerty_device *qd = qwerty_device(xd);
	float frequency = value->vibration.frequency;
	float amplitude = value->vibration.amplitude;
	time_duration_ns duration = value->vibration.duration_ns;
	if (amplitude || duration || frequency) {
		QWERTY_INFO(qd,
		            "[%s] Haptic output: \n"
		            "\tfrequency=%.2f amplitude=%.2f duration=%" PRId64,
		            xd->str, frequency, amplitude, duration);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
qwerty_get_tracked_pose(struct xrt_device *xd,
                        enum xrt_input_name name,
                        int64_t at_timestamp_ns,
                        struct xrt_space_relation *out_relation)
{
	struct qwerty_device *qd = qwerty_device(xd);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE && name != XRT_INPUT_WMR_GRIP_POSE && name != XRT_INPUT_WMR_AIM_POSE) {
		U_LOG_XDEV_UNSUPPORTED_INPUT(&qd->base, qd->sys->log_level, name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	// Position

	float sprint_boost = qd->sprint_pressed ? powf(MOVEMENT_SPEED_STEP, SPRINT_STEPS) : 1;
	float mov_speed = qd->movement_speed * sprint_boost;
	struct xrt_vec3 pos_delta = {
	    mov_speed * (qd->right_pressed - qd->left_pressed),
	    0, // Up/down movement will be relative to base space
	    mov_speed * (qd->backward_pressed - qd->forward_pressed),
	};
	math_quat_rotate_vec3(&qd->pose.orientation, &pos_delta, &pos_delta);
	pos_delta.y += mov_speed * (qd->up_pressed - qd->down_pressed);

	math_vec3_accum(&pos_delta, &qd->pose.position);

	// Mouse-driven position delta (world space XY, not rotated by device orientation)
	qd->pose.position.x += qd->x_pos_delta;
	qd->pose.position.y += qd->y_pos_delta;
	qd->x_pos_delta = 0;
	qd->y_pos_delta = 0;

	// Orientation

	// View rotation caused by keys
	float y_look_speed = qd->look_speed * (qd->look_left_pressed - qd->look_right_pressed);
	float x_look_speed = qd->look_speed * (qd->look_up_pressed - qd->look_down_pressed);

	// View rotation caused by mouse
	y_look_speed += qd->yaw_delta;
	x_look_speed += qd->pitch_delta;
	qd->yaw_delta = 0;
	qd->pitch_delta = 0;

	struct xrt_quat x_rotation;
	struct xrt_quat y_rotation;
	const struct xrt_vec3 x_axis = XRT_VEC3_UNIT_X;
	const struct xrt_vec3 y_axis = XRT_VEC3_UNIT_Y;
	math_quat_from_angle_vector(x_look_speed, &x_axis, &x_rotation);
	math_quat_from_angle_vector(y_look_speed, &y_axis, &y_rotation);
	math_quat_rotate(&qd->pose.orientation, &x_rotation, &qd->pose.orientation); // local-space pitch
	math_quat_rotate(&y_rotation, &qd->pose.orientation, &qd->pose.orientation); // base-space yaw
	math_quat_normalize(&qd->pose.orientation);

	// HMD Parenting

	bool qd_is_ctrl = name == XRT_INPUT_WMR_GRIP_POSE || name == XRT_INPUT_WMR_AIM_POSE;
	struct qwerty_controller *qc = qd_is_ctrl ? qwerty_controller(&qd->base) : NULL;
	if (qd_is_ctrl && qc->follow_hmd) {
		struct xrt_relation_chain relation_chain = {0};
		struct qwerty_device *qd_hmd = &qd->sys->hmd->base;
		m_relation_chain_push_pose(&relation_chain, &qd->pose);     // controller pose
		m_relation_chain_push_pose(&relation_chain, &qd_hmd->pose); // base space is hmd space
		m_relation_chain_resolve(&relation_chain, out_relation);
	} else {
		out_relation->pose = qd->pose;
	}
	out_relation->relation_flags =
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT;

	return XRT_SUCCESS;
}

static void
qwerty_destroy(struct xrt_device *xd)
{
	// Note: do not destroy a single device of a qwerty system or its var tracking
	// ui will make a null reference
	struct qwerty_device *qd = qwerty_device(xd);
	qwerty_system_remove(qd->sys, qd);
	u_device_free(xd);
}

struct qwerty_hmd *
qwerty_hmd_create(void)
{
	enum u_device_alloc_flags flags = U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE;
	size_t input_count = 1;
	size_t output_count = 0;
	struct qwerty_hmd *qh = U_DEVICE_ALLOCATE(struct qwerty_hmd, flags, input_count, output_count);
	assert(qh);

	struct qwerty_device *qd = &qh->base;
	qd->pose.orientation.w = 1.f;
	qd->pose.position = QWERTY_HMD_CAMERA_POS; // Default is camera-centric mode
	qd->movement_speed = QWERTY_HMD_INITIAL_MOVEMENT_SPEED;
	qd->look_speed = QWERTY_HMD_INITIAL_LOOK_SPEED;

	struct xrt_device *xd = &qd->base;
	xd->name = XRT_DEVICE_GENERIC_HMD;
	xd->device_type = XRT_DEVICE_TYPE_HMD;

	snprintf(xd->str, XRT_DEVICE_NAME_LEN, QWERTY_HMD_STR);
	snprintf(xd->serial, XRT_DEVICE_NAME_LEN, QWERTY_HMD_STR);

	// Fill in xd->hmd
	struct u_device_simple_info info;
	info.display.w_pixels = 1280;
	info.display.h_pixels = 720;
	info.display.w_meters = 0.13f;
	info.display.h_meters = 0.07f;
	info.lens_horizontal_separation_meters = 0.13f / 2.0f;
	info.lens_vertical_position_meters = 0.07f / 2.0f;
	info.fov[0] = 85.0f * (M_PI / 180.0f);
	info.fov[1] = 85.0f * (M_PI / 180.0f);

	if (!u_device_setup_split_side_by_side(xd, &info)) {
		QWERTY_ERROR(qd, "Failed to setup HMD properties");
		qwerty_destroy(xd);
		assert(false);
		return NULL;
	}

	xd->tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
	snprintf(xd->tracking_origin->name, XRT_TRACKING_NAME_LEN, QWERTY_HMD_TRACKER_STR);

	// Report orientation and position tracking as supported.
	// Required for Chrome WebXR to expose controllers as positionally-tracked
	// (without this, xrGetSystemProperties reports orientationTracking=0
	// positionTracking=0, causing some WebXR sites to hide controllers).
	xd->supported.orientation_tracking = true;
	xd->supported.position_tracking = true;

	xd->inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

	xd->update_inputs = u_device_noop_update_inputs;
	xd->get_tracked_pose = qwerty_get_tracked_pose;
	xd->get_view_poses = u_device_get_view_poses;
	xd->destroy = qwerty_destroy;
	u_distortion_mesh_set_none(xd); // Fill in xd->compute_distortion()

	return qh;
}

struct qwerty_controller *
qwerty_controller_create(bool is_left, struct qwerty_hmd *qhmd)
{
	struct qwerty_controller *qc = U_DEVICE_ALLOCATE(struct qwerty_controller, U_DEVICE_ALLOC_TRACKING_NONE, 11, 1);
	assert(qc);
	qc->follow_hmd = qhmd != NULL;

	struct qwerty_device *qd = &qc->base;
	qd->pose.orientation.w = 1.f;
	qd->pose.position = QWERTY_CONTROLLER_INITIAL_POS(is_left);
	qd->movement_speed = QWERTY_CONTROLLER_INITIAL_MOVEMENT_SPEED;
	qd->look_speed = QWERTY_CONTROLLER_INITIAL_LOOK_SPEED;

	struct xrt_device *xd = &qd->base;

	xd->name = XRT_DEVICE_WMR_CONTROLLER;
	xd->device_type = is_left ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;

	char *controller_name = is_left ? QWERTY_LEFT_STR : QWERTY_RIGHT_STR;
	snprintf(xd->str, XRT_DEVICE_NAME_LEN, "%s", controller_name);
	snprintf(xd->serial, XRT_DEVICE_NAME_LEN, "%s", controller_name);

	// Share the HMD's tracking origin so all qwerty devices occupy the same
	// node in the space overseer graph. With separate origins the IPC client
	// may treat cross-origin xrLocateSpace as invalid, zeroing controller poses.
	if (qhmd != NULL) {
		xd->tracking_origin = qhmd->base.base.tracking_origin;
	} else {
		xd->tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
		char *tracker_name = is_left ? QWERTY_LEFT_TRACKER_STR : QWERTY_RIGHT_TRACKER_STR;
		snprintf(xd->tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", tracker_name);
	}

	xd->inputs[QWERTY_TRIGGER].name = XRT_INPUT_WMR_TRIGGER_VALUE;
	xd->inputs[QWERTY_MENU].name = XRT_INPUT_WMR_MENU_CLICK;
	xd->inputs[QWERTY_SQUEEZE].name = XRT_INPUT_WMR_SQUEEZE_CLICK;
	xd->inputs[QWERTY_SYSTEM].name = XRT_INPUT_WMR_HOME_CLICK;
	xd->inputs[QWERTY_THUMBSTICK].name = XRT_INPUT_WMR_THUMBSTICK;
	xd->inputs[QWERTY_THUMBSTICK_CLICK].name = XRT_INPUT_WMR_THUMBSTICK_CLICK;
	xd->inputs[QWERTY_TRACKPAD].name = XRT_INPUT_WMR_TRACKPAD;
	xd->inputs[QWERTY_TRACKPAD_TOUCH].name = XRT_INPUT_WMR_TRACKPAD_TOUCH;
	xd->inputs[QWERTY_TRACKPAD_CLICK].name = XRT_INPUT_WMR_TRACKPAD_CLICK;
	xd->inputs[QWERTY_GRIP].name = XRT_INPUT_WMR_GRIP_POSE;
	//!< @todo: aim input offset not implemented, equal to grip pose
	xd->inputs[QWERTY_AIM].name = XRT_INPUT_WMR_AIM_POSE;
	xd->outputs[QWERTY_VIBRATION].name = XRT_OUTPUT_NAME_WMR_HAPTIC;

	xd->binding_profiles = binding_profiles;
	xd->binding_profile_count = ARRAY_SIZE(binding_profiles);

	xd->update_inputs = qwerty_update_inputs;
	xd->get_tracked_pose = qwerty_get_tracked_pose;
	xd->set_output = qwerty_set_output;
	xd->destroy = qwerty_destroy;

	return qc;
}

// System methods

static void
qwerty_setup_var_tracking(struct qwerty_system *qs)
{
	struct qwerty_device *qd_hmd = qs->hmd ? &qs->hmd->base : NULL;
	struct qwerty_device *qd_left = &qs->lctrl->base;
	struct qwerty_device *qd_right = &qs->rctrl->base;

	u_var_add_root(qs, "Qwerty System", true);
	u_var_add_log_level(qs, &qs->log_level, "Log level");
	u_var_add_bool(qs, &qs->process_keys, "process_keys");
	u_var_add_bool(qs, &qs->force_2d_mode, "force_2d_mode");

	u_var_add_ro_text(qs, "", "Focused Device");
	if (qd_hmd) {
		u_var_add_bool(qs, &qs->hmd_focused, "HMD Focused");
	}
	u_var_add_bool(qs, &qs->lctrl_focused, "Left Controller Focused");
	u_var_add_bool(qs, &qs->rctrl_focused, "Right Controller Focused");

	if (qd_hmd) {
		u_var_add_gui_header(qs, NULL, qd_hmd->base.str);
		u_var_add_pose(qs, &qd_hmd->pose, "hmd.pose");
		u_var_add_f32(qs, &qd_hmd->movement_speed, "hmd.movement_speed");
		u_var_add_f32(qs, &qd_hmd->look_speed, "hmd.look_speed");
	}

	u_var_add_gui_header(qs, NULL, qd_left->base.str);
	u_var_add_pose(qs, &qd_left->pose, "left.pose");
	u_var_add_f32(qs, &qd_left->movement_speed, "left.movement_speed");
	u_var_add_f32(qs, &qd_left->look_speed, "left.look_speed");

	u_var_add_gui_header(qs, NULL, qd_right->base.str);
	u_var_add_pose(qs, &qd_right->pose, "right.pose");
	u_var_add_f32(qs, &qd_right->movement_speed, "right.movement_speed");
	u_var_add_f32(qs, &qd_right->look_speed, "right.look_speed");

	u_var_add_gui_header(qs, NULL, "Help");
	u_var_add_ro_text(qs, "FD: focused device. FC: focused controller.", "Notation");
	u_var_add_ro_text(qs, "HMD is FD by default. Right is FC by default", "Defaults");
	u_var_add_ro_text(qs, "Hold left/right controller focus", "CTRL/ALT");
	u_var_add_ro_text(qs, "Move FD", "WASDQE");
	u_var_add_ro_text(qs, "Rotate FD", "Arrow keys");
	u_var_add_ro_text(qs, "Rotate FD", "Hold right click");
	u_var_add_ro_text(qs, "Hold for sprint", "LSHIFT");
	u_var_add_ro_text(qs, "HMD: Wh=Conv/vH  Sh+Wh=IPD+Prlx", "Mouse wheel");
	u_var_add_ro_text(qs, "Toggle camera/display 3D mode", "P");
	u_var_add_ro_text(qs, "Reset 3D to camera defaults", "Space");
	u_var_add_ro_text(qs, "Modify FD movement speed", "Numpad +/-");
	u_var_add_ro_text(qs, "Reset both or FC pose", "R");
	u_var_add_ro_text(qs, "Toggle both or FC parenting to HMD", "C");
	u_var_add_ro_text(qs, "FC Trigger click", "Left Click");
	u_var_add_ro_text(qs, "FC Squeeze click", "Middle Click");
	u_var_add_ro_text(qs, "FC Menu click", "N");
	u_var_add_ro_text(qs, "FC System click", "B");
	u_var_add_ro_text(qs, "HMD: 2D/3D toggle, FC: Joystick click", "V");
	u_var_add_ro_text(qs, "FC Joystick+Trackpad direction", "TFGH");
	u_var_add_ro_text(qs, "FC Joystick+Trackpad click", "V(ctrl)");
}

struct qwerty_system *
qwerty_system_create(struct qwerty_hmd *qhmd,
                     struct qwerty_controller *qleft,
                     struct qwerty_controller *qright,
                     enum u_logging_level log_level)
{
	assert(qleft && "Cannot create a qwerty system when Left controller is NULL");
	assert(qright && "Cannot create a qwerty system when Right controller is NULL");

	struct qwerty_system *qs = U_TYPED_CALLOC(struct qwerty_system);
	qs->hmd = qhmd;
	qs->lctrl = qleft;
	qs->rctrl = qright;
	qs->log_level = log_level;
	qs->process_keys = true;

	// Default rendering mode matches sim_display default (mode 1 = first 3D mode)
	qs->rendering_mode = 1;

	// View defaults
	qs->camera_mode = true;

	// Camera-centric defaults
	qs->cam_spread_factor = 1.0f;
	qs->cam_parallax_factor = 1.0f;
	qs->cam_convergence = 0.5f;      // 0.5 diopters = 2m convergence
	qs->cam_half_tan_vfov = 0.3249f; // tan(18 deg) -> 36 deg vFOV

	// Display-centric defaults
	qs->disp_spread_factor = 1.0f;
	qs->disp_parallax_factor = 1.0f;
	qs->disp_vHeight = 1.3f; // 1.3m

	// Hardware config defaults (overridden by target builder)
	qs->nominal_viewer_z = 0.6f;  // meters
	qs->screen_height_m = 0.194f; // meters

	if (qhmd) {
		qhmd->base.sys = qs;
	}
	qleft->base.sys = qs;
	qright->base.sys = qs;

	qwerty_setup_var_tracking(qs);

	return qs;
}

static void
qwerty_system_remove(struct qwerty_system *qs, struct qwerty_device *qd)
{
	if (eq(qd, qs->hmd)) {
		qs->hmd = NULL;
	} else if (eq(qd, qs->lctrl)) {
		qs->lctrl = NULL;
	} else if (eq(qd, qs->rctrl)) {
		qs->rctrl = NULL;
	} else {
		assert(false && "Trying to remove a device that is not in the qwerty system");
	}

	bool all_devices_clean = !qs->hmd && !qs->lctrl && !qs->rctrl;
	if (all_devices_clean) {
		qwerty_system_destroy(qs);
	}
}

static void
qwerty_system_destroy(struct qwerty_system *qs)
{
	bool all_devices_clean = !qs->hmd && !qs->lctrl && !qs->rctrl;
	assert(all_devices_clean && "Tried to destroy a qwerty_system without destroying its devices before.");
	if (!all_devices_clean) {
		return;
	}
	u_var_remove_root(qs);
	free(qs);
}

// Device methods

// clang-format off
void qwerty_press_left(struct qwerty_device *qd) { qd->left_pressed = true; }
void qwerty_release_left(struct qwerty_device *qd) { qd->left_pressed = false; }
void qwerty_press_right(struct qwerty_device *qd) { qd->right_pressed = true; }
void qwerty_release_right(struct qwerty_device *qd) { qd->right_pressed = false; }
void qwerty_press_forward(struct qwerty_device *qd) { qd->forward_pressed = true; }
void qwerty_release_forward(struct qwerty_device *qd) { qd->forward_pressed = false; }
void qwerty_press_backward(struct qwerty_device *qd) { qd->backward_pressed = true; }
void qwerty_release_backward(struct qwerty_device *qd) { qd->backward_pressed = false; }
void qwerty_press_up(struct qwerty_device *qd) { qd->up_pressed = true; }
void qwerty_release_up(struct qwerty_device *qd) { qd->up_pressed = false; }
void qwerty_press_down(struct qwerty_device *qd) { qd->down_pressed = true; }
void qwerty_release_down(struct qwerty_device *qd) { qd->down_pressed = false; }

void qwerty_press_look_left(struct qwerty_device *qd) { qd->look_left_pressed = true; }
void qwerty_release_look_left(struct qwerty_device *qd) { qd->look_left_pressed = false; }
void qwerty_press_look_right(struct qwerty_device *qd) { qd->look_right_pressed = true; }
void qwerty_release_look_right(struct qwerty_device *qd) { qd->look_right_pressed = false; }
void qwerty_press_look_up(struct qwerty_device *qd) { qd->look_up_pressed = true; }
void qwerty_release_look_up(struct qwerty_device *qd) { qd->look_up_pressed = false; }
void qwerty_press_look_down(struct qwerty_device *qd) { qd->look_down_pressed = true; }
void qwerty_release_look_down(struct qwerty_device *qd) { qd->look_down_pressed = false; }
// clang-format on

void
qwerty_press_sprint(struct qwerty_device *qd)
{
	qd->sprint_pressed = true;
}
void
qwerty_release_sprint(struct qwerty_device *qd)
{
	qd->sprint_pressed = false;
}

void
qwerty_add_look_delta(struct qwerty_device *qd, float yaw, float pitch)
{
	qd->yaw_delta += yaw * qd->look_speed;
	qd->pitch_delta += pitch * qd->look_speed;

	// Log device pointer and accumulated deltas (first call only)
	static bool first_log = true;
	if (first_log) {
		first_log = false;
		U_LOG_I("qwerty_add_look_delta: qd=%p yaw_delta=%.4f pitch_delta=%.4f",
		        (void *)qd, qd->yaw_delta, qd->pitch_delta);
	}
}

void
qwerty_add_position_delta(struct qwerty_device *qd, float dx, float dy)
{
	qd->x_pos_delta += dx * qd->movement_speed;
	qd->y_pos_delta += dy * qd->movement_speed;
}

void
qwerty_change_movement_speed(struct qwerty_device *qd, float steps)
{
	qd->movement_speed *= powf(MOVEMENT_SPEED_STEP, steps);
}

void
qwerty_release_all(struct qwerty_device *qd)
{
	qd->left_pressed = false;
	qd->right_pressed = false;
	qd->forward_pressed = false;
	qd->backward_pressed = false;
	qd->up_pressed = false;
	qd->down_pressed = false;
	qd->look_left_pressed = false;
	qd->look_right_pressed = false;
	qd->look_up_pressed = false;
	qd->look_down_pressed = false;
	qd->sprint_pressed = false;
	qd->yaw_delta = 0;
	qd->pitch_delta = 0;
	qd->x_pos_delta = 0;
	qd->y_pos_delta = 0;
}

// Controller methods

void
qwerty_press_trigger(struct qwerty_controller *qc)
{
	qc->trigger_clicked = true;
	qc->trigger_timestamp = os_monotonic_get_ns();
}

void
qwerty_release_trigger(struct qwerty_controller *qc)
{
	qc->trigger_clicked = false;
	qc->trigger_timestamp = os_monotonic_get_ns();
}

void
qwerty_press_menu(struct qwerty_controller *qc)
{
	qc->menu_clicked = true;
	qc->menu_timestamp = os_monotonic_get_ns();
}

void
qwerty_release_menu(struct qwerty_controller *qc)
{
	qc->menu_clicked = false;
	qc->menu_timestamp = os_monotonic_get_ns();
}

void
qwerty_press_squeeze(struct qwerty_controller *qc)
{
	qc->squeeze_clicked = true;
	qc->squeeze_timestamp = os_monotonic_get_ns();
}

void
qwerty_release_squeeze(struct qwerty_controller *qc)
{
	qc->squeeze_clicked = false;
	qc->squeeze_timestamp = os_monotonic_get_ns();
}

void
qwerty_press_system(struct qwerty_controller *qc)
{
	qc->system_clicked = true;
	qc->system_timestamp = os_monotonic_get_ns();
}

void
qwerty_release_system(struct qwerty_controller *qc)
{
	qc->system_clicked = false;
	qc->system_timestamp = os_monotonic_get_ns();
}

// clang-format off
void qwerty_press_thumbstick_left(struct qwerty_controller *qc) { qc->thumbstick_left_pressed = true;
                                                                  qc->thumbstick_timestamp = os_monotonic_get_ns(); }
void qwerty_release_thumbstick_left(struct qwerty_controller *qc) { qc->thumbstick_left_pressed = false;
                                                                    qc->thumbstick_timestamp = os_monotonic_get_ns(); }
void qwerty_press_thumbstick_right(struct qwerty_controller *qc) { qc->thumbstick_right_pressed = true;
                                                                   qc->thumbstick_timestamp = os_monotonic_get_ns(); }
void qwerty_release_thumbstick_right(struct qwerty_controller *qc) { qc->thumbstick_right_pressed = false;
                                                                     qc->thumbstick_timestamp = os_monotonic_get_ns(); }
void qwerty_press_thumbstick_up(struct qwerty_controller *qc) { qc->thumbstick_up_pressed = true;
                                                                qc->thumbstick_timestamp = os_monotonic_get_ns(); }
void qwerty_release_thumbstick_up(struct qwerty_controller *qc) { qc->thumbstick_up_pressed = false;
                                                                  qc->thumbstick_timestamp = os_monotonic_get_ns(); }
void qwerty_press_thumbstick_down(struct qwerty_controller *qc) { qc->thumbstick_down_pressed = true;
                                                                  qc->thumbstick_timestamp = os_monotonic_get_ns(); }
void qwerty_release_thumbstick_down(struct qwerty_controller *qc) { qc->thumbstick_down_pressed = false;
                                                                    qc->thumbstick_timestamp = os_monotonic_get_ns(); }
void qwerty_press_thumbstick_click(struct qwerty_controller *qc) { qc->thumbstick_clicked = true;
                                                                   qc->thumbstick_click_timestamp = os_monotonic_get_ns(); }
void qwerty_release_thumbstick_click(struct qwerty_controller *qc) { qc->thumbstick_clicked = false;
                                                                     qc->thumbstick_click_timestamp = os_monotonic_get_ns(); }

void qwerty_press_trackpad_left(struct qwerty_controller *qc) { qc->trackpad_left_pressed = true;
                                                                qc->trackpad_timestamp = os_monotonic_get_ns(); }
void qwerty_release_trackpad_left(struct qwerty_controller *qc) { qc->trackpad_left_pressed = false;
                                                                  qc->trackpad_timestamp = os_monotonic_get_ns(); }
void qwerty_press_trackpad_right(struct qwerty_controller *qc) { qc->trackpad_right_pressed = true;
                                                                 qc->trackpad_timestamp = os_monotonic_get_ns(); }
void qwerty_release_trackpad_right(struct qwerty_controller *qc) { qc->trackpad_right_pressed = false;
                                                                   qc->trackpad_timestamp = os_monotonic_get_ns(); }
void qwerty_press_trackpad_up(struct qwerty_controller *qc) { qc->trackpad_up_pressed = true;
                                                              qc->trackpad_timestamp = os_monotonic_get_ns(); }
void qwerty_release_trackpad_up(struct qwerty_controller *qc) { qc->trackpad_up_pressed = false;
                                                                qc->trackpad_timestamp = os_monotonic_get_ns(); }
void qwerty_press_trackpad_down(struct qwerty_controller *qc) { qc->trackpad_down_pressed = true;
                                                                qc->trackpad_timestamp = os_monotonic_get_ns(); }
void qwerty_release_trackpad_down(struct qwerty_controller *qc) { qc->trackpad_down_pressed = false;
                                                                  qc->trackpad_timestamp = os_monotonic_get_ns(); }
void qwerty_press_trackpad_click(struct qwerty_controller *qc) { qc->trackpad_clicked = true;
                                                                 qc->trackpad_click_timestamp = os_monotonic_get_ns(); }
void qwerty_release_trackpad_click(struct qwerty_controller *qc) { qc->trackpad_clicked = false;
                                                                   qc->trackpad_click_timestamp = os_monotonic_get_ns(); }
// clang-format on

void
qwerty_follow_hmd(struct qwerty_controller *qc, bool follow)
{
	struct qwerty_device *qd = &qc->base;
	bool no_qhmd = !qd->sys->hmd;
	bool unchanged = qc->follow_hmd == follow;
	if (no_qhmd || unchanged) {
		return;
	}

	struct qwerty_device *qd_hmd = &qd->sys->hmd->base;
	struct xrt_relation_chain chain = {0};
	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;

	m_relation_chain_push_pose(&chain, &qd->pose);
	if (follow) { // From global to hmd
		m_relation_chain_push_inverted_pose_if_not_identity(&chain, &qd_hmd->pose);
	} else { // From hmd to global
		m_relation_chain_push_pose(&chain, &qd_hmd->pose);
	}
	m_relation_chain_resolve(&chain, &rel);

	qd->pose = rel.pose;
	qc->follow_hmd = follow;
}

bool
qwerty_check_display_mode_toggle(struct xrt_device **xdevs, size_t xdev_count, bool *out_force_2d)
{
	// Find a qwerty device in the device list
	struct qwerty_system *qs = NULL;
	for (size_t i = 0; i < xdev_count; i++) {
		if (xdevs[i] == NULL || xdevs[i]->destroy != qwerty_destroy) {
			continue;
		}
		const char *name = xdevs[i]->tracking_origin->name;
		if (strcmp(name, QWERTY_HMD_TRACKER_STR) == 0 || strcmp(name, QWERTY_LEFT_TRACKER_STR) == 0 ||
		    strcmp(name, QWERTY_RIGHT_TRACKER_STR) == 0) {
			qs = qwerty_device(xdevs[i])->sys;
			break;
		}
	}

	if (qs == NULL) {
		*out_force_2d = false;
		return false;
	}

	*out_force_2d = qs->force_2d_mode;

	if (qs->display_mode_toggle_pending) {
		qs->display_mode_toggle_pending = false;
		return true;
	}

	return false;
}

bool
qwerty_get_hmd_pose(struct xrt_device **xdevs, size_t xdev_count, struct xrt_pose *out_pose)
{
	if (xdevs == NULL || out_pose == NULL) {
		return false;
	}

	for (size_t i = 0; i < xdev_count; i++) {
		if (xdevs[i] == NULL) {
			continue;
		}
		const char *name = xdevs[i]->tracking_origin->name;
		if (strcmp(name, QWERTY_HMD_TRACKER_STR) == 0) {
			struct qwerty_device *qd = qwerty_device(xdevs[i]);
			*out_pose = qd->pose;
			return true;
		}
	}

	return false;
}

// Clamp helper
static inline float
clampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

void
qwerty_toggle_display_mode(struct qwerty_system *qs)
{
	qs->force_2d_mode = !qs->force_2d_mode;
	qs->display_mode_toggle_pending = true;
	U_LOG_W("Qwerty: display mode toggled to %s", qs->force_2d_mode ? "2D" : "3D");
}

void
qwerty_set_rendering_mode(struct qwerty_system *qs, int mode)
{
	qs->rendering_mode = mode;
	qs->rendering_mode_change_pending = true;
	U_LOG_W("Qwerty: rendering mode set to %d", mode);
}

bool
qwerty_check_rendering_mode_change(struct xrt_device **xdevs, size_t xdev_count, int *out_mode)
{
	struct qwerty_system *qs = NULL;
	for (size_t i = 0; i < xdev_count; i++) {
		if (xdevs[i] == NULL || xdevs[i]->destroy != qwerty_destroy) {
			continue;
		}
		const char *name = xdevs[i]->tracking_origin->name;
		if (strcmp(name, QWERTY_HMD_TRACKER_STR) == 0 || strcmp(name, QWERTY_LEFT_TRACKER_STR) == 0 ||
		    strcmp(name, QWERTY_RIGHT_TRACKER_STR) == 0) {
			qs = qwerty_device(xdevs[i])->sys;
			break;
		}
	}

	if (qs == NULL) {
		return false;
	}

	if (qs->rendering_mode_change_pending) {
		qs->rendering_mode_change_pending = false;
		*out_mode = qs->rendering_mode;
		return true;
	}

	return false;
}

void
qwerty_set_rendering_mode_silent(struct xrt_device **xdevs, size_t xdev_count, int mode)
{
	struct qwerty_system *qs = NULL;
	for (size_t i = 0; i < xdev_count; i++) {
		if (xdevs[i] == NULL || xdevs[i]->destroy != qwerty_destroy) {
			continue;
		}
		const char *name = xdevs[i]->tracking_origin->name;
		if (strcmp(name, QWERTY_HMD_TRACKER_STR) == 0 || strcmp(name, QWERTY_LEFT_TRACKER_STR) == 0 ||
		    strcmp(name, QWERTY_RIGHT_TRACKER_STR) == 0) {
			qs = qwerty_device(xdevs[i])->sys;
			break;
		}
	}
	if (qs != NULL) {
		qs->rendering_mode = mode;
	}
}

void
qwerty_toggle_camera_mode(struct qwerty_system *qs)
{
	if (qs->hmd == NULL) {
		return;
	}

	struct xrt_pose *pose = &qs->hmd->base.pose;

	// Compute forward direction from current orientation
	struct xrt_vec3 fwd_in = {0, 0, -1};
	struct xrt_vec3 fwd;
	math_quat_rotate_vec3(&pose->orientation, &fwd_in, &fwd);

	if (qs->camera_mode) {
		// Camera -> Display: derive display state from camera state
		float conv_dist = (qs->cam_convergence > 0.001f) ? (1.0f / qs->cam_convergence) : 1000.0f;

		// Display position = camera position + forward * convergence_distance
		struct xrt_vec3 disp_pos = {
		    pose->position.x + fwd.x * conv_dist,
		    pose->position.y + fwd.y * conv_dist,
		    pose->position.z + fwd.z * conv_dist,
		};

		// Derive vHeight from camera FOV and convergence distance
		qs->disp_vHeight = clampf(2.0f * qs->cam_half_tan_vfov * conv_dist, 0.1f, 10.0f);

		// IPD/parallax: keep current display values (independent per mode)

		// Move HMD to display position
		pose->position = disp_pos;
	} else {
		// Display -> Camera: reset to camera defaults
		float cam_distance = 2.0f; // 1 / 0.5 diopters
		qs->cam_convergence = 0.5f;
		qs->cam_half_tan_vfov = 0.3249f;

		// Move HMD backward by default camera distance
		struct xrt_vec3 cam_pos = {
		    pose->position.x - fwd.x * cam_distance,
		    pose->position.y - fwd.y * cam_distance,
		    pose->position.z - fwd.z * cam_distance,
		};

		pose->position = cam_pos;
	}

	qs->camera_mode = !qs->camera_mode;

	// Reset controllers to mode-appropriate default positions (attached to head)
	if (qs->lctrl) {
		reset_controller_for_mode(qs, qs->lctrl, true);
	}
	if (qs->rctrl) {
		reset_controller_for_mode(qs, qs->rctrl, false);
	}

	U_LOG_W("Qwerty: view mode -> %s (derived from previous state)",
	        qs->camera_mode ? "Camera" : "Display");
}

void
qwerty_adjust_view_factor(struct qwerty_system *qs, float multiplier)
{
	if (qs->camera_mode) {
		float v = clampf(qs->cam_spread_factor * multiplier, 0.01f, 1.0f);
		qs->cam_spread_factor = v;
		qs->cam_parallax_factor = v;
		U_LOG_I("Qwerty: Camera IPD/Parallax = %.3f", v);
	} else {
		float v = clampf(qs->disp_spread_factor * multiplier, 0.01f, 1.0f);
		qs->disp_spread_factor = v;
		qs->disp_parallax_factor = v;
		U_LOG_I("Qwerty: Display IPD/Parallax = %.3f", v);
	}
}

void
qwerty_adjust_convergence(struct qwerty_system *qs, float direction)
{
	if (!qs->camera_mode) {
		return; // No-op in display mode
	}
	qs->cam_convergence = clampf(qs->cam_convergence + direction * 0.05f, 0.0f, 2.0f);
	U_LOG_I("Qwerty: Convergence = %.2f diopters", qs->cam_convergence);
}

void
qwerty_adjust_vheight(struct qwerty_system *qs, float multiplier)
{
	if (qs->camera_mode) {
		return; // No-op in camera mode
	}
	qs->disp_vHeight = clampf(qs->disp_vHeight * multiplier, 0.1f, 10.0f);
	U_LOG_I("Qwerty: vHeight = %.2f m", qs->disp_vHeight);
}

void
qwerty_reset_view_state(struct qwerty_system *qs)
{
	qs->camera_mode = true;

	qs->cam_spread_factor = 1.0f;
	qs->cam_parallax_factor = 1.0f;
	qs->cam_convergence = 0.5f;
	qs->cam_half_tan_vfov = 0.3249f;

	qs->disp_spread_factor = 1.0f;
	qs->disp_parallax_factor = 1.0f;
	qs->disp_vHeight = 1.3f;

	if (qs->hmd != NULL) {
		qs->hmd->base.pose.position = QWERTY_HMD_CAMERA_POS;
		qs->hmd->base.pose.orientation = (struct xrt_quat)XRT_QUAT_IDENTITY;
	}

	U_LOG_W("Qwerty: view state reset to camera defaults");
}

bool
qwerty_get_view_state(struct xrt_device **xdevs, size_t xdev_count, struct qwerty_view_state *out)
{
	if (xdevs == NULL || out == NULL) {
		return false;
	}

	// Find a qwerty device in the device list
	struct qwerty_system *qs = NULL;
	for (size_t i = 0; i < xdev_count; i++) {
		if (xdevs[i] == NULL || xdevs[i]->destroy != qwerty_destroy) {
			continue;
		}
		const char *name = xdevs[i]->tracking_origin->name;
		if (strcmp(name, QWERTY_HMD_TRACKER_STR) == 0 || strcmp(name, QWERTY_LEFT_TRACKER_STR) == 0 ||
		    strcmp(name, QWERTY_RIGHT_TRACKER_STR) == 0) {
			qs = qwerty_device(xdevs[i])->sys;
			break;
		}
	}

	if (qs == NULL) {
		return false;
	}

	out->camera_mode = qs->camera_mode;

	out->cam_spread_factor = qs->cam_spread_factor;
	out->cam_parallax_factor = qs->cam_parallax_factor;
	out->cam_convergence = qs->cam_convergence;
	out->cam_half_tan_vfov = qs->cam_half_tan_vfov;

	out->disp_spread_factor = qs->disp_spread_factor;
	out->disp_parallax_factor = qs->disp_parallax_factor;
	out->disp_vHeight = qs->disp_vHeight;

	out->nominal_viewer_z = qs->nominal_viewer_z;
	out->screen_height_m = qs->screen_height_m;
	return true;
}

void
qwerty_set_process_keys(struct xrt_device **xdevs, size_t xdev_count, bool enabled)
{
	if (xdevs == NULL) {
		return;
	}

	for (size_t i = 0; i < xdev_count; i++) {
		if (xdevs[i] == NULL || xdevs[i]->destroy != qwerty_destroy) {
			continue;
		}
		const char *name = xdevs[i]->tracking_origin->name;
		if (strcmp(name, QWERTY_HMD_TRACKER_STR) == 0 || strcmp(name, QWERTY_LEFT_TRACKER_STR) == 0 ||
		    strcmp(name, QWERTY_RIGHT_TRACKER_STR) == 0) {
			qwerty_device(xdevs[i])->sys->process_keys = enabled;
			return;
		}
	}
}

static void
reset_controller_for_mode(struct qwerty_system *qs, struct qwerty_controller *qc, bool is_left)
{
	struct qwerty_device *qd = &qc->base;

	// Ensure controller is attached to head
	qwerty_follow_hmd(qc, true);

	if (qs->camera_mode) {
		// Camera mode: standard HMD-relative offsets
		qd->pose = (struct xrt_pose){XRT_QUAT_IDENTITY, QWERTY_CONTROLLER_INITIAL_POS(is_left)};
	} else {
		// Display mode: x,y are standard hand offsets (same as camera mode),
		// z depth scales with zoomScale to place controllers between viewer and display
		float zs = qs->screen_height_m / qs->disp_vHeight;
		float offX = is_left ? -0.2f : 0.2f;
		float offY = -0.3f;
		float offZ = (qs->nominal_viewer_z - 0.3f) / zs;

		qd->pose = (struct xrt_pose){
		    XRT_QUAT_IDENTITY,
		    {offX, offY, offZ},
		};
	}
}

void
qwerty_reset_controller_pose(struct qwerty_controller *qc)
{
	struct qwerty_device *qd = &qc->base;

	bool no_qhmd = !qd->sys->hmd;
	if (no_qhmd) {
		return;
	}

	bool is_left = qc == qd->sys->lctrl;

	qwerty_follow_hmd(qc, true);
	struct xrt_pose pose = {XRT_QUAT_IDENTITY, QWERTY_CONTROLLER_INITIAL_POS(is_left)};
	qd->pose = pose;
}
