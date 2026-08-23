// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The output-device split's activation decision (#918).
 * @ingroup comp_xbridge
 */

#include "comp_split_gate.h"

#include <stdlib.h>
#include <string.h>

bool
comp_split_luid_equal(struct comp_split_luid a, struct comp_split_luid b)
{
	return a.low == b.low && a.high == b.high;
}

bool
comp_split_gate_parse_requested(const char *value)
{
	/*
	 * #918 Phase 3: DEFAULT ALLOWED. Unset — the overwhelmingly common case —
	 * means "apply ADR-037 §1", so only an explicit false spelling kills the
	 * split. `=1` and every other non-false value stay allowed, which is what
	 * keeps the Phase-1/2 scripts, the t918* harnesses and the shipped docs
	 * working unchanged.
	 *
	 * Leading-character test, deliberately NOT `debug_string_to_bool`: see the
	 * non-unification note in comp_split_gate.h.
	 */
	if (value == NULL || value[0] == '\0') {
		return true;
	}
	if (value[0] == '0' || value[0] == 'f' || value[0] == 'F' || value[0] == 'n' || value[0] == 'N') {
		return false;
	}
	// "off" / "OFF" — the one false spelling whose leading character ('o')
	// collides with a true one ("on"), so it is matched whole.
	if (strcmp(value, "off") == 0 || strcmp(value, "OFF") == 0 || strcmp(value, "Off") == 0) {
		return false;
	}
	return true;
}

bool
comp_split_gate_env_requested(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = comp_split_gate_parse_requested(getenv("DXR_WEAVE_ON_SCANOUT")) ? 1 : 0;
	}
	return enabled == 1;
}

bool
comp_split_gate_env_test_fail_stage_a(void)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("DXR_TEST_SPLIT_FAIL_STAGEA");
		on = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return on == 1;
}

enum comp_split_ingress_policy
comp_split_gate_env_ingress_policy(void)
{
	const char *e = getenv("DXR_SPLIT_INGRESS");
	if (e != NULL && strcmp(e, "staged") == 0) {
		return COMP_SPLIT_INGRESS_POLICY_STAGED;
	}
	return COMP_SPLIT_INGRESS_POLICY_ADAPTIVE;
}

void
comp_split_gate_evaluate(const struct comp_split_gate_inputs *inputs, struct comp_split_gate_result *out_result)
{
	memset(out_result, 0, sizeof(*out_result));
	out_result->reason = COMP_SPLIT_REASON_HANDLED;
	out_result->short_reason = COMP_SPLIT_REASON_KILLED_BY_ENV;
	out_result->ingress = comp_split_gate_env_ingress_policy();

	if (inputs == NULL || !inputs->requested) {
		// The kill switch. Since Phase 3 this is the only way to reach here, so
		// the short reason names the env rather than "nobody asked".
		return;
	}
	out_result->requested = true;

	// The caller's own list, first: an ineligible session need never resolve an
	// adapter, which is why this is an input rather than something the gate asks
	// for after the fact.
	if (inputs->ineligible_reason != NULL) {
		out_result->reason = inputs->ineligible_reason;
		out_result->short_reason = inputs->ineligible_reason;
		return;
	}

	if (!inputs->scanout_resolved) {
		out_result->reason = COMP_SPLIT_REASON_SCANOUT_UNRESOLVABLE;
		out_result->short_reason = COMP_SPLIT_REASON_SCANOUT_UNRESOLVABLE;
		return;
	}

	if (comp_split_luid_equal(inputs->render_luid, inputs->scanout_luid)) {
		// Not a failure: on a MUX'd / single-GPU box — or under a forced
		// scanout-adapter selection — the weave is already local, so the split
		// has nothing to do. The caller says so in its own words; nothing here
		// should follow it with a fallback WARN.
		out_result->same_adapter = true;
		out_result->reason = COMP_SPLIT_REASON_HANDLED;
		out_result->short_reason = COMP_SPLIT_REASON_SAME_ADAPTER;
		return;
	}

	out_result->out_adapter_luid = inputs->scanout_luid;
	out_result->split_active = true;
	out_result->reason = NULL;
	out_result->short_reason = NULL;
}
