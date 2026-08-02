// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to the network-fed input provider.
 *
 * net_input is the second in-tree reference INPUT provider (ADR-034 /
 * #823): a loopback-TCP hub that lets an EXTERNAL process feed
 * timestamped motion-controller poses + button state into the runtime
 * and receive haptic events back — the integration seam for tracking
 * systems that cannot (or don't want to) ship a plug-in DLL themselves.
 * Wire protocol: `net_input_proto.h` + the discovery spec §5; reference
 * feeder: `scripts/net_input_feeder.py`.
 *
 * Derived from Monado's removed `remote` driver (r_hub/r_device),
 * modernized: versioned handshake, per-message framing, controllers
 * only (no head), poses pushed into `m_relation_history` and predicted
 * at the requested timestamp.
 *
 * @author David Fattal
 * @ingroup drv_net_input
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;

/*!
 * Create the net_input hub (loopback listen socket + feed thread) and
 * its left+right motion-controller devices. The hub's lifetime is
 * refcounted onto the two devices — when the runtime destroys the last
 * one, the thread is stopped and the sockets closed.
 *
 * @param port TCP port to listen on (loopback only); 0 = default
 *             @ref NET_INPUT_DEFAULT_PORT.
 *
 * @ingroup drv_net_input
 */
xrt_result_t
net_input_create_devices(uint16_t port, struct xrt_device **out_left, struct xrt_device **out_right);

#ifdef __cplusplus
}
#endif
