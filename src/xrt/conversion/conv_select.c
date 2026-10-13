// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion selection (pure).
 * @ingroup aux_conversion
 */

#include "conv_select.h"
#include "conv_readiness.h"

#include "xrt/xrt_conversion_plugin.h"

#include <stdlib.h>
#include <string.h>

/*
 *
 * Tasks.
 *
 */

static const struct
{
	uint32_t task;
	const char *name;
} k_task_names[] = {
    {XRT_CONV_TASK_DEPTH_MONO, "depth-mono"},
    {XRT_CONV_TASK_DEPTH_VIDEO, "depth-video"},
    {XRT_CONV_TASK_DISPARITY_STEREO, "disparity-stereo"},
    {XRT_CONV_TASK_VIEWS_MONO, "views-mono"},
    {XRT_CONV_TASK_VIEWS_FROM_DEPTH, "views-from-depth"},
    {XRT_CONV_TASK_SPLATS_MONO, "splats-mono"},
    {XRT_CONV_TASK_SPLATS_STEREO, "splats-stereo"},
    {XRT_CONV_TASK_SPLATS_VIDEO, "splats-video"},
    {XRT_CONV_TASK_INPAINT, "inpaint"},
};

uint32_t
conv_task_from_name(const char *name)
{
	if (name == NULL) {
		return XRT_CONV_TASK_NONE;
	}
	for (size_t i = 0; i < sizeof(k_task_names) / sizeof(k_task_names[0]); i++) {
		if (strcmp(name, k_task_names[i].name) == 0) {
			return k_task_names[i].task;
		}
	}
	return XRT_CONV_TASK_NONE;
}

const char *
conv_task_name(uint32_t task)
{
	for (size_t i = 0; i < sizeof(k_task_names) / sizeof(k_task_names[0]); i++) {
		if (k_task_names[i].task == task) {
			return k_task_names[i].name;
		}
	}
	return "none";
}

bool
conv_task_is_served(uint32_t task)
{
	return task >= XRT_CONV_TASK_DEPTH_MONO && task <= XRT_CONV_TASK_SPLATS_STEREO;
}

static bool
task_is_views(uint32_t task)
{
	return task == XRT_CONV_TASK_VIEWS_MONO || task == XRT_CONV_TASK_VIEWS_FROM_DEPTH;
}

/*
 *
 * Policy.
 *
 */

static bool
str_set(const char *s)
{
	return s != NULL && s[0] != '\0';
}

const char *
conv_select_resolve_preference(const char *hkcu, const char *hklm, bool policy_lock)
{
	if (policy_lock) {
		return str_set(hklm) ? hklm : NULL;
	}
	if (str_set(hkcu)) {
		return hkcu;
	}
	return str_set(hklm) ? hklm : NULL;
}

bool
conv_select_resolve_research_only(int hkcu, int hklm, bool policy_lock)
{
	if (policy_lock) {
		return hklm > 0;
	}
	if (hkcu >= 0) {
		return hkcu > 0;
	}
	return hklm > 0;
}

bool
conv_select_preference_matches(const char *preferred, const struct conv_select_row *row)
{
	if (!str_set(preferred) || row->provider_id == NULL) {
		return false;
	}
	const char *slash = strchr(preferred, '/');
	size_t id_len = slash != NULL ? (size_t)(slash - preferred) : strlen(preferred);
	if (strlen(row->provider_id) != id_len || strncmp(row->provider_id, preferred, id_len) != 0) {
		return false;
	}
	if (slash == NULL || slash[1] == '\0') {
		return true; // whole provider
	}
	const char *suffix = slash + 1;

	// All digits → cap_id; anything else → model_id.
	bool digits = true;
	for (const char *p = suffix; *p != '\0'; p++) {
		if (*p < '0' || *p > '9') {
			digits = false;
			break;
		}
	}
	if (digits) {
		return strtoul(suffix, NULL, 10) == row->cap_id;
	}
	return row->model_id != NULL && strcmp(row->model_id, suffix) == 0;
}

/*
 *
 * Filter + order.
 *
 */

static bool
licence_needs_opt_in(uint32_t licence_class)
{
	// UNKNOWN is treated as RESEARCH_ONLY; an out-of-range class too.
	return licence_class != XRT_CONV_LICENCE_PERMISSIVE && licence_class != XRT_CONV_LICENCE_COMMERCIAL;
}

static uint32_t
filter_row(const struct conv_select_row *r, const struct conv_select_request *req, const struct conv_select_policy *pol)
{
	if (r->task != req->task) {
		return CONV_ROW_OTHER_TASK;
	}
	if (!r->provider_enabled) {
		return CONV_ROW_DROP_DISABLED;
	}
	if (!r->provider_usable) {
		return CONV_ROW_DROP_UNUSABLE;
	}
	if (r->hidden) {
		return CONV_ROW_DROP_HIDDEN;
	}
	if ((r->provider_flags & CONV_PROV_LICENCE_EXEMPT) == 0 && licence_needs_opt_in(r->licence_class) &&
	    !pol->allow_research_only) {
		return CONV_ROW_DROP_LICENCE;
	}
	if (req->input_kind != 0 && (r->input_kinds & req->input_kind) == 0) {
		return CONV_ROW_DROP_INPUT;
	}
	if (req->lane != 0 && (r->lanes & req->lane) == 0) {
		return CONV_ROW_DROP_LANE;
	}
	if (task_is_views(req->task)) {
		// #2029: a row that is not READY may still describe a conservative
		// max_views (dp-lift: 2 until its module is up). Keep it for counts up
		// to XRT_CONV_MAX_VIEWS; the READY refine decides (the stream goes
		// unsatisfied if the capability lacks the views).
		const uint32_t hi = r->composed || r->state == XRT_CONV_STATE_READY ? r->max_views : XRT_CONV_MAX_VIEWS;
		if (req->view_count < r->min_views || req->view_count > hi) {
			return CONV_ROW_DROP_VIEWS;
		}
		if ((r->flags & XRT_CONV_CAP_EXPLICIT_VIEWPOINTS) == 0) {
			return CONV_ROW_DROP_VIEWPOINTS;
		}
	}
	return CONV_ROW_BEHIND; // kept; refined after ordering
}

struct sort_ctx
{
	const struct conv_select_row *rows;
	const char *preferred;
};

//! < 0: a before b. Everything up to the id compare is the "rank".
static int
compare_rank(const struct sort_ctx *ctx, uint32_t ia, uint32_t ib)
{
	const struct conv_select_row *a = &ctx->rows[ia];
	const struct conv_select_row *b = &ctx->rows[ib];

	// An explicit per-task preference wins first, even for a FALLBACK
	// provider: a user who asks for it gets it.
	int pa = conv_select_preference_matches(ctx->preferred, a) ? 0 : 1;
	int pb = conv_select_preference_matches(ctx->preferred, b) ? 0 : 1;
	if (pa != pb) {
		return pa - pb;
	}
	// Inside the ProbeOrder step, FALLBACK providers sort last whatever their ProbeOrder.
	int fa = (a->provider_flags & CONV_PROV_FALLBACK) != 0;
	int fb = (b->provider_flags & CONV_PROV_FALLBACK) != 0;
	if (fa != fb) {
		return fa - fb;
	}
	if (a->probe_order != b->probe_order) {
		return a->probe_order < b->probe_order ? -1 : 1;
	}
	int ca = a->composed ? 1 : 0;
	int cb = b->composed ? 1 : 0;
	if (ca != cb) {
		return ca - cb;
	}
	return 0;
}

static int
compare_full(const struct sort_ctx *ctx, uint32_t ia, uint32_t ib)
{
	int c = compare_rank(ctx, ia, ib);
	if (c != 0) {
		return c;
	}
	c = strcmp(ctx->rows[ia].provider_id, ctx->rows[ib].provider_id);
	if (c != 0) {
		return c;
	}
	if (ctx->rows[ia].cap_id != ctx->rows[ib].cap_id) {
		return ctx->rows[ia].cap_id < ctx->rows[ib].cap_id ? -1 : 1;
	}
	return ia < ib ? -1 : (ia > ib ? 1 : 0); // total + stable
}

struct conv_select_result
conv_select(const struct conv_select_row *rows,
            uint32_t count,
            const struct conv_select_request *req,
            const struct conv_select_policy *policy,
            uint32_t *verdicts,
            uint32_t *order)
{
	struct conv_select_result res;
	memset(&res, 0, sizeof(res));
	res.row = -1;

	struct conv_select_policy empty_policy = {NULL, false};
	if (policy == NULL) {
		policy = &empty_policy;
	}
	if (count > CONV_SELECT_MAX_ROWS) {
		count = CONV_SELECT_MAX_ROWS;
	}

	if (verdicts != NULL) {
		for (uint32_t i = 0; i < count; i++) {
			verdicts[i] = CONV_ROW_OTHER_TASK;
		}
	}
	if (!conv_task_is_served(req->task)) {
		res.reason = CONV_SEL_NONE_RESERVED_TASK;
		return res;
	}

	uint32_t kept[CONV_SELECT_MAX_ROWS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t v = filter_row(&rows[i], req, policy);
		if (verdicts != NULL) {
			verdicts[i] = v;
		}
		if (v == CONV_ROW_BEHIND) {
			kept[n++] = i;
		}
	}
	res.candidate_count = n;
	if (n == 0) {
		res.reason = CONV_SEL_NONE_NO_CANDIDATE;
		return res;
	}

	// Insertion sort: n is small, and the comparator is total.
	struct sort_ctx ctx = {rows, policy->preferred};
	for (uint32_t i = 1; i < n; i++) {
		uint32_t v = kept[i];
		uint32_t j = i;
		while (j > 0 && compare_full(&ctx, kept[j - 1], v) > 0) {
			kept[j] = kept[j - 1];
			j--;
		}
		kept[j] = v;
	}

	// Two different providers at the same rank: the id decides, with one WARN.
	for (uint32_t i = 1; i < n; i++) {
		if (compare_rank(&ctx, kept[i - 1], kept[i]) == 0 &&
		    strcmp(rows[kept[i - 1]].provider_id, rows[kept[i]].provider_id) != 0) {
			res.tie_warn = true;
		}
	}

	if (order != NULL) {
		memcpy(order, kept, n * sizeof(uint32_t));
	}

	// Readiness never reorders: walk in order, skip terminal rows only.
	for (uint32_t i = 0; i < n; i++) {
		const struct conv_select_row *r = &rows[kept[i]];
		if (conv_state_is_terminal(r->state)) {
			res.fallthrough++;
			if (verdicts != NULL) {
				verdicts[kept[i]] = CONV_ROW_FELL_THROUGH;
			}
			continue;
		}
		res.row = (int32_t)kept[i];
		res.wait = r->state != XRT_CONV_STATE_READY;
		if (res.fallthrough > 0) {
			res.reason = CONV_SEL_OK_FALLTHROUGH;
		} else if (conv_select_preference_matches(policy->preferred, r)) {
			res.reason = CONV_SEL_OK_PREFERRED;
		} else {
			res.reason = CONV_SEL_OK_ORDER;
		}
		if (verdicts != NULL) {
			verdicts[kept[i]] = CONV_ROW_SELECTED;
		}
		return res;
	}

	res.reason = CONV_SEL_NONE_ALL_TERMINAL;
	return res;
}
