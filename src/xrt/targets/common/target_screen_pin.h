// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen plug-in pin (`DXR_SCREEN_PLUGIN`) and the per-monitor
 *         winner rule of the DP registry (multi-screen M0/M4, #793 phase 3).
 * @ingroup targets_common
 *
 * `DXR_SCREEN_PLUGIN=<match>=<plugin-id>[,<match>=<plugin-id>...]` pins ONE
 * monitor to a plug-in. `<match>` is an OS output / connector name ("HDMI-1",
 * "HDMI-A-1", "eDP-1", case-insensitive) or a monitor id in hex ("0x886e…" or
 * bare). For that monitor only, a pin outranks every global rule — the
 * PreferredPlugin override (#791) and the active-plug-in rule (#1521) — as long
 * as the pinned plug-in claims the monitor; everything else resolves exactly as
 * before. That is what lets a sim-display session (`XRT_PREFERRED_PLUGIN_ID=
 * sim-display`) keep the DS1 woven by leia-sr: `DXR_SCREEN_PLUGIN=HDMI-1=leia-sr`.
 *
 * Pure functions (no loader state), unit-tested by
 * `tests/tests_target_screen_pin.cpp`; target_plugin_resolve_displays() uses them.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TARGET_SCREEN_PIN_MAX 8
#define TARGET_SCREEN_PIN_STR 64

struct target_screen_pin
{
	char match[TARGET_SCREEN_PIN_STR];
	char plugin_id[TARGET_SCREEN_PIN_STR];
};

struct target_screen_pins
{
	uint32_t count;
	struct target_screen_pin pin[TARGET_SCREEN_PIN_MAX];
};

/*!
 * Parse a `DXR_SCREEN_PLUGIN` value. Entries are comma-separated `match=id`;
 * whitespace around either side is trimmed. An entry without `=`, with an empty
 * side, or past @ref TARGET_SCREEN_PIN_MAX is skipped and counted in
 * @p out_malformed (may be NULL). NULL / empty spec = no pins.
 *
 * @return the number of pins parsed.
 */
uint32_t
target_screen_pin_parse(const char *spec, struct target_screen_pins *out, uint32_t *out_malformed);

/*!
 * Does @p pin name this monitor? By output name or connector (either may be
 * NULL/""), case-insensitive, or by its id in hex (`0x` optional).
 */
bool
target_screen_pin_matches(const struct target_screen_pin *pin,
                          uint64_t monitor_id,
                          const char *output_name,
                          const char *connector);

/*!
 * The first pin naming this monitor, or -1.
 */
int
target_screen_pin_find(const struct target_screen_pins *pins,
                       uint64_t monitor_id,
                       const char *output_name,
                       const char *connector);

/*!
 * One plug-in's claim on one monitor, as the winner rule sees it. Candidates
 * are listed in source order (ascending ProbeOrder).
 */
struct target_screen_candidate
{
	const char *plugin_id;
	uint32_t confidence;
	bool is_active;
};

enum target_screen_pick_reason
{
	TARGET_SCREEN_PICK_NONE = 0,       //!< No candidate.
	TARGET_SCREEN_PICK_PIN = 1,        //!< `DXR_SCREEN_PLUGIN` pin for this monitor.
	TARGET_SCREEN_PICK_PREFERRED = 2,  //!< PreferredPlugin override (#791).
	TARGET_SCREEN_PICK_ACTIVE = 3,     //!< The active plug-in claims it (#1521).
	TARGET_SCREEN_PICK_CONFIDENCE = 4, //!< Highest confidence, ties to the lower ProbeOrder.
};

/*!
 * The per-monitor winner: a pin (if its plug-in claims the monitor), then the
 * preferred plug-in, then the active one, then confidence. @p pin_plugin and
 * @p preferred may be NULL.
 *
 * @param[out] out_pin_unclaimed  Set when a pin was given but its plug-in has
 *                                no claim on this monitor (the caller WARNs).
 * @return the winning candidate index, or -1.
 */
int
target_screen_pick(const struct target_screen_candidate *cands,
                   uint32_t count,
                   const char *pin_plugin,
                   const char *preferred,
                   enum target_screen_pick_reason *out_reason,
                   bool *out_pin_unclaimed);

#ifdef __cplusplus
}
#endif
