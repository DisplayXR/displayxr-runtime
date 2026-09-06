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

	//! Guards @ref published / @ref published_valid only.
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
