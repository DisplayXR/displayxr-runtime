// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) R2: vendor-neutral stereo
 *         rectification — Bouguet rectification to a parallel pair with zero
 *         disparity at infinity, cropped to the valid region, applied as a
 *         per-pixel remap LUT. No OpenCV dependency; the numbers follow
 *         OpenCV's `stereoRectify(..., CALIB_ZERO_DISPARITY, alpha = 0)` +
 *         `initUndistortRectifyMap` (golden-tested against OpenCV-generated
 *         fixtures in tests_stereo_rectify).
 *
 * Three layers, so a GPU path can replace only the last one:
 *
 *  1. **Geometry** — @ref u_stereo_rectify_compute: from the RAW calibration
 *     (per-eye K + distortion, OpenCV extrinsics `x_R = R x_L + T`) to the
 *     rectified rotations R1/R2, the common focal and principal point, the
 *     projection matrices P1/P2 and the baseline. Pure math, no allocation.
 *  2. **Maps** — @ref u_stereo_rectify_map_point / @ref
 *     u_stereo_rectify_build_map: for each OUTPUT pixel, the RAW source
 *     position (float). A float map is what a GPU backend uploads (RG32F
 *     texture sampled bilinearly in a compute/fragment pass).
 *  3. **CPU backend** — @ref u_stereo_rectify_lut: the maps packed as
 *     fixed-point bilinear taps over the whole side-by-side image, applied per
 *     plane (GRAY8 = 1 channel, NV12 = a full-res Y LUT + a half-res 2-channel
 *     UV LUT, BGRA8 = 4 channels).
 *
 * Conventions: pixel centres at integer coordinates (OpenCV), intrinsics fx
 * fy cx cy with zero skew, distortion coefficients in OpenCV order
 * (k1 k2 p1 p2 k3 k4 k5 k6 for RADTAN5/8; k1..k4 for KB4 fisheye).
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Lens models (values equal xrt_plugin_stereo_camera_distortion).
enum u_stereo_rectify_model
{
	U_STEREO_RECTIFY_MODEL_NONE = 0,
	U_STEREO_RECTIFY_MODEL_RADTAN5 = 1,
	U_STEREO_RECTIFY_MODEL_RADTAN8 = 2,
	U_STEREO_RECTIFY_MODEL_KB4 = 3,
};

//! One eye's pinhole + lens model.
struct u_stereo_rectify_lens
{
	double fx, fy, cx, cy;
	uint32_t model; //!< enum u_stereo_rectify_model
	double d[8];    //!< OpenCV order
};

//! The RAW calibration of a pair.
struct u_stereo_rectify_input
{
	//! Per-eye size of the frames to rectify.
	uint32_t width, height;
	//! Per-eye size the intrinsics describe; 0 = same as the frames. When it
	//! differs, K is rescaled (pixel-centre aware); distortion is unitless.
	uint32_t calib_width, calib_height;
	struct u_stereo_rectify_lens eye[2];
	//! OpenCV extrinsics: x_right = R * x_left + T (T in any unit, e.g. mm).
	double R[3][3];
	double T[3];
};

//! The rectified geometry.
struct u_stereo_rectify_result
{
	uint32_t width, height; //!< per eye, unchanged by rectification
	//! RAW intrinsics rescaled to the frame size (what the maps sample).
	struct u_stereo_rectify_lens raw[2];
	//! Rectified-from-raw rotation per eye (OpenCV R1, R2).
	double rot[2][3][3];
	//! Rectified intrinsics shared by both eyes: fx = fy = f, one principal
	//! point (CALIB_ZERO_DISPARITY: a point at infinity has zero disparity).
	double f, cx, cy;
	//! OpenCV P1, P2; P[1][0][3] = f * Tx (units of T; Tx < 0 when the right
	//! camera is at +x).
	double P[2][3][4];
	//! |T|, units of T.
	double baseline;
	//! Rectified translation R2 * T (≈ (-baseline, 0, 0)), units of T.
	double t_rect[3];
	//! The valid-region zoom applied to f (alpha = 0): >= 1 when the lens and
	//! rotations would otherwise leave black borders.
	double crop_scale;
};

/*!
 * Bouguet rectification of @p in, horizontal pair, zero disparity at
 * infinity, cropped so every output pixel of both eyes samples inside its raw
 * image (no black corners), per-eye size unchanged, no convergence shear.
 * @return false on a degenerate calibration (zero baseline, non-positive
 *         focal, a vertical pair, or a rotation that does not converge).
 */
bool
u_stereo_rectify_compute(const struct u_stereo_rectify_input *in, struct u_stereo_rectify_result *out);

/*!
 * Map one rectified pixel (@p u, @p v) of eye @p eye to its RAW source pixel.
 * @return false when the ray points behind the raw camera.
 */
bool
u_stereo_rectify_map_point(
    const struct u_stereo_rectify_result *r, uint32_t eye, double u, double v, double *out_x, double *out_y);

/*!
 * Float map of eye @p eye on a grid subsampled by @p sub (1 = full
 * resolution; 2 = NV12 chroma: output sample (x, y) sits at full-res
 * (2x + 0.5, 2y + 0.5) and the result is in half-res source coordinates).
 * @p map_xy gets 2 floats per output sample, row-major,
 * (width/sub) x (height/sub) samples. Samples whose source falls outside the
 * raw image are written as (-1, -1).
 * @return the number of valid samples.
 */
uint32_t
u_stereo_rectify_build_map(const struct u_stereo_rectify_result *r, uint32_t eye, uint32_t sub, float *map_xy);

/*!
 * Forward lens model: undistorted normalized (x, y) -> distorted normalized.
 */
void
u_stereo_rectify_distort(const struct u_stereo_rectify_lens *lens, double x, double y, double *out_x, double *out_y);

/*!
 * Inverse lens model: RAW pixel -> undistorted normalized coordinates
 * (iterative, converged to 1e-12).
 */
void
u_stereo_rectify_undistort_pixel(
    const struct u_stereo_rectify_lens *lens, double u, double v, double *out_x, double *out_y);

//! Rotation vector (axis * angle, radians) -> matrix.
void
u_stereo_rectify_rodrigues(const double rvec[3], double out[3][3]);

//! Rotation matrix -> rotation vector (axis * angle, radians).
void
u_stereo_rectify_rodrigues_inv(const double m[3][3], double out_rvec[3]);


/*
 *
 * CPU backend: fixed-point bilinear LUT over the whole SBS image.
 *
 */

//! One output sample: top-left source tap + 8-bit fractions. sx < 0 = black.
struct u_stereo_rectify_tap
{
	int16_t sx, sy;
	uint8_t wx, wy; //!< fraction * 256, 0..255 (the 256 case folds into the next tap)
};

struct u_stereo_rectify_lut
{
	uint32_t width;  //!< SBS width of this plane (samples)
	uint32_t height; //!< rows of this plane
	uint32_t sub;    //!< 1 = full-res, 2 = NV12 chroma
	uint32_t valid;  //!< taps that sample inside the raw image
	struct u_stereo_rectify_tap *taps;
};

/*!
 * Build the SBS LUT for one plane: the left eye fills x < width/2, the right
 * eye x >= width/2 (sampling its own half of the source).
 * @return false on allocation failure or an extent the tap format cannot hold.
 */
bool
u_stereo_rectify_lut_init(struct u_stereo_rectify_lut *lut, const struct u_stereo_rectify_result *r, uint32_t sub);

void
u_stereo_rectify_lut_fini(struct u_stereo_rectify_lut *lut);

/*!
 * Remap one plane of @p channels interleaved 8-bit channels (1, 2 or 4).
 * @p src / @p dst are SBS planes of lut->width x lut->height samples.
 */
void
u_stereo_rectify_lut_apply(const struct u_stereo_rectify_lut *lut,
                           uint32_t channels,
                           const uint8_t *src,
                           uint32_t src_pitch,
                           uint8_t *dst,
                           uint32_t dst_pitch);

/*!
 * Remap rows [@p y0, @p y1) only — the unit a threaded CPU path splits on.
 */
void
u_stereo_rectify_lut_apply_rows(const struct u_stereo_rectify_lut *lut,
                                uint32_t channels,
                                const uint8_t *src,
                                uint32_t src_pitch,
                                uint8_t *dst,
                                uint32_t dst_pitch,
                                uint32_t y0,
                                uint32_t y1);

#ifdef __cplusplus
}
#endif
