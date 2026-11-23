// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime's own focus of a splat cloud (see conv_splat_focus.h).
 * @ingroup aux_conversion
 */

#include "conv_splat_focus.h"

#include "util/u_lift_autoconv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

//! Long side of the nearest-surface image the cloud is rasterised into.
#define FOCUS_RASTER_LONG 192u
//! Splats sampled at most (a stride over larger clouds).
#define FOCUS_MAX_SAMPLES (256u * 1024u)
//! Opaque enough to count: sigmoid(opacity) at or above this.
#define FOCUS_MIN_ALPHA 0.25f
//! Fewer usable splats than this: no estimate.
#define FOCUS_MIN_SPLATS 64u


/*
 *
 * PLY header.
 *
 */

static uint32_t
ply_type_size(const char *t, size_t n)
{
	static const struct
	{
		const char *name;
		uint32_t size;
	} k[] = {
	    {"char", 1},  {"uchar", 1},   {"int8", 1},   {"uint8", 1},   {"short", 2}, {"ushort", 2},
	    {"int16", 2}, {"uint16", 2},  {"int", 4},    {"uint", 4},    {"int32", 4}, {"uint32", 4},
	    {"float", 4}, {"float32", 4}, {"double", 8}, {"float64", 8},
	};
	for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		if (strlen(k[i].name) == n && memcmp(k[i].name, t, n) == 0) {
			return k[i].size;
		}
	}
	return 0;
}

static bool
is_float_type(const char *t, size_t n)
{
	return (n == 5 && memcmp(t, "float", 5) == 0) || (n == 7 && memcmp(t, "float32", 7) == 0);
}

//! Next whitespace-separated token of [*p, end) on the current line.
static bool
next_tok(const char **p, const char *end, const char **tok, size_t *len)
{
	const char *s = *p;
	while (s < end && (*s == ' ' || *s == '\t' || *s == '\r')) {
		s++;
	}
	if (s >= end) {
		return false;
	}
	const char *e = s;
	while (e < end && *e != ' ' && *e != '\t' && *e != '\r') {
		e++;
	}
	*tok = s;
	*len = (size_t)(e - s);
	*p = e;
	return true;
}

static bool
tok_is(const char *tok, size_t len, const char *word)
{
	return strlen(word) == len && memcmp(tok, word, len) == 0;
}

bool
conv_ply_vertex_layout(
    const uint8_t *ply, size_t size, size_t *out_header, uint64_t *out_count, uint32_t *out_stride, int32_t out_off[4])
{
	if (ply == NULL || size < 16 || memcmp(ply, "ply", 3) != 0) {
		return false;
	}
	const char *text = (const char *)ply;
	const size_t scan = size < 65536 ? size : 65536;
	const char *end_header = NULL;
	for (size_t i = 0; i + 10 <= scan; i++) {
		if (memcmp(text + i, "end_header", 10) == 0) {
			end_header = text + i;
			break;
		}
	}
	if (end_header == NULL) {
		return false;
	}
	const char *body = end_header + 10;
	if (body < text + size && *body == '\r') {
		body++;
	}
	if (body >= text + size || *body != '\n') {
		return false;
	}
	body++;

	bool binary_le = false;
	int element = -1; // 0 = vertex (must be the first element)
	uint64_t count = 0;
	uint32_t stride = 0;
	int32_t off[4] = {-1, -1, -1, -1};
	const char *line = text;
	while (line < end_header) {
		const char *eol = memchr(line, '\n', (size_t)(end_header - line));
		if (eol == NULL) {
			eol = end_header;
		}
		const char *p = line;
		const char *tok;
		size_t len;
		if (next_tok(&p, eol, &tok, &len)) {
			if (tok_is(tok, len, "format")) {
				binary_le = next_tok(&p, eol, &tok, &len) && tok_is(tok, len, "binary_little_endian");
			} else if (tok_is(tok, len, "element")) {
				element++;
				if (element == 0) {
					const char *name;
					size_t nlen;
					if (!next_tok(&p, eol, &name, &nlen) || !tok_is(name, nlen, "vertex") ||
					    !next_tok(&p, eol, &tok, &len)) {
						return false;
					}
					char num[24] = {0};
					memcpy(num, tok, len < sizeof(num) - 1 ? len : sizeof(num) - 1);
					count = strtoull(num, NULL, 10);
				}
			} else if (tok_is(tok, len, "property") && element == 0) {
				const char *type, *name;
				size_t tlen, nlen;
				if (!next_tok(&p, eol, &type, &tlen) || tok_is(type, tlen, "list") ||
				    !next_tok(&p, eol, &name, &nlen)) {
					return false;
				}
				const uint32_t sz = ply_type_size(type, tlen);
				if (sz == 0) {
					return false;
				}
				const char *names[4] = {"x", "y", "z", "opacity"};
				for (int k = 0; k < 4; k++) {
					if (tok_is(name, nlen, names[k]) && is_float_type(type, tlen)) {
						off[k] = (int32_t)stride;
					}
				}
				stride += sz;
			}
		}
		line = eol + 1;
	}
	if (!binary_le || element < 0 || count == 0 || stride == 0 || off[0] < 0 || off[1] < 0 || off[2] < 0) {
		return false;
	}
	const size_t header = (size_t)(body - text);
	if (count > (uint64_t)(size - header) / stride) {
		return false; // truncated
	}
	*out_header = header;
	*out_count = count;
	*out_stride = stride;
	memcpy(out_off, off, sizeof(off));
	return true;
}


/*
 *
 * Estimate.
 *
 */

static float
rdf(const uint8_t *p)
{
	float v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static int
cmp_float(const void *a, const void *b)
{
	const float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}

//! Percentile @p q (0..1) of @p v (sorted in place).
static float
percentile(float *v, uint32_t n, float q)
{
	qsort(v, n, sizeof(float), cmp_float);
	const double at = (double)q * (double)(n - 1);
	const uint32_t i = (uint32_t)at;
	if (i + 1 >= n) {
		return v[n - 1];
	}
	const double f = at - (double)i;
	return (float)((1.0 - f) * v[i] + f * v[i + 1]);
}

bool
conv_splat_focus_from_ply(
    const uint8_t *ply, size_t size, bool rub, const float intrinsics[6], struct conv_splat_focus *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	size_t header = 0;
	uint64_t count = 0;
	uint32_t stride = 0;
	int32_t off[4];
	if (!conv_ply_vertex_layout(ply, size, &header, &count, &stride, off)) {
		return false;
	}

	// Sample the opaque splats in front of the camera: tangent-space position + depth.
	const uint64_t step = count > FOCUS_MAX_SAMPLES ? (count + FOCUS_MAX_SAMPLES - 1) / FOCUS_MAX_SAMPLES : 1;
	const uint32_t cap = (uint32_t)((count + step - 1) / step);
	float *tu = (float *)malloc(sizeof(float) * cap);
	float *tv = (float *)malloc(sizeof(float) * cap);
	float *dz = (float *)malloc(sizeof(float) * cap);
	float *scratch = (float *)malloc(sizeof(float) * cap);
	float *raster = (float *)malloc(sizeof(float) * FOCUS_RASTER_LONG * FOCUS_RASTER_LONG);
	struct u_lift_autoconv_grid *grid = (struct u_lift_autoconv_grid *)malloc(sizeof(*grid));
	bool ok = false;
	if (tu == NULL || tv == NULL || dz == NULL || scratch == NULL || raster == NULL || grid == NULL) {
		goto done;
	}
	uint32_t n = 0;
	const float min_logit = logf(FOCUS_MIN_ALPHA / (1.0f - FOCUS_MIN_ALPHA));
	for (uint64_t i = 0; i < count && n < cap; i += step) {
		const uint8_t *v = ply + header + i * stride;
		const float x = rdf(v + off[0]);
		const float y = rdf(v + off[1]);
		const float z = rdf(v + off[2]);
		const float depth = rub ? -z : z;
		if (!isfinite(x) || !isfinite(y) || !(depth > 1e-4f) || !isfinite(depth)) {
			continue;
		}
		if (off[3] >= 0) {
			const float o = rdf(v + off[3]);
			if (!(o >= min_logit)) {
				continue; // faint (or NaN): floaters must not drive the screen plane
			}
		}
		tu[n] = x / depth;
		tv[n] = (rub ? -y : y) / depth; // image rows grow downwards
		dz[n] = depth;
		n++;
	}
	if (n < FOCUS_MIN_SPLATS) {
		goto done;
	}

	// The frame: the camera's when known, else the cloud's robust angular extent.
	float u0, u1, v0, v1;
	if (intrinsics != NULL && intrinsics[0] > 0.0f && intrinsics[4] > 0.0f && intrinsics[5] > 0.0f) {
		const float fx = intrinsics[0], fy = intrinsics[1] > 0.0f ? intrinsics[1] : intrinsics[0];
		u0 = -intrinsics[2] / fx;
		u1 = (intrinsics[4] - intrinsics[2]) / fx;
		v0 = -intrinsics[3] / fy;
		v1 = (intrinsics[5] - intrinsics[3]) / fy;
	} else {
		memcpy(scratch, tu, sizeof(float) * n);
		u0 = percentile(scratch, n, 0.02f);
		u1 = percentile(scratch, n, 0.98f);
		memcpy(scratch, tv, sizeof(float) * n);
		v0 = percentile(scratch, n, 0.02f);
		v1 = percentile(scratch, n, 0.98f);
	}
	if (!(u1 - u0 > 1e-6f) || !(v1 - v0 > 1e-6f)) {
		goto done;
	}
	const float aspect = (u1 - u0) / (v1 - v0);
	uint32_t rw = FOCUS_RASTER_LONG, rh = FOCUS_RASTER_LONG;
	if (aspect >= 1.0f) {
		rh = (uint32_t)fmaxf(8.0f, fminf((float)FOCUS_RASTER_LONG, (float)FOCUS_RASTER_LONG / aspect + 0.5f));
	} else {
		rw = (uint32_t)fmaxf(8.0f, fminf((float)FOCUS_RASTER_LONG, (float)FOCUS_RASTER_LONG * aspect + 0.5f));
	}
	for (uint32_t i = 0; i < rw * rh; i++) {
		raster[i] = NAN;
	}
	// Nearest surface per cell (max inverse depth); robust near / far over every used splat.
	uint32_t used = 0;
	for (uint32_t i = 0; i < n; i++) {
		const float fx = (tu[i] - u0) / (u1 - u0) * (float)rw;
		const float fy = (tv[i] - v0) / (v1 - v0) * (float)rh;
		if (!(fx >= 0.0f && fx < (float)rw && fy >= 0.0f && fy < (float)rh)) {
			continue;
		}
		float *cell = &raster[(uint32_t)fy * rw + (uint32_t)fx];
		const float q = 1.0f / dz[i];
		if (!(*cell >= q)) {
			*cell = q;
		}
		scratch[used++] = dz[i];
	}
	if (used < FOCUS_MIN_SPLATS) {
		goto done;
	}
	const float near_m = percentile(scratch, used, 0.02f);
	const float far_m = percentile(scratch, used, 0.98f);

	// Screen plane: the lifted-video rule on the nearest-surface image.
	float qmin = INFINITY, qmax = -INFINITY;
	uint32_t cells = 0;
	for (uint32_t i = 0; i < rw * rh; i++) {
		if (isfinite(raster[i])) {
			qmin = fminf(qmin, raster[i]);
			qmax = fmaxf(qmax, raster[i]);
			scratch[cells++] = raster[i];
		}
	}
	float subject = 0.0f;
	enum conv_splat_focus_method method = CONV_SPLAT_FOCUS_NONE;
	if (cells > 0 && u_lift_autoconv_grid_from_samples(raster, rw, rh, rw, true, 1.0f, 0.0f, 64, grid)) {
		struct u_lift_autoconv_params p;
		u_lift_autoconv_params_default(&p);
		struct u_lift_autoconv_estimate e;
		if (u_lift_autoconv_find_subject(grid, NULL, &p, &e) && e.valid) {
			const float q = qmin + e.mode_h * (qmax - qmin);
			if (q > 0.0f && isfinite(q)) {
				subject = 1.0f / q;
				method = CONV_SPLAT_FOCUS_CONVERGENCE;
			}
		}
	}
	if (method == CONV_SPLAT_FOCUS_NONE && cells > 0) {
		const float q = percentile(scratch, cells, 0.80f); // nearness: larger = nearer
		if (q > 0.0f && isfinite(q)) {
			subject = 1.0f / q;
			method = CONV_SPLAT_FOCUS_PERCENTILE;
		}
	}
	if (method == CONV_SPLAT_FOCUS_NONE) {
		goto done;
	}
	out->method = method;
	out->subject_m = subject;
	out->near_m = near_m < subject ? near_m : subject;
	out->far_m = far_m > subject ? far_m : subject;
	out->point[0] = 0.0f;
	out->point[1] = 0.0f;
	out->point[2] = rub ? -subject : subject;
	out->splats = used;
	ok = true;

done:
	free(tu);
	free(tv);
	free(dz);
	free(scratch);
	free(raster);
	free(grid);
	return ok;
}
