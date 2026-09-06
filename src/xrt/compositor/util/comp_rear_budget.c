// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-session rear-depth-budget runner shared by the native compositors.
 * @author David Fattal
 * @ingroup comp_util
 */

#include "comp_rear_budget.h"

#include "xrt/xrt_config_os.h"

#include "os/os_time.h"

#include "util/u_logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// STB_IMAGE_WRITE_STATIC scopes stbi_write_* to this TU. Every compositor that
// encodes a PNG already carries its own static copy (comp_d3d11_compositor.cpp,
// comp_gl_compositor.cpp, …), so this one cannot clash with them at link time.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


/*
 * ---------------------------------------------------------------------------
 * DXR_REAR_BUDGET_DUMP — the picture behind the verdict
 * ---------------------------------------------------------------------------
 */

//! %LOCALAPPDATA%\\DisplayXR\\rear_budget_preview.png, or empty on failure.
static void
comp_rear_budget_dump_path(char *out, size_t out_len)
{
	out[0] = '\0';
	const char *dir = getenv("LOCALAPPDATA");
	if (dir == NULL || dir[0] == '\0') {
		dir = getenv("TEMP");
	}
#ifndef XRT_OS_WINDOWS
	if (dir == NULL || dir[0] == '\0') {
		dir = getenv("TMPDIR");
	}
	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}
	snprintf(out, out_len, "%s/DisplayXR/rear_budget_preview.png", dir);
#else
	if (dir == NULL || dir[0] == '\0') {
		return;
	}
	snprintf(out, out_len, "%s\\DisplayXR\\rear_budget_preview.png", dir);
#endif
}

/*
 * Write the preview the analysis actually saw. The verdict is a single boolean
 * over a whole desktop; without the picture behind it, a wrong verdict is
 * unfalsifiable.
 *
 * The default sink. It takes raw pixels rather than an
 * @ref xrt_dp_background_preview, because by the time a state change fires the
 * DP-owned buffer is long gone — what gets written is the runner's own
 * retained copy (see @ref comp_rear_budget::dump_bgra).
 */
static void
comp_rear_budget_dump_png(void *ctx, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride)
{
	(void)ctx;
	if (bgra == NULL || w == 0 || h == 0) {
		return;
	}
	char path[512];
	comp_rear_budget_dump_path(path, sizeof(path));
	if (path[0] == '\0') {
		return;
	}

	// BGRA -> RGBA; the preview is opaque by contract, so alpha is passed
	// through rather than forced (a forced 255 would hide a DP that handed
	// over a transparent buffer).
	const size_t pitch = (size_t)w * 4u;
	uint8_t *rgba = (uint8_t *)malloc(pitch * h);
	if (rgba == NULL) {
		return;
	}
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *src = bgra + (size_t)y * stride;
		uint8_t *dst = rgba + (size_t)y * pitch;
		for (uint32_t x = 0; x < w; x++) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = src[x * 4 + 3];
		}
	}
	const int ok = stbi_write_png(path, (int)w, (int)h, 4, rgba, (int)pitch);
	free(rgba);
	U_LOG_W("REAR_BUDGET: preview dump %s -> %s", ok ? "wrote" : "FAILED", path);
}

//! Take the runner's own tightly-packed copy of @p pv. Only while armed.
static void
comp_rear_budget_retain_preview(struct comp_rear_budget *b, const struct xrt_dp_background_preview *pv)
{
	const size_t need = (size_t)pv->width * 4u * pv->height;
	if (need == 0) {
		return;
	}
	if (b->dump_cap < need) {
		uint8_t *grown = (uint8_t *)realloc(b->dump_bgra, need);
		if (grown == NULL) {
			// Keep whatever is already retained: a stale picture of the
			// desktop is worth more than none, and the log line names the
			// state it was written for.
			return;
		}
		b->dump_bgra = grown;
		b->dump_cap = need;
	}
	for (uint32_t y = 0; y < pv->height; y++) {
		memcpy(b->dump_bgra + (size_t)y * pv->width * 4u, pv->bgra + (size_t)y * pv->stride_bytes,
		       (size_t)pv->width * 4u);
	}
	b->dump_w = pv->width;
	b->dump_h = pv->height;
	b->dump_gen = pv->generation;
	b->dump_have = true;
}


/*
 * ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

void
comp_rear_budget_init(struct comp_rear_budget *b, const char *label)
{
	if (b == NULL) {
		return;
	}
	memset(b, 0, sizeof(*b));
	b->dump = -1;

	if (os_mutex_init(&b->publish_mutex) != 0) {
		// Without the lock the publish/read pair is a data race, so the
		// runner stays off rather than running unsynchronised. Every entry
		// point tests `initialised`, and the app sees the extension's
		// zero-default — i.e. today's behaviour.
		U_LOG_W("REAR_BUDGET: mutex init failed — the policy stays off for this session");
		return;
	}

	struct u_rear_budget_tuning tuning;
	u_rear_budget_tuning_defaults(&tuning);
	u_rear_budget_tuning_from_env(&tuning);
	u_rear_budget_init(&b->policy, &tuning, label, os_monotonic_get_ns());

	b->initialised = true;
}

void
comp_rear_budget_fini(struct comp_rear_budget *b)
{
	if (b == NULL || !b->initialised) {
		return;
	}
	b->initialised = false;
	b->running = false;
	free(b->dump_bgra);
	b->dump_bgra = NULL;
	b->dump_cap = 0;
	b->dump_have = false;
	os_mutex_destroy(&b->publish_mutex);
}

void
comp_rear_budget_set_requested(struct comp_rear_budget *b, bool requested)
{
	if (b == NULL) {
		return;
	}
	b->requested = requested;
}

void
comp_rear_budget_arm(struct comp_rear_budget *b, bool transparent)
{
	if (b == NULL) {
		return;
	}
	b->transparent = transparent;

	// Both halves are required: an opaque session has no conflict to police,
	// and an app that never enabled the extension must not pay for the poll.
	const bool running = b->initialised && b->requested && transparent;
	if (running != b->running) {
		b->running = running;
		U_LOG_W("REAR_BUDGET: policy %s (requested=%d transparent=%d)", running ? "ARMED" : "off",
		        b->requested ? 1 : 0, transparent ? 1 : 0);
	}
}

bool
comp_rear_budget_is_running(const struct comp_rear_budget *b)
{
	return b != NULL && b->running;
}


/*
 * ---------------------------------------------------------------------------
 * The per-frame runner
 * ---------------------------------------------------------------------------
 */

bool
comp_rear_budget_should_poll(struct comp_rear_budget *b, uint64_t now_ns)
{
	if (b == NULL || !b->running) {
		return false;
	}
	if (now_ns < b->next_poll_ns) {
		return false;
	}
	b->next_poll_ns = now_ns + COMP_REAR_BUDGET_POLL_INTERVAL_NS;
	return true;
}

void
comp_rear_budget_tick(struct comp_rear_budget *b,
                      const struct xrt_dp_background_preview *pv,
                      bool polled,
                      bool transparent,
                      uint64_t now_ns)
{
	if (b == NULL || !b->running) {
		return;
	}
	b->transparent = transparent;

	// Resolved BEFORE the analyse block, not after it: retention is what the
	// dump writes, so probing afterwards would silently skip the first
	// generation — which on a quiet desktop can be the only one there is.
	if (b->dump < 0) {
		const char *e = getenv("DXR_REAR_BUDGET_DUMP");
		b->dump = (e != NULL && e[0] == '1') ? 1 : 0;
		if (b->dump == 1) {
			U_LOG_W(
			    "REAR_BUDGET: DXR_REAR_BUDGET_DUMP armed = 1 (preview PNG on each "
			    "state change)");
		}
	}

	/*
	 * The source verdict is recomputed only on a polling frame and then
	 * REUSED: between polls the last answer still describes the source, and
	 * re-deriving it from a stale `pv` would flap the state machine at the
	 * frame rate instead of at the capture rate.
	 */
	if (polled) {
		b->source_available = pv != NULL && pv->bgra != NULL && pv->width >= 2 && pv->height >= 1 &&
		                      pv->stride_bytes >= pv->width * 4u &&
		                      (pv->flags & XRT_DP_BG_PREVIEW_STALE) == 0;
	}
	const bool have_preview = b->source_available;

	if (polled && have_preview) {
		// Re-analysing an unchanged capture would burn the CPU to reach the
		// same answer; the policy's own staleness rule covers a generation
		// that stops advancing entirely.
		if (!b->have_generation || pv->generation != b->last_generation) {
			struct u_bg_roi roi = {0, 0, pv->width, pv->height}; // v1 ROI = the canvas
			struct u_bg_neutrality_result res = {0};
			if (u_bg_neutrality_analyse(pv->bgra, pv->width, pv->height, pv->stride_bytes, &roi, NULL,
			                            &res)) {
				b->result = res;
				b->have_result = true;
			}
			b->have_generation = true;
			b->last_generation = pv->generation;
			if (b->dump == 1) {
				comp_rear_budget_retain_preview(b, pv);
			}
		}
	}

	struct u_rear_budget_in in = {0};
	in.transparent = transparent;
	// In-process native sessions are standalone by construction: a session
	// under a workspace controller is an IPC client of the service, and no
	// native compositor is on that path at all.
	in.under_workspace = false;
	in.source_available = have_preview;
	in.have_result = have_preview && b->have_result;
	in.generation = b->last_generation;
	in.result = b->result;

	const enum u_rear_budget_state before = b->policy.state;
	struct u_rear_budget_out out = {0};
	u_rear_budget_update(&b->policy, &in, now_ns, &out);

	os_mutex_lock(&b->publish_mutex);
	b->published = out;
	b->published_valid = true;
	os_mutex_unlock(&b->publish_mutex);

	/*
	 * A transition fires when a dwell or a close grace ELAPSES, which is a
	 * different frame from the one that polled: 66 ms between polls, ~8 ms
	 * between frames, 100/400 ms of hysteresis. So what is written here is the
	 * RETAINED copy, never the live `pv` — gating this on `polled` made the
	 * dump unreachable in practice, and an armed run produced no PNG and not
	 * even a FAILED line.
	 */
	if (b->dump == 1 && out.state != before) {
		if (b->dump_have) {
			const comp_rear_budget_dump_fn sink =
			    (b->dump_sink != NULL) ? b->dump_sink : comp_rear_budget_dump_png;
			sink(b->dump_sink_ctx, b->dump_bgra, b->dump_w, b->dump_h, b->dump_w * 4u);
		} else if (!b->dump_missing_logged) {
			// Name the negative path once. "Armed and silent" was exactly the
			// symptom of the bug above, so it must never read that way again.
			b->dump_missing_logged = true;
			U_LOG_W(
			    "REAR_BUDGET: dump armed but no preview has been analysed yet — "
			    "state %s with no source to picture",
			    u_rear_budget_state_str(out.state));
		}
	}
}

void
comp_rear_budget_debug_set_dump_sink(struct comp_rear_budget *b, comp_rear_budget_dump_fn fn, void *ctx)
{
	if (b == NULL) {
		return;
	}
	b->dump_sink = fn;
	b->dump_sink_ctx = ctx;
	if (fn != NULL) {
		// Arm without consulting the environment, so a test never depends on
		// the developer's shell and never writes a PNG.
		b->dump = 1;
	}
}

bool
comp_rear_budget_debug_last_preview(const struct comp_rear_budget *b, uint32_t *out_w, uint32_t *out_h)
{
	if (b == NULL || !b->dump_have) {
		return false;
	}
	if (out_w != NULL) {
		*out_w = b->dump_w;
	}
	if (out_h != NULL) {
		*out_h = b->dump_h;
	}
	return true;
}

bool
comp_rear_budget_get(struct comp_rear_budget *b, struct u_rear_budget_out *out)
{
	if (b == NULL || out == NULL || !b->initialised) {
		return false;
	}

	// An app that never enabled the extension has no policy to read; the
	// caller applies the extension's zero-default rule instead.
	if (!b->requested) {
		return false;
	}

	// An opaque session is unrestricted by definition, and says so without
	// waiting for a render-thread tick that will never run for it.
	if (!b->transparent) {
		out->far_offset_vh = U_REAR_BUDGET_UNRESTRICTED_VH;
		out->state = U_REAR_BUDGET_UNRESTRICTED_OPAQUE;
		out->cue_energy = 0.0f;
		return true;
	}

	os_mutex_lock(&b->publish_mutex);
	const bool valid = b->published_valid;
	if (valid) {
		*out = b->published;
	}
	os_mutex_unlock(&b->publish_mutex);

	if (!valid) {
		// Transparent and armed, but no evaluation has landed yet. Clip —
		// which is exactly what the app does today, so the first frames are
		// unchanged rather than briefly and wrongly open.
		out->far_offset_vh = 0.0f;
		out->state = U_REAR_BUDGET_CLIPPED_NO_SOURCE;
		out->cue_energy = 0.0f;
	}
	return true;
}
