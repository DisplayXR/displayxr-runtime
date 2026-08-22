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
 *   1. Did anyone ASK for the split (`DXR_WEAVE_ON_SCANOUT`)?
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
 * was never asked for, or the caller has already logged the situation itself
 * (the same-adapter no-op). Distinct from NULL, which means "proceed".
 */
#define COMP_SPLIT_REASON_HANDLED ""

//! The scanout adapter (or the render adapter it is compared against) could not
//! be resolved, so there is no way to tell whether this session crosses adapters.
#define COMP_SPLIT_REASON_SCANOUT_UNRESOLVABLE "scanout unresolvable"

/*!
 * What the caller knows before the gate decides. Everything here is resolved by
 * the caller in its own graphics API; the gate only reasons about it.
 */
struct comp_split_gate_inputs
{
	//! `DXR_WEAVE_ON_SCANOUT` — see @ref comp_split_gate_env_requested.
	bool requested;
	/*!
	 * The caller's own eligibility verdict: a static string naming why this
	 * session cannot split, or NULL when it can. Checked FIRST, so a caller
	 * that is ineligible need not resolve any adapter at all.
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
};

/*!
 * Decide. Deterministic, allocation-free, and safe to call before any device
 * exists.
 */
void
comp_split_gate_evaluate(const struct comp_split_gate_inputs *inputs, struct comp_split_gate_result *out_result);

/*!
 * `DXR_WEAVE_ON_SCANOUT=1` asks for the output-device split. Default OFF for a
 * release cycle (supervisor ruling); #918 Phase 3 owns default-on. Latched on
 * first call for process lifetime.
 *
 * **The in-process D3D11 compositor deliberately does NOT call this.** It reads
 * the same variable through `DEBUG_GET_ONCE_BOOL_OPTION`, whose parse of a
 * non-boolean value differs from this one's leading-character test, and
 * unifying the two is a behaviour change that does not belong in a mechanical
 * refactor. It passes its own answer in as
 * @ref comp_split_gate_inputs::requested instead.
 */
bool
comp_split_gate_env_requested(void);

/*!
 * `DXR_TEST_SPLIT_FAIL_STAGEA=1` — testability hook for the fallback matrix.
 * Forces Stage A to fail at the point the bridge would be created, so the "one
 * WARN, stock path" degrade can be exercised without a machine that genuinely
 * cannot allocate the heap. Latched on first call.
 */
bool
comp_split_gate_env_test_fail_stage_a(void);

//! `DXR_SPLIT_INGRESS`, parsed fresh (it is read once per session).
enum comp_split_ingress_policy
comp_split_gate_env_ingress_policy(void);

#ifdef __cplusplus
}
#endif
