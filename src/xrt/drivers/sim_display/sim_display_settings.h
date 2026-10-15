// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's settings, read through the host iface (ADR-054
 *         decision 7, the one options contract — N3c).
 *
 * The plug-in never calls `getenv` for a knob: every key resolves through
 * `host->get_setting` (`DXR_DP_SIM_DISPLAY_<KEY>` in the hosting process's
 * environment, the per-user store, the registration row's `Settings\<KEY>` /
 * manifest `settings`), which is what `displayxr-cli dp options|set|unset
 * sim-display` reads and writes. Values are resolved ONCE per key per
 * process and cached (the DEBUG_GET_ONCE contract these replaced; several
 * are read on hot paths), so a write applies to the next process that loads
 * the plug-in.
 *
 * This file is the ONE place the legacy `SIM_DISPLAY_*` environment names
 * are still honoured (one release, with a deprecation WARN once per key):
 * `SIM_DISPLAY_FAKE_TRACKING` → `TRACKING`, `SIM_DISPLAY_FAKE_TRACKING_PERIOD_MS`
 * → `TRACKING_PERIOD_MS`, `SIM_DISPLAY_<X>` → `<X>` otherwise (the `FAKE_`
 * prefix dropped). The lint guard allow-lists it and nothing else under
 * `drivers/sim_display/`.
 *
 * @ingroup drv_sim_display
 */

#pragma once

#include "xrt/xrt_component_settings.h"
#include "xrt/xrt_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The plug-in's id as registered (== iface->id), what every lookup is keyed on.
#define SIM_DISPLAY_ID "sim-display"

/*!
 * Remember the host's `get_setting` (process-lifetime storage on the host's
 * side), when the host is new enough to carry it. Called from
 * `xrtPluginNegotiate`; a NULL / too-old host leaves only the legacy
 * aliases. Clears the per-key cache.
 */
void
sim_display_settings_bind(const struct xrt_plugin_host_iface *host);

/*!
 * The effective value of @p key into @p buf (@p cap bytes), or false (with
 * `buf[0] = 0`) when nothing sets it. Host first; then the legacy
 * `SIM_DISPLAY_*` alias, if set in this process's environment. Cached per
 * key after the first resolution.
 */
bool
sim_display_setting(const char *key, char *buf, size_t cap);

//! Is anything (host or alias) setting @p key? The former "environment variable present" test.
bool
sim_display_setting_is_set(const char *key);

//! Is @p key set to something truthy? Unset / empty / "0" / "false" / "no" / "off" are off.
bool
sim_display_setting_on(const char *key);

//! @p key as a number, or @p def when unset / unparseable (the DEBUG_GET_ONCE_NUM_OPTION contract).
long
sim_display_setting_num(const char *key, long def);

//! @p key as a float, or @p def when unset / unparseable (the DEBUG_GET_ONCE_FLOAT_OPTION contract).
float
sim_display_setting_float(const char *key, float def);

//! The `describe_settings` slot: this plug-in's declared keys.
uint32_t
sim_display_describe_settings(const struct xrt_component_setting_desc **out_descs);

#ifdef __cplusplus
}
#endif
