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
 */
static void
comp_rear_budget_dump_preview(const struct xrt_dp_background_preview *pv)
{
	if (pv == NULL || pv->bgra == NULL || pv->width == 0 || pv->height == 0) {
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
	const size_t pitch = (size_t)pv->width * 4u;
	uint8_t *rgba = (uint8_t *)malloc(pitch * pv->height);
	if (rgba == NULL) {
		return;
	}
	for (uint32_t y = 0; y < pv->height; y++) {
		const uint8_t *src = pv->bgra + (size_t)y * pv->stride_bytes;
		uint8_t *dst = rgba + (size_t)y * pitch;
		for (uint32_t x = 0; x < pv->width; x++) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = src[x * 4 + 3];
		}
	}
	const int ok = stbi_write_png(path, (int)pv->width, (int)pv->height, 4, rgba, (int)pitch);
	free(rgba);
	U_LOG_W("REAR_BUDGET: preview dump %s -> %s", ok ? "wrote" : "FAILED", path);
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

	if (b->dump < 0) {
		const char *e = getenv("DXR_REAR_BUDGET_DUMP");
		b->dump = (e != NULL && e[0] == '1') ? 1 : 0;
		if (b->dump == 1) {
			U_LOG_W(
			    "REAR_BUDGET: DXR_REAR_BUDGET_DUMP armed = 1 (preview PNG on each "
			    "state change)");
		}
	}
	// The preview bytes are only valid on the frame that polled them.
	if (b->dump == 1 && out.state != before && polled && have_preview) {
		comp_rear_budget_dump_preview(pv);
	}
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
