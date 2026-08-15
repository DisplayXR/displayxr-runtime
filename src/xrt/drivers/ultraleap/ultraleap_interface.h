// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to the Ultraleap (Leap Motion) input provider.
 *
 * Tier-1 hand-as-motion-controller provider (#825, on the #823 /
 * ADR-034 channel): LeapC (Gemini) hand frames drive two
 * `khr/simple_controller` devices — palm pose → grip/aim, pinch →
 * select, grab → menu — and each device also carries the full 26-joint
 * `xrt_hand_joint_set` via `get_hand_tracking`, so the Tier-2
 * (`XR_EXT_hand_tracking`) runtime wiring needs no provider changes.
 *
 * Adapted from Monado's removed `ultraleap_v5` driver (restored from
 * the pre-strip history). Ships as the plug-in DLL
 * `DisplayXR-Ultraleap`; the target only builds where the Ultraleap
 * Gemini SDK (LeapC) is found at configure time.
 *
 * @author David Fattal
 * @ingroup drv_ultraleap
 */

#pragma once

#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;

/*!
 * Create the Ultraleap hub (LeapC connection + poll thread) and its
 * left+right motion-controller devices. Hub lifetime is refcounted onto
 * the two devices, like net_input. Fails only if the LeapC connection
 * cannot be created at all (tracking service absent) — with the service
 * up but no device plugged in, the devices exist and report untracked
 * until hands appear, mirroring net_input's no-feeder semantics.
 *
 * Before returning it waits a bounded settle window
 * (`DXR_ULTRALEAP_SETTLE_MS`, default 300) for the LeapC connection to
 * say whether a device is actually attached, so the runtime's very first
 * role arbitration usually gets a definitive answer rather than
 * @ref XRT_INPUT_PROVIDER_PRESENCE_UNKNOWN.
 *
 * @ingroup drv_ultraleap
 */
xrt_result_t
ul_create_devices(struct xrt_device **out_left, struct xrt_device **out_right);

/*!
 * Is a Leap Motion device actually attached right now? Non-blocking
 * mutex read of state the poll thread maintains from LeapC's
 * `Device` / `DeviceLost` / `ConnectionLost` events — this is what backs
 * @ref xrt_input_plugin_iface::get_presence.
 *
 * Reports @ref XRT_INPUT_PROVIDER_PRESENCE_PRESENT while a device is
 * attached even when no hand is in view: an empty tracking volume is
 * inactive inputs, not absent hardware.
 *
 * @ingroup drv_ultraleap
 */
enum xrt_input_provider_presence
ul_get_presence(void);

#ifdef __cplusplus
}
#endif
