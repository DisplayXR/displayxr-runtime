// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_stylus's settings, read ONLY through the host iface (ADR-054
 *         decision 7, the one options contract).
 *
 * Every key resolves through `host->get_setting`: `DXR_INPUT_SIM_STYLUS_<KEY>`
 * in the hosting process's environment, the per-user store, the registration
 * row's `Settings\<KEY>` / manifest `settings` — what `displayxr-cli input
 * options|set|unset sim-stylus` reads and writes. No `getenv` anywhere in the
 * provider (a new provider has no legacy aliases to honour).
 *
 * @ingroup drv_sim_stylus
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
#define SIM_STYLUS_ID "sim-stylus"

//! Remember the host's `get_setting`, when the host is new enough to carry it.
void
sim_stylus_settings_bind(const struct xrt_input_plugin_host_iface *host);

//! The effective value of @p key into @p buf, or false (with `buf[0] = 0`) when nothing sets it.
bool
sim_stylus_setting(const char *key, char *buf, size_t cap);

//! Is @p key set to something truthy? Unset = @p fallback; "0" / "false" / "no" / "off" are off.
bool
sim_stylus_setting_bool(const char *key, bool fallback);

//! A real-valued knob: @p fallback when unset / empty / unparseable.
float
sim_stylus_setting_float(const char *key, float fallback);

//! A millisecond knob: @p fallback when unset / unparseable; negative clamps to 0.
int64_t
sim_stylus_setting_ms(const char *key, int64_t fallback);

//! The `describe_settings` slot: this provider's declared keys.
uint32_t
sim_stylus_describe_settings(const struct xrt_component_setting_desc **out_descs);

#ifdef __cplusplus
}
#endif
