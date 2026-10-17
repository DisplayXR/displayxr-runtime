// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  One task, ranked the way a create selects and `conv list` shows it,
 *         and the app-facing READ-ONLY candidate list derived from it (lift
 *         spec 5, #2178; ADR-054 decision 8).
 *
 * conv_rank_task() is the single "candidates + selection" pass over a
 * registry: target_conv_status (the `conv list` / status-snapshot rows) and
 * the conversion core (xrEnumerateLiftTaskCandidatesDXR) both call it, so the
 * rows an app sees are the rows the CLI prints, never a re-derivation.
 *
 * conv_candidates_from_ranking() folds the ranked rows into one candidate per
 * provider: only rows that survived selection's filter (a disabled,
 * superseded, licence-gated or unusable provider is not a candidate), in
 * selection order. A provider's representative row is the first of its rows
 * that is not terminal (the row a hint naming the provider binds), else its
 * first row. The display name is the capability's own, verbatim.
 *
 * Pure C11: no threads, no logging.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conv_wire.h"

#include "conv_registry.h"
#include "conv_select.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Rows one ranking holds (direct + composed).
#define CONV_RANK_MAX_ROWS (CONV_REGISTRY_MAX_ROWS * 2u)

//! Candidates one task can list (one per provider).
#define CONV_CANDIDATES_MAX CONV_REGISTRY_MAX_PROVIDERS

XRT_CONV_ABI_ASSERT(CONV_CANDIDATES_MAX == XRT_CONV_WIRE_MAX_CANDIDATES,
                    "the wire's candidate bound is the registry's");
XRT_CONV_ABI_ASSERT(CONV_ID_MAX <= XRT_CONV_WIRE_PROVIDER_ID_MAX, "a provider id must fit the wire candidate");

/*! One task's candidates and the selection over them. Large: heap or static. */
struct conv_task_ranking
{
	uint32_t task;
	uint32_t count; //!< rows[] / cap_row[] / verdicts[] entries
	struct conv_select_row rows[CONV_RANK_MAX_ROWS];
	uint32_t cap_row[CONV_RANK_MAX_ROWS];  //!< registry row behind each candidate
	uint32_t verdicts[CONV_RANK_MAX_ROWS]; //!< conv_row_verdict per row
	uint32_t order[CONV_RANK_MAX_ROWS];    //!< res.candidate_count filtered rows, in selection order
	struct conv_select_result res;
};

/*!
 * Build @p task's candidates from @p reg and run selection over them with
 * @p policy (whose hint, if any, is applied). @p view_count is the resolved
 * count for a VIEWS_* task (2 = "could it be served at all", the `conv list`
 * and properties convention).
 */
void
conv_rank_task(const struct conv_registry *reg,
               uint32_t task,
               bool include_composed,
               uint32_t view_count,
               const struct conv_select_policy *policy,
               struct conv_task_ranking *out);

/*!
 * One candidate per provider of @p rk (see the file comment). @p preferred is
 * the task's PreferredProvider policy (NULL = none): it sets
 * XRT_CONV_WIRE_CANDIDATE_USER_DEFAULT; the ranking's choice sets
 * XRT_CONV_WIRE_CANDIDATE_SELECTED. Hosting comes from the provider's
 * Hosting and the capability's REMOTE flag. Returns how many were written
 * (at most @p max; zero providers = 0).
 */
uint32_t
conv_candidates_from_ranking(const struct conv_registry *reg,
                             const struct conv_task_ranking *rk,
                             const char *preferred,
                             struct xrt_conv_wire_candidate *out,
                             uint32_t max);

/*!
 * A stable signature of a candidate list (ids, states, flags, order): the
 * spec-5 event rule compares it to decide whether a readiness change behind
 * the selection changed what an app enumerated.
 */
uint64_t
conv_candidates_signature(const struct xrt_conv_wire_candidate *c, uint32_t count);

#ifdef __cplusplus
}
#endif
