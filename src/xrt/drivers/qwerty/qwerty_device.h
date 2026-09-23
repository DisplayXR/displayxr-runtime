// Copyright 2021, Mateo de Mayo.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Internal header for qwerty_device and its friends.
 * @author Mateo de Mayo <mateodemayo@gmail.com>
 * @ingroup drv_qwerty
 */
#pragma once

#include "util/u_logging.h"
#include "util/u_camera_profile.h"
#include "xrt/xrt_device.h"

#include "os/os_threading.h"

/*!
 * @addtogroup drv_qwerty
 * @{
 */

#define QWERTY_HMD_STR "Qwerty HMD"
#define QWERTY_HMD_TRACKER_STR QWERTY_HMD_STR " Tracker"
#define QWERTY_LEFT_STR "Qwerty Left Controller"
#define QWERTY_LEFT_TRACKER_STR QWERTY_LEFT_STR " Tracker"
#define QWERTY_RIGHT_STR "Qwerty Right Controller"
#define QWERTY_RIGHT_TRACKER_STR QWERTY_RIGHT_STR " Tracker"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Container of qwerty devices and driver properties.
 * @see qwerty_hmd, qwerty_controller
 */
struct qwerty_system
{
	struct qwerty_hmd *hmd;          //!< Can be NULL
	struct qwerty_controller *lctrl; //!< Cannot be NULL
	struct qwerty_controller *rctrl; //!< Cannot be NULL
	enum u_logging_level log_level;
	bool process_keys;  //!< If false disable keyboard and mouse input
	bool hmd_focused;   //!< For gui var tracking only, true if hmd is the focused device
	bool lctrl_focused; //!< Same as `hmd_focused` but for the left controller
	bool rctrl_focused; //!< Same as `hmd_focused` but for the right controller
	bool force_2d_mode;              //!< Runtime-side 2D/3D toggle (V key when HMD focused)
	bool display_mode_toggle_pending; //!< Set by key handler, cleared by compositor

	int rendering_mode;                  //!< Current unified rendering mode index (0=2D, 1+=3D modes)
	bool rendering_mode_change_pending;  //!< Set by key handler, cleared by caller

	// View controls (P toggles mode with derivation, spacebar resets)
	bool camera_mode; //!< true=camera-centric (default), false=display-centric
	struct os_mutex view_lock; //!< Serializes view tuning, profile seeding and Space reset.
	bool camera_profile_active;
	struct u_camera_profile camera_profile;

	// Camera-centric state (user adjusts when camera_mode=true)
	float cam_spread_factor;   //!< Absolute eye scale; profile may seed it independently of parallax.
	float cam_parallax_factor; //!< Absolute parallax scale; the combined keyboard control couples both.
	float cam_convergence;     //!< [0,2] diopters, default 0.5
	float cam_half_tan_vfov;   //!< default 0.3249 — derived only, not user-adjustable
	float cam_m2v;             //!< meters→world scale, default 1.0 (qwerty perspective is always 1)

	// Display-centric state (user adjusts when camera_mode=false)
	float disp_spread_factor;      //!< [0.01,1] default 1.0 (= disp_parallax always)
	float disp_parallax_factor; //!< [0.01,1] default 1.0 (= disp_ipd always)
	float disp_vHeight;         //!< [0.1,10] meters, default 1.3
	float disp_perspective;     //!< [0.1,10] default 1.0; set to the camera→display
	                            //!< converted perspective on P (so P is seamless)

	// Hardware config (set by target builder)
	float nominal_viewer_z; //!< meters (e.g. 0.6 from sim_display)
	float screen_height_m;  //!< meters (e.g. 0.194 from sim_display)

	/*
	 * Platform input front-end state (#1538).
	 *
	 * This used to live in function statics inside qwerty_{win32,macos,sdl},
	 * which made it process-global: it survived the xrt_system_devices that
	 * owned the devices it pointed at (the use-after-free #1538 crashed on),
	 * and it was shared across every instance a process built. It lives here
	 * now so its lifetime is exactly this system's lifetime.
	 *
	 * LOCK ORDER: @ref input_lock is a LEAF. Never hold it while taking
	 * @ref view_lock or any qwerty_device::lock — the front-ends read/update
	 * this block under the lock, drop it, and only then call the
	 * qwerty_press_* / qwerty_release_* / view-tuning helpers (all of which
	 * take their own locks).
	 */
	struct os_mutex input_lock;
	bool input_bound;                              //!< Defaults below resolved; also gates the one-off bind log.
	struct qwerty_device *input_default_qdev;      //!< Focused device when no modifier is held.
	struct qwerty_controller *input_default_qctrl; //!< Ditto, for controller-only methods.
	bool input_ctrl_pressed;                       //!< CTRL held = left controller focused.
	bool input_alt_pressed;                        //!< ALT/Option held = right controller focused.
	bool input_mouse_look_active;                  //!< RMB held: mouse drives rotation.
	float input_last_mouse_x;                      //!< Screen-space mouse baseline for delta computation.
	float input_last_mouse_y;
	bool input_lmb_was_down; //!< LMB latch (touchpad drivers can swallow the messages).
	bool input_mmb_was_down; //!< MMB latch, same reason.
};

/*!
 * Fake device that modifies its tracked pose through its methods.
 * @implements xrt_device
 */
struct qwerty_device
{
	struct xrt_device base;
	//! #958: guards the pose/delta/speed integrator state below, which the
	//! win32 input thread writes and N action-path consumers read+consume.
	struct os_mutex lock;

	//! #962: last keyboard-integration timestamp (os_monotonic_get_ns); 0 = never.
	int64_t last_integrate_ns;
	struct xrt_pose pose;      //!< Internal pose state
	struct qwerty_system *sys; //!< Reference to the system this device is in.

	float movement_speed; //!< In meters per frame
	bool left_pressed;
	bool right_pressed;
	bool forward_pressed;
	bool backward_pressed;
	bool up_pressed;
	bool down_pressed;

	float look_speed; //!< In radians per frame
	bool look_left_pressed;
	bool look_right_pressed;
	bool look_up_pressed;
	bool look_down_pressed;
	//! #1692: roll, the third rotation axis. Device-local, like pitch.
	bool roll_left_pressed;
	bool roll_right_pressed;

	bool sprint_pressed; //!< Movement and look speed boost
	float yaw_delta;     //!< How much extra yaw to add for the next pose. Then reset to 0.
	float pitch_delta;   //!< Similar to `yaw_delta`
	float x_pos_delta;   //!< Mouse-driven position delta in world X. Reset to 0 each frame.
	float y_pos_delta;   //!< Mouse-driven position delta in world Y. Reset to 0 each frame.

	/*!
	 * #1692: velocity of @ref pose over the integration step that produced
	 * it, in the same (base) frame as the pose — m/s and rad/s. Always
	 * meaningful: a step with no input reports zero, which is a real value,
	 * not an unknown one. Written under @ref lock by
	 * qwerty_get_tracked_pose(), read there and by a parented controller
	 * reading its HMD.
	 */
	struct xrt_vec3 linear_velocity;
	struct xrt_vec3 angular_velocity;
};

/*!
 * #1692: the velocity of one integration step, in the BASE frame.
 *
 * Analytic, not a finite difference of two sampled poses: the driver already
 * knows the exact translation and the exact rotation it just applied, so the
 * step's rate is `delta / dt` with no sampling noise. The rotation is rebuilt
 * from the two SMALL per-step rotations rather than from `after * before^-1`,
 * because every consumer polling this device shortens the step — at a
 * microsecond dt the difference of two near-equal orientations is float noise,
 * while the small rotations keep every significant bit.
 *
 * The step is `after = base_rotation * before * local_rotation` (the driver
 * applies pitch and roll in the device's own frame and yaw in the base frame),
 * so the base-frame rotation of the step is
 * `base_rotation * (before * local_rotation * before^-1)`.
 *
 * @param pos_delta      Translation applied this step, base frame, metres.
 * @param ori_before     Orientation at the start of the step.
 * @param local_rotation Rotation applied in the device's own frame (pitch, roll).
 * @param base_rotation  Rotation applied in the base frame (yaw).
 * @param dt_s           Step length in seconds; <= 0 yields zero velocity.
 * @param[out] out_linear  Linear velocity, base frame, m/s.
 * @param[out] out_angular Angular velocity, base frame, rad/s.
 *
 * @public @memberof qwerty_device
 */
void
qwerty_step_velocity(const struct xrt_vec3 *pos_delta,
                     const struct xrt_quat *ori_before,
                     const struct xrt_quat *local_rotation,
                     const struct xrt_quat *base_rotation,
                     float dt_s,
                     struct xrt_vec3 *out_linear,
                     struct xrt_vec3 *out_angular);

/*!
 * @implements qwerty_device
 * @see qwerty_system
 */
struct qwerty_hmd
{
	struct qwerty_device base;
};

/*!
 * Supports input actions and can be attached to the HMD pose.
 * @implements qwerty_device
 * @see qwerty_system
 */
struct qwerty_controller
{
	struct qwerty_device base;

	bool trigger_clicked;
	int64_t trigger_timestamp;
	bool menu_clicked;
	int64_t menu_timestamp;
	bool squeeze_clicked;
	int64_t squeeze_timestamp;
	bool system_clicked;
	int64_t system_timestamp;

	bool thumbstick_left_pressed;
	bool thumbstick_right_pressed;
	bool thumbstick_up_pressed;
	bool thumbstick_down_pressed;
	int64_t thumbstick_timestamp;
	bool thumbstick_clicked;
	int64_t thumbstick_click_timestamp;

	bool trackpad_left_pressed;
	bool trackpad_right_pressed;
	bool trackpad_up_pressed;
	bool trackpad_down_pressed;
	int64_t trackpad_timestamp;
	bool trackpad_clicked;
	int64_t trackpad_click_timestamp;

	/*!
	 * Only used when a qwerty_hmd exists in the system.
	 * Do not modify directly; use qwerty_follow_hmd().
	 * If true, `pose` is relative to the qwerty_hmd.
	 */
	bool follow_hmd; // @todo: Make this work with non-qwerty HMDs.
};

/*!
 * @public @memberof qwerty_system
 */
struct qwerty_system *
qwerty_system_create(struct qwerty_hmd *qhmd,
                     struct qwerty_controller *qleft,
                     struct qwerty_controller *qright,
                     enum u_logging_level log_level);

/*
 *
 * qwerty_device methods
 *
 */

/*!
 * @brief Cast to qwerty_device. Ensures returning a valid device or crashing.
 * @public @memberof qwerty_device
 */
struct qwerty_device *
qwerty_device(struct xrt_device *xd);

//! @public @memberof qwerty_device
void
qwerty_press_left(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_left(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_press_right(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_right(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_press_forward(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_forward(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_press_backward(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_backward(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_press_up(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_up(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_press_down(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_down(struct qwerty_device *qd);

//! @public @memberof qwerty_device
void
qwerty_press_look_left(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_look_left(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_press_look_right(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_look_right(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_press_look_up(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_look_up(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_press_look_down(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_look_down(struct qwerty_device *qd);

/*!
 * Roll — the third rotation axis, device-local like pitch (#1692). Positive
 * ("left") tips the device's right side up, matching `look_left` turning left.
 * Bound to Z / X; before it, only two of the three angular axes were driveable
 * from the keyboard at all, so no keyboard recipe could exercise roll.
 * @public @memberof qwerty_device
 */
void
qwerty_press_roll_left(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_roll_left(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_press_roll_right(struct qwerty_device *qd);
//! @public @memberof qwerty_device
void
qwerty_release_roll_right(struct qwerty_device *qd);

/*!
 * Momentarily increase `movement_speed` *and* `look_speed` until
 * `qwerty_release_sprint()`
 * @public @memberof qwerty_device
 */
void
qwerty_press_sprint(struct qwerty_device *qd);

/*!
 * Stop doing what @ref qwerty_press_sprint started.
 * @public @memberof qwerty_device
 */
void
qwerty_release_sprint(struct qwerty_device *qd);

/*!
 * Add yaw and pitch movement for the next frame
 * @public @memberof qwerty_device
 */
void
qwerty_add_look_delta(struct qwerty_device *qd, float yaw, float pitch);

/*!
 * Add world-space XY position delta for the next frame (mouse-driven translation)
 * @public @memberof qwerty_device
 */
void
qwerty_add_position_delta(struct qwerty_device *qd, float dx, float dy);

/*!
 * Change movement speed in exponential steps (usually integers, but any float allowed)
 * @public @memberof qwerty_device
 */
void
qwerty_change_movement_speed(struct qwerty_device *qd, float steps);

/*!
 * Release all movement input
 * @public @memberof qwerty_device
 */
void
qwerty_release_all(struct qwerty_device *qd);

/*!
 * Create qwerty_hmd. Crash on failure.
 * @public @memberof qwerty_hmd
 */
struct qwerty_hmd *
qwerty_hmd_create(void);

/*!
 * Cast to qwerty_hmd. Ensures returning a valid HMD or crashing.
 * @public @memberof qwerty_hmd
 */
struct qwerty_hmd *
qwerty_hmd(struct xrt_device *xd);

/*
 *
 * qwerty_controller methods
 *
 */
/*!
 * Create qwerty_controller. Crash on failure.
 * @public @memberof qwerty_controller
 */
struct qwerty_controller *
qwerty_controller_create(bool is_left, struct qwerty_hmd *qhmd);

/*!
 * Cast to qwerty_controller. Ensures returning a valid controller or crashing.
 * @public @memberof qwerty_controller
 */
struct qwerty_controller *
qwerty_controller(struct xrt_device *xd);

/*!
 * Simulate pressing input/trigger/value to 1.0
 * @public @memberof qwerty_controller
 */
void
qwerty_press_trigger(struct qwerty_controller *qc);

/*!
 * Simulate releasing input/trigger/value to 0.0
 * @public @memberof qwerty_controller
 */
void
qwerty_release_trigger(struct qwerty_controller *qc);

/*!
 * Simulate pressing input/menu/click
 * @public @memberof qwerty_controller
 */
void
qwerty_press_menu(struct qwerty_controller *qc);

/*!
 * Simulate releasing input/menu/click
 * @public @memberof qwerty_controller
 */
void
qwerty_release_menu(struct qwerty_controller *qc);

/*!
 * Simulate pressing input/squeeze/click
 * @public @memberof qwerty_controller
 */
void
qwerty_press_squeeze(struct qwerty_controller *qc);

/*!
 * Simulate releasing input/squeeze/click
 * @public @memberof qwerty_controller
 */
void
qwerty_release_squeeze(struct qwerty_controller *qc);

/*!
 * Simulate pressing input/system/click
 * @public @memberof qwerty_controller
 */
void
qwerty_press_system(struct qwerty_controller *qc);

/*!
 * Simulate releasing input/system/click
 * @public @memberof qwerty_controller
 */
void
qwerty_release_system(struct qwerty_controller *qc);

//! @public @memberof qwerty_controller
void
qwerty_press_thumbstick_left(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_release_thumbstick_left(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_press_thumbstick_right(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_release_thumbstick_right(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_press_thumbstick_up(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_release_thumbstick_up(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_press_thumbstick_down(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_release_thumbstick_down(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_press_thumbstick_click(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_release_thumbstick_click(struct qwerty_controller *qc);

//! @public @memberof qwerty_controller
void
qwerty_press_trackpad_left(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_release_trackpad_left(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_press_trackpad_right(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_release_trackpad_right(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_press_trackpad_up(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_release_trackpad_up(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_press_trackpad_down(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_release_trackpad_down(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_press_trackpad_click(struct qwerty_controller *qc);
//! @public @memberof qwerty_controller
void
qwerty_release_trackpad_click(struct qwerty_controller *qc);

/*!
 * Attach/detach the pose of `qc` to its HMD. Only works when a qwerty_hmd is present.
 * @public @memberof qwerty_controller
 */
void
qwerty_follow_hmd(struct qwerty_controller *qc, bool follow);

/*!
 * Reset controller to initial pose and makes it follow the HMD
 * @public @memberof qwerty_controller
 */
void
qwerty_reset_controller_pose(struct qwerty_controller *qc);

/*!
 * Toggle runtime-side 2D/3D display mode. Sets force_2d_mode and
 * marks the toggle as pending for the compositor to pick up.
 * @public @memberof qwerty_system
 */
void
qwerty_toggle_display_mode(struct qwerty_system *qs);

/*!
 * Set the rendering mode index (1/2/3 keys). Marks the change as
 * pending for the compositor to pick up via
 * qwerty_check_rendering_mode_change().
 * @public @memberof qwerty_system
 */
void
qwerty_set_rendering_mode(struct qwerty_system *qs, int mode);

/*!
 * Toggle between camera-centric and display-centric view mode.
 * @public @memberof qwerty_system
 */
void
qwerty_toggle_camera_mode(struct qwerty_system *qs);

//! Read the view-mode selector under the same lock as profile and keyboard changes.
bool
qwerty_is_camera_mode(struct qwerty_system *qs);

/*!
 * Adjust IPD+parallax factor by multiplier (both set to same value, clamped [0.01,1]).
 * @public @memberof qwerty_system
 */
void
qwerty_adjust_view_factor(struct qwerty_system *qs, float multiplier);

/*!
 * Adjust convergence by additive direction (camera mode only, ±0.05, clamped [0,2]).
 * No-op if in display mode.
 * @public @memberof qwerty_system
 */
void
qwerty_adjust_convergence(struct qwerty_system *qs, float direction);

/*!
 * Adjust virtual display height by multiplier (display mode only, clamped [0.1,10]).
 * No-op if in camera mode.
 * @public @memberof qwerty_system
 */
void
qwerty_adjust_vheight(struct qwerty_system *qs, float multiplier);

/*!
 * Reset all view state to camera defaults and HMD position to (0, 1.6, 0).
 * @public @memberof qwerty_system
 */
void
qwerty_reset_view_state(struct qwerty_system *qs);


/*!
 * @}
 */

#ifdef __cplusplus
}
#endif
