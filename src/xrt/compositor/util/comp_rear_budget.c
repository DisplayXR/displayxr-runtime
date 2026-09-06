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

#include <math.h>
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
 * The content-bounds ROI (XR_DXR_depth_budget v2)
 * ---------------------------------------------------------------------------
 */

/*!
 * Dilation applied to every side of the mapped bounds, as a fraction of the
 * preview width. The disparity conflict is read in the BAND around the
 * silhouette rather than strictly under it, so measuring the exact projected
 * AABB would answer a question nobody asked.
 */
#define COMP_REAR_BUDGET_ROI_DILATE_FRAC 0.04f

//! Never dilate by less than this, so a small preview still gets a real band.
#define COMP_REAR_BUDGET_ROI_DILATE_MIN_PX 8.0f

/*!
 * Bounds older than this are treated as absent: the app stopped chaining, and
 * a rect from a second ago describes geometry that has since moved.
 */
#define COMP_REAR_BUDGET_ROI_MAX_AGE_NS (1000ULL * 1000ULL * 1000ULL)

static float
comp_rear_budget_clampf(float v, float lo, float hi)
{
	return (v < lo) ? lo : ((v > hi) ? hi : v);
}

const char *
comp_rear_budget_roi_src_str(enum comp_rear_budget_roi_src src)
{
	switch (src) {
	case COMP_REAR_BUDGET_ROI_SRC_BOUNDS: return "app content bounds";
	case COMP_REAR_BUDGET_ROI_SRC_BOUNDS_IN_ZONES: return "app content bounds clamped to the 3D zones";
	case COMP_REAR_BUDGET_ROI_SRC_ZONES: return "3D zone union";
	case COMP_REAR_BUDGET_ROI_SRC_ZONES_BOUNDS_OUTSIDE: return "3D zone union (bounds fell outside)";
	case COMP_REAR_BUDGET_ROI_SRC_WHOLE_PREVIEW:
	default: return "whole preview";
	}
}

/*!
 * The ROI this frame's analysis should use, in preview pixels.
 *
 * Two authorities, in this order:
 *
 * 1. the app's content bounds, window-normalised (what it says it drew), and
 * 2. this frame's 3D display zones (what the runtime WEAVES).
 *
 * The second is not advice, it is a fact of the frame — outside a 3D zone the
 * app's content cannot occlude anything, so measuring the desktop there answers
 * a question about pixels nobody will ever see through the lens. #1365: a zoned
 * app whose bounds were still zone-normalised reported a rect reaching into a
 * Local2D band, and the verdict was read off desktop behind that band.
 *
 * Every failure path lands on the WHOLE preview — the v1 answer — EXCEPT where
 * the frame has 3D zones, where it lands on the zone union instead. Never on
 * "neutral": a region the runner could not derive is a question it did not ask,
 * not a background it measured and found quiet.
 */
static void
comp_rear_budget_derive_roi(struct comp_rear_budget *b,
                            const struct xrt_dp_background_preview *pv,
                            uint64_t now_ns,
                            struct u_bg_roi *out_roi,
                            bool *out_narrowed,
                            enum comp_rear_budget_roi_src *out_src)
{
	const struct u_bg_roi whole = {0, 0, pv->width, pv->height};
	*out_roi = whole;
	*out_narrowed = false;
	*out_src = COMP_REAR_BUDGET_ROI_SRC_WHOLE_PREVIEW;

	// Probed once. An armed kill switch says so: an A/B whose two arms are
	// indistinguishable in the log is not an A/B.
	if (b->roi_enabled < 0) {
		const char *e = getenv("DXR_REAR_BUDGET_ROI");
		b->roi_enabled = (e != NULL && e[0] == '0') ? 0 : 1;
		if (b->roi_enabled == 0) {
			U_LOG_W(
			    "REAR_BUDGET: DXR_REAR_BUDGET_ROI armed = 0 (content-bounds ROI off, "
			    "analysing the whole preview)");
		}
	}
	if (b->roi_enabled == 0) {
		return;
	}

	struct u_bg_rect_norm zones[COMP_REAR_BUDGET_MAX_ZONES];
	uint32_t zone_count = 0;

	os_mutex_lock(&b->publish_mutex);
	const bool valid = b->bounds_valid;
	const uint64_t age_ns = (now_ns > b->bounds_ns) ? (now_ns - b->bounds_ns) : 0;
	const float u0 = b->bounds_u0, v0 = b->bounds_v0, u1 = b->bounds_u1, v1 = b->bounds_v1;
	const uint64_t zones_age_ns = (now_ns > b->zones_ns) ? (now_ns - b->zones_ns) : 0;
	if (b->zone_count > 0 && zones_age_ns <= COMP_REAR_BUDGET_ROI_MAX_AGE_NS) {
		zone_count = b->zone_count;
		memcpy(zones, b->zones, sizeof(zones[0]) * (size_t)zone_count);
	}
	os_mutex_unlock(&b->publish_mutex);

	const bool have_bounds = valid && age_ns <= COMP_REAR_BUDGET_ROI_MAX_AGE_NS;
	if (!have_bounds && zone_count == 0) {
		return;
	}

	/*
	 * Resolve the region in WINDOW-NORMALISED space, before anything meets the
	 * preview's pixel grid — the bounds and the zone rects are both in that
	 * space, and intersecting them after the mapping would make the answer
	 * depend on the preview's resolution.
	 */
	float ru0 = u0, rv0 = v0, ru1 = u1, rv1 = v1;

	// The 3D zone union's bounding box. A union of rects is not a rect and the
	// ROI is one, so what is carried forward is the box that contains them —
	// the tightest rect that cannot exclude woven content.
	float zu0 = 0.0f, zv0 = 0.0f, zu1 = 1.0f, zv1 = 1.0f;
	if (zone_count > 0) {
		zu0 = zones[0].u0;
		zv0 = zones[0].v0;
		zu1 = zones[0].u1;
		zv1 = zones[0].v1;
		for (uint32_t i = 1; i < zone_count; i++) {
			if (zones[i].u0 < zu0) {
				zu0 = zones[i].u0;
			}
			if (zones[i].v0 < zv0) {
				zv0 = zones[i].v0;
			}
			if (zones[i].u1 > zu1) {
				zu1 = zones[i].u1;
			}
			if (zones[i].v1 > zv1) {
				zv1 = zones[i].v1;
			}
		}

		if (!have_bounds) {
			// Zones but no usable bounds. NOT the whole window: outside a 3D
			// zone there is nothing to occlude, so the honest default region
			// is what the frame actually weaves.
			ru0 = zu0;
			rv0 = zv0;
			ru1 = zu1;
			rv1 = zv1;
			*out_src = COMP_REAR_BUDGET_ROI_SRC_ZONES;
		} else {
			// Intersect the bounds with EACH zone and take the box around what
			// survives — per-zone, not against the union box, so a rect that
			// only touches the gap between two zones is correctly empty.
			bool any = false;
			float iu0 = 0.0f, iv0 = 0.0f, iu1 = 0.0f, iv1 = 0.0f;
			for (uint32_t i = 0; i < zone_count; i++) {
				const float a0 = (ru0 > zones[i].u0) ? ru0 : zones[i].u0;
				const float a1 = (ru1 < zones[i].u1) ? ru1 : zones[i].u1;
				const float c0 = (rv0 > zones[i].v0) ? rv0 : zones[i].v0;
				const float c1 = (rv1 < zones[i].v1) ? rv1 : zones[i].v1;
				if (!(a1 > a0) || !(c1 > c0)) {
					continue;
				}
				if (!any) {
					iu0 = a0;
					iv0 = c0;
					iu1 = a1;
					iv1 = c1;
					any = true;
					continue;
				}
				iu0 = (a0 < iu0) ? a0 : iu0;
				iv0 = (c0 < iv0) ? c0 : iv0;
				iu1 = (a1 > iu1) ? a1 : iu1;
				iv1 = (c1 > iv1) ? c1 : iv1;
			}

			if (any) {
				ru0 = iu0;
				rv0 = iv0;
				ru1 = iu1;
				rv1 = iv1;
				*out_src = COMP_REAR_BUDGET_ROI_SRC_BOUNDS_IN_ZONES;
			} else {
				/*
				 * The bounds live entirely outside every 3D zone. That is an
				 * APP bug — almost always a zone-normalised projection chained
				 * as window-normalised — and the region it names is unusable.
				 * The zone union, never the whole window and never "neutral":
				 * falling back to the window is exactly the failure this clamp
				 * exists to stop, and neutral would open the budget over a
				 * desktop nobody measured.
				 */
				ru0 = zu0;
				rv0 = zv0;
				ru1 = zu1;
				rv1 = zv1;
				*out_src = COMP_REAR_BUDGET_ROI_SRC_ZONES_BOUNDS_OUTSIDE;
				if (!b->zones_outside_logged) {
					b->zones_outside_logged = true;
					U_LOG_W(
					    "REAR_BUDGET: content bounds fall outside the 3D zones — check "
					    "the app's zone→window rebase (bounds %.3f,%.3f..%.3f,%.3f vs "
					    "zone union %.3f,%.3f..%.3f,%.3f); measuring the zones instead",
					    (double)u0, (double)v0, (double)u1, (double)v1, (double)zu0,
					    (double)zv0, (double)zu1, (double)zv1);
				}
			}
		}
	} else {
		*out_src = COMP_REAR_BUDGET_ROI_SRC_BOUNDS;
	}

	/*
	 * Map the resolved region into preview pixels. The preview covers
	 * `canvas_u0..v1` OF THE CANVAS — normally 0,0,1,1, but a display
	 * processor may include a margin. One that predates the field leaves it
	 * zeroed, and that is the documented normal case rather than a degenerate
	 * one, so it reads as the identity instead of disabling the ROI.
	 */
	float cu0 = pv->canvas_u0, cv0 = pv->canvas_v0, cu1 = pv->canvas_u1, cv1 = pv->canvas_v1;
	if (!(cu1 > cu0) || !(cv1 > cv0)) {
		cu0 = 0.0f;
		cv0 = 0.0f;
		cu1 = 1.0f;
		cv1 = 1.0f;
	}

	const float w_px = (float)pv->width;
	const float h_px = (float)pv->height;
	float x0 = (ru0 - cu0) / (cu1 - cu0) * w_px;
	float x1 = (ru1 - cu0) / (cu1 - cu0) * w_px;
	float y0 = (rv0 - cv0) / (cv1 - cv0) * h_px;
	float y1 = (rv1 - cv0) / (cv1 - cv0) * h_px;

	// Dilate BEFORE clamping, so a band that runs off the preview edge is
	// clipped by the preview rather than by the arithmetic.
	float dilate = COMP_REAR_BUDGET_ROI_DILATE_FRAC * w_px;
	if (dilate < COMP_REAR_BUDGET_ROI_DILATE_MIN_PX) {
		dilate = COMP_REAR_BUDGET_ROI_DILATE_MIN_PX;
	}
	x0 -= dilate;
	x1 += dilate;
	y0 -= dilate;
	y1 += dilate;

	x0 = comp_rear_budget_clampf(floorf(x0), 0.0f, w_px);
	x1 = comp_rear_budget_clampf(ceilf(x1), 0.0f, w_px);
	y0 = comp_rear_budget_clampf(floorf(y0), 0.0f, h_px);
	y1 = comp_rear_budget_clampf(ceilf(y1), 0.0f, h_px);

	// The zone box in preview pixels, and the second half of the clamp: the
	// dilation is a band around the silhouette, and a band is just as able to
	// reach into a 2D strip as the bounds were. Applying the clamp only before
	// dilation would leave an 8-px window on exactly the pixels #1365 is about.
	float zx0 = 0.0f, zy0 = 0.0f, zx1 = w_px, zy1 = h_px;
	if (zone_count > 0) {
		zx0 = comp_rear_budget_clampf(floorf((zu0 - cu0) / (cu1 - cu0) * w_px), 0.0f, w_px);
		zx1 = comp_rear_budget_clampf(ceilf((zu1 - cu0) / (cu1 - cu0) * w_px), 0.0f, w_px);
		zy0 = comp_rear_budget_clampf(floorf((zv0 - cv0) / (cv1 - cv0) * h_px), 0.0f, h_px);
		zy1 = comp_rear_budget_clampf(ceilf((zv1 - cv0) / (cv1 - cv0) * h_px), 0.0f, h_px);

		x0 = (x0 > zx0) ? x0 : zx0;
		y0 = (y0 > zy0) ? y0 : zy0;
		x1 = (x1 < zx1) ? x1 : zx1;
		y1 = (y1 < zy1) ? y1 : zy1;
	}

	struct u_bg_roi roi = {(uint32_t)x0, (uint32_t)y0, (uint32_t)((x1 > x0) ? (x1 - x0) : 0.0f),
	                       (uint32_t)((y1 > y0) ? (y1 - y0) : 0.0f)};

	/*
	 * Degenerate after clamping — a region that landed off the preview
	 * entirely. A region too small to hold a measurement must never be
	 * measured: an empty ROI reads as neutral and would open the budget over a
	 * desktop nobody looked at. Where the frame has zones the retry is the zone
	 * box; only a frame with no zones at all falls back to the whole preview.
	 */
	if (roi.w < 2 || roi.h < 1) {
		if (zone_count == 0 || !(zx1 - zx0 >= 2.0f) || !(zy1 - zy0 >= 1.0f)) {
			*out_src = COMP_REAR_BUDGET_ROI_SRC_WHOLE_PREVIEW;
			return;
		}
		roi.x = (uint32_t)zx0;
		roi.y = (uint32_t)zy0;
		roi.w = (uint32_t)(zx1 - zx0);
		roi.h = (uint32_t)(zy1 - zy0);
		// Keep the "bounds were outside" diagnosis if that is how we got here;
		// otherwise the honest label is just "the zones".
		if (*out_src != COMP_REAR_BUDGET_ROI_SRC_ZONES_BOUNDS_OUTSIDE) {
			*out_src = COMP_REAR_BUDGET_ROI_SRC_ZONES;
		}
	}

	*out_roi = roi;
	*out_narrowed = roi.w < pv->width || roi.h < pv->height;
}

void
comp_rear_budget_set_content_bounds(struct comp_rear_budget *b, float u0, float v0, float u1, float v1, uint64_t now_ns)
{
	if (b == NULL || !b->initialised) {
		return;
	}

	// oxr validates and clamps; this is the second gate, because the rect
	// steers a memory read and "advisory" must never come to mean "unchecked".
	const bool finite = isfinite(u0) && isfinite(v0) && isfinite(u1) && isfinite(v1);

	os_mutex_lock(&b->publish_mutex);
	b->bounds_u0 = u0;
	b->bounds_v0 = v0;
	b->bounds_u1 = u1;
	b->bounds_v1 = v1;
	b->bounds_ns = now_ns;
	b->bounds_valid = finite && u1 > u0 && v1 > v0;
	os_mutex_unlock(&b->publish_mutex);
}

void
comp_rear_budget_set_zone_rects(struct comp_rear_budget *b,
                                const struct u_bg_rect_norm *rects,
                                uint32_t count,
                                uint64_t now_ns)
{
	if (b == NULL || !b->initialised) {
		return;
	}
	if (rects == NULL) {
		count = 0;
	}

	/*
	 * More zones than the runner can hold drops the CLAMP, not the tail: the
	 * union of the first n is a subset of the real one, and clamping to a
	 * subset would exclude content the app really drew. It cannot happen (the
	 * compositors' gather loops stop at XRT_MAX_LAYERS, and so does this
	 * array), which is precisely why the impossible case must not be the one
	 * that quietly narrows the answer.
	 */
	if (count > COMP_REAR_BUDGET_MAX_ZONES) {
		count = 0;
	}

	struct u_bg_rect_norm keep[COMP_REAR_BUDGET_MAX_ZONES];
	uint32_t kept = 0;
	for (uint32_t i = 0; i < count; i++) {
		const float u0 = rects[i].u0, v0 = rects[i].v0, u1 = rects[i].u1, v1 = rects[i].v1;
		if (!isfinite(u0) || !isfinite(v0) || !isfinite(u1) || !isfinite(v1)) {
			continue;
		}
		const float cu0 = comp_rear_budget_clampf(u0, 0.0f, 1.0f);
		const float cv0 = comp_rear_budget_clampf(v0, 0.0f, 1.0f);
		const float cu1 = comp_rear_budget_clampf(u1, 0.0f, 1.0f);
		const float cv1 = comp_rear_budget_clampf(v1, 0.0f, 1.0f);
		if (!(cu1 > cu0) || !(cv1 > cv0)) {
			continue; // a zone with no area weaves nothing
		}
		keep[kept].u0 = cu0;
		keep[kept].v0 = cv0;
		keep[kept].u1 = cu1;
		keep[kept].v1 = cv1;
		kept++;
	}

	os_mutex_lock(&b->publish_mutex);
	b->zone_count = kept;
	if (kept > 0) {
		memcpy(b->zones, keep, sizeof(keep[0]) * (size_t)kept);
	}
	b->zones_ns = now_ns;
	os_mutex_unlock(&b->publish_mutex);
}

bool
comp_rear_budget_debug_last_roi(const struct comp_rear_budget *b, struct u_bg_roi *out_roi, bool *out_narrowed)
{
	if (b == NULL || !b->have_roi) {
		return false;
	}
	if (out_roi != NULL) {
		*out_roi = b->last_roi;
	}
	if (out_narrowed != NULL) {
		*out_narrowed = b->last_roi_narrowed;
	}
	return true;
}

enum comp_rear_budget_roi_src
comp_rear_budget_debug_last_roi_src(const struct comp_rear_budget *b)
{
	if (b == NULL || !b->have_roi) {
		return COMP_REAR_BUDGET_ROI_SRC_WHOLE_PREVIEW;
	}
	return b->last_roi_src;
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
	b->roi_enabled = -1;

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
		struct u_bg_roi roi;
		bool narrowed = false;
		enum comp_rear_budget_roi_src src = COMP_REAR_BUDGET_ROI_SRC_WHOLE_PREVIEW;
		comp_rear_budget_derive_roi(b, pv, now_ns, &roi, &narrowed, &src);

		/*
		 * Two reasons to re-measure, and the second is what makes the ROI
		 * live: a capture that CHANGED, or the same capture seen through a
		 * different region. Gating on the generation alone would freeze the
		 * verdict of wherever the content used to be — on a quiet desktop the
		 * generation never advances again, which is the best case for the
		 * budget and the worst case for a stale ROI. Re-analysing an
		 * unchanged capture through an unchanged region stays skipped.
		 */
		const bool gen_new = !b->have_generation || pv->generation != b->last_generation;
		const bool roi_new = !b->have_roi || roi.x != b->last_roi.x || roi.y != b->last_roi.y ||
		                     roi.w != b->last_roi.w || roi.h != b->last_roi.h;

		if (gen_new || roi_new) {
			struct u_bg_neutrality_result res = {0};
			if (u_bg_neutrality_analyse(pv->bgra, pv->width, pv->height, pv->stride_bytes, &roi, NULL,
			                            &res)) {
				b->result = res;
				b->have_result = true;
			}
		}
		b->last_roi = roi;
		b->last_roi_narrowed = narrowed;
		b->last_roi_src = src;
		b->have_roi = true;

		if (gen_new) {
			b->have_generation = true;
			b->last_generation = pv->generation;
			// Retention follows the PICTURE, not the region: the dump shows
			// what the analysis saw, and a moved ROI over an unchanged desktop
			// is the same picture.
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
	if (out.state != before) {
		// Beside u_rear_budget's own transition line, which cannot name the
		// region because the policy is deliberately ROI-blind. Without this a
		// busy verdict is unattributable: measured under the content, or over
		// a canvas the content was nowhere near?
		U_LOG_W("REAR_BUDGET %s: roi=%u,%u,%u,%u (%s)", b->policy.label[0] != '\0' ? b->policy.label : "session",
		        b->last_roi.x, b->last_roi.y, b->last_roi.w, b->last_roi.h,
		        !b->have_roi ? "no preview analysed" : comp_rear_budget_roi_src_str(b->last_roi_src));
	}

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
