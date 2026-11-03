// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061: the service-side synthetic app-share source
 *         (DXR_APP_SHARE_FAKE=1) — an NV12 side-by-side frame with a known
 *         horizontal disparity ramp and a zero-disparity marker band.
 *
 * What a real app share delivers, without an app, a GPU or a platform tap, so
 * enumerate -> consent -> start -> acquire -> frame info runs hardware-free in
 * CI (tests_stereo_camera_manager) and `displayxr-cli share probe` has a
 * deterministic target:
 *  - each eye is a block texture that scrolls 2 px per frame (frames differ);
 *  - the right eye's row y is the left's shifted by d(y) = D * (y - cy) / cy,
 *    D = U_APP_SHARE_FAKE_MAX_DISPARITY_FRAC * eye width: disparity (right x
 *    minus left x) ramps from -D at the top to +D at the bottom, zero at the
 *    centre — display-referred, like a real app's off-axis views;
 *  - the rows of the centre band (u_app_share_fake_band) have d = 0 exactly
 *    and are framed by two bright lines: the "zero-disparity marker" a probe
 *    measures (|d| ~ 0 there, the XrAppShareFrameInfoDXR value is 0).
 *
 * Portable C, no OS calls but the monotonic clock and a sleep.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! |disparity| at the top / bottom row, as a fraction of the eye width.
#define U_APP_SHARE_FAKE_MAX_DISPARITY_FRAC 0.02f
//! Height of the zero-disparity band, as a fraction of the eye height.
#define U_APP_SHARE_FAKE_BAND_FRAC 0.10f

struct u_app_share_fake;

struct u_app_share_fake_frame
{
	uint64_t sequence; //!< from 1
	int64_t time_ns;   //!< os_monotonic
	uint32_t width;    //!< full SBS (2 * eye width)
	uint32_t height;
	const uint8_t *planes[2]; //!< NV12: Y, interleaved UV
	uint32_t pitches[2];
};

/*!
 * @p eye_w / @p eye_h even, >= 16; @p fps > 0. NULL on a bad size or no
 * memory.
 */
struct u_app_share_fake *
u_app_share_fake_create(uint32_t eye_w, uint32_t eye_h, float fps);

void
u_app_share_fake_destroy(struct u_app_share_fake **f_ptr);

/*!
 * Wait until the next frame is due (at most @p timeout_ns), then render it.
 * false = not due within the timeout. The frame stays valid until the next call.
 */
bool
u_app_share_fake_next(struct u_app_share_fake *f, int64_t timeout_ns, struct u_app_share_fake_frame *out);

//! Expected disparity (right x - left x, px) of eye row @p y.
float
u_app_share_fake_disparity_px(uint32_t eye_w, uint32_t eye_h, uint32_t y);

//! The zero-disparity band: rows [@p out_y0, @p out_y1) of each eye.
void
u_app_share_fake_band(uint32_t eye_h, uint32_t *out_y0, uint32_t *out_y1);

#ifdef __cplusplus
}
#endif
