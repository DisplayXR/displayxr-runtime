// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to the touch input provider.
 *
 * touch_input is the first Android INPUT provider (ADR-034 / #132): on a flat
 * 3D display there is no tracked head or hand, so content needs a synthesised
 * rig — on desktops the qwerty floor builds one from the keyboard and mouse,
 * and on a tablet the only thing the user has is the touch screen. The
 * provider turns the pointer events the runtime drains from its surfaces
 * (@ref xrt_input_plugin_host_iface::poll_pointer_events) into
 *
 *   - a NAVIGATION device (the rig role, ADR-034 Amendment 4): one-finger
 *     drag orbits, two-finger pinch dollies, two-finger drag pans, double
 *     tap recenters — `touch_input_nav.c`;
 *   - an emulated `khr/simple_controller` whose aim ray runs from the nominal
 *     viewer through the finger and whose SELECT is a tap / a press-and-hold,
 *     so an action-based OpenXR app gets `select` with no code of its own —
 *     `touch_input_controller.c`.
 *
 * Every gesture rule lives in `touch_gesture.{h,c}` (platform-free, covered
 * by tests_touch_gesture). Ships as `libdxri100_touch_input.so` in the
 * runtime APK (loader prefix `libdxri`); desktop builds get the same plug-in
 * (`DisplayXR-TouchInput`) for development, where `probe()` declines unless
 * its `ENABLE` setting is 1, because nothing feeds pointer events there yet.
 *
 * @ingroup drv_touch_input
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_input_plugin.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct touch_input_provider;

/*!
 * Pump the provider: drain the host's pointer queue into the recogniser and
 * advance it to @p now_ns. Both devices call this from `update_inputs`
 * (`xrSyncActions` sweeps every device, so it runs at frame rate whichever
 * device is polled first); it is idempotent within a poll and thread-safe.
 */
void
touch_input_pump(struct touch_input_provider *tp, int64_t now_ns);

/*!
 * Create the NAVIGATION device for @p tp. Poses are absolute in the provider's
 * frame F (`touch_gesture.h`), always valid; RECENTER is the durable
 * level-with-timestamp of the contract, raised by a double tap.
 */
struct xrt_device *
touch_input_create_navigation(struct touch_input_provider *tp);

/*!
 * Create the emulated controller for @p tp: `XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER`
 * binding `khr/simple_controller`, RIG_LOCAL poses (a provider that navigates
 * publishes display-plane-relative poses, ADR-034 Amendment 4).
 */
struct xrt_device *
touch_input_create_controller(struct touch_input_provider *tp);

#ifdef __cplusplus
}
#endif
