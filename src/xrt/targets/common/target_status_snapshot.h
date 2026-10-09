// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Headless builder of the display status snapshot (ADR-051 D3,
 *         "headless fallback").
 *
 * Lives next to its dependencies — the plug-in loader, the per-monitor
 * registry and the screen-list builder — so `aux_util` (which holds the pure
 * warning / JSON / text helpers, `util/u_status_snapshot.h`) never depends on
 * `targets/common`.
 *
 * @ingroup targets_common
 */

#pragma once

#include "xrt/xrt_display_status.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_instance;
struct xrt_system_devices;

/*!
 * Build the snapshot a process starting now would get, in-process: runtime
 * identity, every registered plug-in with its load result + platform state +
 * hint, and one row per screen of the per-monitor registry (identity, EDID,
 * desktop rect + scale, native mode, millimetres, roles, the claim with its
 * serial and per-API factories, metres + nominal viewer, eye-tracking caps).
 * `source = HEADLESS`, generation 0, no clients, no live DP state (every
 * screen `NO_DP`), vendor cells absent (phase 3). Warnings are derived last.
 *
 * Passive (ADR-051 D5): reads the registry and display info the loader
 * already has; creates no DP, tracker or window.
 *
 * @param xi    The headless instance (`xrt_instance_create` with no compositor). Its
 *              `enumerate_displays` is used when it has a system with a compositor;
 *              otherwise the list is built from a freshly resolved registry.
 * @param xsysd The instance's system devices, for the head the active plug-in
 *              describes its panel against (may be NULL: then the default screen
 *              carries no system display info).
 * @param out   Filled completely (zeroed first).
 */
void
target_status_snapshot_build_headless(struct xrt_instance *xi,
                                      struct xrt_system_devices *xsysd,
                                      struct xrt_status_snapshot *out);

#ifdef __cplusplus
}
#endif
