// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-027 tier-1: WHICH display processor answers "does the weaver consume
 *         zone masks?"
 * @ingroup comp_util
 *
 * The tier-1 fallback is "any zone active on a legacy DP ⟹ request hardware 3D
 * once". Getting the *decision* right was never the hard part; getting the
 * decision's **input** from the right place was, and that is what #1331 fixed.
 *
 * On a split or reroute session the weaver does not live on the tier that owns
 * the compositor — it lives on the d3d11 fill arm — and the tier's own
 * `display_processor` is NULL **by construction** (the VK DP's create is guarded
 * `if (c->split == NULL)`; the #1264 reroute has no own DP at all). Both tiers
 * therefore used to answer "legacy DP" for a weaver that in fact advertises zone
 * slots, fire the fallback on the first zones frame, and silently override an
 * app's already-honoured 2D rendering mode for the rest of the session.
 *
 * The dispatch lives here, as one pure function over the two facts, for two
 * reasons:
 *
 *  1. **Both tiers must agree.** `comp_vk_native_compositor.c` and
 *     `comp_d3d12_compositor.cpp` had byte-identical gates and were fixed with
 *     byte-identical predicates. Two copies of a rule is how one of them drifts.
 *  2. **It is the only part that can be tested without a GPU.** The gate itself
 *     needs a compositor, a device and a weaver; this needs three booleans. See
 *     `tests/tests_comp_zone_tier1.cpp`, which pins the regression case
 *     directly — the app-level arm cannot run on a hosted CI runner (no Vulkan
 *     ICD, "Microsoft Hyper-V Video" only), so without this seam the fix is
 *     covered on developer boxes and nowhere else.
 *
 * Deliberately owns no graphics types and no compositor types, so a test links
 * it for free.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The answer the ADR-027 tier-1 gate must use: ask whichever arm OWNS the weaver.
 *
 * @param weaver_on_arm     This session's weaver lives on the fill arm — a split
 *                          (#918) or reroute (#1264) session. The tier's own
 *                          `display_processor` is NULL whenever this is true, so
 *                          its answer describes nothing.
 * @param arm_consumes      The ARM's answer (`comp_vk_split_zone_dp_supported`).
 *                          Only meaningful when @p weaver_on_arm.
 * @param own_dp_consumes   The tier's OWN display processor's answer. Only
 *                          meaningful when the tier owns its weaver.
 *
 * @return true if the weaver consumes zone masks, so tier-1 must NOT force 3D.
 *
 * The one case that matters, and the one that shipped broken:
 * `weaver_on_arm=true, arm_consumes=true, own_dp_consumes=false` must be **true**.
 * Pre-#1331 this read `own_dp_consumes` unconditionally and returned false, which
 * is precisely how a zones app lost its 2D request on the shipped defaults.
 */
static inline bool
comp_zone_tier1_dp_consumes_zones(bool weaver_on_arm, bool arm_consumes, bool own_dp_consumes)
{
	return weaver_on_arm ? arm_consumes : own_dp_consumes;
}

/*!
 * Whether tier-1 should force hardware 3D this frame.
 *
 * Split out from the callers so the rising-edge rule ("once, on the first zones
 * frame") is stated once rather than in two compositors. @p dp_consumes_zones is
 * expected to come from @ref comp_zone_tier1_dp_consumes_zones.
 *
 * @param zones_frame        This frame submitted at least one zone layer.
 * @param already_requested  The gate already fired this zones run.
 * @param dp_consumes_zones  The weaver consumes zone masks.
 */
static inline bool
comp_zone_tier1_should_force_3d(bool zones_frame, bool already_requested, bool dp_consumes_zones)
{
	return zones_frame && !already_requested && !dp_consumes_zones;
}

#ifdef __cplusplus
}
#endif
