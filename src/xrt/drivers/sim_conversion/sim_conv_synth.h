// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Synthetic 2D→3D outputs shared by the sim conversion provider
 *         (DisplayXR-SimConversion, ADR-053) and sim_display's deprecated
 *         SIM_DISPLAY_FAKE_LIFT module (ADR-042).
 *
 * Not a model. Every output is obviously fake and cheap:
 *
 *  - VIEWS: the input, horizontally SHIFTED per view. The shift of view i is
 *    derived from ITS OWN viewpoint (sim_conv_synth_view_shifts), so explicit
 *    viewpoints are honoured exactly — an asymmetric pair renders an
 *    asymmetric parallax. Without viewpoints it falls back to the historical
 *    fixed step (2 % of the view width per view at strength 1).
 *  - DEPTH: a vertical ramp, 0 at the top.
 *  - DISPARITY: a constant.
 *
 * Two implementations of the same pictures: a CPU one (C, every platform;
 * the provider's only compute path, DETERMINISTIC), and the D3D11 shader one
 * the DP-side fake has always used (Windows only).
 *
 * @ingroup drv_sim_conversion
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Views a synthesis call can produce (== XRT_CONV_MAX_VIEWS).
#define SIM_CONV_SYNTH_MAX_VIEWS 8

//! Fixed per-view step, fraction of the view width, when no viewpoints are given.
#define SIM_CONV_SYNTH_DEFAULT_STEP 0.02f

/*!
 * Viewpoint → shift gain. The fake treats the picture as a plane that floats
 * in front of the display plane: a viewpoint x metres right of the rect
 * centre samples the input SIM_CONV_SYNTH_PARALLAX_GAIN · x / rect_width_m of
 * the view width further right, so the picture appears shifted LEFT in a
 * right-hand view (crossed disparity — the historical fake's sign). An eye
 * pair at ±31.5 mm on a 0.3 m rect yields ±1.05 %, close to the old fixed
 * ±1 % step.
 */
#define SIM_CONV_SYNTH_PARALLAX_GAIN 0.1f

/*!
 * Per-view horizontal sample offsets, as a fraction of ONE view's width.
 *
 * With @p viewpoints_xyz (x, y, z per view, metres, rect-relative) covering
 * all @p view_count views and @p rect_width_m > 0:
 *   out[i] = GAIN · strength · x_i / rect_width_m          (exact, per view)
 * With only a pair (2 ≤ viewpoint_count < view_count — the DP lift path's
 * tracked eyes) the N views are spread linearly from the first to the last.
 * else the historical symmetric fan:
 *   out[i] = (i − (n − 1) / 2) · DEFAULT_STEP · strength
 *
 * @p strength ≤ 0 is read as 1. Returns true when the viewpoints were used.
 */
bool
sim_conv_synth_view_shifts(uint32_t view_count,
                           float strength,
                           const float *viewpoints_xyz,
                           uint32_t viewpoint_count,
                           float rect_width_m,
                           float out_shift[SIM_CONV_SYNTH_MAX_VIEWS]);

/*!
 * One shifted view, CPU. 4-byte pixels (RGBA8 / BGRA8, UNORM or SRGB — bytes
 * are copied, never re-encoded); @p swap_rb swaps bytes 0 and 2 (RGBA ↔ BGRA).
 * dst(x, y) = src(clamp(x + round(shift_px · f(x, y))), y), shift_px =
 * @p shift · width, f = 1, or (1 − clamp(depth(x, y), 0, 1)) when @p depth
 * (R32F, nearer = smaller) is given: the depth-scaled shift of VIEWS_FROM_DEPTH.
 */
void
sim_conv_synth_view_rgba(const uint8_t *src,
                         uint32_t src_stride,
                         uint32_t width,
                         uint32_t height,
                         const float *depth,
                         uint32_t depth_stride,
                         float shift,
                         bool swap_rb,
                         uint8_t *dst,
                         uint32_t dst_stride);

//! R32F vertical ramp: value(y) = offset + scale · (y + 0.5) / height.
void
sim_conv_synth_depth_ramp(float *dst, uint32_t dst_stride, uint32_t width, uint32_t height, float scale, float offset);

//! R32F constant fill.
void
sim_conv_synth_fill_r32f(float *dst, uint32_t dst_stride, uint32_t width, uint32_t height, float value);

#ifdef _WIN32
/*
 * D3D11 shader implementation (the DP-side fake's GPU path). @p device /
 * @p context / resources are ID3D11Device* / ID3D11DeviceContext* /
 * ID3D11ShaderResourceView* / ID3D11RenderTargetView*.
 */
struct sim_conv_d3d11_synth;

struct sim_conv_d3d11_synth *
sim_conv_d3d11_synth_create(void *device);

void
sim_conv_d3d11_synth_destroy(struct sim_conv_d3d11_synth *s);

/*!
 * @p view_count views side by side into @p rtv (view_count · width × height),
 * view i sampling @p src_srv shifted by @p shifts[i] (sim_conv_synth_view_shifts).
 * Source alpha is kept (runtime#1742).
 */
bool
sim_conv_d3d11_synth_views(struct sim_conv_d3d11_synth *s,
                           void *context,
                           void *src_srv,
                           void *rtv,
                           uint32_t width,
                           uint32_t height,
                           uint32_t view_count,
                           const float *shifts);

//! The depth ramp (scale 1, offset 0) into an R32F @p rtv of width × height.
bool
sim_conv_d3d11_synth_depth(struct sim_conv_d3d11_synth *s, void *context, void *rtv, uint32_t width, uint32_t height);
#endif

#ifdef __cplusplus
}
#endif
