// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The output-device split's activation decision (#918) — pure, and
 *         therefore shared.
 * @ingroup comp_xbridge
 *
 * Standing the split up is graphics-API work: create a device on the scanout
 * adapter, open a DXGI factory, allocate a cross-adapter heap. DECIDING to
 * stand it up is not. It is four questions, and every consumer of the bridge
 * asks the same four:
 *
 *   1. Has anyone KILLED the split (`DXR_WEAVE_ON_SCANOUT=0`)? Since #918
 *      Phase 3 the split is the DEFAULT (ADR-037 §1), so this question is the
 *      kill switch, not an opt-in.
 *   2. Is this session ELIGIBLE at all? (Each caller has its own list — no
 *      HWND, a shared-texture session, `DXR_LEGACY_STANDALONE`, no panel
 *      dimensions — so the caller passes its verdict in rather than the gate
 *      guessing it.)
 *   3. Did the scanout adapter RESOLVE, and is it a DIFFERENT adapter from the
 *      one the caller renders on? Same adapter is a no-op, not a failure.
 *   4. Which INGRESS flavour should the bridge use (`DXR_SPLIT_INGRESS`)?
 *
 * The D3D11 in-process compositor and the D3D11 service each had that written
 * out inline, and the D3D12 pair behind them (#918 D12-*) would have made it
 * four. So the decision lives here, named, free of every graphics type, and
 * returning pure data the caller logs in its own words.
 *
 * The gate takes NO side effects and emits NO log lines. That is deliberate:
 * the two existing call sites word their WARNs differently ("D3D11
 * output-device split: …" vs "#918 output-device split: …") and those strings
 * are what field logs are grepped for.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * An adapter LUID, mirrored free of `<windows.h>`.
 *
 * The member types match Win32's `LUID` exactly (`DWORD LowPart`,
 * `LONG HighPart`) so a caller's conversion is two field copies in each
 * direction and can lose nothing.
 */
struct comp_split_luid
{
	uint32_t low;
	int32_t high;
};

//! True when @p a and @p b name the same adapter.
bool
comp_split_luid_equal(struct comp_split_luid a, struct comp_split_luid b);

//! What the bridge should do with the caller's app-device source texture.
enum comp_split_ingress_policy
{
	/*!
	 * Read a source that has held still IN PLACE; stage only the frame a
	 * source CHANGE lands on. The default.
	 */
	COMP_SPLIT_INGRESS_POLICY_ADAPTIVE = 0,
	/*!
	 * `DXR_SPLIT_INGRESS=staged` — one extra full-content app-device copy on
	 * every frame. The A/B control the perf claim is measured against, and the
	 * one-line escape if adaptive ever misbehaves in the field.
	 */
	COMP_SPLIT_INGRESS_POLICY_STAGED = 1,
};

/*!
 * The gate's answer when it has nothing for the caller to log: either the split
 * was killed outright, or the caller has already logged the situation itself
 * (the same-adapter no-op). Distinct from NULL, which means "proceed".
 *
 * Note this is the PROSE channel only — @ref comp_split_gate_result::short_reason
 * is never empty, precisely so that "nothing to say to a human" and "nothing to
 * report to a support case" stop being the same thing.
 */
#define COMP_SPLIT_REASON_HANDLED ""

/*!
 * @name Canonical short reasons (ADR-037 §3)
 *
 * ONE snake_case token per fallback rung, so `[RENDER] split=0 reason=<token>`
 * and the `weave placement:` line always name the same thing and a support case
 * can be grepped with a fixed string. Never a phrase, never with spaces: the
 * token is a parse target.
 *
 * Since #918 Phase 3 the split is the DEFAULT, so `env_not_requested` — which
 * used to be the overwhelmingly common answer — is gone. Anything that is not
 * one of these tokens is a caller-specific detail string and belongs in that
 * caller's own WARN, with @ref COMP_SPLIT_REASON_STAGE_A_FAILED as the short
 * form.
 * @{
 */
//! `DXR_WEAVE_ON_SCANOUT=0` (or another false spelling) — the kill switch.
#define COMP_SPLIT_REASON_KILLED_BY_ENV "killed_by_env"
//! Render adapter IS the scanout adapter; the split has nothing to do.
#define COMP_SPLIT_REASON_SAME_ADAPTER "same_adapter"
//! The scanout adapter could not be resolved, so there is no way to tell
//! whether this session crosses adapters at all.
#define COMP_SPLIT_REASON_SCANOUT_UNRESOLVABLE "scanout_unresolvable"
//! The RENDER adapter could not be identified, so there is nothing to compare
//! the scanout adapter against. The mirror image of the token above.
#define COMP_SPLIT_REASON_RENDER_UNRESOLVABLE "render_unresolvable"
//! No window to present into — an offscreen session.
#define COMP_SPLIT_REASON_NO_HWND "no_hwnd"
//! The app owns the present (shared-texture handoff), so the runtime has no
//! present path to move.
#define COMP_SPLIT_REASON_SHARED_TEXTURE "shared_texture_session"
//! The panel extent is unknown, so the scanout adapter cannot be located.
#define COMP_SPLIT_REASON_NO_PANEL_DIMS "no_panel_dimensions"
//! `DXR_LEGACY_STANDALONE` — the byte-for-byte pre-#964 A/B reference path.
#define COMP_SPLIT_REASON_LEGACY_STANDALONE "legacy_standalone"
/*!
 * The graphics API this session runs on has no split implementation, so
 * ADR-037 §3 rung 2 applies: everything on the render adapter. Vulkan and
 * OpenGL. This is a HONEST NO, not a guess — a path that cannot answer "is the
 * split implemented for me?" must report this rather than half-engage.
 */
#define COMP_SPLIT_REASON_API_UNSUPPORTED "api_unsupported"
/*!
 * This presenter is structurally unable to split: the client owns the present
 * (`CLIENT_TEXTURE`, self-presenting clients), so there is no runtime present
 * to move to the scanout adapter. ADR-037 §7.
 */
#define COMP_SPLIT_REASON_PRESENTER_INELIGIBLE "presenter_ineligible"
/*!
 * The gate said proceed and Stage A then failed — device creation, the DXGI
 * factory, the cross-adapter heap, the egress ring. The specific failure is in
 * the caller's own WARN immediately above; this is the short form.
 */
#define COMP_SPLIT_REASON_STAGE_A_FAILED "stage_a_failed"
/*!
 * ADR-037 §3a — the display processor declined to create a weaver on the scanout
 * adapter. A vendor plug-in is allowed to; the split retires and the session
 * weaves on the render adapter, where the DP demonstrably worked before.
 * POST-activation, so it is not a Stage-A failure.
 */
#define COMP_SPLIT_REASON_DP_REFUSED_SCANOUT "dp_refused_scanout"
/*!
 * The frame carries layers this rung's transport cannot move (the D3D12-ends
 * bridge is projection-only: zones, Local2D and authored masks have their
 * composite inputs on the app device). Retires the split for the session rather
 * than draw a half-split frame.
 */
#define COMP_SPLIT_REASON_LAYERS_UNSUPPORTED "layers_unsupported"
/*! @} */

/*!
 * What the caller knows before the gate decides. Everything here is resolved by
 * the caller in its own graphics API; the gate only reasons about it.
 */
struct comp_split_gate_inputs
{
	/*!
	 * The split has NOT been killed by `DXR_WEAVE_ON_SCANOUT` — see
	 * @ref comp_split_gate_env_requested. TRUE by default since #918 Phase 3;
	 * a caller that leaves this false is asking for the kill-switch branch.
	 */
	bool requested;
	/*!
	 * The caller's own eligibility verdict: one of the canonical short-reason
	 * tokens above naming why this session cannot split, or NULL when it can.
	 * Checked FIRST, so a caller that is ineligible need not resolve any
	 * adapter at all.
	 */
	const char *ineligible_reason;
	//! The caller resolved the scanout adapter AND read its description.
	bool scanout_resolved;
	//! The adapter the caller renders on. Only read when @ref scanout_resolved.
	struct comp_split_luid render_luid;
	//! The adapter that scans out the panel. Only read when @ref scanout_resolved.
	struct comp_split_luid scanout_luid;
};

//! The gate's verdict — pure data, no side effects taken on the caller's behalf.
struct comp_split_gate_result
{
	//! Echo of @ref comp_split_gate_inputs::requested.
	bool requested;
	/*!
	 * The gate says PROCEED: stand the output device and the bridge up.
	 *
	 * Not "the split is running" — everything after this point can still fail
	 * (device creation, the DXGI factory, the cross-adapter heap), and the
	 * caller owns that half of the verdict.
	 */
	bool split_active;
	/*!
	 * The scanout adapter IS the render adapter. Not a failure — on a MUX'd or
	 * single-GPU box the weave is already local, so the split has nothing to
	 * do. The caller logs its own one-line note; @ref reason is then
	 * @ref COMP_SPLIT_REASON_HANDLED so no fallback WARN follows it.
	 */
	bool same_adapter;
	//! The adapter the output half belongs on. Only set when @ref split_active.
	struct comp_split_luid out_adapter_luid;
	//! `DXR_SPLIT_INGRESS`, for a caller whose bridge has a choice.
	enum comp_split_ingress_policy ingress;
	/*!
	 * NULL to proceed; @ref COMP_SPLIT_REASON_HANDLED ("") when there is
	 * nothing to say; otherwise the static string the caller names in its one
	 * fallback WARN.
	 */
	const char *reason;
	/*!
	 * The canonical short reason, ALWAYS set when @ref split_active is false and
	 * always NULL when it is true. Never empty and never a phrase — this is what
	 * `[RENDER] split=0 reason=` and the `weave placement:` line print, and what
	 * a support case is grepped for.
	 *
	 * Distinct from @ref reason only in the two states the gate reports as flags
	 * rather than prose (killed by env, same adapter), where @ref reason is
	 * @ref COMP_SPLIT_REASON_HANDLED because there is nothing for a human to
	 * read but there is very much something for a log to say.
	 */
	const char *short_reason;
};

/*!
 * Decide. Deterministic, allocation-free, and safe to call before any device
 * exists.
 */
void
comp_split_gate_evaluate(const struct comp_split_gate_inputs *inputs, struct comp_split_gate_result *out_result);

/*!
 * Is the output-device split allowed to engage? **TRUE by default** since #918
 * Phase 3 (ADR-037 §1: the split is not a mode to opt into, it is what the
 * placement rule degenerates to when render and scanout differ). Latched on
 * first call for process lifetime.
 *
 * `DXR_WEAVE_ON_SCANOUT` is therefore a **kill switch**, not an opt-in:
 *
 * | value | meaning |
 * |---|---|
 * | unset | split allowed (the default) |
 * | `0` / `f…` / `F…` / `n…` / `N…` / `off` / `OFF` | split KILLED — old single-adapter behaviour |
 * | `1` / anything else | split allowed (so every existing `=1` script and doc still works) |
 *
 * **The in-process D3D11 compositor deliberately does NOT call this.** It reads
 * the same variable through `DEBUG_GET_ONCE_BOOL_OPTION`, and the two parsers
 * still disagree on non-boolean values — `debug_string_to_bool` matches whole
 * strings exactly (`"nope"` → true), this one tests the leading character
 * (`"nope"` → false/killed). D12-0 declined to unify them because that is a
 * behaviour change; Phase 3 flips the DEFAULT of each in its own idiom and
 * leaves the disagreement exactly where it was. Both now default to allowed.
 * The compositor passes its own answer in as
 * @ref comp_split_gate_inputs::requested instead.
 */
bool
comp_split_gate_env_requested(void);

/*!
 * The unlatched parser behind @ref comp_split_gate_env_requested, exposed so the
 * default flip is unit-testable (the latched wrapper answers once per process
 * and so can only ever be tested for one value).
 *
 * @param value the raw environment value, or NULL when unset.
 * @return true when the split is allowed.
 */
bool
comp_split_gate_parse_requested(const char *value);

/*!
 * `DXR_TEST_SPLIT_FAIL_STAGEA=1` — testability hook for the fallback matrix.
 * Forces Stage A to fail at the point the bridge would be created, so the "one
 * WARN, stock path" degrade can be exercised without a machine that genuinely
 * cannot allocate the heap. Latched on first call.
 *
 * This is the PRE-activation half of the matrix. Its sibling is
 * `DXR_TEST_FAKE_DP_REFUSE=1` (D3D12 in-process compositor,
 * `d3d12_test_fake_dp_refuse`), which walks the POST-activation half: a display
 * processor that declines a weaver on the scanout adapter once the split is
 * already up. Named here rather than only there because a verification pass
 * that finds one arm and not the other covers half the matrix and reads as if
 * it covered all of it. The two are deliberately independent envs — they fail
 * at different points and are meant to be walked separately.
 */
bool
comp_split_gate_env_test_fail_stage_a(void);

//! `DXR_SPLIT_INGRESS`, parsed fresh (it is read once per session).
enum comp_split_ingress_policy
comp_split_gate_env_ingress_policy(void);

#ifdef __cplusplus
}
#endif
