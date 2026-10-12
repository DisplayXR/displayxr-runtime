// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Reference view synthesis (ADR-053 stage S5): the platform-neutral
 *         contract and the pure math of the depth-image-based renderer.
 *
 * The stage turns ONE colour image + ONE depth image into N views at explicit
 * viewpoints. It is a runtime pipeline stage, never a provider and never
 * selectable (ADR-053 decision 3); `conv_pipeline` runs it for
 * VIEWS_FROM_DEPTH and after any provider that outputs depth only. It is a
 * port of the browser's in-page `live-dibr.js` (displayxr-web-lift), whose
 * hole-fill it carries over unchanged so the browser can delete its copy.
 *
 * What the stage does NOT do (ADR-053 decision 4 — policy is runtime-owned and
 * lives in other stages): percentile normalisation, EMA, auto-convergence,
 * scene-cut detection, look-around anchoring, the budget decision. It receives
 * their RESULTS: depth in its declared units, a normalisation range and a
 * convergence plane in DECODED units, a disparity budget and the ADR-040 rear
 * cap. There is no per-frame min-max normalisation anywhere in this stage.
 *
 * Geometry. Everything inside the renderer is in units of the lifted RECT's
 * HEIGHT ("window heights"), origin at the rect centre, +x right, +y up, +z
 * toward the viewer — the same frame the browser recovered from each view's
 * Kooima projection. Viewpoints arrive in metres in that frame (ADR-048 D1,
 * `viewpoint_frame = RECT`), so the eye is `viewpoint / rect_height_m`.
 *
 * Parallax model (docs/architecture/conversion-view-synthesis.md):
 *   n  = clamp((N - N_far) / (N_near - N_far))       normalised nearness
 *   q  = qScale * (n - n_conv) * taper              relative parallax, 0 on the glass
 *   q <- q * rear_k  for q < 0                      ADR-040 rear cap (compress)
 *   qScale = budget * A / (KAPPA * C.z)
 * and the ray from eye E through output point p' reaches source position
 *   x_s = C + (E + (p' - E)(E.z - z)/E.z - C)(1 + q),  z = q C.z / (1 + q).
 *
 * The draft ABI (`xrt_conversion_plugin.h`, docs/roadmap/conversion-providers-
 * headers-draft.md) is being created on another branch. The few fields this
 * stage needs are MIRRORED below with the same names and values; reconcile
 * them with `struct xrt_conv_depth_info` / `XRT_CONV_DEPTH_*` /
 * `XRT_CONV_VIEWS_*` once that header lands (TODO(conv-p1a-hdr)).
 *
 * C11, no allocation, no graphics API; host-tested by
 * tests/tests_conv_viewsyn_math.cpp.
 *
 * @ingroup comp_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Upper bound on views; == XRT_CONV_MAX_VIEWS (draft) == XR_LIFT_MAX_VIEWS_DXR. */
#define U_CONV_VIEWSYN_MAX_VIEWS 8u

/*! Nominal viewing geometry: IPD / viewing distance (63 mm at 0.6 m). Sets how
 *  much parallax a budget means for an eye at a given offset; it cancels for
 *  the nominal pair. Same constant as live-dibr.js `KAPPA`. */
#define U_CONV_VIEWSYN_KAPPA (0.063 / 0.6)

/*! @name Depth kinds — MIRROR of XRT_CONV_DEPTH_* (draft), same values.
 * @{ */
#define U_CONV_VIEWSYN_DEPTH_UNKNOWN 0u             //!< treated as AFFINE_DISPARITY (larger = nearer)
#define U_CONV_VIEWSYN_DEPTH_METRIC 1u              //!< metres
#define U_CONV_VIEWSYN_DEPTH_METRIC_INVERSE 2u      //!< 1 / metres
#define U_CONV_VIEWSYN_DEPTH_AFFINE_INVERSE 3u      //!< inverse depth up to scale + shift (larger = nearer)
#define U_CONV_VIEWSYN_DEPTH_AFFINE_DISPARITY 4u    //!< disparity up to scale + shift (larger = nearer)
#define U_CONV_VIEWSYN_DEPTH_AFFINE 5u              //!< depth up to scale + shift (larger = farther)
#define U_CONV_VIEWSYN_DEPTH_STEREO_DISPARITY_PX 6u //!< px disparity of a rectified pair (larger = nearer)
/*! @} */

/*! @name Output layouts — MIRROR of XRT_CONV_VIEWS_* (draft), same values.
 * @{ */
#define U_CONV_VIEWSYN_LAYOUT_ROW 0u   //!< N views side by side, view 0 leftmost
#define U_CONV_VIEWSYN_LAYOUT_ARRAY 1u //!< view i in array slice i (ADR-032)
/*! @} */

/*! Rear cap value meaning "no cap" (any negative value). */
#define U_CONV_VIEWSYN_REAR_UNLIMITED (-1.0f)

/*!
 * The subset of `struct xrt_conv_depth_info` (draft, FROZEN 128 bytes) this
 * stage reads. Field names and semantics are the draft's; reconcile with the
 * real struct when `xrt_conversion_plugin.h` lands.
 */
struct u_conv_viewsyn_depth_desc
{
	uint32_t kind;        //!< U_CONV_VIEWSYN_DEPTH_* (== XRT_CONV_DEPTH_*)
	float value_scale;    //!< decoded = value_scale * sample + value_shift; 0 is read as 1
	float value_shift;    //!< see value_scale
	float invalid_sample; //!< raw "no data" sample value; NaN = none
};

/*!
 * Every input of one synthesis, already decided by the policy stages.
 * Defaults (u_conv_viewsyn_params_defaults) are live-dibr.js's as shipped by
 * the browser's `lift()` (dilate 0, everything else DEFAULT_PARAMS).
 */
struct u_conv_viewsyn_params
{
	/* Geometry (ADR-048 D1: rect-relative, metres, +z toward the viewer). */
	uint32_t view_count;                             //!< 1..U_CONV_VIEWSYN_MAX_VIEWS
	float viewpoints_m[U_CONV_VIEWSYN_MAX_VIEWS][3]; //!< eye of view i
	float rect_width_m, rect_height_m;               //!< the lifted rect's physical size
	bool has_source_camera;                          //!< false = the eyes' centroid (live-dibr.js)
	float source_camera_m[3];                        //!< explicit source camera (look-around policy)

	/* Depth units and the policy's numbers, all in DECODED units. */
	struct u_conv_viewsyn_depth_desc depth;
	float range_near, range_far; //!< decoded values mapped to n = 1 / n = 0 (smoothed range, policy-owned)
	float convergence;           //!< decoded value placed on the glass (zero parallax)
	float budget;     //!< total near–far disparity of the nominal pair, fraction of width (strength folded in)
	float rear_cap_m; //!< ADR-040: max distance behind the glass, metres; < 0 = unlimited, 0 = nothing behind

	/* Look (live-dibr.js DEFAULT_PARAMS). */
	float edge_taper;       //!< border fade of disparity to 0, fraction of width
	uint32_t steps;         //!< coarse march steps, clamped 4..64
	uint32_t dilate;        //!< foreground max filter at depth res, depth px (<= 16); lift() ships 0
	bool upsample;          //!< joint-bilateral upsample of the depth to colour res
	float upsample_sigma_s; //!< spatial sigma, depth px
	float upsample_sigma_r; //!< range sigma, RGB 0..1
	uint32_t upsample_max;  //!< long-side cap of the upsampled raster, px
	uint32_t dilate_hi;     //!< max filter after the upsample, upsampled px (<= 8)
	bool snap;              //!< edge snap (needs upsample)
	float snap_core;        //!< core erosion, px at 364 wide (scaled with the depth width)
	float snap_radius;      //!< snap window radius, px at 364 wide (scaled, <= 12)
	float snap_tau;         //!< min local depth range, fraction of the normalised range
	uint32_t debug;         //!< 1 = draw the normalised nearness (grey)
};

/*! Plain 3-vector in window heights. */
struct u_conv_viewsyn_vec3
{
	double x, y, z;
};

/*! Per-view numbers the shader consumes (one draw per view). */
struct u_conv_viewsyn_view
{
	struct u_conv_viewsyn_vec3 eye; //!< window heights, rect centre origin
	double q_scale;                 //!< q per unit of (n - n_conv)
};

/*! Per-frame numbers shared by every view. */
struct u_conv_viewsyn_frame
{
	uint32_t view_count;
	struct u_conv_viewsyn_view views[U_CONV_VIEWSYN_MAX_VIEWS];
	struct u_conv_viewsyn_vec3 cam; //!< source camera, window heights
	double aspect;                  //!< A = rect width / height
	double near_lo, near_hi;        //!< nearness range (N_far, N_near), hi > lo
	double conv_n;                  //!< convergence as normalised nearness, [0, 1]
	double rear_k;                  //!< scale on q < 0 (1 = no cap)
	double taper;                   //!< edge_taper
	uint32_t steps;                 //!< clamped 4..64
};

/*! Fill @p p with the browser's shipping defaults; geometry and policy fields zeroed. */
void
u_conv_viewsyn_params_defaults(struct u_conv_viewsyn_params *p);

/*! value = scale * sample + shift (scale 0 read as 1). */
double
u_conv_viewsyn_decode(const struct u_conv_viewsyn_depth_desc *d, double sample);

/*!
 * Decoded value -> NEARNESS (larger = nearer, linear in disparity where the
 * kind allows). METRIC -> 1/z (non-positive -> 0, live-dibr.js `toDisparity`);
 * AFFINE (larger = farther) -> -value; every inverse/disparity kind and
 * UNKNOWN pass through.
 */
double
u_conv_viewsyn_nearness(uint32_t kind, double decoded);

/*!
 * Raw sample -> unclamped normalised nearness u = (N - lo) / (hi - lo).
 * An invalid sample (== invalid_sample) reads as the far end (u = 0).
 */
double
u_conv_viewsyn_normalise(const struct u_conv_viewsyn_depth_desc *d, double near_lo, double near_hi, double sample);

/*! Viewpoint (metres, rect-relative) -> eye in window heights. */
struct u_conv_viewsyn_vec3
u_conv_viewsyn_eye_from_viewpoint(const float viewpoint_m[3], double rect_height_m);

/*!
 * live-dibr.js `viewEye`: eye (window heights) and window aspect from a
 * column-major Kooima projection. eye = (-P8 P5 / 2P0, -P9 / 2, P5 / 2), A = P5 / P0.
 */
struct u_conv_viewsyn_vec3
u_conv_viewsyn_eye_from_projection(const float P[16], double *out_aspect);

/*! live-dibr.js `kooimaProjection`: column-major off-axis projection for @p eye. */
void
u_conv_viewsyn_kooima_projection(
    struct u_conv_viewsyn_vec3 eye, double aspect, double near_z, double far_z, float out_P[16]);

/*!
 * live-dibr.js `sourceCamera`: the eyes' centroid in all three axes; with
 * @p anchor and @p look_around in (0, 1], x/y blend toward the anchor (z never).
 */
struct u_conv_viewsyn_vec3
u_conv_viewsyn_source_camera(const struct u_conv_viewsyn_vec3 *eyes,
                             uint32_t count,
                             const struct u_conv_viewsyn_vec3 *anchor,
                             double look_around);

/*! live-dibr.js `qScale`: budget * gain * A / (kappa * D0). */
double
u_conv_viewsyn_q_scale(double budget, double gain, double aspect, double d0, double kappa);

/*!
 * ADR-040 rear cap as a scale on negative q: content may not sit further than
 * @p rear_cap_wh window heights behind the glass. The deepest q this frame can
 * produce is q_far = q_scale * (0 - conv_n); the cap's q is -R / (C.z + R).
 * Returns 1 when the cap is not binding or unlimited (< 0).
 */
double
u_conv_viewsyn_rear_k(double q_scale, double conv_n, double cam_z, double rear_cap_wh);

/*!
 * live-dibr.js `srcUvRef` (the shader's `srcUv`): the source uv (v UP, 0..1)
 * that the ray from @p eye through output @p uv_out samples at parallax @p q.
 */
void
u_conv_viewsyn_src_uv(const double uv_out[2],
                      double q,
                      struct u_conv_viewsyn_vec3 eye,
                      struct u_conv_viewsyn_vec3 cam,
                      double aspect,
                      double out_uv[2]);

/*! Border taper factor at source uv (v up): smoothstep(0, taper, min(u-dist, v-dist / A)). */
double
u_conv_viewsyn_taper(const double uvs[2], double taper, double aspect);

/*!
 * Depth-px snap radii at depth width @p depth_w from params given at the
 * 364-wide tier (live-dibr.js `snapRadii`); radius clamped 1..12.
 */
void
u_conv_viewsyn_snap_radii(
    double snap_core, double snap_radius, uint32_t depth_w, uint32_t *out_core, uint32_t *out_radius);

/*! Size of the upsampled raster: the colour size, long side capped at @p max_edge. */
void
u_conv_viewsyn_upsample_size(uint32_t src_w, uint32_t src_h, uint32_t max_edge, uint32_t *out_w, uint32_t *out_h);

/*!
 * Resolve @p p into the per-frame / per-view numbers the GPU pass needs.
 * @return false on invalid geometry (no views, a non-positive rect size or eye
 *         distance); @p out is then untouched.
 */
bool
u_conv_viewsyn_resolve(const struct u_conv_viewsyn_params *p, struct u_conv_viewsyn_frame *out);

/*! @name The march schedule (shader `main`, mirrored on the CPU).
 * @{ */

/*!
 * Parallax at a source uv (v up), as the GPU computes it: sample the
 * normalised nearness there, clamp, apply taper and the rear cap. The
 * callback returns the UNCLAMPED normalised nearness u at that source uv.
 */
typedef double (*u_conv_viewsyn_sample_fn)(void *user, double u, double v_up);

double
u_conv_viewsyn_q_at(const struct u_conv_viewsyn_frame *f,
                    const struct u_conv_viewsyn_view *view,
                    u_conv_viewsyn_sample_fn sample,
                    void *user,
                    const double uvs[2]);

/*! The march bracket: [q_far, q_near], both widened to include 0 (the taper pulls borders to 0). */
void
u_conv_viewsyn_march_bracket(const struct u_conv_viewsyn_frame *f,
                             const struct u_conv_viewsyn_view *view,
                             double *out_q_near,
                             double *out_q_far);

/*! Number of bisection refinements after the coarse hit (the marcher's halving). */
#define U_CONV_VIEWSYN_REFINE_STEPS 6

/*!
 * CPU reference of the per-pixel ray march (no hole-fill): coarse march from
 * near to far over f->steps steps, first surface at or in front of the ray
 * wins, then U_CONV_VIEWSYN_REFINE_STEPS bisections. Writes the source uv
 * (v up) and returns the parallax q of the hit.
 */
double
u_conv_viewsyn_march(const struct u_conv_viewsyn_frame *f,
                     const struct u_conv_viewsyn_view *view,
                     u_conv_viewsyn_sample_fn sample,
                     void *user,
                     const double uv_out[2],
                     double out_src_uv[2]);
/*! @} */

/*! @name CPU references of the depth-res filters (the GPU pre-passes mirror them).
 * @{ */

/*! Separable max filter (foreground dilation), radius @p r px, clamped window. @p out may not alias @p in. */
void
u_conv_viewsyn_ref_dilate_max(const float *in, uint32_t w, uint32_t h, uint32_t r, float *tmp, float *out);

/*! Separable min filter (foreground erosion). */
void
u_conv_viewsyn_ref_erode_min(const float *in, uint32_t w, uint32_t h, uint32_t r, float *tmp, float *out);

/*!
 * The snap's depth-res texel: r = value, g = min-filtered (foreground core),
 * b = max-filtered (background core), a = 1; @p out holds w * h * 4 floats,
 * @p tmp and @p tmp2 w * h floats each.
 */
void
u_conv_viewsyn_ref_snap_pack(
    const float *in, uint32_t w, uint32_t h, uint32_t core, float *tmp, float *tmp2, float *out);
/*! @} */

#ifdef __cplusplus
}
#endif
