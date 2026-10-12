// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion policy stages (ADR-053 decision 4; roadmap
 *         conversion-providers.md §5.1): viewpoints, convergence, depth
 *         budget, depth range, observers, and the per-DP eyes snapshot.
 *
 * Policy stays in the runtime; providers do pixels and units. These are thin
 * stage wrappers around the code that already exists, never a rewrite:
 *
 *  - S3 viewpoints: the d3d11_lift resolve_viewpoints() logic, over
 *    u_lift_viewpoint (ipd / parallax / axis mask / u_lift_recenter / clamp)
 *    and u_lift_rig (the v3 app rig);
 *  - S3 convergence: the app's [0,1] "relative depth at glass" (or an
 *    observer's) mapped onto the provider's DECODED depth units with the last
 *    result's range, so estimator and provider cannot disagree on
 *    normalisation; a provider's suggested_convergence is ignored unless its
 *    capability row is corpus-validated;
 *  - S3 budget: from the content's range for METRIC kinds only; for AFFINE
 *    kinds the strength-scaled default under the ADR-040 rear cap;
 *  - S6 depth range: per-frame decoded extent + p5/p95, never a silent
 *    min-max normalisation of the samples;
 *  - S7 observers: an interface the auto-convergence of rt#1978
 *    (u_lift_autoconv) slots into — it consumes aux depth with
 *    same_inference = 1 only and resets on FRAME_RESET and on a result
 *    discontinuity (provider rebind).
 *
 * Eyes: one snapshot per live display processor, published from its eye
 * listener or a timer, read by the dispatcher. Its lock is a leaf; conversion
 * never takes the compositor's render_mutex (G7).
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include "os/os_threading.h"
#include "util/u_lift_rig.h"
#include "util/u_lift_viewpoint.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *
 * Eyes snapshot (one per live display processor).
 *
 */

//! Eyes a snapshot carries.
#define CONV_EYES_MAX 8u
//! Nominal interpupillary distance used when no eyes are known, metres.
#define CONV_POLICY_NOMINAL_IPD_M 0.063f

struct conv_eyes
{
	bool valid;
	bool is_tracking;
	uint32_t count;
	float xyz[CONV_EYES_MAX][3]; //!< display space, metres
	uint64_t timestamp_ns;
};

/*! A per-DP snapshot slot. Writer: the DP's eye listener / timer; readers:
 *  dispatchers. The mutex is a leaf held for one copy. */
struct conv_eyes_slot
{
	struct os_mutex mtx;
	struct conv_eyes eyes;
	uint64_t generation; //!< bumped per publish; carries the #363 lifetime guarantee
	bool live;           //!< false once the DP went away (readers get stale)
};

int
conv_eyes_slot_init(struct conv_eyes_slot *s);

void
conv_eyes_slot_fini(struct conv_eyes_slot *s);

void
conv_eyes_slot_publish(struct conv_eyes_slot *s, const struct conv_eyes *e);

//! The DP went away: later reads report stale.
void
conv_eyes_slot_retire(struct conv_eyes_slot *s);

/*! Copy the snapshot. Returns false (out->valid = false) when stale: the DP is
 *  gone or never published, or the snapshot is older than @p max_age_ns
 *  (0 = no age limit) at @p now_ns. */
bool
conv_eyes_slot_read(struct conv_eyes_slot *s, uint64_t now_ns, uint64_t max_age_ns, struct conv_eyes *out);


/*
 *
 * S3: viewpoints (ADR-048).
 *
 */

struct conv_policy_view_in
{
	uint32_t view_count; //!< VIEWS tasks: views to produce; 0 = not a view-producing stream

	/* EXPLICIT viewpoints (the app's, display space): only rebased. */
	uint32_t explicit_count;
	float explicit_xyz[XRT_CONV_MAX_VIEWS][3];

	struct u_lift_view_control vc; //!< raw: sanitized here
	struct u_lift_rig rig;         //!< raw: sanitized here
	bool recenter_off;             //!< DXR_LIFT_RECENTER=0

	bool rect_valid;
	float rect_center[3]; //!< display space, metres
	float rect_w_m, rect_h_m;
	float nominal_z_m; //!< ≤ 0 = U_LIFT_NOMINAL_Z_DEFAULT_M

	const struct conv_eyes *eyes; //!< NULL / !valid = stale: the nominal viewer
	uint64_t now_ns;
};

//! Per-stream S3 state.
struct conv_policy_view_state
{
	struct u_lift_recenter recenter;
};

//! What S3 decided, beyond what it wrote into the params.
struct conv_policy_view_out
{
	float mid[3]; //!< viewpoint midpoint (rect-relative)
	float centre[3];
	bool nominal;    //!< no live eyes: the nominal viewer was used
	uint32_t source; //!< XRT_CONV_VIEWPOINTS_*
};

/*!
 * Resolve the viewpoints of one conversion into @p params (viewpoint_frame =
 * RECT, viewpoint_source, axis_mode, viewpoint_count, viewpoints_xyz,
 * rect_width_m / rect_height_m, baseline_m, max_offset_m, nominal_z_m). For a
 * VIEWS task exactly view_count viewpoints are written: N tracked eyes when
 * the panel reports N, else the pair (or the nominal pair) spread at its own
 * spacing around its midpoint. A non-view task gets viewpoint_count = 0.
 */
void
conv_policy_viewpoints(const struct conv_policy_view_in *in,
                       struct conv_policy_view_state *state,
                       struct xrt_conv_params *params,
                       struct conv_policy_view_out *out);


/*
 *
 * Depth units.
 *
 */

/*! Decoded value -> nearness (larger = nearer): METRIC 1/z (≤ 0 -> 0), AFFINE
 *  -v, every inverse / disparity kind and UNKNOWN pass through. The same rule
 *  as the reference view synthesis (u_conv_viewsyn_nearness). */
double
conv_policy_nearness(uint32_t depth_kind, double decoded);

//! Inverse of conv_policy_nearness (METRIC: nearness ≤ 0 -> +inf as FLT_MAX).
double
conv_policy_from_nearness(uint32_t depth_kind, double nearness);

/*! The decoded values at the NEAR and FAR ends of a result's content: p05 /
 *  p95 when computed, else min / max, oriented by the kind's polarity.
 *  False when the result carries no range. */
bool
conv_policy_depth_range(const struct xrt_conv_depth_info *di, float *out_near, float *out_far);

/*!
 * S6: fill di->range_min / range_max / range_p05 / range_p95 from CPU
 * samples (decoded with value_scale / value_shift; invalid_sample skipped),
 * unless the provider already did. Samples are R32 floats, @p stride_floats
 * per row. Subsamples to ≤ 4096 values for the percentiles.
 * @return true when a range is (now) present.
 */
bool
conv_policy_depth_stats(
    struct xrt_conv_depth_info *di, const float *samples, uint32_t w, uint32_t h, uint32_t stride_floats);


/*
 *
 * S3: convergence, in DECODED units.
 *
 */

/*!
 * Map an app's [0,1] relative depth at the glass (0 = the nearest content on
 * the glass, 1 = the farthest; the autoconv contract's convergence) onto the
 * decoded units of @p last's depth, linearly in nearness over its range.
 * False when @p last has no range (the caller then hands AUTO).
 */
bool
conv_policy_convergence_to_decoded(float rel01, const struct xrt_conv_depth_info *last, float *out_decoded);

//! Inverse mapping (round trip, diagnostics). False without a range.
bool
conv_policy_convergence_from_decoded(float decoded, const struct xrt_conv_depth_info *last, float *out_rel01);

/*!
 * xrt_conv_params::convergence_mode for a CONV_PROV_LEGACY_UNITS provider
 * only (the in-service `dp-lift` adapter, #2003): params.convergence is the
 * RELATIVE plane of the legacy contract, [0,1], 0 = the nearest content on
 * the glass. Runtime-internal: never sent to a registered provider (they get
 * AUTO / DECODED only), so it is not part of the provider ABI.
 */
#define CONV_CONVERGENCE_MODE_LEGACY_RELATIVE 0x4C524C31u

/*! @name Convergence source (conv_policy_conv_out::source) @{ */
#define CONV_CONVERGENCE_SRC_AUTO 0u     //!< nothing given: the provider's own estimate / default
#define CONV_CONVERGENCE_SRC_APP 1u      //!< the app's explicit plane
#define CONV_CONVERGENCE_SRC_OBSERVER 2u //!< the runtime estimator (S7)
#define CONV_CONVERGENCE_SRC_HINT 3u     //!< a corpus-validated provider hint
/*! @} */

struct conv_policy_conv_in
{
	bool app_set;    //!< the app gave a plane
	float app_rel01; //!< [0,1]
	bool observer_valid;
	float observer_rel01;
	//! The bound capability row is validated against the stereo corpus AND
	//! declares SUGGESTS_CONVERGENCE: its hint may be used.
	bool hint_allowed;
	const struct xrt_conv_depth_info *last; //!< last published result's depth (NULL = none)
};

struct conv_policy_conv_out
{
	uint32_t source; //!< CONV_CONVERGENCE_SRC_*
	float rel01;     //!< the relative plane used (NaN-free; 0 when AUTO)
};

/*! Fill params->convergence_mode / convergence. Order: app, observer,
 *  validated hint, AUTO. A relative plane without a last range is AUTO. */
void
conv_policy_convergence(const struct conv_policy_conv_in *in,
                        struct xrt_conv_params *params,
                        struct conv_policy_conv_out *out);


/*
 *
 * S3: depth budget.
 *
 */

//! Default total near-far disparity of the nominal pair, fraction of width.
#define CONV_POLICY_BUDGET_DEFAULT 0.024f
//! METRIC budgets are capped at this multiple of the default.
#define CONV_POLICY_BUDGET_METRIC_MAX_FACTOR 2.0f

/*! @name Budget source @{ */
#define CONV_BUDGET_SRC_METRIC_RANGE 1u   //!< derived from the content's metric range
#define CONV_BUDGET_SRC_STRENGTH 2u       //!< affine: strength × default (rear-capped)
#define CONV_BUDGET_SRC_VALIDATED_HINT 3u //!< affine × a corpus-validated suggested_strength
/*! @} */

struct conv_policy_budget_in
{
	uint32_t depth_kind;                    //!< XRT_CONV_DEPTH_* of the stream's depth
	const struct xrt_conv_depth_info *last; //!< last result (range, hints); may be NULL
	float strength;                         //!< app / user strength, 1 = default (≤ 0 -> 1)
	float default_budget;                   //!< ≤ 0 -> CONV_POLICY_BUDGET_DEFAULT
	float rear_cap_m;                       //!< ADR-040; < 0 = unlimited
	float viewer_distance_m;                //!< ≤ 0 -> U_LIFT_NOMINAL_Z_DEFAULT_M
	float display_width_m;                  //!< the lifted rect's width, metres
	float ipd_m;                            //!< ≤ 0 -> CONV_POLICY_NOMINAL_IPD_M
	bool corpus_validated;                  //!< hints may be used
};

struct conv_policy_budget_out
{
	uint32_t source;  //!< CONV_BUDGET_SRC_*
	float budget;     //!< total near-far disparity of the nominal pair, fraction of width
	float rear_cap_m; //!< passed on (S5 and the provider's params)
	float strength;   //!< budget / default: what xrt_conv_params::strength carries (1 = provider budget)
};

/*!
 * The budget rule (roadmap §5.1). METRIC / METRIC_INVERSE with a range:
 * natural = ipd · (1/z_near − 1/z_far) · D / W, clamped to
 * [0, default · CONV_POLICY_BUDGET_METRIC_MAX_FACTOR], × strength. Every
 * other kind (affine: scale unknown by definition): default × strength, × a
 * validated suggested_strength when allowed. The rear cap rides along.
 */
void
conv_policy_budget(const struct conv_policy_budget_in *in, struct conv_policy_budget_out *out);

//! METRIC or METRIC_INVERSE.
bool
conv_policy_kind_is_metric(uint32_t depth_kind);


/*
 *
 * S7: observers.
 *
 */

//! CPU view of a depth result (NULL samples = GPU-only, no readback).
struct conv_depth_samples
{
	const float *samples;
	uint32_t width, height, stride_floats;
};

struct conv_observer_ops
{
	//! FRAME_RESET, a result discontinuity (rebind), stream create.
	void (*reset)(void *ctx);
	//! One same-inference depth result; @p active = x0,y0,x1,y1 in [0,1] (letterbox).
	void (*observe)(void *ctx,
	                const struct xrt_conv_depth_info *di,
	                const struct conv_depth_samples *s,
	                const float active[4],
	                uint64_t now_ns);
	//! The relative plane to deliver now ([0,1]); false = nothing yet.
	bool (*value)(void *ctx, uint64_t now_ns, float *out_rel01);
	void (*destroy)(void *ctx);
};

struct conv_observer
{
	const struct conv_observer_ops *ops;
	void *ctx;
};

/*!
 * Feed @p obs one result. Only aux depth with same_inference = 1 is observed:
 * otherwise the plane is computed on a different frame than the one shown.
 * @return true when the observer took it.
 */
bool
conv_policy_observe(struct conv_observer *obs,
                    const struct xrt_conv_depth_info *di,
                    const struct conv_depth_samples *s,
                    const float active[4],
                    uint64_t now_ns);

void
conv_policy_observer_reset(struct conv_observer *obs);

#ifdef __cplusplus
}
#endif
