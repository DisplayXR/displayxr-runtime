// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  webcam eye tracking (#1855, sim_tracking SOURCE=webcam): the pure, inference-free
 *         halves of the YuNet face detector: frame -> input tensor, and
 *         output tensors -> the two eye landmarks.
 *
 * YuNet (opencv_zoo `face_detection_yunet_2026may.onnx`, MIT) takes a
 * 1 x 3 x H x W float tensor of BGR values in 0..255 (no mean, no scale; H and
 * W multiples of 32) and returns, per stride s in {8, 16, 32}, one row per
 * grid cell (row-major over (H/s) x (W/s)): `cls_s` [1], `obj_s` [1],
 * `bbox_s` [4] and `kps_s` [10] (right eye, left eye, nose, right and left
 * mouth corner, from the subject's point of view). A cell's score is
 * sqrt(cls * obj), each clamped to 0..1; keypoint n is
 * ((kps[2n] + col) * s, (kps[2n+1] + row) * s).
 *
 * Kept free of ONNX Runtime so the default build compiles and unit-tests it:
 * only sim_face_estimator_onnx.c (XRT_SIM_WEBCAM_ESTIMATOR=onnx)
 * runs the network between these two calls.
 *
 * @ingroup drv_sim_tracking
 */

#pragma once

#include "sim_webcam_tracking.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_stereo_uvc_raw_frame;

//! Width the frame is reduced to before inference: faces at desk distance
//! stay inside YuNet's ~10..300 px training range, and a 320 x 256 pass is a
//! few milliseconds on one CPU core.
#define SIM_YUNET_NET_WIDTH 320u

//! Minimum sqrt(cls * obj) for a detection to count as a face.
#define SIM_YUNET_SCORE_MIN 0.6f

//! Output heads, in stride order 8, 16, 32.
#define SIM_YUNET_STRIDES 3

//! How a source frame maps onto the network input.
struct sim_yunet_plan
{
	uint32_t src_w, src_h; //!< the camera frame
	uint32_t net_w, net_h; //!< the scaled image inside the tensor (top-left)
	uint32_t pad_w, pad_h; //!< the tensor: net_* rounded up to a multiple of 32
	float src_per_net;     //!< source pixels per network pixel (>= 1)
};

/*!
 * Plan the reduction of a @p src_w x @p src_h frame to at most
 * @p max_net_w wide (aspect kept; never upscaled). @return false for an empty
 * or absurd frame.
 */
bool
sim_yunet_make_plan(uint32_t src_w, uint32_t src_h, uint32_t max_net_w, struct sim_yunet_plan *out);

//! Floats the tensor of @p plan holds (3 * pad_h * pad_w).
size_t
sim_yunet_tensor_floats(const struct sim_yunet_plan *plan);

/*!
 * Fill @p tensor (sim_yunet_tensor_floats() floats, NCHW) from the luma of
 * @p frame (NV12 or YUY2), bilinearly reduced per @p plan and replicated into
 * all three channels; the padding is zero. Grey input costs YuNet little
 * accuracy and spares a colour conversion. @return false on a format or size
 * mismatch.
 */
bool
sim_yunet_fill_tensor(const struct u_stereo_uvc_raw_frame *frame, const struct sim_yunet_plan *plan, float *tensor);

//! One stride's output tensors, as the network returned them.
struct sim_yunet_head
{
	const float *cls; //!< [rows][1]
	const float *obj; //!< [rows][1]
	const float *kps; //!< [rows][10]
	size_t rows;      //!< must equal (pad_h / stride) * (pad_w / stride)
};

/*!
 * Pick the best-scoring face across the three strides (8, 16, 32, in that
 * order in @p heads) and report its two eye keypoints in SOURCE-frame pixels.
 * @p out->found is false when nothing reaches @p score_min.
 * @return false when a head's row count does not match @p plan (a model that
 *         is not the one this code was written for).
 */
bool
sim_yunet_decode(const struct sim_yunet_plan *plan,
                 const struct sim_yunet_head heads[SIM_YUNET_STRIDES],
                 float score_min,
                 struct sim_face_landmarks *out);

#ifdef __cplusplus
}
#endif
