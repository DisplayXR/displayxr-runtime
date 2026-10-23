// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_tracking's scripted viewer: a pure function of time (no camera).
 *
 * What the hardware-free CI gate drives `xrLocateViews` with. Pure so the
 * script can be pinned without a thread (tests/tests_viewer_arbitration.cpp).
 *
 * @ingroup drv_sim_tracking
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Which script runs.
enum sim_tracking_script_kind
{
	SIM_TRACKING_SCRIPT_SWEEP = 0,  //!< the head sweeps left-right: x = offset_x + amplitude * sin(2 pi t / period)
	SIM_TRACKING_SCRIPT_OFFSET = 1, //!< the head holds still at nominal + offset (deterministic)
};

struct sim_tracking_script
{
	enum sim_tracking_script_kind kind;
	float nominal[3];       //!< the display's nominal viewer (host geometry), display space, metres
	float offset[3];        //!< added to the nominal viewer, metres
	float amplitude_m;      //!< SWEEP half-width
	int64_t period_ns;      //!< SWEEP period (> 0)
	float ipd_m;            //!< eye separation
	int64_t loss_period_ns; //!< > 0: tracking square-waves off for half of every period (loss edges); 0 = always
	                        //!< tracking
	int64_t epoch_ns;       //!< t = 0 of the script
};

//! Parse "sweep" / "offset" (case-insensitive). False (out untouched) on anything else.
bool
sim_tracking_parse_script(const char *s, enum sim_tracking_script_kind *out);

/*!
 * The viewer at @p t_ns: left / right eye and head centre, display space,
 * metres; returns whether the script reports "tracking" at that instant.
 */
bool
sim_tracking_script_eval(
    const struct sim_tracking_script *s, int64_t t_ns, float left[3], float right[3], float head[3]);

/*
 *
 * The scripted camera (calibration test bed). Pure, like the script.
 *
 */

//! Most CAMERA_POSE_<n> slots a provider scans (n = 0 .. SIM_TRACKING_POSE_SLOTS-1).
#define SIM_TRACKING_POSE_SLOTS 16

/*!
 * A simulated webcam. It *measures* the scripted eyes from its TRUE pose (the
 * hidden ground truth, `CAMERA_TRUE_POSE`) — a camera-space unit direction and
 * a distance (times `depth_bias`, a simulated per-user iris error) — and the
 * provider *reports* display-space eyes by mapping those measurements back
 * through its BELIEVED pose: the calibrated `CAMERA_POSE_<n>` for its camera,
 * else the prior (top-centre bezel, identity). A correct calibration makes
 * the reported eyes equal the scripted ones.
 */
struct sim_tracking_camera
{
	double true_t[3];   //!< ground-truth camera position, display space, metres
	double true_q[4];   //!< ground-truth orientation x, y, z, w (camera frame -> display frame)
	double depth_bias;  //!< reported distance = true distance * this (1 = exact)
	double pose_t[3];   //!< believed pose (calibrated or prior)
	double pose_q[4];   //!< believed orientation
	double depth_scale; //!< believed per-user depth correction (CAMERA_POSE_<n>_DEPTH_SCALE), 1 = none
	double focal_scale; //!< believed focal scale (CAMERA_POSE_<n>_FOCAL_SCALE), 1 = none
};

//! Parse "tx,ty,tz,qx,qy,qz,qw" (metres; the quaternion is normalised). False (outputs untouched) on error.
bool
sim_tracking_parse_pose(const char *s, double t[3], double q[4]);

//! The prior a camera provider uses before calibration: top-centre bezel, T_y = H/2 + 8 mm, identity.
void
sim_tracking_camera_prior(float display_height_m, double t[3], double q[4]);

//! What the camera measures for a display-space eye: unit direction + distance (times depth_bias).
void
sim_tracking_camera_measure(const struct sim_tracking_camera *c, const float eye[3], float dir[3], float *distance_m);

//! A measurement mapped to display space through the BELIEVED pose: T + R * (depth_scale * d * u'(focal_scale)).
void
sim_tracking_camera_to_display(const struct sim_tracking_camera *c, const float dir[3], float distance_m, float out[3]);

#ifdef __cplusplus
}
#endif
