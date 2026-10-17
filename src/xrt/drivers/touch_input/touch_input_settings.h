// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  touch_input's settings, read through the host iface (ADR-054
 *         decision 7, the one options contract).
 *
 * Never `getenv`: every key resolves through `host->get_setting` with the
 * provider's own id — `DXR_INPUT_TOUCH_INPUT_<KEY>` in the hosting process's
 * environment (on Android the `debug.xrt.DXR_INPUT_TOUCH_INPUT_<KEY>` system
 * property), the per-user store, the registration row / manifest
 * `settings`. `displayxr-cli input options touch-input` lists them.
 *
 * @ingroup drv_touch_input
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
#define TOUCH_INPUT_ID "touch-input"

//! Remember the host's `get_setting` (process-lifetime storage on the host's side).
void
touch_input_settings_bind(const struct xrt_input_plugin_host_iface *host);

//! The effective value of @p key into @p buf, or false (with `buf[0] = 0`) when unset.
bool
touch_input_setting(const char *key, char *buf, size_t cap);

/*!
 * Is @p key truthy? Unset → @p dflt; "0" / "false" / "no" / "off" (any case)
 * are off, anything else on.
 */
bool
touch_input_setting_on(const char *key, bool dflt);

//! A positive float knob: @p dflt when unset / unparseable / non-positive.
float
touch_input_setting_float(const char *key, float dflt);

//! The `describe_settings` slot: this provider's declared keys.
uint32_t
touch_input_describe_settings(const struct xrt_component_setting_desc **out_descs);

#ifdef __cplusplus
}
#endif
