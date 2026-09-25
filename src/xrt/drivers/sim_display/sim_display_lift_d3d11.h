// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's env-gated FAKE 2D→3D conversion module
 *         (SIM_DISPLAY_FAKE_LIFT=1; XR_DXR_lift, ADR-042).
 *
 * sim_display ships no conversion module, so by default its lift slots stay
 * NULL and the runtime reports supportedModes 0. With SIM_DISPLAY_FAKE_LIFT=1
 * the D3D11 DP installs this fake so the whole path — lift thread, mailbox,
 * ring, IPC, weave-rect lifting, blob transport — can run hardware-free:
 *
 *  - SBS / NVIEW: the input, horizontally SHIFTED per view (a constant
 *    parallax, not depth-aware), views side by side.
 *  - DEPTH: a vertical gradient (R32_FLOAT, 0 at the top, 1 at the bottom).
 *  - GAUSSIANS: a tiny valid two-layer 3DGS PLY (sim_display_fake_ply.h).
 *
 * SIM_DISPLAY_FAKE_LIFT_LATENCY_MS (default 8) sleeps inside each convert so
 * the runtime's asynchrony and latest-wins dropping are observable.
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include "xrt/xrt_dp_lift.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sim_fake_lift;

//! True when SIM_DISPLAY_FAKE_LIFT is set.
bool
sim_fake_lift_enabled(void);

//! Create the fake on @p d3d11_device (ID3D11Device*). NULL on failure.
struct sim_fake_lift *
sim_fake_lift_create(void *d3d11_device);

void
sim_fake_lift_destroy(struct sim_fake_lift *fl);

bool
sim_fake_lift_get_caps(struct sim_fake_lift *fl, struct xrt_dp_lift_caps *out);

bool
sim_fake_lift_stream_create(struct sim_fake_lift *fl, const struct xrt_dp_lift_stream_info *info, uint64_t *out_id);

void
sim_fake_lift_stream_destroy(struct sim_fake_lift *fl, uint64_t id);

bool
sim_fake_lift_convert(struct sim_fake_lift *fl,
                      uint64_t id,
                      void *d3d11_context,
                      void *input_resource,
                      uint32_t w,
                      uint32_t h,
                      const struct xrt_dp_lift_params *p,
                      void **out_resource,
                      uint32_t *out_w,
                      uint32_t *out_h,
                      uint32_t *out_format);

bool
sim_fake_lift_convert_blob(struct sim_fake_lift *fl,
                           uint64_t id,
                           void *d3d11_context,
                           void *input_resource,
                           uint32_t w,
                           uint32_t h,
                           uint32_t *out_format,
                           const void **out_bytes,
                           size_t *out_size);

#ifdef __cplusplus
}
#endif
