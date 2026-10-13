// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The legacy DP-lift adapter (`dp-lift`, ADR-053; discovery spec
 *         §4.5; roadmap conversion-providers.md §3.3): the display plug-in's
 *         frozen D3D11 lift slots (BASE+25..29, +33) and its lift-only factory
 *         exposed as a conversion provider.
 *
 * A runtime-internal provider: no DLL, no negotiate, ProbeOrder 150, hosted
 * in-service always (the display plug-in never leaves the service, ADR-035
 * D4). It is the provider-ABI half only: every GPU call runs on the ONE lift
 * thread of the D3D11 service (d3d11_lift.cpp), reached through
 * conv_dp_lift_backend, so the vendor module keeps its single-thread contract
 * and there is exactly one module instance per service. The in-service
 * shim's exec thread (conv_host_inproc) calls this adapter's [S] slots; each
 * forwards one command to the lift thread and waits for it.
 *
 * What it declares (static describe, before any factory call):
 *
 * | cap_id | task        | from DP mode | notes                                  |
 * |--------|-------------|--------------|----------------------------------------|
 * | 1      | DEPTH_MONO  | DEPTH        | content hint PHOTO                     |
 * | 2      | DEPTH_VIDEO | DEPTH        | TEMPORAL; content hint VIDEO           |
 * | 3      | VIEWS_MONO  | SBS / NVIEW  | min 2, max caps.max_views (≤ 8); aux depth iff AUX_DEPTH |
 *
 * INSTALLED while a lift factory resolves and nobody asked yet; refined after
 * the first lift_get_caps (generation bump → the core re-describes). No SPLATS
 * capability (no shipping plug-in implements the blob slot). Licence UNKNOWN,
 * commercial_ok 0, exempt from the AllowResearchOnly gate (grandfathered); the
 * module's own detail string is surfaced as the state detail.
 *
 * State is a plain copy of xrt_dp_lift_caps::module_status (values equal
 * XRT_CONV_STATE_*, asserted); without one: READY → READY, ACTIVATING →
 * WARMING, UNAVAILABLE → ABSENT. #2029 keeps three cases apart: the kill
 * switch (DXR_LIFT=0 / DXR_CONV=0) is DISABLED, not retryable -> terminal
 * ABSENT(policy); a module that itself answers ABSENT is not retryable ->
 * terminal ABSENT(model); "the factory found no module" stays ABSENT and
 * RETRYABLE -> FAILED(model) with the backoff, so a module installed later is
 * picked up without a restart (G5). Every warm re-resolves the factory from
 * the plug-in that registered it; a registry change (ConversionProviders or
 * DisplayProcessors), `conv warm` or a re-attached backend retries at once.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"
#include "xrt/xrt_dp_lift.h"

#include "conv_dp_lift_describe.h" // CONV_DP_LIFT_ID, the cap ids, the static describe

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! What the lift thread knows right now (answered from memory, never blocks).
struct conv_dp_lift_snapshot
{
	bool factory_present;         //!< a lift-only (or a lift-capable) factory resolves right now
	bool attempted;               //!< an activation attempt finished
	bool have_module;             //!< the lift DP is alive and answered lift_get_caps
	bool has_depth_slot;          //!< the DP carries lift_get_depth (ADR-048 Addendum A era: OFFAXIS_CAMERA)
	struct xrt_dp_lift_caps caps; //!< the last answer (valid when have_module)
	char detail[96];              //!< why there is no module (factory refused / none shipped)
	uint64_t change_count;        //!< bumps whenever anything above changed
};

//! One conversion, lift thread side.
struct conv_dp_lift_convert
{
	/* In. */
	const struct xrt_conv_image *input; //!< RGBA8, exactly the converted extent
	const struct xrt_dp_lift_params *params;
	const float *viewpoints; //!< rect-relative metres, 3 floats each; NULL = none
	uint32_t viewpoint_floats;
	bool want_depth;                           //!< fetch lift_get_depth (aux) into depth_target
	bool primary_is_depth;                     //!< a DEPTH-mode stream: lift_convert's output IS the depth
	const struct xrt_conv_image *views_target; //!< memory_kind 0 = none
	const struct xrt_conv_image *depth_target; //!< memory_kind 0 = none

	/* Out. */
	bool views_written;
	bool depth_written;
	uint32_t out_w, out_h, out_format; //!< the module's own output (XRT_CONV_FMT_*)
	bool have_depth_meta;
	struct xrt_dp_lift_depth depth; //!< aux depth metadata (resource cleared)
};

/*!
 * The lift thread, as the adapter sees it. [C] = answers from memory on the
 * caller's thread; [S] = one command run on the lift thread, the caller
 * blocked until it finished.
 */
struct conv_dp_lift_backend
{
	//! [C]
	void (*snapshot)(void *ud, struct conv_dp_lift_snapshot *out);
	/*! [S] Bring the lift device + DP up (re-resolving the factory pair from
	 *  the plug-in that registered it; G5). Returns once the attempt finished
	 *  (not once the module is READY). @p retry: also when an earlier attempt
	 *  found no module. */
	xrt_result_t (*activate)(void *ud, bool retry);
	//! [S] One lift_get_caps (the ≤ 1 Hz poll while not READY).
	void (*poll)(void *ud);
	//! [S]
	xrt_result_t (*stream_create)(void *ud, const struct xrt_dp_lift_stream_info *info, uint64_t *out_dp_id);
	//! [S]
	void (*stream_destroy)(void *ud, uint64_t dp_id);
	//! [S] One lift_convert (+ lift_get_depth) into the runtime's targets.
	xrt_result_t (*convert)(void *ud, uint64_t dp_id, struct conv_dp_lift_convert *io);
	void *ud;
};

/*!
 * Create the adapter over @p backend (copied). @p out_iface / @p out_inst go
 * into a conv_provider_desc (id CONV_DP_LIFT_ID, ProbeOrder 150, in-service,
 * CONV_PROV_LICENCE_EXEMPT | CONV_PROV_LEGACY_UNITS).
 */
bool
conv_provider_legacy_dp_create(const struct conv_dp_lift_backend *backend,
                               const struct xrt_conversion_plugin_iface **out_iface,
                               struct xrt_conversion_plugin_instance **out_inst);

/*!
 * The backend goes away (the D3D11 service system is destroyed): waits for
 * every in-flight backend call to return, then answers every slot as "no
 * module" (ABSENT, retryable) without calling it again. The instance itself
 * stays valid until the core destroys it through its host.
 */
void
conv_provider_legacy_dp_detach(struct xrt_conversion_plugin_instance *inst);

//! Re-attach a (new) backend after a detach (a re-created service system).
void
conv_provider_legacy_dp_attach(struct xrt_conversion_plugin_instance *inst, const struct conv_dp_lift_backend *backend);

#ifdef __cplusplus
}
#endif
