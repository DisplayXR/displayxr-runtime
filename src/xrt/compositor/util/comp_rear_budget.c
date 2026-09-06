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


//! Grow @p buf to at least @p need bytes, keeping what is there. False = OOM.
static bool
comp_rear_budget_grow_u8(uint8_t **buf, size_t *cap, size_t need)
{
	if (*cap >= need) {
		return true;
	}
	uint8_t *grown = (uint8_t *)realloc(*buf, need);
	if (grown == NULL) {
		return false;
	}
	*buf = grown;
	*cap = need;
	return true;
}

//! @copydoc comp_rear_budget_grow_u8, for the dilation's prefix sums.
static bool
comp_rear_budget_grow_u32(uint32_t **buf, size_t *cap, size_t need_elems)
{
	if (*cap >= need_elems) {
		return true;
	}
	uint32_t *grown = (uint32_t *)realloc(*buf, need_elems * sizeof(uint32_t));
	if (grown == NULL) {
		return false;
	}
	*buf = grown;
	*cap = need_elems;
	return true;
}


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

/*!
 * The image the dump sink is handed: the retained preview, with the v3
 * silhouette mask — the pixels the analysis actually judged — tinted 50% toward
 * green.
 *
 * Only the MASK is drawn, and only when one was used. A rect region is already
 * fully described by the `roi=` line, so tinting it would add nothing; a mask is
 * not, and cannot be — a bar and the box around it have the same bounding rect
 * and measure completely different pixels. The tint is what lets an eyeball
 * disagree with the number.
 *
 * The retained copy is never modified: it is reused across dumps, and a tint
 * baked into it would accumulate.
 *
 * @return @ref comp_rear_budget::dump_bgra when there is nothing to draw.
 */
static const uint8_t *
comp_rear_budget_dump_image(struct comp_rear_budget *b)
{
	const uint32_t w = b->dump_w, h = b->dump_h;
	if (!b->dump_have || b->dump_bgra == NULL || !b->roi_mask_in_use || b->roi_mask == NULL ||
	    b->roi_mask_w != w || b->roi_mask_h != h) {
		return b->dump_bgra;
	}

	const size_t bytes = (size_t)w * 4u * (size_t)h;
	if (!comp_rear_budget_grow_u8(&b->dump_tint, &b->dump_tint_cap, bytes)) {
		return b->dump_bgra; // the untinted picture beats no picture
	}
	memcpy(b->dump_tint, b->dump_bgra, bytes);

	for (uint32_t y = 0; y < h; y++) {
		uint8_t *row = b->dump_tint + (size_t)y * (size_t)w * 4u;
		const uint8_t *mrow = b->roi_mask + (size_t)y * (size_t)w;
		for (uint32_t x = 0; x < w; x++) {
			if (mrow[x] == 0) {
				continue;
			}
			uint8_t *px = row + (size_t)x * 4u;
			px[0] = (uint8_t)(px[0] / 2u);           // B
			px[1] = (uint8_t)((px[1] + 255u) / 2u);  // G — toward green
			px[2] = (uint8_t)(px[2] / 2u);           // R
		}
	}
	return b->dump_tint;
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
	case COMP_REAR_BUDGET_ROI_SRC_MASK: return "app content mask";
	case COMP_REAR_BUDGET_ROI_SRC_BOUNDS: return "app content bounds";
	case COMP_REAR_BUDGET_ROI_SRC_BOUNDS_IN_ZONES: return "app content bounds clamped to the 3D zones";
	case COMP_REAR_BUDGET_ROI_SRC_ZONES: return "3D zone union";
	case COMP_REAR_BUDGET_ROI_SRC_ZONES_BOUNDS_OUTSIDE: return "3D zone union (bounds fell outside)";
	case COMP_REAR_BUDGET_ROI_SRC_WHOLE_PREVIEW:
	default: return "whole preview";
	}
}

/*
 * ---------------------------------------------------------------------------
 * The content occupancy mask (XR_DXR_depth_budget v3)
 *
 * The shape, not the box around it. Four steps, in this order, and the order is
 * the argument:
 *
 *   resample -> clamp to the 3D zones -> dilate -> clamp again
 *
 * The zone clamp runs on BOTH sides of the dilation for the same reason the
 * rect path does it twice: the dilation is a band, and a band reaches into a
 * Local2D 2D strip just as readily as the silhouette itself could. Clamping
 * only before would leave a dilation-wide window onto pixels the content can
 * never occlude; clamping only after would let the band grow out of a cell that
 * should not have been there in the first place.
 * ---------------------------------------------------------------------------
 */

/*!
 * One window-normalised coordinate in preview pixels, with cell edges that land
 * on pixel edges SNAPPED to them.
 *
 * Both grids are half-open, so an unsnapped edge is a one-pixel error either
 * way: cell 6 of 20 over a 200-px preview is pixel 60.0, but `6.0f/20.0f*200`
 * is 60.000002, and `ceil` of that claims pixel 60 for a cell that ends exactly
 * where it begins. The snap makes an aligned grid exact instead of leaving it to
 * whichever way the last bit rounded — and 1e-4 of a pixel is far below anything
 * a mask cell could mean.
 */
static double
comp_rear_budget_map_px(double u, double c0, double dc, double n)
{
	double p = (u - c0) / dc * n;
	const double nearest = floor(p + 0.5);
	if (fabs(p - nearest) < 1e-4) {
		p = nearest;
	}
	return p;
}

/*!
 * Mark every preview pixel any nonzero cell overlaps ("any-coverage").
 *
 * Deliberately iterated CELL-first rather than pixel-first. A mask can be
 * coarser or finer than the preview, and the direction that survives both is
 * the one that asks "which pixels does this cell touch" — a cell smaller than a
 * pixel still marks the pixel it lands in, where a pixel-first "which cell is
 * at my centre" would drop it. Under-reporting the silhouette is the one error
 * that opens the budget over something it never measured.
 *
 * @param out Preview-res, tight stride; fully overwritten.
 */
static void
comp_rear_budget_mask_resample(const uint8_t *cells,
                               uint32_t mw,
                               uint32_t mh,
                               uint8_t *out,
                               uint32_t pw,
                               uint32_t ph,
                               float cu0,
                               float cv0,
                               float cu1,
                               float cv1)
{
	memset(out, 0, (size_t)pw * (size_t)ph);

	const double du = (double)cu1 - (double)cu0;
	const double dv = (double)cv1 - (double)cv0;

	for (uint32_t cy = 0; cy < mh; cy++) {
		const uint8_t *crow = cells + (size_t)cy * (size_t)mw;

		// The cell's window-normalised v extent, mapped through the
		// preview's own canvas rect and taken half-open, so a cell edge
		// landing exactly on a pixel boundary does not claim both sides.
		const double va = (double)cy / (double)mh;
		const double vb = (double)(cy + 1u) / (double)mh;
		int y0 = (int)floor(comp_rear_budget_map_px(va, cv0, dv, (double)ph));
		int y1 = (int)ceil(comp_rear_budget_map_px(vb, cv0, dv, (double)ph)) - 1;
		if (y1 < y0) {
			y1 = y0; // a cell finer than a pixel still marks its pixel
		}
		if (y1 < 0 || y0 >= (int)ph) {
			continue;
		}
		if (y0 < 0) {
			y0 = 0;
		}
		if (y1 >= (int)ph) {
			y1 = (int)ph - 1;
		}

		for (uint32_t cx = 0; cx < mw; cx++) {
			if (crow[cx] == 0) {
				continue;
			}
			const double ua = (double)cx / (double)mw;
			const double ub = (double)(cx + 1u) / (double)mw;
			int x0 = (int)floor(comp_rear_budget_map_px(ua, cu0, du, (double)pw));
			int x1 = (int)ceil(comp_rear_budget_map_px(ub, cu0, du, (double)pw)) - 1;
			if (x1 < x0) {
				x1 = x0;
			}
			if (x1 < 0 || x0 >= (int)pw) {
				continue;
			}
			if (x0 < 0) {
				x0 = 0;
			}
			if (x1 >= (int)pw) {
				x1 = (int)pw - 1;
			}
			for (int y = y0; y <= y1; y++) {
				memset(out + (size_t)y * (size_t)pw + (size_t)x0, 1,
				       (size_t)(x1 - x0 + 1));
			}
		}
	}
}

/*!
 * Clear every masked pixel that lies outside every 3D zone.
 *
 * Per-ZONE, not against the box around them: a mask is not a rect, so there is
 * no reason to give away the gap between two zones the way the rect path has to.
 *
 * @param row_scratch At least @p pw bytes.
 * @return Nonzero pixels left.
 */
static uint32_t
comp_rear_budget_mask_clamp_zones(uint8_t *m,
                                  uint8_t *row_scratch,
                                  uint32_t pw,
                                  uint32_t ph,
                                  const struct u_bg_rect_norm *zones,
                                  uint32_t zone_count,
                                  float cu0,
                                  float cv0,
                                  float cu1,
                                  float cv1)
{
	uint32_t kept = 0;

	if (zone_count == 0) {
		// No zones means the whole canvas IS the 3D zone (every full-window
		// app); there is nothing to clamp against.
		for (size_t i = 0, n = (size_t)pw * (size_t)ph; i < n; i++) {
			kept += (m[i] != 0) ? 1u : 0u;
		}
		return kept;
	}

	int zx0[COMP_REAR_BUDGET_MAX_ZONES], zx1[COMP_REAR_BUDGET_MAX_ZONES];
	int zy0[COMP_REAR_BUDGET_MAX_ZONES], zy1[COMP_REAR_BUDGET_MAX_ZONES];
	const double du = (double)cu1 - (double)cu0;
	const double dv = (double)cv1 - (double)cv0;
	for (uint32_t i = 0; i < zone_count; i++) {
		zx0[i] = (int)floor(comp_rear_budget_map_px(zones[i].u0, cu0, du, (double)pw));
		zx1[i] = (int)ceil(comp_rear_budget_map_px(zones[i].u1, cu0, du, (double)pw));
		zy0[i] = (int)floor(comp_rear_budget_map_px(zones[i].v0, cv0, dv, (double)ph));
		zy1[i] = (int)ceil(comp_rear_budget_map_px(zones[i].v1, cv0, dv, (double)ph));
		if (zx0[i] < 0) {
			zx0[i] = 0;
		}
		if (zy0[i] < 0) {
			zy0[i] = 0;
		}
		if (zx1[i] > (int)pw) {
			zx1[i] = (int)pw;
		}
		if (zy1[i] > (int)ph) {
			zy1[i] = (int)ph;
		}
	}

	for (uint32_t y = 0; y < ph; y++) {
		memset(row_scratch, 0, pw);
		for (uint32_t i = 0; i < zone_count; i++) {
			if ((int)y < zy0[i] || (int)y >= zy1[i] || zx1[i] <= zx0[i]) {
				continue;
			}
			memset(row_scratch + zx0[i], 1, (size_t)(zx1[i] - zx0[i]));
		}
		uint8_t *mrow = m + (size_t)y * (size_t)pw;
		for (uint32_t x = 0; x < pw; x++) {
			mrow[x] = (uint8_t)(mrow[x] && row_scratch[x]);
			kept += (mrow[x] != 0) ? 1u : 0u;
		}
	}
	return kept;
}

/*!
 * Dilate the mask by a BOX of Chebyshev radius @p r — a (2r+1) x (2r+1) square
 * structuring element, run as two separable 1D max filters over prefix sums, so
 * the cost is O(w*h) rather than O(w*h*r).
 *
 * A box rather than a diamond: the conflict band is not isotropic in any way
 * this metric can exploit (the metric itself only looks at horizontal
 * differences), and a box is the shape whose radius means the same thing on
 * both axes — the number the log and the spec quote.
 *
 * @param scratch At least max(w, h) + 1 entries.
 */
static void
comp_rear_budget_mask_dilate(uint8_t *m, uint32_t *scratch, uint32_t pw, uint32_t ph, uint32_t r)
{
	if (r == 0) {
		return;
	}

	for (uint32_t y = 0; y < ph; y++) {
		uint8_t *row = m + (size_t)y * (size_t)pw;
		scratch[0] = 0;
		for (uint32_t x = 0; x < pw; x++) {
			scratch[x + 1] = scratch[x] + (row[x] != 0 ? 1u : 0u);
		}
		// The prefix sums are a snapshot, so writing the row back as we go
		// cannot feed the filter its own output (which would smear the mask
		// across the whole row instead of dilating it by r).
		for (uint32_t x = 0; x < pw; x++) {
			const uint32_t lo = (x > r) ? (x - r) : 0u;
			const uint32_t hi = (x + r + 1u < pw) ? (x + r + 1u) : pw;
			row[x] = (scratch[hi] - scratch[lo]) != 0 ? 1u : 0u;
		}
	}

	for (uint32_t x = 0; x < pw; x++) {
		scratch[0] = 0;
		for (uint32_t y = 0; y < ph; y++) {
			scratch[y + 1] = scratch[y] + (m[(size_t)y * (size_t)pw + x] != 0 ? 1u : 0u);
		}
		for (uint32_t y = 0; y < ph; y++) {
			const uint32_t lo = (y > r) ? (y - r) : 0u;
			const uint32_t hi = (y + r + 1u < ph) ? (y + r + 1u) : ph;
			m[(size_t)y * (size_t)pw + x] = (scratch[hi] - scratch[lo]) != 0 ? 1u : 0u;
		}
	}
}

/*!
 * (Re)build @ref comp_rear_budget::roi_mask from the app grid already copied
 * into @ref comp_rear_budget::mask_work.
 *
 * Cached on every input the result depends on — the app's mask generation, the
 * zone rects, the preview dims and the preview's canvas rect — because on a
 * quiet desktop the analysis runs at the poll rate over an unchanged mask, and
 * rebuilding it each time would be the only per-poll work in the whole feature.
 *
 * @return false when the result is unusable (too few pixels to measure, or
 *         cleared away entirely by the zone clamp); the caller then falls
 *         through to the rect path.
 */
static bool
comp_rear_budget_build_mask(struct comp_rear_budget *b,
                            const struct xrt_dp_background_preview *pv,
                            float cu0,
                            float cv0,
                            float cu1,
                            float cv1,
                            const struct u_bg_rect_norm *zones,
                            uint32_t zone_count,
                            uint32_t zone_gen,
                            uint32_t mask_gen)
{
	const uint32_t pw = pv->width;
	const uint32_t ph = pv->height;

	const bool cached = b->roi_mask_key.valid && b->roi_mask_key.mask_gen == mask_gen &&
	                    b->roi_mask_key.zone_gen == zone_gen && b->roi_mask_key.zone_count == zone_count &&
	                    b->roi_mask_key.pw == pw && b->roi_mask_key.ph == ph && b->roi_mask_key.cu0 == cu0 &&
	                    b->roi_mask_key.cv0 == cv0 && b->roi_mask_key.cu1 == cu1 && b->roi_mask_key.cv1 == cv1;
	if (cached) {
		return b->roi_mask_px >= COMP_REAR_BUDGET_MASK_MIN_PX;
	}

	const size_t px_count = (size_t)pw * (size_t)ph;
	const size_t scratch_elems = (size_t)((pw > ph) ? pw : ph) + 1u;
	if (!comp_rear_budget_grow_u8(&b->roi_mask, &b->roi_mask_cap, px_count) ||
	    !comp_rear_budget_grow_u32(&b->dilate_scratch, &b->dilate_scratch_cap, scratch_elems)) {
		// Out of memory for a diagnostic region. The rect path needs no
		// allocation at all, so it is the honest fallback.
		b->roi_mask_px = 0;
		b->roi_mask_key.valid = false;
		return false;
	}

	comp_rear_budget_mask_resample(b->mask_work, b->mask_work_w, b->mask_work_h, b->roi_mask, pw, ph, cu0, cv0,
	                               cu1, cv1);

	// The row scratch for the zone clamp rides on the dilation's prefix-sum
	// buffer: it is uint32 and at least max(pw, ph) + 1 long, so it holds pw
	// bytes with room to spare, and byte access to it is well defined.
	uint8_t *row_scratch = (uint8_t *)b->dilate_scratch;

	uint32_t kept = comp_rear_budget_mask_clamp_zones(b->roi_mask, row_scratch, pw, ph, zones, zone_count, cu0,
	                                                  cv0, cu1, cv1);
	if (kept == 0) {
		/*
		 * The silhouette lies entirely outside every 3D zone — the same app
		 * bug the rect path diagnoses (a zone-normalised grid chained as
		 * window-normalised), and the same answer: this region is unusable,
		 * so fall through rather than measure it. Never "neutral".
		 */
		b->roi_mask_px = 0;
		b->roi_mask_key.valid = false;
		if (!b->mask_outside_logged) {
			b->mask_outside_logged = true;
			U_LOG_W(
			    "REAR_BUDGET: the content mask falls outside the 3D zones — check the "
			    "app's zone→window rebase; falling back to the content bounds");
		}
		return false;
	}

	// Dilation: the runtime's own band, whatever the app asked for on top, and
	// never less than a real band on a small preview. marginNormalized is in
	// window units, so it maps through the preview's canvas rect like anything
	// else does.
	float dilate = COMP_REAR_BUDGET_ROI_DILATE_FRAC * (float)pw;
	const float span = cu1 - cu0;
	const float margin_px = (span > 0.0f) ? (b->mask_work_margin / span * (float)pw) : 0.0f;
	if (margin_px > dilate) {
		dilate = margin_px;
	}
	if (dilate < COMP_REAR_BUDGET_ROI_DILATE_MIN_PX) {
		dilate = COMP_REAR_BUDGET_ROI_DILATE_MIN_PX;
	}
	// Clamped BEFORE the cast, not after: a preview covering a sliver of the
	// window turns even a legal margin into a huge pixel count, and converting
	// a float past UINT32_MAX is undefined rather than merely large. Past both
	// axes the mask is full anyway, so the cap costs nothing.
	const float max_r = (float)(pw + ph);
	if (!(dilate >= 0.0f)) {
		dilate = COMP_REAR_BUDGET_ROI_DILATE_MIN_PX;
	}
	if (dilate > max_r) {
		dilate = max_r;
	}
	const uint32_t r = (uint32_t)(dilate + 0.5f);
	comp_rear_budget_mask_dilate(b->roi_mask, b->dilate_scratch, pw, ph, r);

	kept = comp_rear_budget_mask_clamp_zones(b->roi_mask, row_scratch, pw, ph, zones, zone_count, cu0, cv0, cu1,
	                                         cv1);

	// The bounding rect of what survived. It is what `roi=` reports and what
	// bounds the analysis scan; the MASK is what decides which pixels inside
	// it count.
	uint32_t bx0 = pw, by0 = ph, bx1 = 0, by1 = 0;
	for (uint32_t y = 0; y < ph; y++) {
		const uint8_t *row = b->roi_mask + (size_t)y * (size_t)pw;
		for (uint32_t x = 0; x < pw; x++) {
			if (row[x] == 0) {
				continue;
			}
			if (x < bx0) {
				bx0 = x;
			}
			if (x + 1u > bx1) {
				bx1 = x + 1u;
			}
			if (y < by0) {
				by0 = y;
			}
			if (y + 1u > by1) {
				by1 = y + 1u;
			}
		}
	}

	b->roi_mask_w = pw;
	b->roi_mask_h = ph;
	b->roi_mask_px = kept;
	b->roi_mask_build_id++;
	b->roi_mask_key.mask_gen = mask_gen;
	b->roi_mask_key.zone_gen = zone_gen;
	b->roi_mask_key.zone_count = zone_count;
	b->roi_mask_key.pw = pw;
	b->roi_mask_key.ph = ph;
	b->roi_mask_key.cu0 = cu0;
	b->roi_mask_key.cv0 = cv0;
	b->roi_mask_key.cu1 = cu1;
	b->roi_mask_key.cv1 = cv1;
	b->roi_mask_key.valid = true;

	if (kept < COMP_REAR_BUDGET_MASK_MIN_PX || bx1 <= bx0 || by1 <= by0 || (bx1 - bx0) < 2u) {
		// Too little to measure. The analysis would refuse it, and a refusal
		// mid-tick freezes the previous verdict instead of producing one.
		b->roi_mask_px = 0;
		return false;
	}

	b->roi_mask_rect.x = bx0;
	b->roi_mask_rect.y = by0;
	b->roi_mask_rect.w = bx1 - bx0;
	b->roi_mask_rect.h = by1 - by0;
	return true;
}

void
comp_rear_budget_set_content_mask(struct comp_rear_budget *b,
                                  const uint8_t *cells,
                                  uint32_t w,
                                  uint32_t h,
                                  uint32_t stride,
                                  float margin_normalized,
                                  uint64_t now_ns)
{
	if (b == NULL || !b->initialised) {
		return;
	}

	// oxr validates first; this is the second gate, because the grid steers a
	// memory read and "advisory" must never come to mean "unchecked".
	const bool usable = cells != NULL && w >= 1u && h >= 1u && w <= COMP_REAR_BUDGET_MASK_MAX_DIM &&
	                    h <= COMP_REAR_BUDGET_MASK_MAX_DIM && stride >= w;
	if (!isfinite(margin_normalized) || margin_normalized < 0.0f) {
		margin_normalized = 0.0f;
	}
	if (margin_normalized > 0.5f) {
		margin_normalized = 0.5f;
	}

	if (!usable) {
		// "Absent", never "empty": an empty region measures as neutral and
		// would open the budget over a desktop nobody looked at. The bounds
		// are the fallback, and they are still fresh.
		os_mutex_lock(&b->publish_mutex);
		b->mask_valid = false;
		b->mask_ns = now_ns;
		b->mask_gen++;
		os_mutex_unlock(&b->publish_mutex);
		return;
	}

	os_mutex_lock(&b->publish_mutex);
	bool ok = comp_rear_budget_grow_u8(&b->mask_cells, &b->mask_cap, (size_t)w * (size_t)h);
	uint32_t nonzero = 0;
	if (ok) {
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *src = cells + (size_t)y * (size_t)stride;
			uint8_t *dst = b->mask_cells + (size_t)y * (size_t)w;
			memcpy(dst, src, w);
			for (uint32_t x = 0; x < w; x++) {
				nonzero += (dst[x] != 0) ? 1u : 0u;
			}
		}
	}
	// An all-zero grid is "the app rendered nothing here", which is absence,
	// not a region — the spec says so and the fallback chain depends on it.
	b->mask_valid = ok && nonzero > 0;
	b->mask_w = ok ? w : 0;
	b->mask_h = ok ? h : 0;
	b->mask_margin = margin_normalized;
	b->mask_ns = now_ns;
	b->mask_gen++;
	os_mutex_unlock(&b->publish_mutex);
}

bool
comp_rear_budget_debug_last_mask(const struct comp_rear_budget *b,
                                 const uint8_t **out_mask,
                                 uint32_t *out_w,
                                 uint32_t *out_h,
                                 uint32_t *out_px)
{
	if (b == NULL || !b->roi_mask_in_use || b->roi_mask == NULL || b->roi_mask_px == 0) {
		return false;
	}
	if (out_mask != NULL) {
		*out_mask = b->roi_mask;
	}
	if (out_w != NULL) {
		*out_w = b->roi_mask_w;
	}
	if (out_h != NULL) {
		*out_h = b->roi_mask_h;
	}
	if (out_px != NULL) {
		*out_px = b->roi_mask_px;
	}
	return true;
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
	b->roi_mask_in_use = false;

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
	// The narrower switch: mask off, bounds still on. Two switches because the
	// v2->v3 question ("is the silhouette better than the box?") and the
	// v1->v2 question ("is a region better than the canvas?") are different
	// A/Bs and collapsing them would answer neither.
	if (b->mask_enabled < 0) {
		const char *e = getenv("DXR_REAR_BUDGET_MASK");
		b->mask_enabled = (e != NULL && e[0] == '0') ? 0 : 1;
		if (b->mask_enabled == 0) {
			U_LOG_W(
			    "REAR_BUDGET: DXR_REAR_BUDGET_MASK armed = 0 (content MASK off, "
			    "falling back to the content bounds)");
		}
	}

	struct u_bg_rect_norm zones[COMP_REAR_BUDGET_MAX_ZONES];
	uint32_t zone_count = 0;
	uint32_t zone_gen = 0;
	bool have_mask = false;
	uint32_t mask_gen = 0;

	os_mutex_lock(&b->publish_mutex);
	const bool valid = b->bounds_valid;
	const uint64_t age_ns = (now_ns > b->bounds_ns) ? (now_ns - b->bounds_ns) : 0;
	const float u0 = b->bounds_u0, v0 = b->bounds_v0, u1 = b->bounds_u1, v1 = b->bounds_v1;
	const uint64_t zones_age_ns = (now_ns > b->zones_ns) ? (now_ns - b->zones_ns) : 0;
	if (b->zone_count > 0 && zones_age_ns <= COMP_REAR_BUDGET_ROI_MAX_AGE_NS) {
		zone_count = b->zone_count;
		zone_gen = b->zone_gen;
		memcpy(zones, b->zones, sizeof(zones[0]) * (size_t)zone_count);
	}
	// The app's grid is copied OUT here rather than resampled in place: the
	// render thread must not read app-owned memory, and the locate thread must
	// not wait behind a 512x512 scan to read a float.
	const uint64_t mask_age_ns = (now_ns > b->mask_ns) ? (now_ns - b->mask_ns) : 0;
	if (b->mask_enabled == 1 && b->mask_valid && b->mask_w > 0 && b->mask_h > 0 &&
	    mask_age_ns <= COMP_REAR_BUDGET_ROI_MAX_AGE_NS) {
		const size_t need = (size_t)b->mask_w * (size_t)b->mask_h;
		if (comp_rear_budget_grow_u8(&b->mask_work, &b->mask_work_cap, need)) {
			memcpy(b->mask_work, b->mask_cells, need);
			b->mask_work_w = b->mask_w;
			b->mask_work_h = b->mask_h;
			b->mask_work_margin = b->mask_margin;
			mask_gen = b->mask_gen;
			have_mask = true;
		}
	}
	os_mutex_unlock(&b->publish_mutex);

	const bool have_bounds = valid && age_ns <= COMP_REAR_BUDGET_ROI_MAX_AGE_NS;
	if (!have_mask && !have_bounds && zone_count == 0) {
		return;
	}

	/*
	 * The preview covers `canvas_u0..v1` OF THE CANVAS — normally 0,0,1,1, but
	 * a display processor may include a margin. One that predates the field
	 * leaves it zeroed, and that is the documented normal case rather than a
	 * degenerate one, so it reads as the identity instead of disabling the ROI.
	 *
	 * Resolved before either region path, because both map through it and the
	 * mask cache is keyed on it.
	 */
	float cu0 = pv->canvas_u0, cv0 = pv->canvas_v0, cu1 = pv->canvas_u1, cv1 = pv->canvas_v1;
	if (!(cu1 > cu0) || !(cv1 > cv0)) {
		cu0 = 0.0f;
		cv0 = 0.0f;
		cu1 = 1.0f;
		cv1 = 1.0f;
	}

	/*
	 * Precedence: MASK first. The silhouette is the most specific statement
	 * the app can make about where its content is, and every way it can fail
	 * (absent, all-zero, stale, cleared by the zone clamp, too small to
	 * measure) falls THROUGH to the rect below rather than to "neutral".
	 */
	if (have_mask && comp_rear_budget_build_mask(b, pv, cu0, cv0, cu1, cv1, zones, zone_count, zone_gen,
	                                             mask_gen)) {
		*out_roi = b->roi_mask_rect;
		*out_narrowed = b->roi_mask_px < (uint32_t)pv->width * pv->height;
		*out_src = COMP_REAR_BUDGET_ROI_SRC_MASK;
		b->roi_mask_in_use = true;
		return;
	}

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

	// Map the resolved region into preview pixels, through the canvas rect
	// resolved above.
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
	// Zones are republished EVERY frame, so the generation must count changes
	// rather than publishes: bumping it unconditionally would invalidate the v3
	// mask cache once per frame and turn it into a rebuild with extra steps.
	const bool changed = kept != b->zone_count ||
	                     (kept > 0 && memcmp(b->zones, keep, sizeof(keep[0]) * (size_t)kept) != 0);
	if (changed) {
		b->zone_count = kept;
		if (kept > 0) {
			memcpy(b->zones, keep, sizeof(keep[0]) * (size_t)kept);
		}
		b->zone_gen++;
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
	b->mask_enabled = -1;

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
	free(b->dump_tint);
	b->dump_tint = NULL;
	b->dump_tint_cap = 0;
	free(b->mask_cells);
	b->mask_cells = NULL;
	b->mask_cap = 0;
	b->mask_valid = false;
	free(b->mask_work);
	b->mask_work = NULL;
	b->mask_work_cap = 0;
	free(b->roi_mask);
	b->roi_mask = NULL;
	b->roi_mask_cap = 0;
	b->roi_mask_px = 0;
	b->roi_mask_in_use = false;
	b->roi_mask_key.valid = false;
	free(b->dilate_scratch);
	b->dilate_scratch = NULL;
	b->dilate_scratch_cap = 0;
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
		// open<= is the cue dead band's lower edge (u_rear_budget's
		// open_cue_max). Without it in the armed line a run that sat in the
		// band all session looks identical to one that never measured
		// anything - which is exactly how the panel's open/clipped flap read
		// before the band existed.
		U_LOG_W("REAR_BUDGET: policy %s (requested=%d transparent=%d open<=%.2f)", running ? "ARMED" : "off",
		        b->requested ? 1 : 0, transparent ? 1 : 0, (double)b->policy.tuning.open_cue_max);
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
		// A silhouette can change shape inside an unchanged bounding rect —
		// an arm coming down moves no edge of the box — so the mask needs its
		// own "this is not what we measured last time" term.
		const bool mask_new = b->roi_mask_in_use && b->roi_mask_build_id != b->last_analysed_mask_build_id;

		if (gen_new || roi_new || mask_new) {
			struct u_bg_neutrality_result res = {0};
			const uint8_t *mask = b->roi_mask_in_use ? b->roi_mask : NULL;
			if (u_bg_neutrality_analyse_masked(pv->bgra, pv->width, pv->height, pv->stride_bytes, &roi,
			                                   mask, b->roi_mask_w, NULL, &res)) {
				b->result = res;
				b->have_result = true;
			}
			b->last_analysed_mask_build_id = b->roi_mask_build_id;
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
		U_LOG_W("REAR_BUDGET %s: roi=%u,%u,%u,%u mask=%u (%s)",
		        b->policy.label[0] != '\0' ? b->policy.label : "session", b->last_roi.x, b->last_roi.y,
		        b->last_roi.w, b->last_roi.h, b->roi_mask_in_use ? b->roi_mask_px : 0u,
		        !b->have_roi ? "no preview analysed" : comp_rear_budget_roi_src_str(b->last_roi_src));
	}

	if (b->dump == 1 && out.state != before) {
		if (b->dump_have) {
			const comp_rear_budget_dump_fn sink =
			    (b->dump_sink != NULL) ? b->dump_sink : comp_rear_budget_dump_png;
			sink(b->dump_sink_ctx, comp_rear_budget_dump_image(b), b->dump_w, b->dump_h, b->dump_w * 4u);
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
