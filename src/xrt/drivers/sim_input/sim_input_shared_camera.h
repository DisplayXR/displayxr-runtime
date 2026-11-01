// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_input `SOURCE=shared-camera`: presence follows the display's
 *         shared stereo camera (the host iface's open_shared_camera slot).
 *
 * The reference consumer of the shared-camera host slot (ADR-043;
 * tracking-provider-discovery.md §10 item 2): it opens the camera ON DEMAND
 * (from create_devices, never probe), a small poll thread acquires frames
 * with a short timeout and releases each one, and the provider's presence
 * follows the stream — frames flowing = PRESENT (the scripted hands run as
 * with `SOURCE=script`); WAITING (no 3D session holds the display's
 * tracker), no frame for more than 500 ms, or no camera at all = ABSENT, so
 * the arbiter falls through to the next hands provider. The hand script
 * itself is untouched: the camera decides only WHETHER the hands exist.
 *
 * One WARN per transition, never per frame.
 *
 * @ingroup drv_sim_input
 */

#pragma once

#include "xrt/xrt_input_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Start the poll thread over @p host's shared-camera slots (idempotent). A
 * host without the slots (an older runtime) leaves the provider ABSENT with
 * one WARN.
 */
void
sim_input_shared_camera_start(const struct xrt_input_plugin_host_iface *host);

//! Stop the thread and close the stream (idempotent).
void
sim_input_shared_camera_stop(void);

//! PRESENT while frames flow, else ABSENT. Non-blocking.
enum xrt_input_provider_presence
sim_input_shared_camera_presence(void);

#ifdef __cplusplus
}
#endif
