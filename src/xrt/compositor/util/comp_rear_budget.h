// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-session rear-depth-budget runner shared by the native compositors.
 *
 * XR_DXR_depth_budget has three layers, and only the middle one is
 * API-specific:
 *
 * - the DP hands over pixels (`get_background_preview`, a per-API vtable slot),
 * - @ref u_bg_neutrality turns one preview into a number and @ref u_rear_budget
 *   turns a stream of numbers into a ramped budget — both pure, both in aux,
 * - and *something* has to run those two on a render thread, at the right
 *   cadence, and publish the answer for the app's locate thread.
 *
 * That third part is this file. It was written once inside the D3D11
 * compositor (#1364); every other native compositor needs the identical
 * sequence — validate, re-analyse only when the generation advanced, update the
 * policy EVERY frame, publish under a lock that is not the frame lock — and
 * duplicating it per backend is how four copies drift into four policies.
 *
 * The one thing the caller keeps is the vtable call itself, because the slot is
 * typed per graphics API: ask @ref comp_rear_budget_should_poll whether this
 * frame is due, call your own
 * `xrt_display_processor_<api>_get_background_preview()` if it is, and hand the
 * preview (or NULL) to @ref comp_rear_budget_tick.
 *
 * Threading: @ref comp_rear_budget_tick runs on the render thread and is NOT
 * re-entrant; @ref comp_rear_budget_get runs on the app's locate thread and
 * takes the instance's OWN mutex, deliberately never the compositor's frame
 * lock — a policy read must not be able to contend with frame submission.
 *
 * @author David Fattal
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_display_processor.h" // struct xrt_dp_background_preview
#include "xrt/xrt_limits.h"           // XRT_MAX_LAYERS

#include "os/os_threading.h"

#include "util/u_bg_neutrality.h"
#include "util/u_rear_budget.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * How often the DP is asked for a fresh preview, in nanoseconds.
 *
 * Two cadences on purpose: the DP poll + analysis is throttled to roughly the
 * vendor capture rate (re-reading an unchanged preview is wasted work), while
 * the policy update + publish runs EVERY frame — the app applies the published
 * far offset as-is, so the open/close ramps must advance per frame or the clip
 * plane steps in a handful of visible jumps instead of sliding.
 */
#define COMP_REAR_BUDGET_POLL_INTERVAL_NS (66ULL * 1000ULL * 1000ULL)

/*!
 * Where a `DXR_REAR_BUDGET_DUMP` preview goes. The default writes a PNG;
 * @ref comp_rear_budget_debug_set_dump_sink replaces it for tests, which is
 * what lets the dump path be exercised without touching the filesystem.
 *
 * @param bgra   Tightly packed BGRA8, top-down; @p stride is always w * 4.
 *
 * @ingroup comp_util
 */
typedef void (*comp_rear_budget_dump_fn)(void *ctx, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride);

/*!
 * How many 3D display zones one frame may contribute to the ROI clamp.
 *
 * Sized to the layer limit rather than to the four zones the extension
 * advertises, so "more zones than we can hold" is unrepresentable: the union of
 * the FIRST n zones is a SUBSET of the real one, and silently clamping to a
 * subset would aim the analysis at less than the app's content covers — the
 * exact class of quiet wrongness this whole file exists to avoid.
 *
 * @ingroup comp_util
 */
#define COMP_REAR_BUDGET_MAX_ZONES XRT_MAX_LAYERS

/*!
 * Widest and tallest content mask the runner will accept, matching
 * `XrContentMaskDXR`'s own 1..512 limit. 512 x 512 = 256 KB, the largest copy
 * one xrEndFrame can hand over.
 *
 * @ingroup comp_util
 */
#define COMP_REAR_BUDGET_MASK_MAX_DIM 512u

/*!
 * Fewest preview pixels the dilated, zone-clamped mask must cover before it is
 * used as the region.
 *
 * A mask that survives as a sliver cannot be measured — @ref
 * u_bg_neutrality_analyse_masked refuses fewer than
 * @ref U_BG_NEUTRALITY_MIN_MASKED_SAMPLES samples — and a refusal here would
 * freeze the previous verdict instead of producing a new one. So the runner
 * checks first and falls through to the rect path, which is a coarser question
 * with an answer rather than a finer one without.
 *
 * @ingroup comp_util
 */
#define COMP_REAR_BUDGET_MASK_MIN_PX 64u

/*!
 * Where the ROI the last analysis used came from. Reported on each state
 * transition, because a rear-depth verdict whose region is unattributable
 * cannot be argued with.
 *
 * @ingroup comp_util
 */
enum comp_rear_budget_roi_src
{
	//! No usable bounds and no zones — v1's whole-preview region.
	COMP_REAR_BUDGET_ROI_SRC_WHOLE_PREVIEW = 0,
	/*!
	 * The app's content occupancy MASK (v3), resampled onto the preview grid,
	 * clamped to the frame's 3D zones and dilated. `roi` is then only the
	 * bounding rect of that mask — what was measured is the mask itself.
	 */
	COMP_REAR_BUDGET_ROI_SRC_MASK,
	//! The app's content bounds, unclamped (full-window app: no 3D zones).
	COMP_REAR_BUDGET_ROI_SRC_BOUNDS,
	//! The app's content bounds intersected with the frame's 3D zones.
	COMP_REAR_BUDGET_ROI_SRC_BOUNDS_IN_ZONES,
	//! The 3D zone union: no usable bounds, or a clamped region that collapsed.
	COMP_REAR_BUDGET_ROI_SRC_ZONES,
	//! Bounds that fell entirely outside every 3D zone — the zone union.
	COMP_REAR_BUDGET_ROI_SRC_ZONES_BOUNDS_OUTSIDE,
};

/*!
 * Human-readable @ref comp_rear_budget_roi_src, for the transition log.
 *
 * @ingroup comp_util
 */
const char *
comp_rear_budget_roi_src_str(enum comp_rear_budget_roi_src src);

/*!
 * One session's rear-depth-budget runner. Zero-init is NOT valid — call
 * @ref comp_rear_budget_init (it owns a mutex).
 *
 * @ingroup comp_util
 */
struct comp_rear_budget
{
	//! App enabled XR_DXR_depth_budget on the instance.
	bool requested;
	//! Session's latched transparent-background flag.
	bool transparent;
	//! requested && transparent — the policy only runs for both.
	bool running;
	//! DXR_REAR_BUDGET_DUMP. -1 = unprobed.
	int dump;

	//! Next frame the DP may be polled on.
	uint64_t next_poll_ns;
	//! Outcome of the LAST DP poll, reused between polls.
	bool source_available;
	//! Preview generation last ANALYSED.
	uint32_t last_generation;
	bool have_generation;
	bool have_result;
	struct u_bg_neutrality_result result;

	struct u_rear_budget policy;

	/*!
	 * @name DXR_REAR_BUDGET_DUMP - the retained preview
	 *
	 * The dump has to show the preview the analysis SAW, and it fires on a
	 * state CHANGE. Those two are almost never the same frame: the DP is
	 * polled every 66 ms while a transition lands whenever the dwell (400 ms)
	 * or the close grace (100 ms) elapses, on a ~8 ms frame boundary. Dumping
	 * the live `pv` therefore required a coincidence that essentially never
	 * happens, and the feature was silently dead — armed runs produced no PNG
	 * and not even a "FAILED" line.
	 *
	 * So while the dump is armed the runner keeps its OWN tightly-packed copy
	 * of the last analysed preview and writes THAT on a transition, whether or
	 * not this frame polled. Bounded and opt-in: previews are <= 512 px on the
	 * long side (<= 1 MB), and nothing is allocated unless the dump is armed.
	 * @{
	 */
	uint8_t *dump_bgra;       //!< Retained copy, tight stride (w * 4). NULL = none.
	size_t dump_cap;          //!< Bytes allocated at @ref dump_bgra.
	uint32_t dump_w, dump_h;
	uint32_t dump_gen;        //!< Generation the retained copy came from.
	bool dump_have;           //!< A copy is present and describes @ref dump_gen.
	bool dump_missing_logged; //!< One-shot: armed, transitioned, nothing retained.
	comp_rear_budget_dump_fn dump_sink; //!< NULL = the built-in PNG writer.
	void *dump_sink_ctx;
	/*! @} */

	/*!
	 * @name XR_DXR_depth_budget v2 - the content-bounds ROI
	 *
	 * The app reports where its content projects (XrContentBoundsDXR on
	 * xrEndFrame) and the analysis measures only there. v1 judged the whole
	 * canvas, which read a busy verdict off an empty Notepad's own menu bar
	 * while the model sat in another corner entirely.
	 *
	 * Written from the APP thread (xrEndFrame) and read from the RENDER
	 * thread (the tick), so the four floats plus their timestamp move under
	 * @ref publish_mutex as one unit - a torn rect would aim the ROI at a
	 * region neither frame asked for.
	 * @{
	 */
	//! Window-normalised, origin top-left. Meaningful only when @ref bounds_valid.
	float bounds_u0, bounds_v0, bounds_u1, bounds_v1;
	//! When the app last chained bounds; older than a second is "stopped chaining".
	uint64_t bounds_ns;
	//! The last chained bounds were usable (finite, positive extent).
	bool bounds_valid;
	//! DXR_REAR_BUDGET_ROI. -1 = unprobed, 0 = kill switch armed.
	int roi_enabled;
	//! ROI the last analysis actually used, in preview pixels.
	struct u_bg_roi last_roi;
	bool have_roi;
	//! The last ROI was narrower than the preview (i.e. the bounds were used).
	bool last_roi_narrowed;
	//! Which rule produced @ref last_roi.
	enum comp_rear_budget_roi_src last_roi_src;
	/*! @} */

	/*!
	 * @name XR_DXR_depth_budget v3 - the content occupancy MASK
	 *
	 * A rect around a character is roughly two thirds background the model
	 * never covers, and any horizontal structure in that surplus closes the
	 * budget. The app already computes its silhouette every frame (it is the
	 * click-through window region), so v3 measures under THAT.
	 *
	 * Two buffers, and they are deliberately different things:
	 *
	 * - @ref mask_cells is the app's grid as it arrived, copied under
	 *   @ref publish_mutex because it crosses from the app thread to the
	 *   render thread. The render thread must never read app memory.
	 * - @ref roi_mask is the runner's own, in PREVIEW pixels: the app grid
	 *   resampled, zone-clamped, dilated. It is what the analysis reads, and
	 *   it is rebuilt only when one of its inputs changed
	 *   (@ref roi_mask_key), because on a quiet desktop nothing else does.
	 * @{
	 */
	uint8_t *mask_cells;  //!< App grid, tightly packed (stride = @ref mask_w). NULL = none.
	size_t mask_cap;      //!< Bytes allocated at @ref mask_cells.
	uint32_t mask_w, mask_h;
	float mask_margin;    //!< marginNormalized, window-normalised units.
	uint64_t mask_ns;     //!< When the app last chained a mask.
	bool mask_valid;      //!< The last chained mask was usable and not all-zero.
	uint32_t mask_gen;    //!< Bumped on every accepted mask; the cache key's first term.

	/*!
	 * The app grid copied OUT from under the lock before it is resampled.
	 * The copy is one bounded memcpy (<= 256 KB, at the poll rate); resampling
	 * under the lock instead would hold it for the length of a 512x512 scan
	 * while the app's locate thread waits to read a float.
	 */
	uint8_t *mask_work;
	size_t mask_work_cap;
	uint32_t mask_work_w, mask_work_h;
	float mask_work_margin;

	uint8_t *roi_mask;    //!< Preview-res, 1 byte/px, tight stride (@ref roi_mask_w).
	size_t roi_mask_cap;
	uint32_t roi_mask_w, roi_mask_h;
	uint32_t roi_mask_px; //!< Nonzero pixels in @ref roi_mask; 0 = not usable.
	//! Bounding rect of the dilated mask, in preview pixels — what `roi=` logs.
	struct u_bg_roi roi_mask_rect;
	//! The mask was the region the LAST analysis measured through.
	bool roi_mask_in_use;
	//! Inputs the current @ref roi_mask was built from; a change rebuilds it.
	struct
	{
		uint32_t mask_gen;
		uint32_t zone_gen;
		uint32_t zone_count;
		uint32_t pw, ph;
		float cu0, cv0, cu1, cv1;
		bool valid;
	} roi_mask_key;
	//! Bumped on every rebuild, so the tick can re-measure an unchanged capture.
	uint32_t roi_mask_build_id;
	uint32_t last_analysed_mask_build_id;
	//! DXR_REAR_BUDGET_MASK. -1 = unprobed, 0 = mask off, bounds still on.
	int mask_enabled;
	//! One-shot: the mask fell entirely outside every 3D zone.
	bool mask_outside_logged;
	//! Scratch for the separable dilation; grown with the preview.
	uint32_t *dilate_scratch;
	size_t dilate_scratch_cap;
	//! Tinted copy handed to the dump sink; only allocated while dumping.
	uint8_t *dump_tint;
	size_t dump_tint_cap;
	/*! @} */

	/*!
	 * @name XR_DXR_depth_budget v2 - the 3D display zones the ROI is clamped to
	 *
	 * The bounds contract is WINDOW-normalised, and an app can get that wrong in
	 * a way no validation catches: a zoned app that projects in the ZONE view
	 * and forgets to rebase through the zone rect reports a rect that reaches
	 * outside its 3D zone and into a Local2D 2D band (#1365, on a zoned Unity
	 * app). The background preview covers the whole window, so the analysis then
	 * measures desktop pixels behind the 2D band — pixels the 3D content can
	 * never occlude — and the verdict still reads authoritative.
	 *
	 * So the runtime defends: the derived region is intersected with the union
	 * of THIS frame's 3D zones. Only 3D zones count; a Local2D zone is a 2D band
	 * and is deliberately not in this list. NO zones at all means the whole
	 * canvas IS the 3D zone (every full-window app), and nothing is clamped.
	 *
	 * Written from the compositor's per-frame layer scan and read by the
	 * analysis, so the rects and their timestamp move under
	 * @ref publish_mutex as one unit.
	 * @{
	 */
	struct u_bg_rect_norm zones[COMP_REAR_BUDGET_MAX_ZONES];
	uint32_t zone_count;
	//! When the zones were last published; older than a second is "stopped".
	uint64_t zones_ns;
	/*!
	 * Bumped only when the RECTS change, never merely because a frame
	 * republished them. Zones are published every frame, so a generation that
	 * counted publishes would rebuild the v3 mask every frame and make the
	 * cache a memcpy with extra steps.
	 */
	uint32_t zone_gen;
	//! One-shot: the bounds fell entirely outside every 3D zone.
	bool zones_outside_logged;
	/*! @} */

	//! Guards @ref published / @ref published_valid and the content bounds.
	struct os_mutex publish_mutex;
	struct u_rear_budget_out published;
	bool published_valid;

	//! False until @ref comp_rear_budget_init succeeded; every entry point no-ops.
	bool initialised;
};

/*!
 * Initialise @p b and its policy. @p label tags the one-line-per-transition
 * log ("d3d11", "vk", …); may be NULL.
 *
 * Reads the `DXR_REAR_BUDGET*` environment overrides once, here — the policy
 * exists from the first frame so that a locate arriving before any preview
 * reads a real CLIPPED_NO_SOURCE rather than an uninitialised struct.
 *
 * @ingroup comp_util
 */
void
comp_rear_budget_init(struct comp_rear_budget *b, const char *label);

/*!
 * Release @p b's mutex. Safe on an uninitialised or already-finalised instance.
 *
 * @ingroup comp_util
 */
void
comp_rear_budget_fini(struct comp_rear_budget *b);

/*!
 * Latch the app's extension opt-in. Does not by itself arm the policy — call
 * @ref comp_rear_budget_arm with the session's transparency.
 *
 * @ingroup comp_util
 */
void
comp_rear_budget_set_requested(struct comp_rear_budget *b, bool requested);

/*!
 * Latch the session's transparency and (re)compute whether the policy runs.
 *
 * Both halves are required: an opaque session has no conflict to police, and an
 * app that never enabled the extension must not pay for the poll. Logs once per
 * change of the armed state — an experiment that is silent when armed cannot be
 * told from one that never ran.
 *
 * @ingroup comp_util
 */
void
comp_rear_budget_arm(struct comp_rear_budget *b, bool transparent);

/*!
 * XR_DXR_depth_budget v2: where this frame's content projects, normalised to
 * the app WINDOW'S CLIENT RECT with the origin top-left, as the app reported it
 * on xrEndFrame.
 *
 * Window-normalised, never zone-normalised: a zoned app projects in the zone
 * view, clamps to [0,1], and then rebases through its 3D zone rect
 * (`dxr::RebaseZoneBoundsToWindow`). An app that skips the rebase reports a
 * rect that can reach outside its zone, which is why
 * @ref comp_rear_budget_set_zone_rects exists — the runtime clamps rather than
 * trusting.
 *
 * Advisory and lossy on purpose. The runtime dilates the region before
 * measuring (the disparity conflict lives in the band around the silhouette,
 * not strictly under it), falls back to the whole preview when the app stops
 * chaining for more than a second, and never treats a region it cannot use as
 * neutral - "I could not measure" and "I measured nothing" must not collapse
 * into the same answer.
 *
 * Called from the APP thread while @ref comp_rear_budget_tick runs on the
 * render thread; the rect moves under the runner's own mutex.
 *
 * @param u0,v0,u1,v1 Window-normalised bounds. A non-positive extent (u1 <= u0
 *                    or v1 <= v0) means "unknown" and selects the whole
 *                    preview - which is exactly v1's behaviour.
 * @param now_ns      Monotonic now, for the staleness rule.
 *
 * @ingroup comp_util
 */
void
comp_rear_budget_set_content_bounds(struct comp_rear_budget *b,
                                    float u0,
                                    float v0,
                                    float u1,
                                    float v1,
                                    uint64_t now_ns);

/*!
 * XR_DXR_depth_budget v3: this frame's content OCCUPANCY MASK — the union over
 * all views of the app's rendered silhouette, as it chained it on xrEndFrame
 * (`XrContentMaskDXR`).
 *
 * Takes precedence over @ref comp_rear_budget_set_content_bounds: a rect around
 * a character is mostly background the character never covers, and horizontal
 * structure in that surplus closes a budget the silhouette itself would have
 * left open. Bounds stay valid and are the fallback — the precedence is
 * mask → bounds → 3D zones → whole preview, each step falling through when the
 * one above is absent, all-zero or older than a second.
 *
 * The cells are copied HERE, under the runner's own mutex, even though the
 * caller has usually copied them already: the render thread must never read a
 * pointer the app owns, and "usually" is not a threading argument.
 *
 * Called from the APP thread while @ref comp_rear_budget_tick runs on the
 * render thread.
 *
 * @param cells   Row-major, top-left origin, one byte per cell, nonzero =
 *                content occupies the cell; normalised to the app WINDOW'S
 *                client rect exactly as the bounds are. NULL (or a mask with no
 *                nonzero cell) means "absent" and drops back to the bounds.
 * @param w,h     Grid dims, 1..@ref COMP_REAR_BUDGET_MASK_MAX_DIM.
 * @param stride  Row pitch of @p cells in bytes; >= @p w.
 * @param margin_normalized Extra dilation the app wants, window-normalised, ON
 *                TOP of the runtime's own default.
 * @param now_ns  Monotonic now, for the staleness rule.
 *
 * @ingroup comp_util
 */
void
comp_rear_budget_set_content_mask(struct comp_rear_budget *b,
                                  const uint8_t *cells,
                                  uint32_t w,
                                  uint32_t h,
                                  uint32_t stride,
                                  float margin_normalized,
                                  uint64_t now_ns);

/*!
 * XR_DXR_depth_budget v2: THIS frame's 3D display zones (XR_DXR_display_zones),
 * normalised to the same app-window client rect the content bounds are in.
 *
 * Called once per APP frame from the compositor's per-frame layer scan — the
 * same scan that resolves `zones_frame` — so the ROI logic stays inside the
 * runner and no backend grows its own copy of the clamp. Pass @p count 0 on a
 * frame with no zones: an empty list means "the whole canvas is the 3D zone",
 * which is the full-window app and is left exactly as v2 had it. Local2D zones
 * are 2D bands and must NOT be in @p rects.
 *
 * Rects are validated and clamped to [0,1]; degenerate ones are dropped. More
 * than @ref COMP_REAR_BUDGET_MAX_ZONES cannot arrive — the compositors' own
 * gather loops stop at @ref XRT_MAX_LAYERS — but the excess is dropped WITH the
 * whole clamp rather than clamping to a subset of the real zone union.
 *
 * @param rects  May be NULL when @p count is 0.
 * @param now_ns Monotonic now; zones older than a second are treated as absent,
 *               because an app that stopped chaining zones has a layout that
 *               has since moved.
 *
 * @ingroup comp_util
 */
void
comp_rear_budget_set_zone_rects(struct comp_rear_budget *b,
                                const struct u_bg_rect_norm *rects,
                                uint32_t count,
                                uint64_t now_ns);

/*!
 * True when the policy is armed for this session. A cheap gate the render
 * thread can take before touching anything else.
 *
 * @ingroup comp_util
 */
bool
comp_rear_budget_is_running(const struct comp_rear_budget *b);

/*!
 * Is this frame due to poll the DP? Stamps the next poll deadline when it
 * returns true, so it must be called at most once per tick.
 *
 * @ingroup comp_util
 */
bool
comp_rear_budget_should_poll(struct comp_rear_budget *b, uint64_t now_ns);

/*!
 * One evaluation. Call once per APP frame from the render thread, after the DP
 * has been handed the atlas (that is the point at which the DP's capture for
 * this frame is settled) — never from a repaint, which replays rendering only
 * and must not advance a per-frame state machine.
 *
 * @param b            Instance.
 * @param pv           The preview the DP handed back, or NULL when this frame
 *                     did not poll or the slot declined. Validated here.
 * @param polled       Whether this frame actually asked the DP
 *                     (@ref comp_rear_budget_should_poll's answer).
 * @param transparent  The session's current transparency.
 * @param now_ns       Monotonic now; must be non-decreasing across calls.
 *
 * @ingroup comp_util
 */
void
comp_rear_budget_tick(struct comp_rear_budget *b,
                      const struct xrt_dp_background_preview *pv,
                      bool polled,
                      bool transparent,
                      uint64_t now_ns);

/*!
 * Read the published budget. Called from the app's locate thread.
 *
 * @return false when this session never enabled the extension — the caller then
 *         applies the extension's zero-default rule. True with a filled @p out
 *         otherwise, including before the first tick has landed (which reports
 *         CLIPPED_NO_SOURCE, i.e. exactly today's behaviour).
 *
 * @ingroup comp_util
 */
bool
comp_rear_budget_get(struct comp_rear_budget *b, struct u_rear_budget_out *out);

/*!
 * TEST ONLY — redirect the dump and arm retention without reading the
 * environment.
 *
 * Arming through @p fn rather than through `DXR_REAR_BUDGET_DUMP` is what lets
 * a unit test assert the dump actually FIRES — the bug this exists to pin was
 * that it never did — without writing a PNG into the developer's
 * `%LOCALAPPDATA%`. Passing NULL restores the built-in PNG writer and leaves
 * the armed state alone.
 *
 * @ingroup comp_util
 */
void
comp_rear_budget_debug_set_dump_sink(struct comp_rear_budget *b, comp_rear_budget_dump_fn fn, void *ctx);

/*!
 * TEST ONLY — the ROI the last analysis used, in preview pixels.
 *
 * @param out_narrowed Set when the ROI came from the app's content bounds
 *                     rather than being the whole preview; may be NULL.
 *
 * @return false before any analysis has run.
 *
 * @ingroup comp_util
 */
bool
comp_rear_budget_debug_last_roi(const struct comp_rear_budget *b, struct u_bg_roi *out_roi, bool *out_narrowed);

/*!
 * TEST ONLY — which rule produced the ROI the last analysis used.
 *
 * The clamp's failure paths are the point of it: "bounds fell entirely outside
 * every 3D zone" and "bounds clamped INTO the zones" produce different regions
 * for the same input and must be distinguishable from outside.
 *
 * @ingroup comp_util
 */
enum comp_rear_budget_roi_src
comp_rear_budget_debug_last_roi_src(const struct comp_rear_budget *b);

/*!
 * TEST ONLY — the dilated, zone-clamped mask the last analysis measured
 * through, in preview pixels.
 *
 * The ROI rect alone cannot describe a mask: a bar and the box around it have
 * the same bounding rect and measure completely different pixels, which is the
 * whole reason v3 exists.
 *
 * @param out_mask  Receives a pointer to the runner's own buffer, valid until
 *                  the next tick; may be NULL.
 * @param out_w,out_h  Preview dims the mask is in; may be NULL.
 * @param out_px    Nonzero pixel count; may be NULL.
 *
 * @return false when the last analysis used a rect rather than a mask.
 *
 * @ingroup comp_util
 */
bool
comp_rear_budget_debug_last_mask(const struct comp_rear_budget *b,
                                 const uint8_t **out_mask,
                                 uint32_t *out_w,
                                 uint32_t *out_h,
                                 uint32_t *out_px);

/*!
 * TEST ONLY — dimensions of the preview currently retained for the dump.
 *
 * @return false when nothing is retained (the dump is not armed, or no valid
 *         preview has been analysed yet).
 *
 * @ingroup comp_util
 */
bool
comp_rear_budget_debug_last_preview(const struct comp_rear_budget *b, uint32_t *out_w, uint32_t *out_h);

#ifdef __cplusplus
}
#endif
