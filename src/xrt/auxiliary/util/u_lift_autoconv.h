// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-owned auto-convergence for lifted (2D→3D) video (epic #1858):
 *         which depth of a converted frame goes on the screen plane, and how
 *         that choice moves over time.
 *
 * Split of labour: the RUNTIME owns the policy (this file), the plug-in maps
 * the result into its module's units (xrt_dp_lift_params::convergence), the
 * conversion module renders pixels.
 *
 * Three pure stages, no graphics API, host-tested (tests_aux_lift_autoconv):
 *
 *  1. u_lift_autoconv_grid_from_samples — the conversion's own depth map (the
 *     auxiliary depth of the same inference, xrt_dp_lift_depth) decoded to
 *     inverse depth, box-downsampled to a small grid and normalised over the
 *     frame's range: h = 0 farthest, h = 1 nearest.
 *  2. u_lift_autoconv_find_subject — the centre-weighted FOREGROUND MODE of that
 *     grid: soft Gaussian centre prior + bottom damp → weighted, smoothed
 *     histogram → the peak scoring highest on weighted mass × (1 + fg_bias ·
 *     (h − weighted median)). A small near detail is little weighted mass and
 *     loses to the subject's coherent cluster; between comparable clusters the
 *     nearer one wins.
 *  3. u_lift_autoconv_filter — the temporal policy, per stream: median of the
 *     last three measurements, a dead band, and a critically damped follower
 *     (no overshoot from rest). A large change confirmed by two consecutive
 *     measurements is a scene cut: it is followed with a much shorter time
 *     constant — a ramp of a few converted frames, never a one-frame snap.
 *     No confident mode = hold.
 *
 * The delivered value is the contract's convergence (xrt_dp_lift_params): the
 * RELATIVE depth on the display plane, 0 = the nearest content on the glass,
 * 1 = the farthest, in the frame's normalised inverse-depth range — i.e.
 * convergence = 1 − h(subject).
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Largest grid the estimator works on (cells).
#define U_LIFT_AUTOCONV_GRID_MAX_W 128
#define U_LIFT_AUTOCONV_GRID_MAX_H 128
//! Histogram resolution of the mode search.
#define U_LIFT_AUTOCONV_BINS 64
//! Coarse depth histogram kept as the scene signature (cut detection).
#define U_LIFT_AUTOCONV_SIG_BINS 16
//! Default sampling period of the depth (the service reads one depth map back
//! at most this often, not every conversion).
#define U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS (125ull * 1000ull * 1000ull)

/*!
 * Who picks the convergence of a lifted stream whose app leaves it AUTO
 * (DXR_LIFT_AUTO_CONVERGENCE).
 */
enum u_lift_autoconv_source
{
	U_LIFT_AUTOCONV_SOURCE_RUNTIME = 0, //!< this policy (default)
	U_LIFT_AUTOCONV_SOURCE_MODULE = 1,  //!< the conversion module's own auto-convergence
};

/*!
 * Parse DXR_LIFT_AUTO_CONVERGENCE: "module" / "off" / "0" → MODULE; NULL, "",
 * "runtime", "1", "on" → RUNTIME; anything else → RUNTIME (the default).
 */
enum u_lift_autoconv_source
u_lift_autoconv_source_parse(const char *value);

//! Normalised inverse depth on a small grid; NaN = no depth at that cell.
struct u_lift_autoconv_grid
{
	uint32_t w, h;
	float v[U_LIFT_AUTOCONV_GRID_MAX_W * U_LIFT_AUTOCONV_GRID_MAX_H];
};

/*!
 * Decode raw depth samples (xrt_dp_lift_depth conventions), box-downsample them
 * to at most @p max_w cells wide (aspect kept, ≤ U_LIFT_AUTOCONV_GRID_MAX_*),
 * and normalise inverse depth over the frame's min..max (from the full-
 * resolution samples).
 *
 * Decode: d = value_scale · sample + value_offset (value_scale 0 reads as 1);
 * inverse depth = d (encoding_inverse, d ≥ 0 valid — 0 is the far end) or 1/d
 * (linear, d > 0 valid).
 *
 * @param stride_floats row pitch of @p samples, in floats (≥ @p sw).
 * @return false when nothing decodes or the frame is flat (no depth range).
 */
bool
u_lift_autoconv_grid_from_samples(const float *samples,
                                  uint32_t sw,
                                  uint32_t sh,
                                  uint32_t stride_floats,
                                  bool encoding_inverse,
                                  float value_scale,
                                  float value_offset,
                                  uint32_t max_w,
                                  struct u_lift_autoconv_grid *out);

//! Estimator tunables (u_lift_autoconv_params_default).
struct u_lift_autoconv_params
{
	float center_sigma;     //!< centre-prior Gaussian σ, fraction of the active width / height
	float bottom_damp;      //!< weight at the bottom row (1 at the top, linear in between)
	float hist_smooth_bins; //!< Gaussian σ of the histogram smoothing, in bins
	float fg_bias;          //!< peak score = mass · (1 + fg_bias · (h − weighted median))
	float far_exclude;      //!< peaks with h below this (the far background) are never the subject
	float percentile;       //!< centre-weighted percentile of h that the screen plane tracks (0..100)
	float blend;            //!< screen h = blend · percentile + (1 − blend) · dominant clump
	float peak_floor;       //!< a peak must hold this fraction of the highest smoothed bin
	float min_confidence;   //!< weighted mass within ±2 bins of the mode, fraction of the total
	uint32_t min_samples;   //!< valid cells needed for any estimate
	//! Subject hysteresis: the h currently followed (< 0 = none). A peak within
	//! prefer_window of it is kept unless the best peak scores switch_ratio ×
	//! higher — two comparable clusters must not trade places every sample.
	float prefer_h;
	float prefer_window; //!< |h − prefer_h| that still counts as "the same subject"
	float switch_ratio;  //!< score ratio a different peak needs to take over
};

void
u_lift_autoconv_params_default(struct u_lift_autoconv_params *p);

//! One measurement.
struct u_lift_autoconv_estimate
{
	bool valid;        //!< a confident screen plane was found
	float mode_h;      //!< normalised inverse depth placed at the screen plane (1 = nearest)
	float peak_h;      //!< the dominant clump (−1 = none outside the far background)
	float pct_h;       //!< the centre-weighted percentile
	float convergence; //!< 1 − mode_h: what the DP contract carries
	float confidence;  //!< weighted mass near the mode / total weighted mass
	float median_h;    //!< weighted median of h
	uint32_t samples;  //!< valid cells used
	//! Unweighted histogram of h over the active cells, normalised to sum 1:
	//! the scene signature the filter compares for cuts.
	float sig[U_LIFT_AUTOCONV_SIG_BINS];
};

/*!
 * Centre-weighted foreground mode of @p g.
 *
 * @param active x0, y0, x1, y1 of the region to use, normalised to the grid
 *               (letterbox bars excluded); NULL = the whole grid. The centre
 *               prior is centred on — and scaled to — this region.
 * @return out->valid.
 */
bool
u_lift_autoconv_find_subject(const struct u_lift_autoconv_grid *g,
                             const float active[4],
                             const struct u_lift_autoconv_params *p,
                             struct u_lift_autoconv_estimate *out);

//! Temporal-policy tunables (u_lift_autoconv_filter_params_default).
struct u_lift_autoconv_filter_params
{
	float tau_s;     //!< follower time constant (1/ω of the critically damped spring)
	float tau_cut_s; //!< the same after a confirmed scene cut
	float deadband;  //!< ignore target changes smaller than this (convergence units)
	float cut_jump;  //!< a measurement this far from the target is a cut candidate…
	float cut_hist;  //!< …as is a scene-signature L1 change above this (0..2)
	float cut_agree; //!< the confirming measurement lies within this of the candidate
	float cut_max_s; //!< the fast follower ends after this long at the latest
	//! A cut needs BOTH a jump > cut_jump AND a signature change > cut_hist
	//! (the per-frame re-normalised depth reshuffles its histogram on its own),
	//! then cut_confirms agreeing measurements, and at most one per refractory.
	uint32_t cut_confirms;
	float cut_refractory_s;
	//! Below this confidence the follower slows (τ × conf_full / confidence,
	//! up to 3×): a weak, ambiguous mode moves the screen plane gently.
	float conf_full;
};

void
u_lift_autoconv_filter_params_default(struct u_lift_autoconv_filter_params *p);

//! Per-stream temporal state. Zero-initialised = reset.
struct u_lift_autoconv_filter
{
	bool have;    //!< a first confident measurement was taken
	float x;      //!< delivered value (convergence)
	float v;      //!< its rate, 1/s
	float target; //!< what x follows
	uint64_t last_ns;
	float med[3];   //!< last raw measurements (median of 3)
	uint32_t n_med; //!< valid entries in med (≤ 3)
	uint32_t med_next;
	float sig[U_LIFT_AUTOCONV_SIG_BINS]; //!< previous scene signature
	bool cut_pending;
	float cut_value;
	uint32_t cut_agreed;  //!< confirming measurements so far for the pending cut
	uint64_t last_cut_ns; //!< 0 = none yet
	float conf;           //!< confidence of the last valid measurement
	bool fast; //!< following a confirmed cut
	uint64_t fast_since_ns;
	// Counters (diagnostics).
	uint32_t measurements; //!< every measure call
	uint32_t held;         //!< of those, without a confident mode
	uint32_t cuts;         //!< confirmed cuts
};

static inline void
u_lift_autoconv_filter_reset(struct u_lift_autoconv_filter *f)
{
	uint8_t *p = (uint8_t *)f;
	for (uint32_t i = 0; i < (uint32_t)sizeof(*f); i++) {
		p[i] = 0;
	}
}

/*!
 * Feed one measurement taken at @p now_ns. An invalid estimate holds the
 * target. The first valid one locks x onto it (the stream's start).
 */
void
u_lift_autoconv_filter_measure(struct u_lift_autoconv_filter *f,
                               const struct u_lift_autoconv_filter_params *p,
                               const struct u_lift_autoconv_estimate *e,
                               uint64_t now_ns);

/*!
 * Advance the follower to @p now_ns and return the value to deliver.
 * @return false while no measurement was taken yet (deliver AUTO then).
 */
bool
u_lift_autoconv_filter_step(struct u_lift_autoconv_filter *f,
                            const struct u_lift_autoconv_filter_params *p,
                            uint64_t now_ns,
                            float *out_value);

#ifdef __cplusplus
}
#endif
