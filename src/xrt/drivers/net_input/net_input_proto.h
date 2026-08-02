// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  net_input wire protocol — the loopback feed format.
 *
 * Version 1 of the protocol an external tracking process speaks to the
 * `net_input` provider (ADR-034 / #823, derived from the transport idea
 * of Monado's removed `remote` driver, modernized: versioned handshake,
 * per-message framing, controllers only — the head never comes from an
 * input provider).
 *
 * Transport: TCP on 127.0.0.1 (loopback ONLY — the provider binds the
 * loopback address, never a routable interface), default port
 * @ref NET_INPUT_DEFAULT_PORT. One feeder connection at a time.
 *
 * Handshake: on connect, EACH side immediately sends a
 * @ref net_input_hello and validates the peer's (magic + version).
 * Mismatch → close.
 *
 * After the handshake the feeder streams @ref net_input_state_msg
 * (one per hand per update) and reads @ref net_input_haptic_msg
 * events off the same socket whenever the runtime applies haptic
 * feedback.
 *
 * All fields are little-endian (both ends are the same host in v1;
 * spelled out for the day that changes). Every struct's size is
 * static-asserted — the structs ARE the frame format, there is no
 * separate length prefix in v1; the leading `type` field routes and
 * future versions bump @ref NET_INPUT_PROTO_VERSION rather than
 * changing v1 layouts.
 *
 * Reference feeder: `scripts/net_input_feeder.py`.
 *
 * @author David Fattal
 * @ingroup drv_net_input
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! "DXRI" in ASCII, little-endian. */
#define NET_INPUT_MAGIC 0x49525844u

#define NET_INPUT_PROTO_VERSION 1u

#define NET_INPUT_DEFAULT_PORT 9427

#define NET_INPUT_MSG_STATE 1u
#define NET_INPUT_MSG_HAPTIC 2u

/*! Hand selector used by both message types. */
#define NET_INPUT_HAND_LEFT 0u
#define NET_INPUT_HAND_RIGHT 1u

/*! Button bits in @ref net_input_state_msg::buttons. */
#define NET_INPUT_BUTTON_SELECT_BIT (1u << 0)
#define NET_INPUT_BUTTON_MENU_BIT (1u << 1)

/*!
 * Exchanged by BOTH sides immediately after connect.
 */
struct net_input_hello
{
	uint32_t magic;   /*!< @ref NET_INPUT_MAGIC */
	uint32_t version; /*!< @ref NET_INPUT_PROTO_VERSION */
};

/*!
 * Feeder → provider: one hand's tracked state.
 */
struct net_input_state_msg
{
	uint32_t type;       /*!< @ref NET_INPUT_MSG_STATE */
	uint8_t hand;        /*!< NET_INPUT_HAND_* */
	uint8_t active;      /*!< 0 = untracked/asleep, 1 = tracked */
	uint8_t buttons;     /*!< NET_INPUT_BUTTON_*_BIT mask */
	uint8_t battery_pct; /*!< 0–100; 255 = unknown */

	/*! Feeder-side monotonic sample time in ns; 0 = "stamp on
	 *  receipt". Non-zero timestamps are mapped into the provider's
	 *  clock via a latency-floor offset estimate, then pushed into
	 *  m_relation_history for timestamp-correct prediction. */
	int64_t timestamp_ns;

	float position[3];         /*!< meters, base-space */
	float orientation[4];      /*!< quaternion x,y,z,w */
	float linear_velocity[3];  /*!< m/s; zeros are fine */
	float angular_velocity[3]; /*!< rad/s, base-space; zeros are fine */

	uint32_t reserved; /*!< must be 0 */
};

/*!
 * Provider → feeder: a haptic event the runtime applied
 * (`xrApplyHapticFeedback` → `xrt_device::set_output`).
 */
struct net_input_haptic_msg
{
	uint32_t type; /*!< @ref NET_INPUT_MSG_HAPTIC */
	uint8_t hand;  /*!< NET_INPUT_HAND_* */
	uint8_t reserved[3];

	float amplitude; /*!< 0.0–1.0 */
	float frequency; /*!< Hz; 0 = unspecified */
	int64_t duration_ns;
};

/* The structs ARE the frame format — pin their layouts. */
#ifdef __cplusplus
static_assert(sizeof(struct net_input_hello) == 8, "wire layout");
static_assert(sizeof(struct net_input_state_msg) == 72, "wire layout");
static_assert(sizeof(struct net_input_haptic_msg) == 24, "wire layout");
#else
_Static_assert(sizeof(struct net_input_hello) == 8, "wire layout");
_Static_assert(sizeof(struct net_input_state_msg) == 72, "wire layout");
_Static_assert(sizeof(struct net_input_haptic_msg) == 24, "wire layout");
#endif

#ifdef __cplusplus
}
#endif
