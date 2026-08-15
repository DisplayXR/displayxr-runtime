// Copyright 2023, Joseph Albers.
// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Ultraleap (Leap Motion) input provider — LeapC → motion controllers.
 *
 * Adapted from Monado's removed `ultraleap_v5` driver (`ulv5_driver.cpp`,
 * restored from the pre-strip history for #825): the LeapC poll thread and
 * 26-joint processing survive; the device model changes from one
 * hand-tracker `xrt_device` to TWO `khr/simple_controller` motion
 * controllers on the ADR-034 input-provider channel:
 *
 * - palm pose → grip/aim, pushed into `m_relation_history` with
 *   Leap-clock→monotonic mapping (latency-floor offset, the net_input
 *   pattern), so `get_tracked_pose(at_time_ns)` predicts correctly;
 * - pinch → select click, grab → menu click (hysteresis so a wavering
 *   strength doesn't chatter);
 * - the full `xrt_hand_joint_set` is still filled per hand and served via
 *   `get_hand_tracking` — inert until the Tier-2 (#825) runtime wiring
 *   lands, at which point this provider needs no changes.
 *
 * Coordinate mapping: DESKTOP tracking mode (device on the desk, facing
 * up). Leap's desktop frame is right-handed, y up, z toward the user —
 * the same handedness as OpenXR — so the mapping is mm→m plus a fixed
 * mount offset that places the interaction volume in the tabletop-scale
 * scene DisplayXR view rigs show (LOCAL origin = stage height 1.6 m; see
 * sim_input_device.c for the same reasoning).
 *
 * @author Joseph Albers <joseph.albers@outlook.de>
 * @author David Fattal
 * @ingroup drv_ultraleap
 */

#include "xrt/xrt_device.h"

#include "os/os_time.h"
#include "os/os_threading.h"

#include "math/m_relation_history.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_device.h"
#include "util/u_logging.h"

#include "ultraleap_interface.h"

#include "LeapC.h"

#include <stdio.h>
#include <string.h>


/*
 *
 * Structs and defines.
 *
 */

// Indices into base.inputs, matching ul_inputs_array below.
#define UL_SELECT 0
#define UL_MENU 1
#define UL_GRIP 2
#define UL_AIM 3
#define UL_HAND_TRACKING 4

// Pinch/grab hysteresis (LeapC strengths are 0..1).
#define UL_PRESS_THRESHOLD 0.85f
#define UL_RELEASE_THRESHOLD 0.55f

/*!
 * #941 idle watchdog: close our LeapC connection when no XR consumer has
 * polled poses/joints for this long. Hyperion runs the full tracking model
 * (measured: one whole CPU core) for connected clients regardless of use,
 * and upstream ships no auto-idle — so the provider stops being a client
 * while nobody looks. Client-isolated by design: we close OUR connection
 * rather than calling LeapSetPause, which is service-global and would stomp
 * concurrent Leap apps (e.g. LeiaViewer).
 */
#define UL_IDLE_TIMEOUT_NS (5LL * 1000 * 1000 * 1000)

/*!
 * Mount offset: where the Leap device's origin sits in stage space.
 * Hands hover 10–40 cm above the device, so an origin at stage height
 * minus ~15 cm puts the interaction volume around y ≈ 1.6 m — the
 * region a display-scale view rig actually shows (LOCAL origin = stage
 * 1.6 m). u_var-tunable at runtime for desk setups that differ.
 */
static const struct xrt_vec3 ul_mount_offset_default = {0.0f, 1.45f, -0.10f};

static const enum xrt_space_relation_flags ul_valid_flags = (enum xrt_space_relation_flags)(
    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
    XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

struct ul_hub;

struct ul_device
{
	struct xrt_device base;

	struct ul_hub *hub;

	//! 0 = left, 1 = right (LeapC eLeapHandType order).
	int hand;

	//! Thread-safe pose buffer, fed by the poll thread.
	struct m_relation_history *history;

	//! Guarded by hub->mutex.
	bool tracked;
	bool btn_select;
	bool btn_menu;
	struct xrt_hand_joint_set joint_set;
};

struct ul_hub
{
	struct os_thread_helper oth;

	//! Guards device button/joint state and the offset estimate.
	struct os_mutex mutex;

	LEAP_CONNECTION connection;

	//! Latency-floor clock-offset estimate (monotonic_ns − leap_us*1000).
	int64_t clock_offset_ns;
	bool have_clock_offset;

	//! Mount offset, u_var-tunable.
	struct xrt_vec3 mount_offset;

	//! Diagnostics (u_var).
	uint64_t frame_count;

	//! #941 idle watchdog. last_activity_ns is guarded by mutex (marked
	//! from app threads); the connection state is poll-thread-owned.
	int64_t last_activity_ns;
	int64_t idle_since_ns;
	bool leap_connected;
	uint64_t idle_disconnects;

	struct ul_device *devices[2];

	//! Live device count; the last xrt_device::destroy tears the hub down.
	int device_refcount;
};

//! #941: an XR consumer touched this provider — keep (or revive) the
//! LeapC connection. Called from update_inputs / get_tracked_pose /
//! get_hand_tracking, i.e. both the controller-emulation and the
//! hand-joint paths count as activity (begin/end_feature deliberately
//! does NOT gate this — controllers never begin the hand-tracking
//! feature and must keep tracking alive on their own).
static inline void
ul_mark_activity(struct ul_hub *hub)
{
	os_mutex_lock(&hub->mutex);
	hub->last_activity_ns = (int64_t)os_monotonic_get_ns();
	os_mutex_unlock(&hub->mutex);
}

static const char *
leap_result_to_string(eLeapRS result)
{
	switch (result) {
	case eLeapRS_Success: return "eLeapRS_Success";
	case eLeapRS_UnknownError: return "eLeapRS_UnknownError";
	case eLeapRS_InvalidArgument: return "eLeapRS_InvalidArgument";
	case eLeapRS_InsufficientResources: return "eLeapRS_InsufficientResources";
	case eLeapRS_InsufficientBuffer: return "eLeapRS_InsufficientBuffer";
	case eLeapRS_Timeout: return "eLeapRS_Timeout";
	case eLeapRS_NotConnected: return "eLeapRS_NotConnected";
	case eLeapRS_HandshakeIncomplete: return "eLeapRS_HandshakeIncomplete";
	case eLeapRS_BufferSizeOverflow: return "eLeapRS_BufferSizeOverflow";
	case eLeapRS_ProtocolError: return "eLeapRS_ProtocolError";
	case eLeapRS_InvalidClientID: return "eLeapRS_InvalidClientID";
	case eLeapRS_UnexpectedClosed: return "eLeapRS_UnexpectedClosed";
	case eLeapRS_UnknownImageFrameRequest: return "eLeapRS_UnknownImageFrameRequest";
	case eLeapRS_UnknownTrackingFrameID: return "eLeapRS_UnknownTrackingFrameID";
	case eLeapRS_RoutineIsNotSeer: return "eLeapRS_RoutineIsNotSeer";
	case eLeapRS_TimestampTooEarly: return "eLeapRS_TimestampTooEarly";
	case eLeapRS_ConcurrentPoll: return "eLeapRS_ConcurrentPoll";
	case eLeapRS_NotAvailable: return "eLeapRS_NotAvailable";
	case eLeapRS_NotStreaming: return "eLeapRS_NotStreaming";
	case eLeapRS_CannotOpenDevice: return "eLeapRS_CannotOpenDevice";
	default: return "unknown result type.";
	}
}


/*
 *
 * Coordinate mapping (desktop mount).
 *
 */

//! Leap desktop-frame mm → stage-space meters (+ mount offset).
static inline struct xrt_vec3
ul_map_position(const struct ul_hub *hub, LEAP_VECTOR v)
{
	struct xrt_vec3 out;
	out.x = v.x / 1000.0f + hub->mount_offset.x;
	out.y = v.y / 1000.0f + hub->mount_offset.y;
	out.z = v.z / 1000.0f + hub->mount_offset.z;
	return out;
}

static inline struct xrt_quat
ul_map_orientation(LEAP_QUATERNION q)
{
	struct xrt_quat out;
	out.x = q.x;
	out.y = q.y;
	out.z = q.z;
	out.w = q.w;
	return out;
}

//! Map a Leap-clock microsecond stamp into the monotonic domain.
static int64_t
ul_map_timestamp(struct ul_hub *hub, int64_t leap_us, int64_t recv_ns)
{
	if (leap_us == 0) {
		return recv_ns;
	}
	int64_t leap_ns = leap_us * 1000;
	int64_t inst = recv_ns - leap_ns;
	if (!hub->have_clock_offset || inst < hub->clock_offset_ns) {
		hub->clock_offset_ns = inst; // latency floor
		hub->have_clock_offset = true;
	} else {
		// Slow decay so genuine clock drift doesn't pin a stale minimum.
		hub->clock_offset_ns += (inst - hub->clock_offset_ns) / 1024;
	}
	return leap_ns + hub->clock_offset_ns;
}


/*
 *
 * Joint processing (from ulv5_driver.cpp, desktop-frame mapping).
 *
 */

static void
ul_process_joint(const struct ul_hub *hub,
                 LEAP_VECTOR joint_pos,
                 LEAP_QUATERNION joint_orientation,
                 float width,
                 struct xrt_hand_joint_value *joint)
{
	joint->radius = (width / 1000.0f) / 2.0f;
	struct xrt_space_relation *relation = &joint->relation;

	relation->pose.orientation = ul_map_orientation(joint_orientation);
	relation->pose.position = ul_map_position(hub, joint_pos);
	relation->relation_flags = ul_valid_flags;
}

static void
ul_process_hand(struct ul_hub *hub, const LEAP_HAND *hand, struct xrt_hand_joint_set *out_set)
{
// Access to individual joints of the set being built.
#define JS(y) &out_set->values.hand_joint_set_default[XRT_HAND_JOINT_##y]

	ul_process_joint(hub, hand->palm.position, hand->palm.orientation, hand->palm.width, JS(PALM));
	// wrist is the next_joint of the arm
	ul_process_joint(hub, hand->arm.next_joint, hand->arm.rotation, hand->arm.width, JS(WRIST));
	// thumb
	ul_process_joint(hub, hand->thumb.proximal.prev_joint, hand->thumb.proximal.rotation,
	                 hand->thumb.proximal.width, JS(THUMB_METACARPAL));
	ul_process_joint(hub, hand->thumb.intermediate.prev_joint, hand->thumb.intermediate.rotation,
	                 hand->thumb.intermediate.width, JS(THUMB_PROXIMAL));
	ul_process_joint(hub, hand->thumb.distal.prev_joint, hand->thumb.distal.rotation, hand->thumb.distal.width,
	                 JS(THUMB_DISTAL));
	ul_process_joint(hub, hand->thumb.distal.next_joint, hand->thumb.distal.rotation, hand->thumb.distal.width,
	                 JS(THUMB_TIP));
	// index
	ul_process_joint(hub, hand->index.metacarpal.prev_joint, hand->index.metacarpal.rotation,
	                 hand->index.metacarpal.width, JS(INDEX_METACARPAL));
	ul_process_joint(hub, hand->index.proximal.prev_joint, hand->index.proximal.rotation,
	                 hand->index.proximal.width, JS(INDEX_PROXIMAL));
	ul_process_joint(hub, hand->index.intermediate.prev_joint, hand->index.intermediate.rotation,
	                 hand->index.intermediate.width, JS(INDEX_INTERMEDIATE));
	ul_process_joint(hub, hand->index.distal.prev_joint, hand->index.distal.rotation, hand->index.distal.width,
	                 JS(INDEX_DISTAL));
	ul_process_joint(hub, hand->index.distal.next_joint, hand->index.distal.rotation, hand->index.distal.width,
	                 JS(INDEX_TIP));
	// middle
	ul_process_joint(hub, hand->middle.metacarpal.prev_joint, hand->middle.metacarpal.rotation,
	                 hand->middle.metacarpal.width, JS(MIDDLE_METACARPAL));
	ul_process_joint(hub, hand->middle.proximal.prev_joint, hand->middle.proximal.rotation,
	                 hand->middle.proximal.width, JS(MIDDLE_PROXIMAL));
	ul_process_joint(hub, hand->middle.intermediate.prev_joint, hand->middle.intermediate.rotation,
	                 hand->middle.intermediate.width, JS(MIDDLE_INTERMEDIATE));
	ul_process_joint(hub, hand->middle.distal.prev_joint, hand->middle.distal.rotation, hand->middle.distal.width,
	                 JS(MIDDLE_DISTAL));
	ul_process_joint(hub, hand->middle.distal.next_joint, hand->middle.distal.rotation, hand->middle.distal.width,
	                 JS(MIDDLE_TIP));
	// ring
	ul_process_joint(hub, hand->ring.metacarpal.prev_joint, hand->ring.metacarpal.rotation,
	                 hand->ring.metacarpal.width, JS(RING_METACARPAL));
	ul_process_joint(hub, hand->ring.proximal.prev_joint, hand->ring.proximal.rotation, hand->ring.proximal.width,
	                 JS(RING_PROXIMAL));
	ul_process_joint(hub, hand->ring.intermediate.prev_joint, hand->ring.intermediate.rotation,
	                 hand->ring.intermediate.width, JS(RING_INTERMEDIATE));
	ul_process_joint(hub, hand->ring.distal.prev_joint, hand->ring.distal.rotation, hand->ring.distal.width,
	                 JS(RING_DISTAL));
	ul_process_joint(hub, hand->ring.distal.next_joint, hand->ring.distal.rotation, hand->ring.distal.width,
	                 JS(RING_TIP));
	// pinky
	ul_process_joint(hub, hand->pinky.metacarpal.prev_joint, hand->pinky.metacarpal.rotation,
	                 hand->pinky.metacarpal.width, JS(LITTLE_METACARPAL));
	ul_process_joint(hub, hand->pinky.proximal.prev_joint, hand->pinky.proximal.rotation,
	                 hand->pinky.proximal.width, JS(LITTLE_PROXIMAL));
	ul_process_joint(hub, hand->pinky.intermediate.prev_joint, hand->pinky.intermediate.rotation,
	                 hand->pinky.intermediate.width, JS(LITTLE_INTERMEDIATE));
	ul_process_joint(hub, hand->pinky.distal.prev_joint, hand->pinky.distal.rotation, hand->pinky.distal.width,
	                 JS(LITTLE_DISTAL));
	ul_process_joint(hub, hand->pinky.distal.next_joint, hand->pinky.distal.rotation, hand->pinky.distal.width,
	                 JS(LITTLE_TIP));
#undef JS
}


/*
 *
 * Poll thread.
 *
 */

static void
ul_handle_tracking_event(struct ul_hub *hub, const LEAP_TRACKING_EVENT *ev, int64_t recv_ns)
{
	bool seen[2] = {false, false};

	os_mutex_lock(&hub->mutex);
	int64_t ts = ul_map_timestamp(hub, (int64_t)ev->info.timestamp, recv_ns);
	hub->frame_count++;
	os_mutex_unlock(&hub->mutex);

	for (uint32_t i = 0; i < ev->nHands; i++) {
		const LEAP_HAND *hand = &ev->pHands[i];
		int idx = hand->type == eLeapHandType_Left ? 0 : 1;
		struct ul_device *dev = hub->devices[idx];
		if (dev == NULL || seen[idx]) {
			continue;
		}
		seen[idx] = true;

		// Pose → relation history (palm = grip/aim anchor).
		struct xrt_space_relation rel = {};
		rel.pose.position = ul_map_position(hub, hand->palm.position);
		rel.pose.orientation = ul_map_orientation(hand->palm.orientation);
		rel.relation_flags = ul_valid_flags;
		m_relation_history_push(dev->history, &rel, ts);

		// Buttons + joints under the lock.
		os_mutex_lock(&hub->mutex);
		dev->tracked = true;
		if (hand->pinch_strength > UL_PRESS_THRESHOLD) {
			dev->btn_select = true;
		} else if (hand->pinch_strength < UL_RELEASE_THRESHOLD) {
			dev->btn_select = false;
		}
		if (hand->grab_strength > UL_PRESS_THRESHOLD) {
			dev->btn_menu = true;
		} else if (hand->grab_strength < UL_RELEASE_THRESHOLD) {
			dev->btn_menu = false;
		}
		ul_process_hand(hub, hand, &dev->joint_set);
		os_mutex_unlock(&hub->mutex);
	}

	// A hand that vanished from the frame goes untracked (inputs
	// deactivate; the pose history keeps predicting briefly, which is
	// the desired coast-out).
	for (int idx = 0; idx < 2; idx++) {
		struct ul_device *dev = hub->devices[idx];
		if (dev == NULL || seen[idx]) {
			continue;
		}
		os_mutex_lock(&hub->mutex);
		dev->tracked = false;
		dev->btn_select = false;
		dev->btn_menu = false;
		os_mutex_unlock(&hub->mutex);
	}
}

static bool
ul_open_connection(struct ul_hub *hub)
{
	if (LeapOpenConnection(hub->connection) != eLeapRS_Success) {
		return false;
	}
	LeapSetTrackingMode(hub->connection, eLeapTrackingMode_Desktop);
	return true;
}

//! Deactivate both devices' inputs (used on idle disconnect, #941).
static void
ul_set_all_untracked(struct ul_hub *hub)
{
	os_mutex_lock(&hub->mutex);
	for (int idx = 0; idx < 2; idx++) {
		struct ul_device *dev = hub->devices[idx];
		if (dev != NULL) {
			dev->tracked = false;
			dev->btn_select = false;
			dev->btn_menu = false;
		}
	}
	os_mutex_unlock(&hub->mutex);
}

static void *
ul_poll_thread(void *ptr)
{
	struct ul_hub *hub = (struct ul_hub *)ptr;

	if (!ul_open_connection(hub)) {
		U_LOG_E("ultraleap: LeapOpenConnection failed — is the Ultraleap tracking service running?");
		return NULL;
	}
	hub->leap_connected = true;
	U_LOG_W("ultraleap: connection open (desktop tracking mode); hands appear when the device sees them.");

	while (os_thread_helper_is_running(&hub->oth)) {
		os_mutex_lock(&hub->mutex);
		int64_t last_activity = hub->last_activity_ns;
		os_mutex_unlock(&hub->mutex);
		int64_t now = (int64_t)os_monotonic_get_ns();

		if (!hub->leap_connected) {
			// Idle (#941): wait cheaply for an activity mark newer
			// than the disconnect, then revive the connection.
			if (last_activity <= hub->idle_since_ns) {
				os_nanosleep(100 * 1000 * 1000); // 100 ms
				continue;
			}
			if (!ul_open_connection(hub)) {
				U_LOG_W("ultraleap: reconnect failed — retrying while input is being polled.");
				os_nanosleep(500 * 1000 * 1000); // 500 ms
				continue;
			}
			hub->leap_connected = true;
			U_LOG_W("ultraleap: input polled again — Leap connection reopened (#941).");
		}

		// #941 idle watchdog: no XR consumer for a while → stop being
		// a tracking-service client until someone polls again.
		if (now - last_activity > UL_IDLE_TIMEOUT_NS) {
			LeapCloseConnection(hub->connection);
			hub->leap_connected = false;
			hub->idle_since_ns = now;
			hub->idle_disconnects++;
			ul_set_all_untracked(hub);
			U_LOG_W(
			    "ultraleap: no input consumer for %d s — Leap connection closed "
			    "(#941; reopens on demand).",
			    (int)(UL_IDLE_TIMEOUT_NS / (1000 * 1000 * 1000)));
			continue;
		}

		LEAP_CONNECTION_MESSAGE msg;
		eLeapRS result = LeapPollConnection(hub->connection, 500 /* ms */, &msg);
		if (result == eLeapRS_Timeout) {
			continue; // idle — re-check the stop flag
		}
		if (result != eLeapRS_Success) {
			U_LOG_W(
			    "ultraleap: LeapPollConnection returned %s — "
			    "TIP: use a full-bandwidth USB port, not a shared hub.",
			    leap_result_to_string(result));
			continue;
		}
		if (msg.type == eLeapEventType_Tracking) {
			ul_handle_tracking_event(hub, msg.tracking_event, (int64_t)os_monotonic_get_ns());
		}
	}

	return NULL;
}


/*
 *
 * Hub lifetime.
 *
 */

static void
ul_hub_destroy(struct ul_hub *hub)
{
	os_thread_helper_stop_and_wait(&hub->oth);

	if (hub->leap_connected) {
		LeapCloseConnection(hub->connection);
	}
	LeapDestroyConnection(hub->connection);

	u_var_remove_root(hub);
	os_thread_helper_destroy(&hub->oth);
	os_mutex_destroy(&hub->mutex);
	free(hub);
}


/*
 *
 * Device member functions.
 *
 */

static inline struct ul_device *
ul_device(struct xrt_device *xdev)
{
	return (struct ul_device *)xdev;
}

static void
ul_device_destroy(struct xrt_device *xdev)
{
	struct ul_device *dev = ul_device(xdev);
	struct ul_hub *hub = dev->hub;

	os_mutex_lock(&hub->mutex);
	hub->devices[dev->hand] = NULL;
	int remaining = --hub->device_refcount;
	os_mutex_unlock(&hub->mutex);

	m_relation_history_destroy(&dev->history);
	u_device_free(&dev->base);

	if (remaining == 0) {
		ul_hub_destroy(hub);
	}
}

static xrt_result_t
ul_device_update_inputs(struct xrt_device *xdev)
{
	struct ul_device *dev = ul_device(xdev);
	int64_t now = (int64_t)os_monotonic_get_ns();

	ul_mark_activity(dev->hub); // #941

	os_mutex_lock(&dev->hub->mutex);
	bool tracked = dev->tracked;
	bool select = dev->btn_select;
	bool menu = dev->btn_menu;
	os_mutex_unlock(&dev->hub->mutex);

	for (uint32_t i = 0; i < xdev->input_count; i++) {
		xdev->inputs[i].active = tracked;
		xdev->inputs[i].timestamp = now;
	}
	if (!tracked) {
		for (uint32_t i = 0; i < xdev->input_count; i++) {
			U_ZERO(&xdev->inputs[i].value);
		}
		return XRT_SUCCESS;
	}
	xdev->inputs[UL_SELECT].value.boolean = select;
	xdev->inputs[UL_MENU].value.boolean = menu;

	return XRT_SUCCESS;
}

static xrt_result_t
ul_device_get_tracked_pose(struct xrt_device *xdev,
                           enum xrt_input_name name,
                           int64_t at_timestamp_ns,
                           struct xrt_space_relation *out_relation)
{
	struct ul_device *dev = ul_device(xdev);

	ul_mark_activity(dev->hub); // #941

	switch (name) {
	case XRT_INPUT_SIMPLE_GRIP_POSE:
	case XRT_INPUT_SIMPLE_AIM_POSE: break;
	default:
		// Plain message, not U_LOG_XDEV_UNSUPPORTED_INPUT — see the
		// matching comment in sim_input_device.c (plug-in link vs the
		// aux export surface).
		U_LOG_E("ultraleap: unsupported input name: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_space_relation rel = {};
	enum m_relation_history_result res = m_relation_history_get(dev->history, at_timestamp_ns, &rel);
	if (res == M_RELATION_HISTORY_RESULT_INVALID) {
		out_relation->pose = XRT_POSE_IDENTITY;
		out_relation->relation_flags = (enum xrt_space_relation_flags)0;
		return XRT_SUCCESS;
	}

	*out_relation = rel;
	return XRT_SUCCESS;
}

static xrt_result_t
ul_device_get_hand_tracking(struct xrt_device *xdev,
                            enum xrt_input_name name,
                            int64_t at_timestamp_ns,
                            struct xrt_hand_joint_set *out_value,
                            int64_t *out_timestamp_ns)
{
	struct ul_device *dev = ul_device(xdev);

	ul_mark_activity(dev->hub); // #941

	enum xrt_input_name expected =
	    dev->hand == 0 ? XRT_INPUT_HT_UNOBSTRUCTED_LEFT : XRT_INPUT_HT_UNOBSTRUCTED_RIGHT;
	if (name != expected) {
		U_LOG_E("ultraleap: unsupported hand-tracking input: 0x%08x", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	os_mutex_lock(&dev->hub->mutex);
	*out_value = dev->joint_set;
	bool tracked = dev->tracked;
	os_mutex_unlock(&dev->hub->mutex);

	// Hand pose rides the same palm history as the grip.
	struct xrt_space_relation rel = {};
	if (m_relation_history_get(dev->history, at_timestamp_ns, &rel) != M_RELATION_HISTORY_RESULT_INVALID) {
		out_value->hand_pose = rel;
	}
	out_value->is_active = tracked;
	*out_timestamp_ns = at_timestamp_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
ul_device_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	// Hands have no haptics — accept and drop, so apps can bind the
	// haptic action unconditionally.
	(void)xdev;
	(void)name;
	(void)value;
	return XRT_SUCCESS;
}


/*
 *
 * khr/simple_controller (+ hand-tracking) data arrays.
 *
 */

static enum xrt_input_name ul_inputs_left[] = {
    XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_SIMPLE_MENU_CLICK,    XRT_INPUT_SIMPLE_GRIP_POSE,
    XRT_INPUT_SIMPLE_AIM_POSE,     XRT_INPUT_HT_UNOBSTRUCTED_LEFT,
};

static enum xrt_input_name ul_inputs_right[] = {
    XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_SIMPLE_MENU_CLICK,     XRT_INPUT_SIMPLE_GRIP_POSE,
    XRT_INPUT_SIMPLE_AIM_POSE,     XRT_INPUT_HT_UNOBSTRUCTED_RIGHT,
};

static enum xrt_output_name ul_outputs_array[] = {
    XRT_OUTPUT_NAME_SIMPLE_VIBRATION,
};

static struct ul_device *
ul_create_device(struct ul_hub *hub, int hand)
{
	const enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	bool is_left = hand == 0;
	const enum xrt_input_name *inputs = is_left ? ul_inputs_left : ul_inputs_right;
	const uint32_t input_count = is_left ? ARRAY_SIZE(ul_inputs_left) : ARRAY_SIZE(ul_inputs_right);
	const uint32_t output_count = ARRAY_SIZE(ul_outputs_array);

	struct ul_device *dev = U_DEVICE_ALLOCATE(struct ul_device, flags, input_count, output_count);
	dev->base.update_inputs = ul_device_update_inputs;
	dev->base.get_tracked_pose = ul_device_get_tracked_pose;
	dev->base.get_hand_tracking = ul_device_get_hand_tracking;
	dev->base.get_view_poses = u_device_ni_get_view_poses;
	dev->base.set_output = ul_device_set_output;
	dev->base.destroy = ul_device_destroy;
	dev->base.supported.orientation_tracking = true;
	dev->base.supported.position_tracking = true;
	dev->base.supported.hand_tracking = true;
	dev->base.name = XRT_DEVICE_SIMPLE_CONTROLLER;
	dev->base.device_type = is_left ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;

	// 6DOF — origin type must not stay TRACKING_TYPE_NONE, or the
	// legacy builder injects arm-model offsets into every pose (see the
	// matching comment in sim_input_device.c).
	dev->base.tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
	snprintf(dev->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "%s", "Ultraleap Tracker");

	snprintf(dev->base.str, sizeof(dev->base.str), "Ultraleap %s Hand (controller emulation)",
	         is_left ? "Left" : "Right");
	snprintf(dev->base.serial, sizeof(dev->base.serial), "ULTRALEAP-%s", is_left ? "L" : "R");

	for (uint32_t i = 0; i < input_count; i++) {
		dev->base.inputs[i].active = false; // inactive until a hand appears
		dev->base.inputs[i].name = inputs[i];
	}
	for (uint32_t i = 0; i < output_count; i++) {
		dev->base.outputs[i].name = ul_outputs_array[i];
	}

	dev->hub = hub;
	dev->hand = hand;
	m_relation_history_create(&dev->history);

	return dev;
}


/*
 *
 * 'Exported' functions.
 *
 */

extern "C" xrt_result_t
ul_create_devices(struct xrt_device **out_left, struct xrt_device **out_right)
{
	struct ul_hub *hub = U_TYPED_CALLOC(struct ul_hub);
	if (hub == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	hub->mount_offset = ul_mount_offset_default;
	// #941: startup grace — the idle watchdog only fires once nothing has
	// polled for the timeout, so probes/tests see frames immediately.
	hub->last_activity_ns = (int64_t)os_monotonic_get_ns();

	if (os_mutex_init(&hub->mutex) != 0) {
		free(hub);
		return XRT_ERROR_ALLOCATION;
	}
	if (os_thread_helper_init(&hub->oth) != 0) {
		os_mutex_destroy(&hub->mutex);
		free(hub);
		return XRT_ERROR_ALLOCATION;
	}

	eLeapRS result = LeapCreateConnection(NULL, &hub->connection);
	if (result != eLeapRS_Success) {
		U_LOG_E("ultraleap: LeapCreateConnection failed (%s) — falling back.", leap_result_to_string(result));
		os_thread_helper_destroy(&hub->oth);
		os_mutex_destroy(&hub->mutex);
		free(hub);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct ul_device *left = ul_create_device(hub, 0);
	struct ul_device *right = ul_create_device(hub, 1);
	hub->devices[0] = left;
	hub->devices[1] = right;
	hub->device_refcount = 2;

	if (os_thread_helper_start(&hub->oth, ul_poll_thread, hub) != 0) {
		left->base.destroy(&left->base);   // refcount 2 → 1
		right->base.destroy(&right->base); // refcount 1 → 0, destroys hub
		return XRT_ERROR_ALLOCATION;
	}

	u_var_add_root(hub, "Ultraleap Input Hub", true);
	u_var_add_ro_u64(hub, &hub->frame_count, "tracking_frame_count");
	u_var_add_vec3_f32(hub, &hub->mount_offset, "mount_offset_m");
	u_var_add_bool(hub, &hub->leap_connected, "leap_connected");
	u_var_add_ro_u64(hub, &hub->idle_disconnects, "idle_disconnects");

	*out_left = &left->base;
	*out_right = &right->base;
	return XRT_SUCCESS;
}
