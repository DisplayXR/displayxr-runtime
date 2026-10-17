// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Task ranking + app-facing candidates (pure).
 * @ingroup aux_conversion
 */

#include "conv_candidates.h"
#include "conv_readiness.h"

#include <string.h>

void
conv_rank_task(const struct conv_registry *reg,
               uint32_t task,
               bool include_composed,
               uint32_t view_count,
               const struct conv_select_policy *policy,
               struct conv_task_ranking *out)
{
	memset(&out->res, 0, sizeof(out->res));
	out->res.row = -1;
	out->task = task;
	out->count =
	    conv_registry_build_candidates(reg, task, include_composed, out->rows, out->cap_row, CONV_RANK_MAX_ROWS);
	struct conv_select_request req;
	memset(&req, 0, sizeof(req));
	req.task = task;
	req.view_count = view_count;
	out->res = conv_select(out->rows, out->count, &req, policy, out->verdicts, out->order);
}

static void
copy_str(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

uint32_t
conv_candidates_from_ranking(const struct conv_registry *reg,
                             const struct conv_task_ranking *rk,
                             const char *preferred,
                             struct xrt_conv_wire_candidate *out,
                             uint32_t max)
{
	uint32_t n = 0;
	const uint32_t ordered = rk->res.candidate_count < rk->count ? rk->res.candidate_count : rk->count;
	for (uint32_t k = 0; k < ordered && n < max; k++) {
		const uint32_t i = rk->order[k];
		const struct conv_select_row *r = &rk->rows[i];
		const uint32_t prov = reg->rows[rk->cap_row[i]].provider;

		// One candidate per provider: the first ordered row claims it.
		bool seen = false;
		for (uint32_t c = 0; c < k; c++) {
			if (reg->rows[rk->cap_row[rk->order[c]]].provider == prov) {
				seen = true;
				break;
			}
		}
		if (seen) {
			continue;
		}
		// Representative: the provider's first non-terminal row (what a hint binds), else this one.
		uint32_t rep = i;
		if (conv_state_is_terminal(r->state)) {
			for (uint32_t c = k + 1; c < ordered; c++) {
				const uint32_t j = rk->order[c];
				if (reg->rows[rk->cap_row[j]].provider == prov &&
				    !conv_state_is_terminal(rk->rows[j].state)) {
					rep = j;
					break;
				}
			}
		}
		const struct conv_select_row *rr = &rk->rows[rep];
		const struct conv_cap_row *cap_row = &reg->rows[rk->cap_row[rep]];

		struct xrt_conv_wire_candidate *o = &out[n++];
		xrt_conv_wire_begin(o, (uint32_t)sizeof(*o));
		o->task = rk->task;
		o->state = rr->state;
		o->failure_reason = rr->reason < CONV_REASON_LOCAL_BASE ? rr->reason : 0u;
		o->licence_class = rr->licence_class;
		if ((cap_row->cap.flags & XRT_CONV_CAP_REMOTE) != 0) {
			o->hosting = XRT_CONV_WIRE_HOSTING_REMOTE;
		} else if (reg->providers[prov].hosting == CONV_HOSTING_SATELLITE) {
			o->hosting = XRT_CONV_WIRE_HOSTING_LOCAL_SATELLITE;
		} else {
			o->hosting = XRT_CONV_WIRE_HOSTING_LOCAL_IN_SERVICE;
		}
		// The provider is the default / the selection when ANY of its rows is.
		for (uint32_t c = k; c < ordered; c++) {
			const uint32_t j = rk->order[c];
			if (reg->rows[rk->cap_row[j]].provider != prov) {
				continue;
			}
			if (conv_select_preference_matches(preferred, &rk->rows[j])) {
				o->candidate_flags |= XRT_CONV_WIRE_CANDIDATE_USER_DEFAULT;
			}
			if (rk->res.row == (int32_t)j) {
				o->candidate_flags |= XRT_CONV_WIRE_CANDIDATE_SELECTED;
			}
		}
		copy_str(o->provider_id, sizeof(o->provider_id), rr->provider_id);
		copy_str(o->display_name, sizeof(o->display_name),
		         cap_row->cap.display_name[0] != '\0' ? cap_row->cap.display_name : rr->provider_id);
	}
	return n;
}

static uint64_t
fnv1a(uint64_t h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	for (size_t i = 0; i < n; i++) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return h;
}

uint64_t
conv_candidates_signature(const struct xrt_conv_wire_candidate *c, uint32_t count)
{
	uint64_t h = 1469598103934665603ull;
	h = fnv1a(h, &count, sizeof(count));
	for (uint32_t i = 0; i < count; i++) {
		h = fnv1a(h, c[i].provider_id, sizeof(c[i].provider_id)); // zero-filled by xrt_conv_wire_begin
		h = fnv1a(h, &c[i].state, sizeof(c[i].state));
		h = fnv1a(h, &c[i].failure_reason, sizeof(c[i].failure_reason));
		h = fnv1a(h, &c[i].licence_class, sizeof(c[i].licence_class));
		h = fnv1a(h, &c[i].hosting, sizeof(c[i].hosting));
		h = fnv1a(h, &c[i].candidate_flags, sizeof(c[i].candidate_flags));
	}
	return h;
}
