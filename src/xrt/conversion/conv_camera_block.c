// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Splat facts JSON and the convenience camera-block writer (see conv_camera_block.h).
 * @ingroup aux_conversion
 */

#include "conv_camera_block.h"

#include <cjson/cJSON.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define DXR_STAT _stat64
#define DXR_STAT_FN _stat64
#define DXR_S_IFMT _S_IFMT
#define DXR_S_IFDIR _S_IFDIR
#else
#define DXR_STAT stat
#define DXR_STAT_FN stat
#define DXR_S_IFMT S_IFMT
#define DXR_S_IFDIR S_IFDIR
#endif

static void
seterr(char *err, size_t n, const char *msg)
{
	if (err != NULL && n > 0) {
		(void)snprintf(err, n, "%s", msg);
	}
}


/*
 *
 * Facts JSON.
 *
 */

//! A float fact as JSON: 7 significant digits (what a float holds), not 0.800000011920929.
static double
fnum(float v)
{
	if (!isfinite(v) || v == 0.0f) {
		return 0.0;
	}
	char b[32];
	(void)snprintf(b, sizeof(b), "%.7g", (double)v);
	return strtod(b, NULL);
}

static cJSON *
farray(const float *v, int n)
{
	cJSON *a = cJSON_CreateArray();
	for (int i = 0; i < n; i++) {
		cJSON_AddItemToArray(a, cJSON_CreateNumber(fnum(v[i])));
	}
	return a;
}

static void
add_str(cJSON *o, const char *k, const char *v)
{
	if (v != NULL && v[0] != '\0') {
		cJSON_AddStringToObject(o, k, v);
	}
}

static void
add_pos(cJSON *o, const char *k, float v)
{
	if (isfinite(v) && v > 0.0f) {
		cJSON_AddNumberToObject(o, k, fnum(v));
	}
}

static const char *
refusal_word(uint32_t c)
{
	switch (c) {
	case XRT_CONV_REFUSAL_METRIC_FIT_REFUSED: return "metric_fit_refused";
	case XRT_CONV_REFUSAL_BASELINE_MISSING: return "baseline_missing";
	case XRT_CONV_REFUSAL_INPUT_TOO_LARGE: return "input_too_large";
	case XRT_CONV_REFUSAL_UNRECTIFIED_PAIR: return "unrectified_pair";
	case XRT_CONV_REFUSAL_UNSUPPORTED_INPUT: return "unsupported_input";
	case XRT_CONV_REFUSAL_REMOTE_ERROR: return "remote_error";
	default: return "none";
	}
}

cJSON *
conv_splat_facts_to_json(const struct xrt_conv_splat_info *si, const struct xrt_conv_splat_result *sr)
{
	cJSON *o = cJSON_CreateObject();
	if (o == NULL || si == NULL) {
		return o;
	}
	const bool have_sr = sr != NULL && sr->struct_size >= sizeof(*sr);
	if (have_sr && (sr->valid & XRT_CONV_SPLAT_VALID_BUILD) != 0) {
		cJSON *b = cJSON_AddObjectToObject(o, "build");
		add_str(b, "provider_commit", sr->build_provider);
		add_str(b, "model_id", sr->build_model_id);
		add_str(b, "model_sha256", sr->build_model_sha256);
		add_str(b, "runtime", sr->build_runtime);
		add_str(b, "runtime_version", sr->build_runtime_version);
		add_str(b, "host_kind",
		        sr->host_kind == XRT_CONV_HOST_KIND_REMOTE  ? "remote"
		        : sr->host_kind == XRT_CONV_HOST_KIND_LOCAL ? "local"
		                                                    : "");
		add_str(b, "host_id", sr->build_host_id);
	}
	if (have_sr && sr->refusal_code != XRT_CONV_REFUSAL_NONE) {
		cJSON *r = cJSON_AddObjectToObject(o, "refusal");
		cJSON_AddStringToObject(r, "code", refusal_word(sr->refusal_code));
		add_str(r, "reason", sr->refusal_reason);
	}
	cJSON_AddStringToObject(o, "coords", si->coords == XRT_CONV_COORDS_RUB ? "rub" : "opencv");
	if (si->fx > 0.0f && si->grid_width > 0 && si->grid_height > 0) {
		cJSON *in = cJSON_AddObjectToObject(o, "intrinsics");
		cJSON_AddNumberToObject(in, "fx", fnum(si->fx));
		cJSON_AddNumberToObject(in, "fy", fnum(si->fy > 0.0f ? si->fy : si->fx));
		cJSON_AddNumberToObject(in, "cx", fnum(si->cx));
		cJSON_AddNumberToObject(in, "cy", fnum(si->cy));
		cJSON_AddNumberToObject(in, "width", si->grid_width);
		cJSON_AddNumberToObject(in, "height", si->grid_height);
		cJSON_AddStringToObject(in, "source",
		                        si->intrinsics_source == XRT_CONV_INTRINSICS_GIVEN     ? "given"
		                        : si->intrinsics_source == XRT_CONV_INTRINSICS_ASSUMED ? "assumed"
		                                                                               : "estimated");
	}
	const bool metric = si->metric != 0 && si->baseline_m > 0.0f;
	cJSON_AddBoolToObject(o, "metric", metric);
	if (metric) {
		cJSON_AddNumberToObject(o, "baseline_m", fnum(si->baseline_m));
	}
	if (have_sr && (sr->valid & XRT_CONV_SPLAT_VALID_METRIC_FIT) != 0) {
		cJSON *m = cJSON_AddObjectToObject(o, "metric_fit");
		add_pos(m, "scale", sr->metric_scale);
		cJSON_AddStringToObject(m, "source",
		                        sr->metric_source == XRT_CONV_METRIC_SOURCE_STEREO  ? "stereo"
		                        : sr->metric_source == XRT_CONV_METRIC_SOURCE_MODEL ? "model"
		                        : sr->metric_source == XRT_CONV_METRIC_SOURCE_OTHER ? "other"
		                                                                            : "none");
		if (sr->metric_inlier_fraction >= 0.0f) {
			cJSON_AddNumberToObject(m, "inlier_fraction", fnum(sr->metric_inlier_fraction));
		}
		if (sr->metric_mad_log >= 0.0f) {
			cJSON_AddNumberToObject(m, "mad_log", fnum(sr->metric_mad_log));
		}
		cJSON_AddNumberToObject(m, "principal_point_shift_px", fnum(sr->principal_point_shift_px));
	}
	add_pos(o, "near_m", si->near_m);
	add_pos(o, "far_m", si->far_m);
	if (have_sr && (sr->valid & XRT_CONV_SPLAT_VALID_LAYERS) != 0) {
		cJSON *ls = cJSON_AddArrayToObject(o, "layers");
		for (uint32_t i = 0; i < sr->layer_count && i < XRT_CONV_MAX_SPLAT_LAYERS; i++) {
			cJSON *l = cJSON_CreateObject();
			cJSON_AddBoolToObject(l, "visible", (sr->layer_flags[i] & XRT_CONV_LAYER_VISIBLE) != 0);
			cJSON_AddBoolToObject(l, "inpainted", (sr->layer_flags[i] & XRT_CONV_LAYER_INPAINTED) != 0);
			cJSON_AddItemToArray(ls, l);
		}
	}
	if (si->splat_count > 0) {
		cJSON_AddNumberToObject(o, "splat_count", (double)si->splat_count);
	}
	if (have_sr && (sr->valid & XRT_CONV_SPLAT_VALID_FOCUS) != 0) {
		cJSON *f = cJSON_AddObjectToObject(o, "focus_suggestion");
		const float p[3] = {sr->focus_point[0], sr->focus_point[1], sr->focus_point[2]};
		cJSON_AddItemToObject(f, "point", farray(p, 3));
		add_pos(f, "subject_m", sr->focus_subject_m);
		add_pos(f, "near_m", sr->focus_near_m);
		add_pos(f, "far_m", sr->focus_far_m);
		cJSON_AddStringToObject(f, "source",
		                        sr->focus_source == XRT_CONV_FOCUS_SOURCE_NEAREST_CLUMP ? "nearest-clump"
		                        : sr->focus_source == XRT_CONV_FOCUS_SOURCE_CONVERGENCE ? "convergence"
		                                                                                : "other");
	}
	return o;
}


/*
 *
 * The block.
 *
 */

static bool
num(const cJSON *o, const char *k, double *out)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) {
		return false;
	}
	*out = v->valuedouble;
	return true;
}

static void
copy_num(cJSON *dst, const cJSON *src, const char *from, const char *to)
{
	double v;
	if (num(src, from, &v)) {
		cJSON_AddNumberToObject(dst, to, v);
	}
}

cJSON *
conv_camera_block_from_facts(const cJSON *facts, char *err, size_t err_size)
{
	const cJSON *in = cJSON_GetObjectItemCaseSensitive(facts, "intrinsics");
	double fx = 0, fy = 0, cx = 0, cy = 0, w = 0, h = 0;
	if (!num(in, "fx", &fx) || fx <= 0 || !num(in, "cx", &cx) || !num(in, "cy", &cy) || !num(in, "width", &w) ||
	    !num(in, "height", &h) || w <= 0 || h <= 0) {
		seterr(err, err_size, "the facts carry no intrinsics: no camera block (a viewer estimates them)");
		return NULL;
	}
	if (!num(in, "fy", &fy) || fy <= 0) {
		fy = fx;
	}
	const cJSON *coords = cJSON_GetObjectItemCaseSensitive(facts, "coords");
	cJSON *b = cJSON_CreateObject();
	// Marks the whole block as written by a tool, not by the author: an author
	// (or a later auto run) may replace it; nothing replaces an authored block.
	cJSON_AddStringToObject(b, "source", "auto");
	cJSON_AddStringToObject(b, "convention",
	                        cJSON_IsString(coords) && strcmp(coords->valuestring, "rub") == 0 ? "rub" : "opencv");
	// Intent defaults of the auto writer (a photo lift conserves its camera).
	cJSON_AddStringToObject(b, "rig", "camera");
	cJSON *rest = cJSON_AddObjectToObject(b, "rest");
	const float pos[3] = {0, 0, 0}, rot[4] = {0, 0, 0, 1};
	cJSON_AddItemToObject(rest, "position", farray(pos, 3));
	cJSON_AddItemToObject(rest, "rotation", farray(rot, 4));
	cJSON *ib = cJSON_AddObjectToObject(b, "intrinsics");
	cJSON_AddNumberToObject(ib, "fx", fx);
	cJSON_AddNumberToObject(ib, "fy", fy);
	cJSON_AddNumberToObject(ib, "cx", cx);
	cJSON_AddNumberToObject(ib, "cy", cy);
	cJSON_AddNumberToObject(ib, "width", (double)(int64_t)w);
	cJSON_AddNumberToObject(ib, "height", (double)(int64_t)h);

	// Metric facts only for a metric result: a guessed baseline is the trap.
	double baseline = 0;
	const bool metric = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(facts, "metric")) &&
	                    num(facts, "baseline_m", &baseline) && baseline > 0;
	if (metric) {
		cJSON *st = cJSON_AddObjectToObject(b, "stereo");
		cJSON_AddNumberToObject(st, "baseline_m", baseline);
		const cJSON *mf = cJSON_GetObjectItemCaseSensitive(facts, "metric_fit");
		if (cJSON_IsObject(mf)) {
			cJSON *m = cJSON_AddObjectToObject(b, "metric");
			copy_num(m, mf, "scale", "scale");
			const cJSON *src = cJSON_GetObjectItemCaseSensitive(mf, "source");
			if (cJSON_IsString(src)) {
				cJSON_AddStringToObject(m, "source", src->valuestring);
			}
			copy_num(m, mf, "inlier_fraction", "inliers");
			copy_num(m, mf, "mad_log", "mad_log");
			copy_num(m, mf, "principal_point_shift_px", "infinity_shift_px");
		}
	}
	// Focus: the provider's SUGGESTION, labelled auto (ADR-057 §3).
	const cJSON *fs = cJSON_GetObjectItemCaseSensitive(facts, "focus_suggestion");
	const cJSON *pt = cJSON_GetObjectItemCaseSensitive(fs, "point");
	if (cJSON_IsArray(pt) && cJSON_GetArraySize(pt) == 3) {
		cJSON *f = cJSON_AddObjectToObject(b, "focus");
		cJSON_AddItemToObject(f, "point", cJSON_Duplicate(pt, true));
		copy_num(f, fs, "subject_m", "subject_m");
		copy_num(f, fs, "near_m", "near_m");
		copy_num(f, fs, "far_m", "far_m");
		cJSON_AddStringToObject(f, "source", "auto");
	}
	cJSON *dxr = cJSON_AddObjectToObject(b, "dxr");
	cJSON_AddNumberToObject(dxr, "ipd_factor", 1.0);
	cJSON_AddNumberToObject(dxr, "parallax_factor", 1.0);
	return b;
}

const char *
conv_camera_block_result_str(enum conv_camera_block_result r)
{
	switch (r) {
	case CONV_CAMERA_BLOCK_WRITTEN: return "written";
	case CONV_CAMERA_BLOCK_REPLACED_AUTO: return "replaced-auto";
	case CONV_CAMERA_BLOCK_KEPT_AUTHORED: return "kept-authored";
	default: return "error";
	}
}

enum conv_camera_block_result
conv_camera_block_apply_meta(const char *meta_json, const cJSON *block, char **out, char *err, size_t err_size)
{
	*out = NULL;
	cJSON *meta = meta_json != NULL ? cJSON_Parse(meta_json) : NULL;
	if (!cJSON_IsObject(meta) || block == NULL) {
		cJSON_Delete(meta);
		seterr(err, err_size, "meta.json is not a JSON object");
		return CONV_CAMERA_BLOCK_ERROR;
	}
	const cJSON *old = cJSON_GetObjectItemCaseSensitive(meta, "camera");
	enum conv_camera_block_result res = CONV_CAMERA_BLOCK_WRITTEN;
	if (old != NULL) {
		const cJSON *src = cJSON_GetObjectItemCaseSensitive(old, "source");
		if (!(cJSON_IsObject(old) && cJSON_IsString(src) && strcmp(src->valuestring, "auto") == 0)) {
			// The author's block: never rewritten, whatever the new facts say.
			cJSON_Delete(meta);
			return CONV_CAMERA_BLOCK_KEPT_AUTHORED;
		}
		res = CONV_CAMERA_BLOCK_REPLACED_AUTO;
	}
	// Rebuild with `camera` right after `count`, every other key in order.
	cJSON *nm = cJSON_CreateObject();
	bool placed = false;
	const cJSON *it = NULL;
	cJSON_ArrayForEach(it, meta)
	{
		if (it->string == NULL || strcmp(it->string, "camera") == 0) {
			continue;
		}
		cJSON_AddItemToObject(nm, it->string, cJSON_Duplicate(it, true));
		if (strcmp(it->string, "count") == 0) {
			cJSON_AddItemToObject(nm, "camera", cJSON_Duplicate(block, true));
			placed = true;
		}
	}
	if (!placed) {
		cJSON_AddItemToObject(nm, "camera", cJSON_Duplicate(block, true));
	}
	char *text = cJSON_Print(nm);
	cJSON_Delete(nm);
	cJSON_Delete(meta);
	if (text == NULL) {
		seterr(err, err_size, "out of memory");
		return CONV_CAMERA_BLOCK_ERROR;
	}
	if (strlen(text) > CONV_SOG_META_MAX_BYTES) {
		free(text);
		seterr(err, err_size, "meta.json would exceed 65536 bytes (viewers refuse it)");
		return CONV_CAMERA_BLOCK_ERROR;
	}
	*out = text;
	return res;
}


/*
 *
 * Files: a bundled .sog (zip, no zip64), meta.json, or a directory.
 *
 */

static uint8_t *
read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	uint8_t *buf = NULL;
	size_t n = 0, cap = 0;
	for (;;) {
		if (n == cap) {
			cap = cap != 0 ? cap * 2 : 1u << 20;
			uint8_t *nb = (uint8_t *)realloc(buf, cap + 1);
			if (nb == NULL) {
				free(buf);
				fclose(f);
				return NULL;
			}
			buf = nb;
		}
		const size_t got = fread(buf + n, 1, cap - n, f);
		n += got;
		if (got == 0) {
			break;
		}
	}
	fclose(f);
	buf[n] = 0;
	*size = n;
	return buf;
}

static bool
write_atomic(const char *path, const void *data, size_t n)
{
	char tmp[1100];
	(void)snprintf(tmp, sizeof(tmp), "%s.dxr-tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (f == NULL) {
		return false;
	}
	const bool ok = fwrite(data, 1, n, f) == n;
	if (fclose(f) != 0 || !ok) {
		(void)remove(tmp);
		return false;
	}
#ifdef _WIN32
	(void)remove(path); // rename does not replace on Windows CRTs
#endif
	if (rename(tmp, path) != 0) {
		(void)remove(tmp);
		return false;
	}
	return true;
}

static uint32_t
rd16(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t
rd32(const uint8_t *p)
{
	return rd16(p) | (rd16(p + 2) << 16);
}

static void
wr16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void
wr32(uint8_t *p, uint32_t v)
{
	wr16(p, v & 0xffffu);
	wr16(p + 2, v >> 16);
}

static uint32_t
crc32_of(const uint8_t *d, size_t n)
{
	static uint32_t table[256];
	static bool ready = false;
	if (!ready) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) {
				c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			}
			table[i] = c;
		}
		ready = true;
	}
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++) {
		c = table[(c ^ d[i]) & 0xffu] ^ (c >> 8);
	}
	return c ^ 0xffffffffu;
}

struct zip_entry
{
	const uint8_t *cd; //!< its central directory record
	uint32_t name_len, extra_len, comment_len;
	uint32_t method, crc, csize, usize, local_off, flags, mtime, mdate, ext_attr, made_by, needed;
	const uint8_t *data; //!< compressed bytes
};

#define ZIP_MAX_ENTRIES 256u

//! Parse the central directory of @p z. False on anything but a plain (non-zip64, non-spanned) zip.
static bool
zip_parse(const uint8_t *z, size_t n, struct zip_entry *e, uint32_t *count, char *err, size_t err_size)
{
	if (n < 22) {
		seterr(err, err_size, "not a zip archive");
		return false;
	}
	size_t eocd = (size_t)-1;
	for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 65535;) {
		if (rd32(z + i) == 0x06054b50u) {
			eocd = i;
			break;
		}
	}
	if (eocd == (size_t)-1) {
		seterr(err, err_size, "not a zip archive (no end of central directory)");
		return false;
	}
	const uint32_t entries = rd16(z + eocd + 10), cd_size = rd32(z + eocd + 12), cd_off = rd32(z + eocd + 16);
	if (entries == 0xffffu || cd_off == 0xffffffffu || (size_t)cd_off + cd_size > n || entries > ZIP_MAX_ENTRIES) {
		seterr(err, err_size, "unsupported zip (zip64, or too many entries)");
		return false;
	}
	size_t p = cd_off;
	for (uint32_t i = 0; i < entries; i++) {
		if (p + 46 > n || rd32(z + p) != 0x02014b50u) {
			seterr(err, err_size, "corrupt zip central directory");
			return false;
		}
		struct zip_entry *x = &e[i];
		x->cd = z + p;
		x->made_by = rd16(z + p + 4);
		x->needed = rd16(z + p + 6);
		x->flags = rd16(z + p + 8);
		x->method = rd16(z + p + 10);
		x->mtime = rd16(z + p + 12);
		x->mdate = rd16(z + p + 14);
		x->crc = rd32(z + p + 16);
		x->csize = rd32(z + p + 20);
		x->usize = rd32(z + p + 24);
		x->name_len = rd16(z + p + 28);
		x->extra_len = rd16(z + p + 30);
		x->comment_len = rd16(z + p + 32);
		x->ext_attr = rd32(z + p + 38);
		x->local_off = rd32(z + p + 42);
		if (x->csize == 0xffffffffu || x->local_off == 0xffffffffu || (size_t)x->local_off + 30 > n ||
		    rd32(z + x->local_off) != 0x04034b50u) {
			seterr(err, err_size, "corrupt zip entry");
			return false;
		}
		const size_t data =
		    (size_t)x->local_off + 30 + rd16(z + x->local_off + 26) + rd16(z + x->local_off + 28);
		if (data + x->csize > n) {
			seterr(err, err_size, "truncated zip entry");
			return false;
		}
		x->data = z + data;
		p += 46 + x->name_len + x->extra_len + x->comment_len;
	}
	*count = entries;
	return true;
}

static bool
entry_is(const struct zip_entry *x, const char *name)
{
	const size_t n = strlen(name);
	return x->name_len == n && memcmp(x->cd + 46, name, n) == 0;
}

//! Growable output for the rewritten archive.
struct zbuf
{
	uint8_t *p;
	size_t n, cap;
	bool failed;
};

static void
zput(struct zbuf *b, const void *d, size_t n)
{
	if (b->failed) {
		return;
	}
	if (b->n + n > b->cap) {
		size_t nc = b->cap != 0 ? b->cap * 2 : 1u << 20;
		while (nc < b->n + n) {
			nc *= 2;
		}
		uint8_t *np = (uint8_t *)realloc(b->p, nc);
		if (np == NULL) {
			b->failed = true;
			return;
		}
		b->p = np;
		b->cap = nc;
	}
	memcpy(b->p + b->n, d, n);
	b->n += n;
}

static char *
zip_read_meta(const uint8_t *z, size_t n, char *err, size_t err_size)
{
	struct zip_entry *e = (struct zip_entry *)calloc(ZIP_MAX_ENTRIES, sizeof(*e));
	uint32_t count = 0;
	char *out = NULL;
	if (e != NULL && zip_parse(z, n, e, &count, err, err_size)) {
		for (uint32_t i = 0; i < count; i++) {
			if (!entry_is(&e[i], "meta.json")) {
				continue;
			}
			if (e[i].method != 0) {
				seterr(err, err_size,
				       "meta.json is compressed in this archive (only stored meta.json is supported)");
				break;
			}
			out = (char *)malloc((size_t)e[i].csize + 1);
			if (out != NULL) {
				memcpy(out, e[i].data, e[i].csize);
				out[e[i].csize] = 0;
			}
			break;
		}
		if (out == NULL && (err == NULL || err[0] == '\0')) {
			seterr(err, err_size, "not a SOG archive: no meta.json entry");
		}
	}
	free(e);
	return out;
}

//! Rewrite archive @p z with meta.json replaced (stored), every other entry byte for byte.
static bool
zip_replace_meta(const uint8_t *z, size_t n, const char *meta, struct zbuf *out, char *err, size_t err_size)
{
	struct zip_entry *e = (struct zip_entry *)calloc(ZIP_MAX_ENTRIES, sizeof(*e));
	uint32_t count = 0;
	if (e == NULL || !zip_parse(z, n, e, &count, err, err_size)) {
		free(e);
		return false;
	}
	uint32_t *offs = (uint32_t *)calloc(count, sizeof(uint32_t));
	const size_t meta_len = strlen(meta);
	const uint32_t meta_crc = crc32_of((const uint8_t *)meta, meta_len);
	for (uint32_t i = 0; offs != NULL && i < count; i++) {
		const struct zip_entry *x = &e[i];
		const bool is_meta = entry_is(x, "meta.json");
		offs[i] = (uint32_t)out->n;
		uint8_t lh[30];
		wr32(lh, 0x04034b50u);
		wr16(lh + 4, x->needed);
		wr16(lh + 6, x->flags & ~0x0008u); // sizes are in the header now, no data descriptor
		wr16(lh + 8, is_meta ? 0u : x->method);
		wr16(lh + 10, x->mtime);
		wr16(lh + 12, x->mdate);
		wr32(lh + 14, is_meta ? meta_crc : x->crc);
		wr32(lh + 18, is_meta ? (uint32_t)meta_len : x->csize);
		wr32(lh + 22, is_meta ? (uint32_t)meta_len : x->usize);
		wr16(lh + 26, x->name_len);
		wr16(lh + 28, 0);
		zput(out, lh, sizeof(lh));
		zput(out, x->cd + 46, x->name_len);
		if (is_meta) {
			zput(out, meta, meta_len);
		} else {
			zput(out, x->data, x->csize);
		}
	}
	const size_t cd_start = out->n;
	for (uint32_t i = 0; offs != NULL && i < count; i++) {
		const struct zip_entry *x = &e[i];
		const bool is_meta = entry_is(x, "meta.json");
		uint8_t ch[46];
		memcpy(ch, x->cd, 46);
		wr16(ch + 8, x->flags & ~0x0008u);
		wr16(ch + 10, is_meta ? 0u : x->method);
		wr32(ch + 16, is_meta ? meta_crc : x->crc);
		wr32(ch + 20, is_meta ? (uint32_t)meta_len : x->csize);
		wr32(ch + 24, is_meta ? (uint32_t)meta_len : x->usize);
		wr16(ch + 30, 0); // extra dropped (local + central stay consistent)
		wr16(ch + 32, 0); // comment dropped
		wr32(ch + 42, offs[i]);
		zput(out, ch, sizeof(ch));
		zput(out, x->cd + 46, x->name_len);
	}
	uint8_t eocd[22];
	memset(eocd, 0, sizeof(eocd));
	wr32(eocd, 0x06054b50u);
	wr16(eocd + 8, count);
	wr16(eocd + 10, count);
	wr32(eocd + 12, (uint32_t)(out->n - cd_start));
	wr32(eocd + 16, (uint32_t)cd_start);
	zput(out, eocd, sizeof(eocd));
	const bool ok = offs != NULL && !out->failed && out->n < 0xffffffffu;
	free(offs);
	free(e);
	if (!ok) {
		seterr(err, err_size, "cannot rebuild the archive");
	}
	return ok;
}

enum sog_kind
{
	SOG_BUNDLED,
	SOG_META,
};

//! Resolve @p path to the file to edit: the .sog itself, or <dir>/meta.json.
static enum sog_kind
sog_target(const char *path, char *file, size_t file_size)
{
	struct DXR_STAT st;
	if (DXR_STAT_FN(path, &st) == 0 && (st.st_mode & DXR_S_IFMT) == DXR_S_IFDIR) {
		(void)snprintf(file, file_size, "%s/meta.json", path);
		return SOG_META;
	}
	(void)snprintf(file, file_size, "%s", path);
	const size_t n = strlen(path);
	return n >= 5 && strcmp(path + n - 5, ".json") == 0 ? SOG_META : SOG_BUNDLED;
}

char *
conv_sog_read_meta(const char *path, char *err, size_t err_size)
{
	if (err != NULL && err_size > 0) {
		err[0] = 0;
	}
	char file[1024];
	const enum sog_kind kind = sog_target(path, file, sizeof(file));
	size_t n = 0;
	uint8_t *data = read_file(file, &n);
	if (data == NULL) {
		seterr(err, err_size, "cannot read the file");
		return NULL;
	}
	if (kind == SOG_META) {
		return (char *)data;
	}
	char *meta = zip_read_meta(data, n, err, err_size);
	free(data);
	return meta;
}

enum conv_camera_block_result
conv_camera_block_write(const char *path, const cJSON *block, char *err, size_t err_size)
{
	if (err != NULL && err_size > 0) {
		err[0] = 0;
	}
	char file[1024];
	const enum sog_kind kind = sog_target(path, file, sizeof(file));
	size_t n = 0;
	uint8_t *data = read_file(file, &n);
	if (data == NULL) {
		seterr(err, err_size, "cannot read the file");
		return CONV_CAMERA_BLOCK_ERROR;
	}
	char *meta = kind == SOG_META ? (char *)data : zip_read_meta(data, n, err, err_size);
	if (meta == NULL) {
		free(data);
		return CONV_CAMERA_BLOCK_ERROR;
	}
	char *new_meta = NULL;
	enum conv_camera_block_result r = conv_camera_block_apply_meta(meta, block, &new_meta, err, err_size);
	if (kind != SOG_META) {
		free(meta);
	}
	if (new_meta != NULL) {
		bool ok;
		if (kind == SOG_META) {
			ok = write_atomic(file, new_meta, strlen(new_meta));
		} else {
			struct zbuf zb;
			memset(&zb, 0, sizeof(zb));
			ok = zip_replace_meta(data, n, new_meta, &zb, err, err_size) && write_atomic(file, zb.p, zb.n);
			free(zb.p);
		}
		if (!ok) {
			if (err != NULL && err[0] == 0) {
				seterr(err, err_size, "cannot write the file");
			}
			r = CONV_CAMERA_BLOCK_ERROR;
		}
		free(new_meta);
	}
	free(data);
	return r;
}
