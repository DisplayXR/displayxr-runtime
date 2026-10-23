// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Mirror-alignment camera-pose calibration solver (viewer-tracking class).
 *
 * The display's front surface is a weak mirror: an eye point that sees a dot
 * on the glass overlap its own reflection lies on the display normal through
 * that dot. Each such alignment therefore pins the eye point's display-space
 * (x, y) to the dot's known position D_i, with no depth.
 *
 * A camera-based viewer-tracking provider measures, for the same instant, the
 * eye point's unit direction u_i in its CAMERA frame and an estimated distance
 * s~_i along it (iris-size depth). With the camera pose (R, T) in display
 * space (display point = T + R * camera point) every sample satisfies
 *
 *     xy( T + s_i * R * u_i ) = D_i          (two equations, one new unknown s_i)
 *
 * The solver eliminates each s_i in closed form for a given pose (the 1-D
 * least-squares foot), so the iteration runs over the pose parameters only:
 *
 *   - MODE_TRANSLATION (a): R = I. Unknowns T_x, T_y. Closed form (each sample
 *     is a line through D_i along xy(u_i); T is their least-squares meet).
 *     Needs >= 2 samples at >= 2 distinct dots.
 *   - MODE_PITCH (b): R = Rx(pitch). Unknowns T_x, T_y, pitch. >= 3 samples;
 *     pitch is only well conditioned with two head distances.
 *   - MODE_FULL (c): full R (rotation vector) + T_x, T_y, optionally a focal
 *     scale k (the camera's true focal length / the one its directions were
 *     computed with: u' = normalize(u_x, u_y, k * u_z)). >= 7 samples at >= 3
 *     distinct dots.
 *
 * Levenberg-Marquardt with a numeric Jacobian, seeded (a) -> (b) -> (c).
 *
 * **T_z is not observable.** It never enters the x, y equations, in any mode;
 * the solver reports the caller's assumed value (default 0, a bezel camera),
 * which biases the viewer distance one-for-one by a few millimetres.
 *
 * **Degenerate input is refused**, never "solved": all samples at one dot (a
 * scale ambiguity — the dot baseline is what fixes the scale), too few
 * samples for the mode, or a rank-deficient normal matrix at the solution.
 *
 * Pure C11, no allocation, no platform calls. Spec:
 * docs/specs/runtime/tracking-provider-discovery.md, "Camera pose calibration".
 *
 * @ingroup aux_math
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Most samples one solve takes.
#define M_MIRROR_CALIB_MAX_SAMPLES 64

//! Two dots closer than this count as the same dot (metres).
#define M_MIRROR_CALIB_DISTINCT_DOT_M 0.010

//! Which pose parameters are solved.
enum m_mirror_calib_mode
{
	M_MIRROR_CALIB_MODE_TRANSLATION = 0, //!< (a) R = I: T_x, T_y
	M_MIRROR_CALIB_MODE_PITCH = 1,       //!< (b) R = Rx(pitch): + pitch
	M_MIRROR_CALIB_MODE_FULL = 2,        //!< (c) full R (+ optional focal scale)
};

//! Which eye point the user aligned (the procedure decides).
enum m_mirror_calib_eye
{
	M_MIRROR_CALIB_EYE_MID = 0,   //!< closed-eye variant: the midpoint of the two eyes
	M_MIRROR_CALIB_EYE_LEFT = 1,  //!< own-pupil variant, left eye open
	M_MIRROR_CALIB_EYE_RIGHT = 2, //!< own-pupil variant, right eye open
};

enum m_mirror_calib_status
{
	M_MIRROR_CALIB_OK = 0,
	M_MIRROR_CALIB_ERROR_INVALID_INPUT = 1,   //!< NULL, NaN, a zero direction, too many samples
	M_MIRROR_CALIB_ERROR_TOO_FEW_SAMPLES = 2, //!< fewer than the mode needs
	M_MIRROR_CALIB_ERROR_DEGENERATE = 3,      //!< all at one dot / too few distinct dots / rank deficient
	M_MIRROR_CALIB_ERROR_BEHIND_CAMERA = 4,   //!< the fit puts an eye point behind the camera
};

//! One alignment: where the dot is, and what the camera measured at that instant.
struct m_mirror_calib_sample
{
	double dot_m[2];       //!< The dot's display-space x, y (metres, display-centre origin, +X right, +Y up)
	double dir[3];         //!< Camera-space direction to the aligned eye point (normalised by the solver)
	double est_distance_m; //!< The provider's distance estimate along @ref dir; <= 0 = none
};

struct m_mirror_calib_options
{
	enum m_mirror_calib_mode mode;
	bool solve_focal_scale;    //!< MODE_FULL only: also solve k
	double reject_threshold_m; //!< > 0: drop the worst sample while its residual exceeds this (and enough remain)
	double assumed_tz_m;       //!< T_z reported (unobservable); 0 = on the glass plane
	int max_iterations;        //!< LM iterations per stage; <= 0 = 100
};

//! Defaults: MODE_TRANSLATION, no focal scale, no rejection, T_z = 0, 100 iterations.
void
m_mirror_calib_options_init(struct m_mirror_calib_options *opt);

struct m_mirror_calib_result
{
	enum m_mirror_calib_status status;
	char reason[256]; //!< Human reason on failure; "" on success

	enum m_mirror_calib_mode mode;
	double t_m[3];      //!< Camera position in display space (t_m[2] = the assumed T_z)
	double q[4];        //!< Camera orientation x, y, z, w (camera frame -> display frame)
	double r[9];        //!< The same rotation, row-major
	double pitch_deg;   //!< R = Rz(roll) * Ry(yaw) * Rx(pitch); + pitch = optical axis tilted DOWN
	double yaw_deg;     //!< + yaw = optical axis turned toward +X
	double roll_deg;    //!< + roll = camera rotated counter-clockwise about the display normal
	double focal_scale; //!< k (1 unless solved)

	int sample_count;                              //!< Input samples
	int used_count;                                //!< Samples in the final fit
	int distinct_dots;                             //!< Distinct dots among the used samples
	double residual_m[M_MIRROR_CALIB_MAX_SAMPLES]; //!< Per input sample: distance from the dot in the plane
	double distance_m[M_MIRROR_CALIB_MAX_SAMPLES]; //!< Per input sample: solved s_i
	bool rejected[M_MIRROR_CALIB_MAX_SAMPLES];     //!< Per input sample: dropped by the rejection threshold
	double rms_m;                                  //!< RMS of the used residuals
	double max_m;                                  //!< Max used residual

	//! Per-user depth correction: median over used samples of s_i / s~_i; 0 when no estimates.
	double iris_scale;

	int params;           //!< Pose parameters solved
	int rank;             //!< Numerical rank of the (column-scaled) normal matrix at the solution
	double condition;     //!< Its condition number (max / min eigenvalue)
	bool ill_conditioned; //!< condition > 1e6: solved, but the geometry barely constrains a parameter
	int iterations;       //!< LM iterations of the final stage
};

/*!
 * Solve. Always fills @p out (status + reason). True on M_MIRROR_CALIB_OK.
 * @p samples may hold up to @ref M_MIRROR_CALIB_MAX_SAMPLES entries.
 */
bool
m_mirror_calib_solve(const struct m_mirror_calib_sample *samples,
                     int count,
                     const struct m_mirror_calib_options *opt,
                     struct m_mirror_calib_result *out);

/*!
 * The camera-space direction + estimated distance of the aligned eye point
 * from a provider's per-eye camera block. MID = the midpoint of the two
 * estimated eye points (its direction depends only on the ratio of the two
 * distance estimates). False on a zero direction.
 */
bool
m_mirror_calib_eye_point(enum m_mirror_calib_eye eye,
                         const float left_dir[3],
                         float left_distance_m,
                         const float right_dir[3],
                         float right_distance_m,
                         double out_dir[3],
                         double *out_distance_m);

//! "a" / "b" / "c" (also "translation" / "pitch" / "full"). False on anything else.
bool
m_mirror_calib_parse_mode(const char *s, enum m_mirror_calib_mode *out);

//! "mid" / "left" / "right". False on anything else.
bool
m_mirror_calib_parse_eye(const char *s, enum m_mirror_calib_eye *out);

//! "a" / "b" / "c".
const char *
m_mirror_calib_mode_str(enum m_mirror_calib_mode mode);

//! "ok" / "invalid_input" / "too_few_samples" / "degenerate" / "behind_camera".
const char *
m_mirror_calib_status_str(enum m_mirror_calib_status status);

/*!
 * Rotate a camera-space point into display space with pose (t, q):
 * out = t + R(q) * p. q = x, y, z, w (normalised here).
 */
void
m_mirror_calib_apply_pose(const double t[3], const double q[4], const double p[3], double out[3]);

#ifdef __cplusplus
}
#endif
