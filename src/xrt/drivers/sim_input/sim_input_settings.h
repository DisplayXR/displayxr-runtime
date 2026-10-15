// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_input's settings, read through the host iface (ADR-054
 *         decision 7, the one options contract — N3c).
 *
 * The provider never calls `getenv` for a knob: every key resolves through
 * `host->get_setting` (`DXR_INPUT_SIM_INPUT_<KEY>` in the hosting process's
 * environment, the per-user store, the registration row's `Settings\<KEY>` /
 * manifest `settings`), which is what `displayxr-cli input options|set|unset
 * sim-input` reads and writes. This file is the ONE place the legacy
 * `DXR_SIM_INPUT*` environment aliases are still honoured (one release, with
 * a deprecation WARN once per key) — the lint guard allow-lists it and
 * nothing else under `drivers/sim_input/`.
 *
 * @ingroup drv_sim_input
 */

#pragma once

#include "xrt/xrt_component_settings.h"
#include "xrt/xrt_input_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The provider's id as registered (== iface->id), what every lookup is keyed on.
#define SIM_INPUT_ID "sim-input"

/*!
 * Remember the host's `get_setting` (process-lifetime storage on the host's
 * side), when the host is new enough to carry it. Called from
 * `xrtInputPluginNegotiate`; a NULL / too-old host leaves only the legacy
 * aliases.
 */
void
sim_input_settings_bind(const struct xrt_input_plugin_host_iface *host);

/*!
 * The effective value of @p key into @p buf (@p cap bytes), or false (with
 * `buf[0] = 0`) when nothing sets it. Host first; then the legacy alias of
 * @p key, if it has one and is set in this process's environment.
 */
bool
sim_input_setting(const char *key, char *buf, size_t cap);

//! Is @p key set to something truthy? Unset / empty / "0" / "false" / "no" / "off" are off.
bool
sim_input_setting_on(const char *key);

//! A non-negative millisecond knob: 0 when unset / empty / unparseable / negative ("feature off").
int64_t
sim_input_setting_ms(const char *key);

//! The `describe_settings` slot: this provider's declared keys.
uint32_t
sim_input_describe_settings(const struct xrt_component_setting_desc **out_descs);

#ifdef __cplusplus
}
#endif
