// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display webcam eye tracking (#1855): the pure parts — camera
 *         model, landmarks -> eye positions, one-euro smoothing, the
 *         tracking-loss timeout and the MANAGED collapse / revival blend, and
 *         device / mode selection. No OS calls, no threads: unit-tested on
 *         every OS (tests/tests_sim_webcam_tracking.cpp).
 *
 * Coordinate frame (the DP's eye-position frame): metres, origin at the
 * centre of the panel's active area, +x right, +y up, +z out of the panel
 * towards the viewer.
 *
 * Camera model: a pinhole with square pixels and the principal point at the
 * image centre, mounted at the TOP-CENTRE of the panel's active area plus a
 * configurable offset, its optical axis along +z (looking straight out of
 * the panel, no tilt). Focal length from the horizontal field of view:
 * f = (W / 2) / tan(hfov / 2). The image is the sensor's own (NOT mirrored):
 * the viewer's right (display +x) appears on the image's LEFT. A mirrored
 * source sets @ref sim_webcam_config::mirrored.
 *
 * Depth is monocular: the inter-pupil pixel distance under an IPD prior
 * (63 mm), Z = f * IPD / d_px. A yawed head shortens d_px and so reads
 * slightly far — the documented accuracy limit of a single plain webcam.
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_stereo_uvc_device;
struct u_stereo_uvc_mode;

#define SIM_WEBCAM_DEFAULT_HFOV_DEG 70.0f
#define SIM_WEBCAM_DEFAULT_IPD_M 0.063f
#define SIM_WEBCAM_DEFAULT_TIMEOUT_NS (300ull * 1000000ull)
#define SIM_WEBCAM_DEFAULT_COLLAPSE_NS (500ull * 1000000ull)
#define SIM_WEBCAM_DEFAULT_REVIVE_NS (300ull * 1000000ull)
//! The capture width the mode picker aims for (inference cost, not accuracy, sets it).
#define SIM_WEBCAM_TARGET_WIDTH 640u

struct sim_webcam_config
{
	float hfov_deg; //!< camera horizontal field of view
	//! Camera position relative to the TOP-CENTRE of the panel's active area, metres (display frame).
	float cam_offset_m[3];
	float ipd_m;   //!< inter-pupillary distance prior used for depth
	bool mirrored; //!< the source delivers a mirrored (selfie) image

	uint64_t timeout_ns;  //!< no face for longer than this -> is_tracking false
	uint64_t collapse_ns; //!< after loss: ease to the nominal viewer over this long (MANAGED)
	uint64_t revive_ns;   //!< after re-acquisition: ease back to tracked over this long

	//! One-euro filter (Casiez et al. 2012), on every eye coordinate in metres.
	float euro_min_cutoff_hz;
	float euro_beta; //!< cutoff increase per m/s of speed
	float euro_d_cutoff_hz;
};

//! Every default (70 deg, 63 mm, 300 ms, top-centre, unmirrored).
void
sim_webcam_config_defaults(struct sim_webcam_config *cfg);

/*!
 * What a face estimator reports for one frame: the two pupil (eye-centre)
 * positions in PIXELS of the analysed image, origin top-left, +y down. Which
 * one is "left" does not matter — the mapping orders them by display x.
 */
struct sim_face_landmarks
{
	bool found;
	float eye_px[2][2]; //!< [eye][x, y]
	float confidence;   //!< 0..1, informational
};

/*!
 * Landmarks -> eye positions in the display frame.
 * @param screen_h_m the panel's active-area height (the camera sits on its top edge)
 * @param out_eyes   [0] = the eye with the smaller display x (viewer's left)
 * @return false when there is no usable face (not found, zero image, pupils
 *         closer than a pixel, or a depth outside 0.1 .. 5 m).
 */
bool
sim_webcam_landmarks_to_eyes(const struct sim_webcam_config *cfg,
                             float screen_h_m,
                             uint32_t image_w,
                             uint32_t image_h,
                             const struct sim_face_landmarks *lm,
                             struct xrt_vec3 out_eyes[2]);

/*!
 * The inverse, for tests and tools: where the camera of @p cfg sees display-
 * frame point @p p, in pixels. @return false behind the camera.
 */
bool
sim_webcam_project(const struct sim_webcam_config *cfg,
                   float screen_h_m,
                   uint32_t image_w,
                   uint32_t image_h,
                   const struct xrt_vec3 *p,
                   float out_px[2]);


/*
 * One-euro filter.
 */

struct sim_one_euro
{
	bool init;
	float x_prev;
	float dx_prev;
	uint64_t t_prev_ns;
};

void
sim_one_euro_reset(struct sim_one_euro *f);

//! Filter sample @p x taken at @p t_ns. A non-increasing timestamp returns the previous output.
float
sim_one_euro_filter(struct sim_one_euro *f, float min_cutoff_hz, float beta, float d_cutoff_hz, float x, uint64_t t_ns);


/*
 * Tracking state: written by the worker per analysed frame, sampled by the
 * display processor per frame.
 */

struct sim_webcam_track_state
{
	bool have_face;        //!< a face was ever seen
	uint64_t last_face_ns; //!< capture time of the newest frame with a face
	uint64_t acquire_ns;   //!< when the current run of faces began
	float revive_from;     //!< the blend weight at that moment (continuity)
	uint64_t acquisitions; //!< runs of faces so far (for the edge count)
	struct xrt_vec3 eyes[2];
	struct sim_one_euro filters[6];
};

void
sim_webcam_track_reset(struct sim_webcam_track_state *s);

/*!
 * One analysed frame captured at @p t_ns: @p found with its @p eyes, or no
 * face. A face after a gap longer than the timeout starts a new run (filters
 * reset, revival starts).
 */
void
sim_webcam_track_update(struct sim_webcam_track_state *s,
                        const struct sim_webcam_config *cfg,
                        bool found,
                        const struct xrt_vec3 eyes[2],
                        uint64_t t_ns);

struct sim_webcam_eyes
{
	struct xrt_vec3 eyes[2]; //!< [0] viewer's left
	bool is_tracking;
	float weight;   //!< 0 = nominal viewer, 1 = tracked
	uint64_t edges; //!< is_tracking edges so far (monotonic)
};

/*!
 * The eyes to report at @p now_ns. Tracking within the timeout: the filtered
 * eyes (eased in from the nominal viewer over revive_ns after a re-
 * acquisition). Lost: is_tracking false and the last tracked eyes ease to
 * @p nominal over collapse_ns — the MANAGED contract (positions always valid,
 * a vendor-style collapse rather than a jump). Never seen a face: @p nominal.
 */
void
sim_webcam_track_sample(const struct sim_webcam_track_state *s,
                        const struct sim_webcam_config *cfg,
                        const struct xrt_vec3 nominal[2],
                        uint64_t now_ns,
                        struct sim_webcam_eyes *out);


/*
 * Device + mode selection.
 */

/*!
 * Which device @p sel names: NULL / "" = the first; all digits = that index;
 * anything else = the first whose name contains it (case-insensitive).
 * @return the index, or -1.
 */
int32_t
sim_webcam_select_device(const struct u_stereo_uvc_device *devs, uint32_t count, const char *sel);

/*!
 * The capture mode to open: width closest to @ref SIM_WEBCAM_TARGET_WIDTH,
 * then the highest frame rate up to 60, then the smaller height.
 * @return false when @p count is 0.
 */
bool
sim_webcam_select_mode(const struct u_stereo_uvc_mode *modes, uint32_t count, struct u_stereo_uvc_mode *out);

/*!
 * Parse "x,y,z" millimetres into metres. @return false (and @p out untouched)
 * on anything else.
 */
bool
sim_webcam_parse_offset_mm(const char *s, float out_m[3]);

#ifdef __cplusplus
}
#endif
