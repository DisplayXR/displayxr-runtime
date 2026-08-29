// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The output-device split's activation decision (#918).
 * @ingroup comp_xbridge
 */

#include "comp_split_gate.h"

#include "util/u_setting.h"

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
		// #1252: resolved through the settings chain (env > per-user >
		// machine), but still handed to THIS file's parser — the leading-
		// character test below is deliberately not debug_string_to_bool, and
		// centralising the parse would silently change what "off" means here.
		char buf[64];
		const char *v = u_setting_get_raw("DXR_WEAVE_ON_SCANOUT", buf, sizeof(buf), NULL);
		enabled = comp_split_gate_parse_requested(v) ? 1 : 0;
	}
	return enabled == 1;
}

bool
comp_split_gate_env_same_adapter(void)
{
	static int on = -1;
	if (on < 0) {
		/*
		 * ADR-039 Phase A: DEFAULT ON — the kill switch is `=0`.
		 *
		 * Flipped on the complete acceptance record (#1264, 2026-08-29):
		 * keyed-mutex smoke clean, partition D=3 exact on both bring-up
		 * apps, the 68-window >=5-minute leg flat through 3+ of the
		 * ~105 s events, and the eyeball on the configuration that
		 * opened #1257. The VK tier's partition support keys on the
		 * split being active (comp_vk_native_compositor.c), so this
		 * default and that gate flip together by construction.
		 */
		const char *e = getenv("DXR_SPLIT_SAME_ADAPTER");
		on = (e != NULL && e[0] == '0') ? 0 : 1;
	}
	return on == 1;
}

bool
comp_split_gate_env_same_adapter_d3d12(void)
{
	static int on = -1;
	if (on < 0) {
		/*
		 * ADR-039 Phase B bring-up switch (the D3D12 tier). Default OFF —
		 * the tier consults the accepted default only after it passes its
		 * own #1260 matrix at >=5-minute legs (ADR-039 §Rollout: its
		 * baselines need REPEATED legs); on acceptance this collapses
		 * into comp_split_gate_env_same_adapter and the env retires.
		 * Deliberately not the Phase A switch: that one is default-ON
		 * for the accepted VK tier, and bring-up must never ride an
		 * acceptance it has not earned.
		 */
		const char *e = getenv("DXR_SPLIT_SAME_ADAPTER_D3D12");
		on = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return on == 1;
}

bool
comp_split_gate_env_same_adapter_d3d11(void)
{
	static int on = -1;
	if (on < 0) {
		/*
		 * ADR-039 Phase C bring-up switch (the in-process D3D11 tier) —
		 * same contract as the Phase B one below: default OFF, collapses
		 * into comp_split_gate_env_same_adapter on that tier's own
		 * acceptance. Pulled forward of Phase B's reroute build because
		 * it doubles as the reroute's hypothesis probe: it puts a REAL
		 * D3D11 app on the d3d11 out arm, measuring whether that arm's
		 * scheduler immunity (Phase A: 0.8-1.4 ms fires under app load)
		 * survives a same-API contender.
		 */
		const char *e = getenv("DXR_SPLIT_SAME_ADAPTER_D3D11");
		on = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return on == 1;
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
		out_result->same_adapter = true;
		if (!inputs->allow_same_adapter) {
			// Not a failure: on a MUX'd / single-GPU box — or under a forced
			// scanout-adapter selection — the weave is already local, so the
			// CROSS-ADAPTER purpose of the split has nothing to do. The
			// caller says so in its own words; nothing here should follow it
			// with a fallback WARN. (ADR-039 makes this branch a per-tier
			// policy rather than a law: see allow_same_adapter.)
			out_result->reason = COMP_SPLIT_REASON_HANDLED;
			out_result->short_reason = COMP_SPLIT_REASON_SAME_ADAPTER;
			return;
		}
		// ADR-039: engage. The split's load-bearing property is the
		// decoupled fill engine, not the copy it was built to remove; the
		// "bridge" ingress becomes a same-adapter shared-texture open
		// (strictly cheaper — no PCIe hop) and everything downstream is
		// the byte-identical hybrid arm.
	}

	out_result->out_adapter_luid = inputs->scanout_luid;
	out_result->split_active = true;
	out_result->reason = NULL;
	out_result->short_reason = NULL;
}
