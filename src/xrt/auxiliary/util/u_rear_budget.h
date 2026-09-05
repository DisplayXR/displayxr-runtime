// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Rear depth budget policy state machine (XR_DXR_depth_budget).
 *
 * How far behind the zero-disparity plane a transparent-background app may
 * render, expressed in vH (virtual display heights): 0 = clip at the ZDP,
 * @ref U_REAR_BUDGET_UNRESTRICTED_VH = unrestricted.
 *
 * The runtime owns this policy: the display processor supplies pixels
 * (@ref xrt_dp_background_preview), @ref u_bg_neutrality_analyse turns one
 * frame of them into a number, and this state machine turns a stream of those
 * numbers into a stable, ramped budget. Vendor code never decides perception;
 * apps never analyse the desktop.
 *
 * One instance per native compositor session (in-process) or per service
 * client (IPC). Pure C: time is a PARAMETER, never read here, so the whole
 * machine is deterministically testable.
 *
 * @author David Fattal
 * @ingroup aux_util
 */

#pragma once

#include "util/u_bg_neutrality.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The "no restriction" budget, in virtual display heights.
#define U_REAR_BUDGET_UNRESTRICTED_VH 1000.0f

/*!
 * Why the current budget is what it is. Kept in lockstep with the extension's
 * `XrRearDepthBudgetStateDXR` — the numeric values are the same.
 *
 * @ingroup aux_util
 */
enum u_rear_budget_state
{
	//! Session is not transparent — nothing composites over the desktop.
	U_REAR_BUDGET_UNRESTRICTED_OPAQUE = 0,
	//! Transparent, but under a workspace controller (today's behaviour).
	U_REAR_BUDGET_UNRESTRICTED_WORKSPACE = 1,
	//! Transparent + standalone, background carries no horizontal cue.
	U_REAR_BUDGET_OPEN = 2,
	//! Transparent + standalone, background is busy.
	U_REAR_BUDGET_CLIPPED_BUSY_BACKGROUND = 3,
	//! No usable background preview (absent or declined).
	U_REAR_BUDGET_CLIPPED_NO_SOURCE = 4,
	//! An environment override is pinning the budget.
	U_REAR_BUDGET_FORCED = 5,
};

/*!
 * Environment override for the whole policy.
 *
 * @ingroup aux_util
 */
enum u_rear_budget_force
{
	U_REAR_BUDGET_FORCE_AUTO = 0, //!< Run the policy (default).
	U_REAR_BUDGET_FORCE_CLIP = 1, //!< Pin to 0 vH.
	U_REAR_BUDGET_FORCE_OPEN = 2, //!< Pin to unrestricted.
};

/*!
 * The tunables, in one place. Defaults via
 * @ref u_rear_budget_tuning_defaults; overrides via
 * @ref u_rear_budget_tuning_from_env.
 *
 * @ingroup aux_util
 */
struct u_rear_budget_tuning
{
	//! Neutral must hold continuously this long before opening. Default 400.
	uint32_t open_dwell_ms;
	/*!
	 * Busy must hold this long before closing. Default 100 — deliberately
	 * much shorter than @ref open_dwell_ms: a visible depth conflict is
	 * worse than a missing rear.
	 */
	uint32_t close_ms;
	//! Time for a full 0 → unrestricted slide. Default 300.
	uint32_t ramp_open_ms;
	//! Time for a full unrestricted → 0 slide. Default 150.
	uint32_t ramp_close_ms;
	//! @ref u_rear_budget_force.
	int force;
};

/*!
 * One evaluation's inputs.
 *
 * @ingroup aux_util
 */
struct u_rear_budget_in
{
	//! xrt_session_info::transparent_background_enabled.
	bool transparent;
	//! Running under a workspace controller / external multi-compositor.
	bool under_workspace;
	//! The DP answered a background-preview query this cycle.
	bool source_available;
	//! True when @ref result holds a fresh analysis.
	bool have_result;
	//! Preview generation the analysis came from (re-analysis gate: unchanged means still valid).
	uint32_t generation;
	//! Latest analysis; read only when @ref have_result.
	struct u_bg_neutrality_result result;
};

/*!
 * One evaluation's outputs — exactly the app-facing triple.
 *
 * @ingroup aux_util
 */
struct u_rear_budget_out
{
	//! 0 = clip at the ZDP; @ref U_REAR_BUDGET_UNRESTRICTED_VH = unrestricted.
	float far_offset_vh;
	enum u_rear_budget_state state;
	//! 0..1 diagnostic; 0 when there is no source.
	float cue_energy;
};

/*!
 * Opaque-ish policy state. Zero-init is NOT valid — call
 * @ref u_rear_budget_init.
 *
 * @ingroup aux_util
 */
struct u_rear_budget
{
	struct u_rear_budget_tuning tuning;

	enum u_rear_budget_state state;
	float cue_energy;

	//! Ramp: value = from + (to - from) * ease_out(progress).
	float ramp_from_vh;
	float ramp_to_vh;
	float current_vh;
	uint64_t ramp_start_ns;
	uint64_t ramp_duration_ns;

	/*!
	 * Start of the current continuous-neutral / continuous-busy run. The
	 * `_active` flags are load-bearing: a monotonic clock legitimately reads
	 * 0 at process start (and always does in a test), so a 0 timestamp cannot
	 * double as "no run in progress".
	 */
	uint64_t neutral_since_ns;
	bool neutral_run_active;
	uint64_t busy_since_ns;
	bool busy_run_active;

	//! Stall detector for the preview generation.
	uint32_t last_generation;
	bool have_generation;

	//! Label used in the one-line-per-transition log ("d3d11 session", …).
	char label[32];
};

/*!
 * Fill @p t with the built-in defaults. No environment is read.
 *
 * @ingroup aux_util
 */
void
u_rear_budget_tuning_defaults(struct u_rear_budget_tuning *t);

/*!
 * Apply the `DXR_REAR_BUDGET*` environment overrides on top of @p t.
 *
 * Every override that is armed logs ONCE at WARN with the value it took —
 * an experiment that is silent when armed cannot be told from one that never
 * ran. Call from the compositor, not from tests: tests pass a tuning struct.
 *
 * Reads: `DXR_REAR_BUDGET` (`clip` | `open` | `auto`),
 * `DXR_REAR_BUDGET_OPEN_DWELL_MS`, `DXR_REAR_BUDGET_CLOSE_MS`,
 * `DXR_REAR_BUDGET_RAMP_OPEN_MS`, `DXR_REAR_BUDGET_RAMP_CLOSE_MS`.
 *
 * @ingroup aux_util
 */
void
u_rear_budget_tuning_from_env(struct u_rear_budget_tuning *t);

/*!
 * Initialise @p b.
 *
 * @param b        Instance to initialise.
 * @param tuning   Tunables; NULL means @ref u_rear_budget_tuning_defaults.
 * @param label    Short name for the transition log; may be NULL.
 * @param now_ns   Monotonic now, the machine's time origin.
 *
 * @ingroup aux_util
 */
void
u_rear_budget_init(struct u_rear_budget *b,
                   const struct u_rear_budget_tuning *tuning,
                   const char *label,
                   uint64_t now_ns);

/*!
 * Advance the policy and read out the current budget.
 *
 * Safe to call at any rate: the ramp is time-based, not step-based, so a call
 * every frame and a call every 66 ms produce the same curve. @p now_ns must be
 * monotonic non-decreasing.
 *
 * @ingroup aux_util
 */
void
u_rear_budget_update(struct u_rear_budget *b,
                     const struct u_rear_budget_in *in,
                     uint64_t now_ns,
                     struct u_rear_budget_out *out);

/*!
 * Short name of a state, for logs.
 *
 * @ingroup aux_util
 */
const char *
u_rear_budget_state_str(enum u_rear_budget_state s);

#ifdef __cplusplus
}
#endif
