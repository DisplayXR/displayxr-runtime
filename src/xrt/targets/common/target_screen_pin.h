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
	TARGET_SCREEN_PICK_NONE = 0,        //!< No candidate.
	TARGET_SCREEN_PICK_PIN = 1,         //!< `DXR_SCREEN_PLUGIN` pin for this monitor.
	TARGET_SCREEN_PICK_PREFERRED = 2,   //!< PreferredPlugin override (#791).
	TARGET_SCREEN_PICK_ACTIVE = 3,      //!< The active plug-in claims it (#1521).
	TARGET_SCREEN_PICK_CONFIDENCE = 4,  //!< Highest confidence, ties to the lower ProbeOrder.
	TARGET_SCREEN_PICK_SCREEN_PREF = 5, //!< Per-screen preference (`dp use <id> --screen <key>`).
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

/*!
 * The per-monitor winner with the per-screen display-processor preference
 * (display dashboard phase 7): a `DXR_SCREEN_PLUGIN` pin, then the per-screen
 * preference @p screen_pref, then the global @p preferred, then the active
 * plug-in, then confidence. The per-screen preference wins at ANY claim
 * confidence (a FALLBACK sim-display claim included) and so outranks the
 * active plug-in's "wins every monitor it claims" rule (#1521), but only when
 * its plug-in claims the monitor; otherwise it is ignored and
 * @p out_pref_unclaimed is set (the caller WARNs). Its plug-in id matches
 * case-insensitively (typed by hand, or written by a UI).
 *
 * @ref target_screen_pick is this with @p screen_pref NULL.
 */
int
target_screen_pick_ex(const struct target_screen_candidate *cands,
                      uint32_t count,
                      const char *pin_plugin,
                      const char *screen_pref,
                      const char *preferred,
                      enum target_screen_pick_reason *out_reason,
                      bool *out_pin_unclaimed,
                      bool *out_pref_unclaimed);

/*!
 * One monitor of a resolve, as the source-set shortcut sees it: its names (for
 * the pin match; either may be NULL/"") and whether the ACTIVE plug-in has a
 * claim on it, at any confidence.
 */
struct target_screen_monitor
{
	uint64_t monitor_id;
	const char *output_name;
	const char *connector;
	bool active_claims;
	//! The per-screen preference for this monitor (NULL/"" = none).
	const char *screen_pref;
};

/*!
 * Can the active plug-in alone decide every monitor, so that no other plug-in
 * need be loaded as a claim source (POSIX, efa3f88d0)? Mirrors
 * @ref target_screen_pick: no other plug-in can win a monitor the active one
 * claims unless it is the PreferredPlugin or a `DXR_SCREEN_PLUGIN` pin names it
 * for that monitor. So this is true only when @p active_id claims every monitor,
 * @p preferred is NULL/"" or the active plug-in itself (exact match, as in
 * target_screen_pick), and no pin that matches one of @p mons names a different
 * plug-in (case-insensitive, as in target_screen_pick), and no monitor's
 * per-screen preference (@ref target_screen_monitor::screen_pref) names a
 * different plug-in.
 *
 * Claim confidence is deliberately NOT consulted: rule 3 (the active plug-in,
 * #1521) is confidence-blind, so a FALLBACK claim by the active plug-in beats a
 * VERIFIED one from any other plug-in. Loading the others could not change the
 * outcome; only a pin can, and it is handled here.
 */
bool
target_screen_active_decides_every_monitor(const char *active_id,
                                           const char *preferred,
                                           const struct target_screen_pins *pins,
                                           const struct target_screen_monitor *mons,
                                           uint32_t count);

/*!
 * One claim source's `probe_displays` claims, flattened for
 * @ref target_screen_collect_candidates: parallel arrays, one entry per claim.
 */
struct target_screen_source_claims
{
	const char *plugin_id;       //!< The source's plug-in id.
	bool is_active;              //!< It is the active plug-in.
	const uint64_t *monitor_ids; //!< Claimed monitor per claim.
	const uint32_t *confidences; //!< Confidence per claim (parallel to @ref monitor_ids).
	uint32_t count;              //!< Claims in the arrays.
};

/*!
 * Every source's claim on @p monitor_id, in source order (ascending
 * ProbeOrder) — the candidate list @ref target_screen_pick_ex decides over,
 * and the set of plug-ins that can drive that monitor (display dashboard: the
 * per-screen selector's options). One claim per source per monitor (its
 * first). @p out_source / @p out_claim (may be NULL) receive, per candidate,
 * the source index and the claim index within that source.
 *
 * @return the number of candidates written (at most @p max).
 */
uint32_t
target_screen_collect_candidates(const struct target_screen_source_claims *sources,
                                 uint32_t source_count,
                                 uint64_t monitor_id,
                                 struct target_screen_candidate *out,
                                 uint32_t *out_source,
                                 uint32_t *out_claim,
                                 uint32_t max);

/*
 *
 * Stable screen key (display dashboard phase 7).
 *
 */

//! Size of a screen key, incl. the NUL.
#define TARGET_SCREEN_KEY_MAX 64

/*!
 * One monitor's identity, as the key derivation sees it.
 */
struct target_screen_key_input
{
	uint16_t manufacturer_id; //!< EDID manufacturer id, as stored (little-endian PNP packing).
	uint16_t product_id;      //!< EDID product code.
	uint32_t serial;          //!< EDID serial number (0 = none).
	const char *device_name;  //!< OS device name (Windows GDI name, Linux connector, macOS display UUID).
};

/*!
 * Decode an EDID manufacturer id (stored little-endian; the spec packs it
 * big-endian as three 5-bit letters) into its 3-letter PNP code ("AUO"); a
 * letter outside A-Z becomes '?'.
 */
void
target_screen_pnp_code(uint16_t manufacturer_id, char out[4]);

/*!
 * Derive every monitor's stable screen key, the identity a per-screen
 * preference is stored under. Unlike the per-boot `monitor_id` it survives a
 * reboot and a desktop re-arrangement: `"<PNP>-<PROD>-<SERIAL>"`, the PNP
 * code, the product as 4 upper-case hex digits and the EDID serial as 8
 * (`"AUO-1234-0000ABCD"`). When the serial is 0, or two monitors of @p in
 * would get the same key, that monitor's key gets `"@<device_name>"` appended
 * (`"AUO-1234-00000000@DISPLAY2"`), unique as long as the OS device names are.
 * A Windows GDI name loses its `\\.\` namespace prefix there, so a key never
 * needs shell escaping. Pure.
 */
void
target_screen_keys_build(const struct target_screen_key_input *in, uint32_t n, char (*out)[TARGET_SCREEN_KEY_MAX]);

#ifdef __cplusplus
}
#endif
