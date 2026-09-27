// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's FAKE 2D→3D conversion module for the Vulkan / Android
 *         lift slots (XR_DXR_lift, ADR-042) — the Android twin of
 *         sim_display_lift_d3d11.h.
 *
 * sim_display ships no conversion module. With the fake enabled its lift-only
 * Vulkan DP factory (xrt_plugin_iface::create_dp_vk_lift) returns a DP that
 * fills the five XRT_DP_VK_HAS_LIFT slots, so the whole Android path — lift
 * thread, AHardwareBuffer mailbox + ring, IPC, weave-rect lifting, blob
 * transport — can run on a device without a vendor module:
 *
 *  - SBS / NVIEW: the input, horizontally SHIFTED per view (a constant
 *    parallax, not depth-aware), views side by side, RGBA8.
 *  - DEPTH: a vertical gradient, RELATIVE (larger = farther): 0 at the top,
 *    255 at the bottom, in R (and G, B) of an RGBA8 buffer — AHardwareBuffer
 *    has no single-channel float format.
 *  - GAUSSIANS: the same tiny two-layer 3DGS PLY as the D3D11 fake
 *    (sim_display_fake_ply.h), front layer coloured from the photo.
 *
 * The fake is deliberately CPU-only: it reads the input through
 * AHardwareBuffer_lock (the runtime allocates it CPU_READ_OFTEN precisely so a
 * CPU / GLES module can) and writes a CPU_WRITE_OFTEN output buffer, returning
 * NO VkImage — which exercises the runtime's "import the DP's output
 * AHardwareBuffer itself" branch, the shape an ImageReader-based vendor module
 * produces.
 *
 * Enabled by SIM_DISPLAY_FAKE_LIFT=1 in the environment OR the Android system
 * property `debug.dxr.lift.fake=1` (getenv does not reach the Android service
 * process). Latency: SIM_DISPLAY_FAKE_LIFT_LATENCY_MS / `debug.dxr.lift.fake_latency_ms`
 * (default 8) sleeps inside each convert so asynchrony and latest-wins drops
 * are observable.
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_display_processor;

#ifdef XRT_OS_ANDROID
//! True when the fake is enabled (env / debug.xrt.SIM_DISPLAY_FAKE_LIFT, or debug.dxr.lift.fake; read once).
bool
sim_fake_lift_vk_enabled(void);

/*!
 * sim_display's lift-only Vulkan DP factory (xrt_dp_factory_vk_fn_t). Returns
 * a DP carrying ONLY the lift slots (+ destroy): no weaver, no pipelines.
 * Refuses (XRT_ERROR_FEATURE_NOT_SUPPORTED) when the fake is disabled, which
 * the runtime reports as UNAVAILABLE.
 */
xrt_result_t
sim_display_dp_factory_vk_lift(void *vk_bundle,
                               void *vk_cmd_pool,
                               void *window_handle,
                               int32_t target_format,
                               struct xrt_display_processor **out_xdp);
#endif

#ifdef __cplusplus
}
#endif
