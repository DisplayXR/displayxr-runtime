// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion selection: pick one capability per task, deterministically
 *         (ADR-053 decision 2).
 *
 * Pure C11: the caller hands a flat array of candidate rows (built from the
 * capability table, conv_registry_build_candidates()) plus the resolved
 * policy, and gets back the chosen row, whether the consumer must wait, and a
 * reason code. Optionally a verdict per row and the full ordering, which is
 * what `displayxr-cli conv list` prints.
 *
 * Spec: docs/specs/runtime/conversion-provider-discovery.md §3.1 (policy
 * values), §3.4 (selection); roadmap docs/roadmap/conversion-providers.md §2.4.
 *
 *   1. Candidates: rows with the task, from enabled, usable, non-hidden providers.
 *   2. Filter: licence gate, input kind, lane, min_views ≤ view_count ≤
 *      max_views (XRT_CONV_MAX_VIEWS while the row is not READY: its
 *      describe may still be provisional, #2029) and EXPLICIT_VIEWPOINTS
 *      (VIEWS_* tasks).
 *   3. Order: the preferred provider for this task first (even a FALLBACK
 *      one); then, inside the ProbeOrder step, FALLBACK providers last
 *      whatever their ProbeOrder; then ProbeOrder ascending; then direct
 *      before composed; then provider id (strcmp; a cross-provider tie sets
 *      tie_warn); then cap_id.
 *   3a. Per-create hint (XR_DXR_lift spec 5, #2178): the first row in that
 *      order that matches conv_select_policy::hint and is NOT terminal moves
 *      to the front. Nothing else moves; a hint that matches no row, or only
 *      terminal ones, changes nothing (hint_missed is set). The hint never
 *      outranks the filter (licence gate, Enabled, Supersedes, lane, views).
 *   4. Readiness never reorders: the first row that is READY or waited-for
 *      (INSTALLED / DOWNLOADING / WARMING / EVICTED) wins; ABSENT,
 *      INCOMPATIBLE, UNLICENSED, DISABLED and ANY FAILED fall through.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Rows beyond this are ignored by conv_select() (the registry holds far fewer). */
#define CONV_SELECT_MAX_ROWS 512u

/*! @name Provider flags (conv_select_row::provider_flags, conv_provider_desc::flags) @{ */
#define CONV_PROV_FALLBACK (1u << 0)       //!< XRT_CONV_PROVIDER_FLAG_FALLBACK: last unless preferred
#define CONV_PROV_LICENCE_EXEMPT (1u << 1) //!< grandfathered legacy adapter: skips the licence gate
/*! The legacy adapter (`dp-lift`, #2003): the provider takes the RELATIVE
 *  convergence of the legacy contract (xrt_dp_lift_params::convergence, [0,1]
 *  over the frame's own range) and the app's strength unchanged ("1 = the
 *  module's calibrated budget"). S3 then hands it
 *  CONV_CONVERGENCE_MODE_LEGACY_RELATIVE and skips the budget rule, so the
 *  plane the policy picked reaches the module exactly (no decoded-units round
 *  trip through a range the legacy contract cannot express). Runtime-internal:
 *  never set from the registry. */
#define CONV_PROV_LEGACY_UNITS (1u << 2)
/*! @} */

/*! One candidate: a capability row joined with its provider and readiness. */
struct conv_select_row
{
	/* Provider. */
	const char *provider_id; //!< never NULL
	uint32_t probe_order;
	uint32_t provider_flags; //!< CONV_PROV_*
	bool provider_enabled;   //!< Enabled != 0
	bool provider_usable;    //!< negotiated, probed OK, status not PLATFORM_ABSENT / INCOMPATIBLE
	bool hidden;             //!< hidden by another provider's Supersedes
	bool composed;           //!< a runtime-composed pipeline (depth + reference view synthesis)

	/* Capability (copied from xrt_conv_capability, or synthesised when composed). */
	uint32_t cap_id;
	uint32_t task;
	uint32_t lanes;
	uint32_t input_kinds;
	uint32_t flags; //!< XRT_CONV_CAP_*
	uint32_t min_views, max_views;
	uint32_t licence_class;
	const char *model_id; //!< may be NULL

	/* Readiness. */
	uint32_t state;  //!< XRT_CONV_STATE_*
	uint32_t reason; //!< XRT_CONV_FAIL_* / CONV_REASON_*
};

struct conv_select_request
{
	uint32_t task;       //!< XRT_CONV_TASK_*
	uint32_t input_kind; //!< one XRT_CONV_INPUT_* bit; 0 = any
	uint32_t lane;       //!< one XRT_CONV_LANE_* bit; 0 = any
	uint32_t view_count; //!< VIEWS_* only; already resolved (0 = "match the weave" is the caller's job)
};

/*! Effective policy (conv_select_resolve_*). */
struct conv_select_policy
{
	const char *preferred;    //!< "<provider-id>[/<cap_id or model_id>]" for this task; NULL = none
	bool allow_research_only; //!< AllowResearchOnly: RESEARCH_ONLY + UNKNOWN classes become eligible
	/*! Per-create preference HINT (lift spec 5, #2178), same grammar as
	 *  @ref preferred: reorders the non-terminal rows of this one selection
	 *  (rule 3a). Never stored, never written back. NULL / "" = none. */
	const char *hint;
};

/*! @name Per-row verdict (conv list "reason" column) @{ */
enum conv_row_verdict
{
	CONV_ROW_OTHER_TASK = 0,  //!< not this task (not a candidate)
	CONV_ROW_DROP_DISABLED,   //!< provider Enabled = 0
	CONV_ROW_DROP_UNUSABLE,   //!< provider not negotiated / probed / platform absent
	CONV_ROW_DROP_HIDDEN,     //!< hidden by Supersedes
	CONV_ROW_DROP_LICENCE,    //!< licence gate (AllowResearchOnly)
	CONV_ROW_DROP_INPUT,      //!< input kind not accepted
	CONV_ROW_DROP_LANE,       //!< lane not offered
	CONV_ROW_DROP_VIEWS,      //!< view_count outside [min_views, max_views]
	CONV_ROW_DROP_VIEWPOINTS, //!< VIEWS_* without EXPLICIT_VIEWPOINTS
	CONV_ROW_FELL_THROUGH,    //!< ordered before the choice, but in a terminal state
	CONV_ROW_SELECTED,        //!< the choice
	CONV_ROW_BEHIND,          //!< kept, ordered after the choice
};
/*! @} */

/*! @name Result reason @{ */
enum conv_select_reason
{
	CONV_SEL_OK_PREFERRED = 0,   //!< the first ordered row, a preferred-provider (or hint) match
	CONV_SEL_OK_ORDER,           //!< the first ordered row, by ProbeOrder / tie-break
	CONV_SEL_OK_FALLTHROUGH,     //!< chosen after ≥ 1 terminal row fell through (log one WARN)
	CONV_SEL_NONE_RESERVED_TASK, //!< NONE / reserved / invalid task → VALIDATION_FAILURE
	CONV_SEL_NONE_NO_CANDIDATE,  //!< nothing survived the filter → FEATURE_UNSUPPORTED / VALIDATION_FAILURE
	CONV_SEL_NONE_ALL_TERMINAL,  //!< candidates exist, all terminal → send CONV_EV_CREATE_REQUESTED to them
};
/*! @} */

struct conv_select_result
{
	int32_t row;              //!< index into rows[]; -1 = none
	uint32_t reason;          //!< conv_select_reason
	bool wait;                //!< chosen row is not READY yet: the consumer waits
	bool tie_warn;            //!< two providers tied on ProbeOrder (log one WARN, the #1466 rule)
	uint32_t fallthrough;     //!< terminal rows skipped before the choice
	uint32_t candidate_count; //!< rows that survived the filter
	bool hint_applied;        //!< the policy's hint chose the row (rule 3a)
	bool hint_missed;         //!< a hint was given but matched no non-terminal candidate (log one INFO)
};

/*!
 * Select one row for @p req.
 *
 * @param verdicts  optional, @p count entries: conv_row_verdict per row
 * @param order     optional, @p count entries: filtered rows in selection
 *                  order, after the hint (result.candidate_count of them
 *                  are written)
 */
struct conv_select_result
conv_select(const struct conv_select_row *rows,
            uint32_t count,
            const struct conv_select_request *req,
            const struct conv_select_policy *policy,
            uint32_t *verdicts,
            uint32_t *order);

/*! Does @p row match a "<id>[/<cap_id or model_id>]" preference? NULL / "" never matches. */
bool
conv_select_preference_matches(const char *preferred, const struct conv_select_row *row);

/*!
 * PreferredProvider\<task>: HKCU over HKLM; under Policy\Lock only HKLM
 * counts (it pins the machine's choice). NULL / "" = not set.
 */
const char *
conv_select_resolve_preference(const char *hkcu, const char *hklm, bool policy_lock);

/*!
 * AllowResearchOnly. Tri-state inputs: < 0 = value absent, 0 / 1 = DWORD.
 * HKCU overrides HKLM; under Policy\Lock only HKLM counts. Absent = 0.
 */
bool
conv_select_resolve_research_only(int hkcu, int hklm, bool policy_lock);

/*! Registry / CLI task name ("views-mono", "depth-video", …) → XRT_CONV_TASK_*; 0 = unknown. */
uint32_t
conv_task_from_name(const char *name);

/*! XRT_CONV_TASK_* → registry / CLI task name; "none" for unknown. */
const char *
conv_task_name(uint32_t task);

/*! A task v1 serves (DEPTH_MONO .. SPLATS_STEREO); reserved and NONE are not. */
bool
conv_task_is_served(uint32_t task);

#ifdef __cplusplus
}
#endif
