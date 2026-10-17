// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The gamepad provider's settings, read through the host iface
 *         (ADR-054 decision 7, the one options contract — N3c).
 *
 * Never `getenv`: every key resolves through `host->get_setting`
 * (`DXR_INPUT_GAMEPAD_<KEY>` in the hosting process's environment, the
 * per-user store, the registration row's `Settings\<KEY>` / manifest
 * `settings`), which is what `displayxr-cli input options|set|unset gamepad`
 * reads and writes. Unlike sim_input there are no legacy aliases — this
 * provider was born after N3c.
 *
 * @ingroup drv_gamepad
 */

#pragma once

#include "xrt/xrt_component_settings.h"
#include "xrt/xrt_input_plugin.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The provider's id as registered (== iface->id), what every lookup is keyed on.
#define GAMEPAD_ID "gamepad"

//! Remember the host's `get_setting` (process-lifetime storage on the host's side).
void
gamepad_settings_bind(const struct xrt_input_plugin_host_iface *host);

//! The effective value of @p key into @p buf (@p cap bytes), or false (with `buf[0] = 0`) when nothing sets it.
bool
gamepad_setting(const char *key, char *buf, size_t cap);

//! Is @p key set to something truthy? Unset falls back to @p default_on.
bool
gamepad_setting_on(const char *key, bool default_on);

//! A real-valued knob clamped to [@p lo, @p hi]; @p default_value when unset / unparseable.
float
gamepad_setting_float(const char *key, float default_value, float lo, float hi);

//! The `describe_settings` slot: this provider's declared keys.
uint32_t
gamepad_describe_settings(const struct xrt_component_setting_desc **out_descs);

#ifdef __cplusplus
}
#endif
