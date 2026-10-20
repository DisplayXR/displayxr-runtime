// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Screen-capture pixel helpers. See u_desktop_capture.h.
 */

#include "u_desktop_capture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool
u_desktop_image_alloc(struct u_desktop_image *img, uint32_t w, uint32_t h)
{
	memset(img, 0, sizeof(*img));
	if (w == 0 || h == 0 || w > U_DESKTOP_CAPTURE_MAX_NATIVE || h > U_DESKTOP_CAPTURE_MAX_NATIVE) {
		return false;
	}
	img->rgba = (uint8_t *)calloc((size_t)w * h, 4);
	if (img->rgba == NULL) {
		return false;
	}
	img->w = w;
	img->h = h;
	return true;
}

void
u_desktop_image_free(struct u_desktop_image *img)
{
	if (img == NULL) {
		return;
	}
	free(img->rgba);
	memset(img, 0, sizeof(*img));
}

bool
u_desktop_rect_intersect(const struct u_desktop_rect *a, const struct u_desktop_rect *b, struct u_desktop_rect *out)
{
	int64_t x0 = a->x > b->x ? a->x : b->x;
	int64_t y0 = a->y > b->y ? a->y : b->y;
	int64_t ax1 = (int64_t)a->x + a->w, bx1 = (int64_t)b->x + b->w;
	int64_t ay1 = (int64_t)a->y + a->h, by1 = (int64_t)b->y + b->h;
	int64_t x1 = ax1 < bx1 ? ax1 : bx1;
	int64_t y1 = ay1 < by1 ? ay1 : by1;
	if (a->w <= 0 || a->h <= 0 || b->w <= 0 || b->h <= 0 || x1 <= x0 || y1 <= y0) {
		memset(out, 0, sizeof(*out));
		return false;
	}
	out->x = (int32_t)x0;
	out->y = (int32_t)y0;
	out->w = (int32_t)(x1 - x0);
	out->h = (int32_t)(y1 - y0);
	return true;
}

struct u_desktop_rect
u_desktop_rect_bounds(const struct u_desktop_rect *r, uint32_t n)
{
	struct u_desktop_rect b = {0, 0, 0, 0};
	int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool any = false;
	for (uint32_t i = 0; i < n; i++) {
		if (r[i].w <= 0 || r[i].h <= 0) {
			continue;
		}
		int64_t rx1 = (int64_t)r[i].x + r[i].w, ry1 = (int64_t)r[i].y + r[i].h;
		if (!any) {
			x0 = r[i].x, y0 = r[i].y, x1 = rx1, y1 = ry1;
			any = true;
			continue;
		}
		x0 = r[i].x < x0 ? r[i].x : x0;
		y0 = r[i].y < y0 ? r[i].y : y0;
		x1 = rx1 > x1 ? rx1 : x1;
		y1 = ry1 > y1 ? ry1 : y1;
	}
	if (any) {
		b.x = (int32_t)x0;
		b.y = (int32_t)y0;
		b.w = (int32_t)(x1 - x0);
		b.h = (int32_t)(y1 - y0);
	}
	return b;
}

void
u_desktop_capture_fit(uint32_t src_w, uint32_t src_h, uint32_t max_w, uint32_t max_h, uint32_t *out_w, uint32_t *out_h)
{
	if (src_w == 0 || src_h == 0) {
		*out_w = *out_h = 0;
		return;
	}
	if (max_w == 0) {
		max_w = 1;
	}
	if (max_h == 0) {
		max_h = 1;
	}
	if (src_w <= max_w && src_h <= max_h) {
		*out_w = src_w;
		*out_h = src_h;
		return;
	}
	double f = (double)max_w / src_w;
	double fh = (double)max_h / src_h;
	if (fh < f) {
		f = fh;
	}
	uint32_t w = (uint32_t)floor(src_w * f + 0.5);
	uint32_t h = (uint32_t)floor(src_h * f + 0.5);
	w = w < 1 ? 1 : w > max_w ? max_w : w;
	h = h < 1 ? 1 : h > max_h ? max_h : h;
	w = w > src_w ? src_w : w;
	h = h > src_h ? src_h : h;
	*out_w = w;
	*out_h = h;
}

//! Image column / row range [*a, *b) touching desktop span [r0, r1) of an image of
//! @p n px covering [s0, s0 + sl).
static void
span_to_px(int64_t r0, int64_t r1, int32_t s0, int32_t sl, uint32_t n, uint32_t *a, uint32_t *b)
{
	double k = (double)n / (double)sl;
	double fa = floor((double)(r0 - s0) * k);
	double fb = ceil((double)(r1 - s0) * k);
	fa = fa < 0 ? 0 : fa > n ? n : fa;
	fb = fb < 0 ? 0 : fb > n ? n : fb;
	*a = (uint32_t)fa;
	*b = (uint32_t)fb;
}

void
u_desktop_image_withhold(struct u_desktop_image *img, const struct u_desktop_rect *src, const struct u_desktop_rect *r)
{
	if (img == NULL || img->rgba == NULL || src->w <= 0 || src->h <= 0 || r->w <= 0 || r->h <= 0) {
		return;
	}
	uint32_t x0, x1, y0, y1;
	span_to_px(r->x, (int64_t)r->x + r->w, src->x, src->w, img->w, &x0, &x1);
	span_to_px(r->y, (int64_t)r->y + r->h, src->y, src->h, img->h, &y0, &y1);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	for (uint32_t y = y0; y < y1; y++) {
		memset(img->rgba + ((size_t)y * img->w + x0) * 4, 0, (size_t)(x1 - x0) * 4);
	}
}

void
u_desktop_image_draw_frame(struct u_desktop_image *img,
                           const struct u_desktop_rect *src,
                           const struct u_desktop_rect *mon,
                           const uint8_t *frame,
                           uint32_t fw,
                           uint32_t fh,
                           uint32_t stride,
                           enum u_desktop_pixfmt fmt)
{
	if (img == NULL || img->rgba == NULL || frame == NULL || fw == 0 || fh == 0 || mon->w <= 0 || mon->h <= 0 ||
	    src->w <= 0 || src->h <= 0 || stride < fw * 4) {
		return;
	}
	// Column map: image column -> frame column, or -1 when its centre is off the monitor.
	int32_t *cols = (int32_t *)malloc(sizeof(int32_t) * img->w);
	if (cols == NULL) {
		return;
	}
	double kx = (double)src->w / img->w, ky = (double)src->h / img->h;
	for (uint32_t px = 0; px < img->w; px++) {
		double dx = src->x + (px + 0.5) * kx;
		if (dx < mon->x || dx >= (double)mon->x + mon->w) {
			cols[px] = -1;
			continue;
		}
		int64_t fx = (int64_t)floor((dx - mon->x) * fw / mon->w);
		cols[px] = (int32_t)(fx < 0 ? 0 : fx >= fw ? fw - 1 : fx);
	}
	bool bgr = fmt == U_DESKTOP_PIXFMT_BGRX || fmt == U_DESKTOP_PIXFMT_BGRA;
	for (uint32_t py = 0; py < img->h; py++) {
		double dy = src->y + (py + 0.5) * ky;
		if (dy < mon->y || dy >= (double)mon->y + mon->h) {
			continue;
		}
		int64_t fy = (int64_t)floor((dy - mon->y) * fh / mon->h);
		fy = fy < 0 ? 0 : fy >= fh ? fh - 1 : fy;
		const uint8_t *row = frame + (size_t)fy * stride;
		uint8_t *out = img->rgba + (size_t)py * img->w * 4;
		for (uint32_t px = 0; px < img->w; px++) {
			int32_t fx = cols[px];
			if (fx < 0) {
				continue;
			}
			const uint8_t *s = row + (size_t)fx * 4;
			uint8_t *d = out + (size_t)px * 4;
			d[0] = bgr ? s[2] : s[0];
			d[1] = s[1];
			d[2] = bgr ? s[0] : s[2];
			d[3] = 255; // screen pixels are opaque, whatever the x / A byte says
		}
	}
	free(cols);
}

//! One output column's footprint: source columns [x0, x1) with partial weights at the ends.
struct col_span
{
	uint32_t x0, x1;
	float w0, w1; //!< weight of x0 and of x1 - 1 (1 in between)
};

bool
u_desktop_image_downscale(const struct u_desktop_image *in, uint32_t out_w, uint32_t out_h, struct u_desktop_image *out)
{
	memset(out, 0, sizeof(*out));
	if (in == NULL || in->rgba == NULL || out_w == 0 || out_h == 0 || out_w > in->w || out_h > in->h) {
		return false;
	}
	if (!u_desktop_image_alloc(out, out_w, out_h)) {
		return false;
	}
	out->frame_ns = in->frame_ns;
	if (out_w == in->w && out_h == in->h) {
		memcpy(out->rgba, in->rgba, (size_t)in->w * in->h * 4);
		return true;
	}

	double sx = (double)in->w / out_w, sy = (double)in->h / out_h;
	struct col_span *spans = (struct col_span *)malloc(sizeof(struct col_span) * out_w);
	float *acc = (float *)malloc(sizeof(float) * 5 * out_w); // premultiplied r,g,b, alpha, weight
	if (spans == NULL || acc == NULL) {
		free(spans);
		free(acc);
		u_desktop_image_free(out);
		return false;
	}
	for (uint32_t ox = 0; ox < out_w; ox++) {
		double a = ox * sx, b = (ox + 1) * sx;
		uint32_t x0 = (uint32_t)floor(a);
		uint32_t x1 = (uint32_t)ceil(b);
		x1 = x1 > in->w ? in->w : x1;
		x1 = x1 <= x0 ? x0 + 1 : x1;
		spans[ox].x0 = x0;
		spans[ox].x1 = x1;
		spans[ox].w0 = (float)((x0 + 1 < b ? x0 + 1 : b) - a);
		spans[ox].w1 = (float)(b - (x1 - 1 > a ? x1 - 1 : a));
	}
	for (uint32_t oy = 0; oy < out_h; oy++) {
		memset(acc, 0, sizeof(float) * 5 * out_w);
		double a = oy * sy, b = (oy + 1) * sy;
		uint32_t y0 = (uint32_t)floor(a);
		uint32_t y1 = (uint32_t)ceil(b);
		y1 = y1 > in->h ? in->h : y1;
		for (uint32_t y = y0; y < y1; y++) {
			double top = y > a ? y : a, bot = y + 1 < b ? y + 1 : b;
			float wy = (float)(bot - top);
			if (wy <= 0.0f) {
				continue;
			}
			const uint8_t *row = in->rgba + (size_t)y * in->w * 4;
			for (uint32_t ox = 0; ox < out_w; ox++) {
				const struct col_span *sp = &spans[ox];
				float *ac = acc + (size_t)ox * 5;
				for (uint32_t x = sp->x0; x < sp->x1; x++) {
					// A one-column footprint: w0 is its whole width (b - a).
					float wx = x == sp->x0 ? sp->w0 : x == sp->x1 - 1 ? sp->w1 : 1.0f;
					float w = wx * wy;
					const uint8_t *p = row + (size_t)x * 4;
					float al = p[3] * (1.0f / 255.0f) * w;
					ac[0] += p[0] * al;
					ac[1] += p[1] * al;
					ac[2] += p[2] * al;
					ac[3] += al;
					ac[4] += w;
				}
			}
		}
		uint8_t *orow = out->rgba + (size_t)oy * out_w * 4;
		for (uint32_t ox = 0; ox < out_w; ox++) {
			const float *ac = acc + (size_t)ox * 5;
			uint8_t *d = orow + (size_t)ox * 4;
			if (ac[3] <= 0.0f || ac[4] <= 0.0f) {
				continue; // wholly withheld: stays (0,0,0,0)
			}
			for (int c = 0; c < 3; c++) {
				float v = ac[c] / ac[3];
				d[c] = (uint8_t)(v < 0 ? 0 : v > 255.0f ? 255 : (int)(v + 0.5f));
			}
			float alpha = ac[3] / ac[4] * 255.0f;
			d[3] = (uint8_t)(alpha > 254.5f ? 255 : (int)(alpha + 0.5f));
			if (d[3] == 0) {
				d[0] = d[1] = d[2] = 0; // never a coloured fully transparent pixel
			}
		}
	}
	free(spans);
	free(acc);
	return true;
}
