// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vendor-neutral UVC side-by-side stereo camera source — see u_stereo_uvc.h.
 * @ingroup aux_util
 */

#include "util/u_stereo_uvc.h"
#include "util/u_stereo_rectify.h"
#include "util/u_logging.h"

#include "os/os_time.h"
#include "xrt/xrt_config_os.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h> // SHGetFolderPathA
#endif

#define PI_D 3.14159265358979323846

static void
seterr(char *err, size_t cap, const char *msg)
{
	if (err != NULL && cap > 0) {
		snprintf(err, cap, "%s", msg);
	}
}

static int
lower_ascii(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

//! Case-insensitive (ASCII) substring search; UTF-8 bytes above 0x7f compare exactly.
static const char *
strcasestr_ascii(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	if (n == 0) {
		return hay;
	}
	for (const char *p = hay; *p != '\0'; p++) {
		size_t i = 0;
		while (i < n && p[i] != '\0' &&
		       lower_ascii((unsigned char)p[i]) == lower_ascii((unsigned char)needle[i])) {
			i++;
		}
		if (i == n) {
			return p;
		}
	}
	return NULL;
}

static bool
streq_ascii_nocase(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		if (lower_ascii((unsigned char)*a) != lower_ascii((unsigned char)*b)) {
			return false;
		}
		a++;
		b++;
	}
	return *a == *b;
}

static int
hexval(int c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	c = lower_ascii(c);
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

//! Exactly 4 hex digits at @p s.
static bool
hex4(const char *s, uint16_t *out)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		int h = hexval((unsigned char)s[i]);
		if (h < 0) {
			return false;
		}
		v = (v << 4) | (uint32_t)h;
	}
	*out = (uint16_t)v;
	return true;
}


/*
 *
 * Config.
 *
 */

void
u_stereo_uvc_entry_defaults(struct u_stereo_uvc_entry *e)
{
	memset(e, 0, sizeof(*e));
	e->layout = U_STEREO_UVC_LAYOUT_SBS_FULL;
}

//! "WxH" or "WxH@fps".
static bool
parse_size(const char *s, uint32_t *w, uint32_t *h, float *fps)
{
	unsigned a = 0, b = 0;
	float f = 0.0f;
	int n = sscanf(s, "%ux%u@%f", &a, &b, &f);
	if (n < 2 || a == 0 || b == 0 || a > 16384 || b > 16384) {
		return false;
	}
	if (fps != NULL) {
		*fps = n == 3 ? f : 0.0f;
		if (n == 3 && !(f > 0.0f && f <= 1000.0f)) {
			return false;
		}
	} else if (n == 3) {
		return false;
	}
	*w = a;
	*h = b;
	return true;
}

static bool
parse_entry(const cJSON *j, uint32_t idx, struct u_stereo_uvc_entry *e, char *err, size_t cap)
{
	char msg[256];
	u_stereo_uvc_entry_defaults(e);
	if (!cJSON_IsObject(j)) {
		snprintf(msg, sizeof(msg), "uvc[%u] is not an object", idx);
		seterr(err, cap, msg);
		return false;
	}
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "match");
	if (v != NULL) {
		if (!cJSON_IsString(v) || v->valuestring[0] == '\0') {
			snprintf(msg, sizeof(msg), "uvc[%u].match must be a non-empty string", idx);
			seterr(err, cap, msg);
			return false;
		}
		snprintf(e->name_contains, sizeof(e->name_contains), "%s", v->valuestring);
	}
	v = cJSON_GetObjectItemCaseSensitive(j, "vid_pid");
	if (v != NULL) {
		if (!cJSON_IsString(v) || !u_stereo_uvc_parse_vid_pid(v->valuestring, &e->vid, &e->pid)) {
			snprintf(msg, sizeof(msg), "uvc[%u].vid_pid must look like \"1234:abcd\"", idx);
			seterr(err, cap, msg);
			return false;
		}
		e->match_vid_pid = true;
	}
	if (e->name_contains[0] == '\0' && !e->match_vid_pid) {
		snprintf(msg, sizeof(msg), "uvc[%u] needs \"match\" (name substring) and/or \"vid_pid\"", idx);
		seterr(err, cap, msg);
		return false;
	}
	v = cJSON_GetObjectItemCaseSensitive(j, "name");
	if (cJSON_IsString(v)) {
		snprintf(e->display_name, sizeof(e->display_name), "%s", v->valuestring);
	}
	v = cJSON_GetObjectItemCaseSensitive(j, "layout");
	if (v != NULL) {
		const char *s = cJSON_IsString(v) ? v->valuestring : "";
		if (strcmp(s, "sbs-full") == 0 || strcmp(s, "full") == 0 || strcmp(s, "sbs") == 0) {
			e->layout = U_STEREO_UVC_LAYOUT_SBS_FULL;
		} else if (strcmp(s, "sbs-half") == 0 || strcmp(s, "half") == 0) {
			e->layout = U_STEREO_UVC_LAYOUT_SBS_HALF;
		} else {
			snprintf(msg, sizeof(msg), "uvc[%u].layout must be \"sbs-full\" or \"sbs-half\"", idx);
			seterr(err, cap, msg);
			return false;
		}
	}
	v = cJSON_GetObjectItemCaseSensitive(j, "eyes");
	if (v != NULL) {
		const char *s = cJSON_IsString(v) ? v->valuestring : "";
		if (strcmp(s, "lr") == 0) {
			e->swap_eyes = false;
		} else if (strcmp(s, "rl") == 0) {
			e->swap_eyes = true;
		} else {
			snprintf(msg, sizeof(msg), "uvc[%u].eyes must be \"lr\" or \"rl\"", idx);
			seterr(err, cap, msg);
			return false;
		}
	}
	v = cJSON_GetObjectItemCaseSensitive(j, "mode");
	if (v != NULL) {
		if (!cJSON_IsString(v) || !parse_size(v->valuestring, &e->mode_width, &e->mode_height, &e->mode_fps) ||
		    (e->mode_width % 4) != 0 || (e->mode_height % 2) != 0) {
			snprintf(msg, sizeof(msg),
			         "uvc[%u].mode must look like \"3840x2160@60\" (width a multiple of 4, even height)",
			         idx);
			seterr(err, cap, msg);
			return false;
		}
	}
	v = cJSON_GetObjectItemCaseSensitive(j, "eye_size");
	if (v != NULL) {
		if (!cJSON_IsString(v) || !parse_size(v->valuestring, &e->eye_width, &e->eye_height, NULL) ||
		    e->eye_width < 16 || e->eye_height < 16) {
			snprintf(msg, sizeof(msg), "uvc[%u].eye_size must look like \"1280x720\"", idx);
			seterr(err, cap, msg);
			return false;
		}
		e->eye_width &= ~1u; // NV12: even extents
		e->eye_height &= ~1u;
	}
	v = cJSON_GetObjectItemCaseSensitive(j, "baseline_mm");
	if (v != NULL) {
		if (!cJSON_IsNumber(v) || !(v->valuedouble >= 1.0 && v->valuedouble <= 1000.0)) {
			snprintf(msg, sizeof(msg), "uvc[%u].baseline_mm must be a number in [1, 1000]", idx);
			seterr(err, cap, msg);
			return false;
		}
		e->baseline_mm = (float)v->valuedouble;
	}
	v = cJSON_GetObjectItemCaseSensitive(j, "hfov_deg");
	if (v != NULL) {
		if (!cJSON_IsNumber(v) || !(v->valuedouble >= 10.0 && v->valuedouble <= 170.0)) {
			snprintf(msg, sizeof(msg), "uvc[%u].hfov_deg must be a number in [10, 170]", idx);
			seterr(err, cap, msg);
			return false;
		}
		e->hfov_deg = (float)v->valuedouble;
	}
	v = cJSON_GetObjectItemCaseSensitive(j, "calibration");
	if (v != NULL) {
		if (!cJSON_IsString(v)) {
			snprintf(msg, sizeof(msg), "uvc[%u].calibration must be a file path", idx);
			seterr(err, cap, msg);
			return false;
		}
		snprintf(e->calibration, sizeof(e->calibration), "%s", v->valuestring);
	}
	return true;
}

bool
u_stereo_uvc_config_parse(const char *json, struct u_stereo_uvc_config *out, char *err, size_t err_cap)
{
	memset(out, 0, sizeof(*out));
	out->fake_params.disparity = 0.03;
	out->fake_params.dy = 0.0;
	out->fake_params.pixel = U_STEREO_UVC_PIXEL_NV12;
	out->fake_params.fps = 30.0f;
	if (json == NULL) {
		seterr(err, err_cap, "no document");
		return false;
	}
	cJSON *root = cJSON_Parse(json);
	if (root == NULL || !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		seterr(err, err_cap, "not a JSON object");
		return false;
	}
	bool ok = true;
	const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "uvc");
	if (list != NULL && !cJSON_IsArray(list)) {
		seterr(err, err_cap, "\"uvc\" must be an array");
		ok = false;
	}
	if (ok && list != NULL) {
		int n = cJSON_GetArraySize(list);
		if (n > U_STEREO_UVC_MAX_ENTRIES) {
			seterr(err, err_cap, "too many \"uvc\" entries (at most 4)");
			ok = false;
		}
		for (int i = 0; ok && i < n; i++) {
			ok = parse_entry(cJSON_GetArrayItem(list, i), (uint32_t)i, &out->entries[out->count], err,
			                 err_cap);
			if (ok) {
				out->count++;
			}
		}
	}
	const cJSON *fake = cJSON_GetObjectItemCaseSensitive(root, "fake");
	if (ok && fake != NULL) {
		if (cJSON_IsBool(fake)) {
			out->fake = cJSON_IsTrue(fake);
		} else if (cJSON_IsObject(fake)) {
			out->fake = true;
			const cJSON *v = cJSON_GetObjectItemCaseSensitive(fake, "disparity");
			if (cJSON_IsNumber(v) && fabs(v->valuedouble) < 0.5) {
				out->fake_params.disparity = v->valuedouble;
			}
			v = cJSON_GetObjectItemCaseSensitive(fake, "dy");
			if (cJSON_IsNumber(v) && fabs(v->valuedouble) < 0.1) {
				out->fake_params.dy = v->valuedouble;
			}
			v = cJSON_GetObjectItemCaseSensitive(fake, "pixel");
			if (cJSON_IsString(v) && strcmp(v->valuestring, "yuy2") == 0) {
				out->fake_params.pixel = U_STEREO_UVC_PIXEL_YUY2;
			}
			v = cJSON_GetObjectItemCaseSensitive(fake, "fps");
			if (cJSON_IsNumber(v) && v->valuedouble >= 1.0 && v->valuedouble <= 240.0) {
				out->fake_params.fps = (float)v->valuedouble;
			}
		} else {
			seterr(err, err_cap, "\"fake\" must be true/false or an object");
			ok = false;
		}
	}
	cJSON_Delete(root);
	if (!ok) {
		out->count = 0;
		out->fake = false;
	}
	return ok;
}

bool
u_stereo_uvc_config_path(char *out, size_t cap)
{
	const char *env = getenv("DXR_STEREO_CAMERA_UVC_CONFIG");
	if (env != NULL && env[0] != '\0') {
		snprintf(out, cap, "%s", env);
		return true;
	}
#ifdef XRT_OS_WINDOWS
	char appdata[MAX_PATH];
	if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata))) {
		return false;
	}
	snprintf(out, cap, "%s\\DisplayXR\\" U_STEREO_UVC_CONFIG_FILENAME, appdata);
#else
	const char *config_home = getenv("XDG_CONFIG_HOME");
	if (config_home != NULL && config_home[0] != '\0') {
		snprintf(out, cap, "%s/displayxr/" U_STEREO_UVC_CONFIG_FILENAME, config_home);
	} else {
		const char *home = getenv("HOME");
		if (home == NULL || home[0] == '\0') {
			return false;
		}
		snprintf(out, cap, "%s/.config/displayxr/" U_STEREO_UVC_CONFIG_FILENAME, home);
	}
#endif
	return true;
}

//! Whole small text file, NUL-terminated; NULL if absent / too big.
static char *
read_text_file(const char *path, size_t max)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 0 || (size_t)len > max) {
		fclose(f);
		return NULL;
	}
	char *buf = (char *)malloc((size_t)len + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

enum u_stereo_uvc_config_load_result
u_stereo_uvc_config_load(struct u_stereo_uvc_config *out, char *path, size_t path_cap, char *err, size_t err_cap)
{
	memset(out, 0, sizeof(*out));
	if (path != NULL && path_cap > 0) {
		path[0] = '\0';
	}
	const char *off = getenv("DXR_STEREO_CAMERA_UVC");
	if (off != NULL && strcmp(off, "0") == 0) {
		seterr(err, err_cap, "DXR_STEREO_CAMERA_UVC=0");
		return U_STEREO_UVC_CONFIG_NONE;
	}
	char p[1024];
	if (!u_stereo_uvc_config_path(p, sizeof(p))) {
		seterr(err, err_cap, "no per-user config directory");
		return U_STEREO_UVC_CONFIG_NONE;
	}
	if (path != NULL && path_cap > 0) {
		snprintf(path, path_cap, "%s", p);
	}
	char *text = read_text_file(p, 256 * 1024);
	if (text == NULL) {
		seterr(err, err_cap, "no config file");
		return U_STEREO_UVC_CONFIG_NONE;
	}
	bool ok = u_stereo_uvc_config_parse(text, out, err, err_cap);
	free(text);
	return ok ? U_STEREO_UVC_CONFIG_OK : U_STEREO_UVC_CONFIG_INVALID;
}


/*
 *
 * Devices + matching.
 *
 */

bool
u_stereo_uvc_parse_vid_pid(const char *s, uint16_t *out_vid, uint16_t *out_pid)
{
	if (s == NULL) {
		return false;
	}
	const char *v = strcasestr_ascii(s, "vid_");
	if (v != NULL) {
		const char *p = strcasestr_ascii(v, "pid_");
		return p != NULL && hex4(v + 4, out_vid) && hex4(p + 4, out_pid);
	}
	// "XXXX:YYYY" (optionally 0x-prefixed halves).
	const char *a = s;
	if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) {
		a += 2;
	}
	if (strlen(a) < 9 || !hex4(a, out_vid) || a[4] != ':') {
		return false;
	}
	const char *b = a + 5;
	if (b[0] == '0' && (b[1] == 'x' || b[1] == 'X')) {
		b += 2;
	}
	return strlen(b) == 4 && hex4(b, out_pid);
}

bool
u_stereo_uvc_entry_matches(const struct u_stereo_uvc_entry *e, const struct u_stereo_uvc_device *d)
{
	if (e->name_contains[0] == '\0' && !e->match_vid_pid) {
		return false; // never "match everything"
	}
	if (e->name_contains[0] != '\0' && strcasestr_ascii(d->name, e->name_contains) == NULL) {
		return false;
	}
	if (e->match_vid_pid && (!d->has_vid_pid || d->vid != e->vid || d->pid != e->pid)) {
		return false;
	}
	return true;
}

void
u_stereo_uvc_assign(const struct u_stereo_uvc_config *cfg,
                    const struct u_stereo_uvc_device *devs,
                    uint32_t device_count,
                    const bool *unavailable,
                    int32_t out_device_for_entry[U_STEREO_UVC_MAX_ENTRIES])
{
	bool taken[U_STEREO_UVC_MAX_DEVICES] = {0};
	uint32_t n = device_count > U_STEREO_UVC_MAX_DEVICES ? U_STEREO_UVC_MAX_DEVICES : device_count;
	for (uint32_t i = 0; i < n; i++) {
		taken[i] = unavailable != NULL && unavailable[i];
	}
	for (uint32_t e = 0; e < U_STEREO_UVC_MAX_ENTRIES; e++) {
		out_device_for_entry[e] = U_STEREO_UVC_ASSIGN_NONE;
		if (e >= cfg->count) {
			continue;
		}
		const struct u_stereo_uvc_entry *ent = &cfg->entries[e];
		int32_t hit = U_STEREO_UVC_ASSIGN_NONE;
		uint32_t matches = 0;
		for (uint32_t i = 0; i < n; i++) {
			if (!taken[i] && u_stereo_uvc_entry_matches(ent, &devs[i])) {
				if (matches++ == 0) {
					hit = (int32_t)i;
				}
			}
		}
		if (matches > 1 && !ent->match_vid_pid) {
			out_device_for_entry[e] = U_STEREO_UVC_ASSIGN_AMBIGUOUS;
			continue;
		}
		if (hit >= 0) {
			taken[hit] = true;
		}
		out_device_for_entry[e] = hit;
	}
}

bool
u_stereo_uvc_device_claimed_by_hint(const struct u_stereo_uvc_device *d, const char *hint)
{
	if (hint == NULL || hint[0] == '\0') {
		return false;
	}
	if (d->id[0] != '\0' && streq_ascii_nocase(d->id, hint)) {
		return true;
	}
	uint16_t vid = 0, pid = 0;
	return d->has_vid_pid && u_stereo_uvc_parse_vid_pid(hint, &vid, &pid) && vid == d->vid && pid == d->pid;
}


/*
 *
 * Geometry.
 *
 */

bool
u_stereo_uvc_eye_size(uint32_t layout,
                      uint32_t frame_w,
                      uint32_t frame_h,
                      uint32_t want_w,
                      uint32_t want_h,
                      uint32_t *out_w,
                      uint32_t *out_h)
{
	if (frame_w < 64 || frame_h < 32 || (frame_w % 4) != 0 || (frame_h % 2) != 0) {
		return false;
	}
	if (want_w > 0 && want_h > 0) {
		*out_w = want_w & ~1u;
		*out_h = want_h & ~1u;
		return *out_w >= 16 && *out_h >= 16;
	}
	double w = frame_w / 2.0;
	double h = layout == U_STEREO_UVC_LAYOUT_SBS_HALF ? frame_h / 2.0 : (double)frame_h;
	if (w > U_STEREO_UVC_DEFAULT_MAX_EYE_WIDTH) {
		h = h * U_STEREO_UVC_DEFAULT_MAX_EYE_WIDTH / w;
		w = U_STEREO_UVC_DEFAULT_MAX_EYE_WIDTH;
	}
	*out_w = (uint32_t)(w + 0.5) & ~1u;
	*out_h = (uint32_t)(h + 0.5) & ~1u;
	return *out_w >= 16 && *out_h >= 16;
}

void
u_stereo_uvc_nominal_calibration(uint32_t eye_w,
                                 uint32_t eye_h,
                                 double hfov_deg,
                                 double baseline_mm,
                                 struct xrt_plugin_stereo_camera_calibration *out)
{
	uint32_t sz = out->struct_size;
	struct xrt_plugin_stereo_camera_calibration c;
	memset(&c, 0, sizeof(c));
	c.struct_size = (uint32_t)sizeof(c);
	c.image_width = eye_w;
	c.image_height = eye_h;
	double f = (eye_w / 2.0) / tan(hfov_deg * 0.5 * PI_D / 180.0);
	for (int e = 0; e < 2; e++) {
		c.k[e][0] = f;
		c.k[e][1] = f;
		c.k[e][2] = (eye_w - 1) / 2.0;
		c.k[e][3] = (eye_h - 1) / 2.0;
	}
	c.distortion_model = XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE;
	c.rotation_right_from_left[0][0] = 1.0;
	c.rotation_right_from_left[1][1] = 1.0;
	c.rotation_right_from_left[2][2] = 1.0;
	c.translation_right_from_left_mm[0] = -baseline_mm;
	size_t n = (sz == 0 || sz > sizeof(c)) ? sizeof(c) : sz;
	memcpy(out, &c, n);
	out->struct_size = sz == 0 ? (uint32_t)sizeof(c) : sz;
}


/*
 *
 * Calibration file (OpenCV FileStorage YAML or JSON).
 *
 */

static bool
is_ident_char(int c)
{
	return isalnum(c) || c == '_';
}

//! Position just after "key" + optional quote + optional spaces + ':' — or NULL.
static const char *
find_key(const char *text, const char *key)
{
	size_t n = strlen(key);
	for (const char *p = text; (p = strstr(p, key)) != NULL; p += n) {
		if (p > text && is_ident_char((unsigned char)p[-1])) {
			continue;
		}
		const char *q = p + n;
		if (is_ident_char((unsigned char)*q)) {
			continue;
		}
		if (*q == '"' || *q == '\'') {
			q++;
		}
		while (*q == ' ' || *q == '\t') {
			q++;
		}
		if (*q == ':') {
			return q + 1;
		}
	}
	return NULL;
}

/*!
 * Numbers of the first [...] after @p p (nested brackets flattened). Stops at
 * the bracket that closes the first one. Returns the count, -1 on no array.
 */
static int
read_array(const char *p, double *out, int cap)
{
	// The array must come before the next line that starts a new key: scan to
	// the first '['; a YAML matrix header (!!opencv-matrix, rows, cols, dt) is skipped.
	const char *b = strchr(p, '[');
	if (b == NULL) {
		return -1;
	}
	int depth = 0, n = 0;
	const char *q = b;
	while (*q != '\0') {
		if (*q == '[') {
			depth++;
			q++;
		} else if (*q == ']') {
			depth--;
			q++;
			if (depth == 0) {
				return n;
			}
		} else if (*q == '-' || *q == '+' || *q == '.' || isdigit((unsigned char)*q)) {
			char *end = NULL;
			double v = strtod(q, &end);
			if (end == q) {
				q++;
				continue;
			}
			if (n < cap) {
				out[n] = v;
			}
			n++;
			q = end;
		} else {
			q++;
		}
	}
	return -1;
}

static bool
read_scalar(const char *text, const char *key, double *out)
{
	const char *p = find_key(text, key);
	if (p == NULL) {
		return false;
	}
	char *end = NULL;
	double v = strtod(p, &end);
	if (end == p) {
		return false;
	}
	*out = v;
	return true;
}

static bool
read_matrix(const char *text, const char *k1, const char *k2, double *out, int cap, int *out_n)
{
	const char *p = find_key(text, k1);
	if (p == NULL && k2 != NULL) {
		p = find_key(text, k2);
	}
	if (p == NULL) {
		return false;
	}
	int n = read_array(p, out, cap);
	if (n < 0 || n > cap) {
		return false;
	}
	*out_n = n;
	return true;
}

bool
u_stereo_uvc_calibration_parse(const char *text,
                               uint32_t default_w,
                               uint32_t default_h,
                               struct xrt_plugin_stereo_camera_calibration *out,
                               char *err,
                               size_t err_cap)
{
	if (text == NULL) {
		seterr(err, err_cap, "no text");
		return false;
	}
	uint32_t sz = out->struct_size;
	struct xrt_plugin_stereo_camera_calibration c;
	memset(&c, 0, sizeof(c));
	c.struct_size = (uint32_t)sizeof(c);

	double k[2][9], d[2][14], r[9], t[3];
	int nk[2], nd[2], nr = 0, nt = 0;
	if (!read_matrix(text, "K1", "M1", k[0], 9, &nk[0]) || nk[0] != 9 ||
	    !read_matrix(text, "K2", "M2", k[1], 9, &nk[1]) || nk[1] != 9) {
		seterr(err, err_cap, "K1/M1 and K2/M2 must be 3x3 matrices");
		return false;
	}
	if (!read_matrix(text, "D1", NULL, d[0], 14, &nd[0]) || !read_matrix(text, "D2", NULL, d[1], 14, &nd[1]) ||
	    nd[0] != nd[1]) {
		seterr(err, err_cap, "D1 and D2 must be distortion vectors of the same length");
		return false;
	}
	if (!read_matrix(text, "R", NULL, r, 9, &nr) || (nr != 9 && nr != 3)) {
		seterr(err, err_cap, "R must be a 3x3 matrix or a rotation vector");
		return false;
	}
	if (!read_matrix(text, "T", NULL, t, 3, &nt) || nt != 3) {
		seterr(err, err_cap, "T must be a 3-vector");
		return false;
	}
	bool fisheye = false;
	const char *dm = find_key(text, "distortion_model");
	if (dm != NULL) {
		dm += strspn(dm, " \t\"'");
		char tok[16] = {0};
		for (size_t i = 0; i + 1 < sizeof(tok) && isalnum((unsigned char)dm[i]); i++) {
			tok[i] = (char)lower_ascii((unsigned char)dm[i]);
		}
		fisheye = strcmp(tok, "fisheye") == 0 || strcmp(tok, "kb4") == 0 || strcmp(tok, "equidistant") == 0;
	}
	if (fisheye && nd[0] == 4) {
		c.distortion_model = XRT_PLUGIN_STEREO_CAMERA_DISTORTION_KB4;
	} else if (nd[0] == 4 || nd[0] == 5) {
		c.distortion_model = XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN5;
	} else if (nd[0] == 8) {
		c.distortion_model = XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN8;
	} else if (nd[0] == 0) {
		c.distortion_model = XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE;
	} else {
		seterr(err, err_cap, "distortion vectors must have 4, 5 or 8 coefficients");
		return false;
	}
	for (int e = 0; e < 2; e++) {
		// OpenCV K = [fx 0 cx; 0 fy cy; 0 0 1].
		if (!(k[e][0] > 0.0) || !(k[e][4] > 0.0) || k[e][8] != 1.0) {
			seterr(err, err_cap, "K must be [fx 0 cx; 0 fy cy; 0 0 1] with positive focals");
			return false;
		}
		c.k[e][0] = k[e][0];
		c.k[e][1] = k[e][4];
		c.k[e][2] = k[e][2];
		c.k[e][3] = k[e][5];
		for (int i = 0; i < nd[e] && i < 8; i++) {
			c.distortion[e][i] = d[e][i];
		}
	}
	if (nr == 3) {
		u_stereo_rectify_rodrigues(r, c.rotation_right_from_left);
	} else {
		for (int i = 0; i < 9; i++) {
			c.rotation_right_from_left[i / 3][i % 3] = r[i];
		}
	}
	double tl = sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
	if (!(tl > 0.0)) {
		seterr(err, err_cap, "T is zero");
		return false;
	}
	double scale = tl < 1.0 ? 1000.0 : 1.0; // metres -> mm
	for (int i = 0; i < 3; i++) {
		c.translation_right_from_left_mm[i] = t[i] * scale;
	}
	double w = 0.0, h = 0.0, size[2];
	int nsize = 0;
	if (read_scalar(text, "image_width", &w) && read_scalar(text, "image_height", &h)) {
		c.image_width = (uint32_t)w;
		c.image_height = (uint32_t)h;
	} else if (read_matrix(text, "image_size", "imageSize", size, 2, &nsize) && nsize == 2) {
		c.image_width = (uint32_t)size[0];
		c.image_height = (uint32_t)size[1];
	} else {
		c.image_width = default_w;
		c.image_height = default_h;
	}
	if (c.image_width == 0 || c.image_height == 0) {
		seterr(err, err_cap, "image size is zero");
		return false;
	}
	size_t n = (sz == 0 || sz > sizeof(c)) ? sizeof(c) : sz;
	memcpy(out, &c, n);
	out->struct_size = sz == 0 ? (uint32_t)sizeof(c) : sz;
	return true;
}


/*
 *
 * Split + resample.
 *
 */

//! One 8-bit channel of a plane region: samples at base + y * pitch + x * step.
struct chan
{
	const uint8_t *base;
	uint32_t pitch;
	uint32_t step;
	uint32_t w, h;
};

struct dchan
{
	uint8_t *base;
	uint32_t pitch;
	uint32_t step;
	uint32_t w, h;
};

/*!
 * One row of the box-reduced intermediate grid: row @p m averages source rows
 * [m * iy, m * iy + iy) and columns in groups of @p ix. Rows are walked in
 * memory order and summed into @p acc (16-bit: ix * iy <= 256), then scaled by
 * a fixed-point reciprocal — no per-sample division. A 1 x 1 reduction of a
 * packed (step 1) channel needs no work at all: the source row itself is
 * returned.
 */
static const uint8_t *
box_row(const struct chan *s, uint32_t ix, uint32_t iy, uint32_t mw, uint32_t m, uint16_t *acc, uint8_t *row)
{
	const size_t step = s->step;
	if (ix == 1 && iy == 1) {
		const uint8_t *p = s->base + (size_t)m * s->pitch;
		if (step == 1) {
			return p;
		}
		for (uint32_t x = 0; x < mw; x++) {
			row[x] = p[x * step];
		}
		return row;
	}
	memset(acc, 0, sizeof(uint16_t) * mw);
	for (uint32_t yy = 0; yy < iy; yy++) {
		const uint8_t *p = s->base + (size_t)(m * iy + yy) * s->pitch;
		if (ix == 1 && step == 1) {
			for (uint32_t x = 0; x < mw; x++) {
				acc[x] = (uint16_t)(acc[x] + p[x]);
			}
		} else if (ix == 1) {
			for (uint32_t x = 0; x < mw; x++) {
				acc[x] = (uint16_t)(acc[x] + p[x * step]);
			}
		} else {
			for (uint32_t x = 0; x < mw; x++) {
				const uint8_t *q = p + (size_t)x * ix * step;
				uint32_t a = 0;
				for (uint32_t xx = 0; xx < ix; xx++) {
					a += q[xx * step];
				}
				acc[x] = (uint16_t)(acc[x] + a);
			}
		}
	}
	const uint32_t div = ix * iy;
	const uint32_t recip = (65536u + div / 2) / div; // sum * recip >> 16 ~= sum / div
	for (uint32_t x = 0; x < mw; x++) {
		uint32_t v = ((uint32_t)acc[x] * recip + 32768u) >> 16;
		row[x] = (uint8_t)(v > 255 ? 255 : v);
	}
	return row;
}

//! Pixel-centre bilinear tap from a @p dst-sized grid into a @p src-sized one.
static void
tap(uint32_t i, uint32_t src, uint32_t dst, uint32_t *i0, uint32_t *i1, uint32_t *w)
{
	double f = ((double)i + 0.5) * (double)src / (double)dst - 0.5;
	if (f < 0.0) {
		f = 0.0;
	}
	if (f > (double)(src - 1)) {
		f = (double)(src - 1);
	}
	uint32_t a = (uint32_t)f;
	uint32_t wt = (uint32_t)((f - (double)a) * 256.0 + 0.5);
	if (wt >= 256) {
		a++;
		wt = 0;
	}
	if (a > src - 1) {
		a = src - 1;
	}
	*i0 = a;
	*i1 = a + 1 < src ? a + 1 : src - 1;
	*w = wt;
}

static bool
resample(const struct chan *s, const struct dchan *d)
{
	if (s->w == 0 || s->h == 0 || d->w == 0 || d->h == 0) {
		return false;
	}
	if (s->w == d->w && s->h == d->h) {
		for (uint32_t y = 0; y < d->h; y++) {
			const uint8_t *sp = s->base + (size_t)y * s->pitch;
			uint8_t *dp = d->base + (size_t)y * d->pitch;
			if (s->step == 1 && d->step == 1) {
				memcpy(dp, sp, d->w);
			} else {
				for (uint32_t x = 0; x < d->w; x++) {
					dp[(size_t)x * d->step] = sp[(size_t)x * s->step];
				}
			}
		}
		return true;
	}
	// Integer box pre-reduction (anti-aliasing), then bilinear for the rest.
	uint32_t ix = s->w / d->w, iy = s->h / d->h;
	ix = ix < 1 ? 1 : ix > 16 ? 16 : ix;
	iy = iy < 1 ? 1 : iy > 16 ? 16 : iy;
	const uint32_t mw = s->w / ix, mh = s->h / iy;
	uint32_t *x0 = (uint32_t *)malloc(sizeof(uint32_t) * d->w * 3);
	uint8_t *rows = (uint8_t *)malloc((size_t)mw * 2);
	uint16_t *acc = (uint16_t *)malloc(sizeof(uint16_t) * mw);
	if (x0 == NULL || rows == NULL || acc == NULL) {
		free(x0);
		free(rows);
		free(acc);
		return false;
	}
	uint32_t *x1 = x0 + d->w, *wx = x0 + 2 * d->w;
	bool h_identity = mw == d->w;
	for (uint32_t x = 0; x < d->w; x++) {
		tap(x, mw, d->w, &x0[x], &x1[x], &wx[x]);
	}
	// Two cached intermediate rows, slot = row parity: y1 is y0 or y0 + 1, so
	// the two rows one output row needs never share a slot.
	uint8_t *rowbuf[2] = {rows, rows + mw};
	const uint8_t *row[2] = {NULL, NULL};
	int64_t have[2] = {-1, -1};
	const size_t dstep = d->step;
	for (uint32_t y = 0; y < d->h; y++) {
		uint32_t y0, y1, wy;
		tap(y, mh, d->h, &y0, &y1, &wy);
		const uint32_t need[2] = {y0, y1};
		const uint8_t *r[2];
		const int rows_needed = wy == 0 ? 1 : 2; // an exact row needs no second one
		for (int k = 0; k < rows_needed; k++) {
			const uint32_t slot = need[k] & 1u;
			if (have[slot] != (int64_t)need[k]) {
				row[slot] = box_row(s, ix, iy, mw, need[k], acc, rowbuf[slot]);
				have[slot] = need[k];
			}
			r[k] = row[slot];
		}
		uint8_t *dp = d->base + (size_t)y * d->pitch;
		if (rows_needed == 1) {
			const uint8_t *r0 = r[0];
			if (h_identity) {
				for (uint32_t x = 0; x < d->w; x++) {
					dp[x * dstep] = r0[x];
				}
			} else {
				for (uint32_t x = 0; x < d->w; x++) {
					uint32_t w = wx[x];
					dp[x * dstep] = (uint8_t)((r0[x0[x]] * (256 - w) + r0[x1[x]] * w + 128) >> 8);
				}
			}
			continue;
		}
		const uint8_t *r0 = r[0], *r1 = r[1];
		for (uint32_t x = 0; x < d->w; x++) {
			uint32_t a = h_identity ? x : x0[x], b = h_identity ? x : x1[x], w = h_identity ? 0 : wx[x];
			uint32_t top = r0[a] * (256 - w) + r0[b] * w;
			uint32_t bot = r1[a] * (256 - w) + r1[b] * w;
			dp[x * dstep] = (uint8_t)((top * (256 - wy) + bot * wy + 32768) >> 16);
		}
	}
	free(x0);
	free(rows);
	free(acc);
	return true;
}

/*!
 * Fast path: the frame already has the output size (a GPU-scaled frame), NV12
 * in and out — each eye is a plain row copy of both planes.
 */
static bool
copy_nv12_halves(const struct u_stereo_uvc_raw_frame *in,
                 bool swap_eyes,
                 uint32_t eye_w,
                 uint32_t eye_h,
                 uint8_t *dst,
                 const struct u_stereo_camera_planes *dst_layout)
{
	for (uint32_t e = 0; e < 2; e++) {
		const uint32_t h = swap_eyes ? 1u - e : e;
		for (uint32_t y = 0; y < eye_h; y++) {
			memcpy(dst + dst_layout->offset[0] + (size_t)y * dst_layout->pitch[0] + (size_t)e * eye_w,
			       in->planes[0] + (size_t)y * in->pitches[0] + (size_t)h * eye_w, eye_w);
		}
		for (uint32_t y = 0; y < eye_h / 2; y++) {
			memcpy(dst + dst_layout->offset[1] + (size_t)y * dst_layout->pitch[1] + (size_t)e * eye_w,
			       in->planes[1] + (size_t)y * in->pitches[1] + (size_t)h * eye_w, eye_w);
		}
	}
	return true;
}

bool
u_stereo_uvc_split(const struct u_stereo_uvc_raw_frame *in,
                   bool swap_eyes,
                   uint32_t eye_w,
                   uint32_t eye_h,
                   uint8_t *dst,
                   const struct u_stereo_camera_planes *dst_layout)
{
	if (in == NULL || dst == NULL || dst_layout == NULL || in->planes[0] == NULL || (in->width % 4) != 0 ||
	    (in->height % 2) != 0 || in->width < 8 || in->height < 2 || (eye_w % 2) != 0 || (eye_h % 2) != 0 ||
	    eye_w == 0 || eye_h == 0 || dst_layout->plane_count != 2) {
		return false;
	}
	const uint32_t W = in->width, H = in->height, hw = W / 2;
	if (in->pixel == U_STEREO_UVC_PIXEL_NV12 && in->planes[1] != NULL && hw == eye_w && H == eye_h) {
		return copy_nv12_halves(in, swap_eyes, eye_w, eye_h, dst, dst_layout);
	}
	for (uint32_t e = 0; e < 2; e++) {
		const uint32_t h = swap_eyes ? 1u - e : e; // which half of the source holds output eye e
		struct chan sy, su, sv;
		if (in->pixel == U_STEREO_UVC_PIXEL_NV12) {
			if (in->planes[1] == NULL) {
				return false;
			}
			sy = (struct chan){in->planes[0] + (size_t)h * hw, in->pitches[0], 1, hw, H};
			su = (struct chan){in->planes[1] + (size_t)h * hw, in->pitches[1], 2, hw / 2, H / 2};
			sv = su;
			sv.base += 1;
		} else if (in->pixel == U_STEREO_UVC_PIXEL_YUY2) {
			const uint8_t *p = in->planes[0] + (size_t)h * hw * 2;
			sy = (struct chan){p, in->pitches[0], 2, hw, H};
			su = (struct chan){p + 1, in->pitches[0], 4, hw / 2, H};
			sv = (struct chan){p + 3, in->pitches[0], 4, hw / 2, H};
		} else {
			return false;
		}
		uint8_t *yb = dst + dst_layout->offset[0] + (size_t)e * eye_w;
		uint8_t *uvb = dst + dst_layout->offset[1] + (size_t)e * eye_w; // eye_w bytes = eye_w/2 UV pairs
		struct dchan dy = {yb, dst_layout->pitch[0], 1, eye_w, eye_h};
		struct dchan du = {uvb, dst_layout->pitch[1], 2, eye_w / 2, eye_h / 2};
		struct dchan dv = {uvb + 1, dst_layout->pitch[1], 2, eye_w / 2, eye_h / 2};
		if (!resample(&sy, &dy) || !resample(&su, &du) || !resample(&sv, &dv)) {
			return false;
		}
	}
	return true;
}


/*
 *
 * The fake backend.
 *
 */

#define FAKE_ID "dxr-fake-uvc"
#define FAKE_NAME "DisplayXR synthetic SBS camera"
#define FAKE_CELLS_X 96.0
#define FAKE_CELLS_Y 72.0

struct fake_handle
{
	struct u_stereo_uvc_fake_params p;
	struct u_stereo_uvc_mode mode;
	uint8_t *buf;
	uint32_t pitch[2];
	uint64_t offset[2];
	int64_t t0_ns;
	int64_t period_ns;
	uint64_t seq;
};

static uint32_t
fake_hash(int32_t x, int32_t y)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

//! Smooth value noise in [40, 215] at lattice coordinates (u, v).
static double
fake_tex(double u, double v)
{
	double fu = floor(u), fv = floor(v);
	int32_t iu = (int32_t)fu, iv = (int32_t)fv;
	double a = u - fu, b = v - fv;
	// Smoothstep keeps the gradient continuous at lattice lines.
	a = a * a * (3.0 - 2.0 * a);
	b = b * b * (3.0 - 2.0 * b);
	double v00 = (double)(fake_hash(iu, iv) & 0xff), v10 = (double)(fake_hash(iu + 1, iv) & 0xff);
	double v01 = (double)(fake_hash(iu, iv + 1) & 0xff), v11 = (double)(fake_hash(iu + 1, iv + 1) & 0xff);
	double t = v00 + (v10 - v00) * a, bt = v01 + (v11 - v01) * a;
	return 40.0 + (t + (bt - t) * b) * (175.0 / 255.0);
}

/*!
 * Luma of raw pixel (@p px, @p py) of half @p half: the scene is defined in
 * EYE-NORMALISED coordinates, so every layout (full / half SBS) reads back the
 * same scene after the split. The right half (the right lens) sees the
 * texture shifted by the disparity and lowered by dy.
 */
static uint8_t
fake_luma(const struct u_stereo_uvc_fake_params *p, uint32_t half, uint32_t hw, uint32_t h, uint32_t px, uint32_t py)
{
	double x = ((double)px + 0.5) / hw;
	double y = ((double)py + 0.5) / h;
	if (half == 1) {
		x += p->disparity;
		y -= p->dy;
	}
	double v = fake_tex(x * FAKE_CELLS_X, y * FAKE_CELLS_Y);
	return (uint8_t)(v + 0.5);
}

static void
fake_render(struct fake_handle *f)
{
	const uint32_t W = f->mode.width, H = f->mode.height, hw = W / 2;
	for (uint32_t y = 0; y < H; y++) {
		uint8_t *row = f->buf + f->offset[0] + (size_t)y * f->pitch[0];
		for (uint32_t x = 0; x < W; x++) {
			uint32_t half = x >= hw ? 1u : 0u;
			uint8_t l = fake_luma(&f->p, half, hw, H, x - half * hw, y);
			if (f->p.pixel == U_STEREO_UVC_PIXEL_YUY2) {
				row[2 * (size_t)x] = l;
				row[2 * (size_t)x + 1] = 128;
			} else {
				row[x] = l;
			}
		}
	}
	if (f->p.pixel == U_STEREO_UVC_PIXEL_NV12) {
		memset(f->buf + f->offset[1], 128, (size_t)f->pitch[1] * (H / 2));
	}
}

static uint32_t
fake_enumerate(void *ctx, struct u_stereo_uvc_device *out, uint32_t cap)
{
	(void)ctx;
	if (out != NULL && cap >= 1) {
		memset(out, 0, sizeof(*out));
		snprintf(out->name, sizeof(out->name), "%s", FAKE_NAME);
		snprintf(out->id, sizeof(out->id), "%s", FAKE_ID);
	}
	return 1;
}

static uint32_t
fake_list_modes(void *ctx, const char *id, struct u_stereo_uvc_mode *out, uint32_t cap)
{
	const struct u_stereo_uvc_fake_params *p = (const struct u_stereo_uvc_fake_params *)ctx;
	if (id == NULL || strcmp(id, FAKE_ID) != 0) {
		return 0;
	}
	static const struct u_stereo_uvc_mode modes[] = {
	    {1280, 480, 30.0f},
	    {2560, 720, 30.0f},
	    {1280, 960, 30.0f},
	};
	uint32_t n = (uint32_t)(sizeof(modes) / sizeof(modes[0]));
	for (uint32_t i = 0; i < n && i < cap && out != NULL; i++) {
		out[i] = modes[i];
		out[i].fps = p != NULL ? p->fps : modes[i].fps;
	}
	return n < cap ? n : cap;
}

static bool
fake_open(
    void *ctx, const char *id, const struct u_stereo_uvc_mode *mode, uint32_t out_w, uint32_t out_h, void **out_handle)
{
	const struct u_stereo_uvc_fake_params *p = (const struct u_stereo_uvc_fake_params *)ctx;
	// The fake ignores the output-size hint: it always delivers the full
	// capture, so the source's own split/downscale stays under test.
	(void)out_w;
	(void)out_h;
	if (id == NULL || strcmp(id, FAKE_ID) != 0 || mode == NULL || mode->width < 64 || (mode->width % 4) != 0 ||
	    mode->height < 32 || (mode->height % 2) != 0 || mode->width > 8192 || mode->height > 8192) {
		return false;
	}
	struct fake_handle *f = (struct fake_handle *)calloc(1, sizeof(*f));
	if (f == NULL) {
		return false;
	}
	f->p = *p;
	f->mode = *mode;
	float fps = mode->fps > 0.0f ? mode->fps : p->fps;
	f->period_ns = (int64_t)(1e9 / (fps > 0.0f ? fps : 30.0f));
	if (p->pixel == U_STEREO_UVC_PIXEL_YUY2) {
		f->pitch[0] = mode->width * 2;
		f->offset[0] = 0;
		f->buf = (uint8_t *)malloc((size_t)f->pitch[0] * mode->height);
	} else {
		f->pitch[0] = mode->width;
		f->pitch[1] = mode->width;
		f->offset[1] = (uint64_t)mode->width * mode->height;
		f->buf = (uint8_t *)malloc((size_t)mode->width * mode->height * 3 / 2);
	}
	if (f->buf == NULL) {
		free(f);
		return false;
	}
	fake_render(f); // static scene: render once
	f->t0_ns = os_monotonic_get_ns();
	*out_handle = f;
	return true;
}

static uint32_t
fake_read(void *handle, int64_t timeout_ns, struct u_stereo_uvc_raw_frame *out)
{
	struct fake_handle *f = (struct fake_handle *)handle;
	int64_t due = f->t0_ns + (int64_t)(f->seq + 1) * f->period_ns;
	int64_t now = os_monotonic_get_ns();
	if (due - now > timeout_ns) {
		if (timeout_ns > 0) {
			os_nanosleep(timeout_ns);
		}
		return U_STEREO_UVC_READ_TIMEOUT;
	}
	if (due > now) {
		os_nanosleep(due - now);
	}
	f->seq++;
	memset(out, 0, sizeof(*out));
	out->pixel = f->p.pixel;
	out->width = f->mode.width;
	out->height = f->mode.height;
	out->planes[0] = f->buf + f->offset[0];
	out->pitches[0] = f->pitch[0];
	if (f->p.pixel == U_STEREO_UVC_PIXEL_NV12) {
		out->planes[1] = f->buf + f->offset[1];
		out->pitches[1] = f->pitch[1];
	}
	out->time_ns = os_monotonic_get_ns();
	out->time_is_exposure = false;
	return U_STEREO_UVC_READ_OK;
}

static void
fake_close(void *handle)
{
	struct fake_handle *f = (struct fake_handle *)handle;
	if (f != NULL) {
		free(f->buf);
		free(f);
	}
}

void
u_stereo_uvc_fake_backend_init(struct u_stereo_uvc_backend *out, const struct u_stereo_uvc_fake_params *params)
{
	memset(out, 0, sizeof(*out));
	out->name = "fake";
	out->ctx = (void *)params; // the caller keeps it alive (u_stereo_uvc keeps its own copy)
	out->enumerate = fake_enumerate;
	out->list_modes = fake_list_modes;
	out->open = fake_open;
	out->read = fake_read;
	out->close = fake_close;
}


/*
 *
 * The source.
 *
 */

struct uvc_cam
{
	struct u_stereo_uvc_entry entry;
	struct u_stereo_uvc_device dev;
	struct u_stereo_uvc_mode mode;
	uint32_t eye_w, eye_h;
	struct xrt_plugin_stereo_camera_calibration calib;
	bool calibrated; //!< from a calibration file
	bool baseline_known, hfov_known;
};

struct u_stereo_uvc
{
	struct u_stereo_uvc_backend backend;  //!< .read == NULL: none
	struct u_stereo_uvc_fake_params fake; //!< ctx of the fake backend
	uint32_t count;
	struct uvc_cam cams[U_STEREO_UVC_MAX_ENTRIES];
};

struct u_stereo_uvc_stream
{
	struct u_stereo_uvc *uvc;
	uint32_t index;
	void *handle;
	uint8_t *buf;
	struct u_stereo_camera_planes layout;
	uint64_t seq;
	bool size_warned;
};

static bool
pick_mode(const struct u_stereo_uvc *u,
          const struct u_stereo_uvc_entry *e,
          const char *id,
          struct u_stereo_uvc_mode *out)
{
	struct u_stereo_uvc_mode modes[U_STEREO_UVC_MAX_MODES];
	uint32_t n = u->backend.list_modes != NULL
	                 ? u->backend.list_modes(u->backend.ctx, id, modes, U_STEREO_UVC_MAX_MODES)
	                 : 0;
	if (e->mode_width > 0) {
		out->width = e->mode_width;
		out->height = e->mode_height;
		out->fps = e->mode_fps;
		bool listed = n == 0;
		float best_fps = 0.0f;
		for (uint32_t i = 0; i < n; i++) {
			if (modes[i].width == out->width && modes[i].height == out->height) {
				listed = true;
				best_fps = modes[i].fps > best_fps ? modes[i].fps : best_fps;
			}
		}
		if (!listed) {
			U_LOG_W(
			    "stereo camera (uvc): configured mode %ux%u is not among the %u modes the device lists — "
			    "trying it anyway",
			    out->width, out->height, n);
		}
		if (out->fps <= 0.0f) {
			out->fps = best_fps;
		}
		return true;
	}
	// Auto: the largest frame that splits cleanly, then the fastest.
	bool found = false;
	for (uint32_t i = 0; i < n; i++) {
		const struct u_stereo_uvc_mode *m = &modes[i];
		if ((m->width % 4) != 0 || (m->height % 2) != 0 || m->width < 64) {
			continue;
		}
		uint64_t area = (uint64_t)m->width * m->height, best = (uint64_t)out->width * out->height;
		if (!found || area > best || (area == best && m->fps > out->fps)) {
			*out = *m;
			found = true;
		}
	}
	return found;
}

static void
resolve_calibration(struct uvc_cam *c)
{
	const struct u_stereo_uvc_entry *e = &c->entry;
	c->baseline_known = e->baseline_mm > 0.0f;
	c->hfov_known = e->hfov_deg > 0.0f;
	if (e->calibration[0] != '\0') {
		char *text = read_text_file(e->calibration, 1024 * 1024);
		char err[160] = {0};
		c->calib.struct_size = (uint32_t)sizeof(c->calib);
		if (text == NULL) {
			U_LOG_W(
			    "stereo camera (uvc) \"%s\": calibration file \"%s\" not readable — using the NOMINAL "
			    "model (uncalibrated)",
			    c->dev.name, e->calibration);
		} else if (!u_stereo_uvc_calibration_parse(text, c->mode.width / 2, c->mode.height, &c->calib, err,
		                                           sizeof(err))) {
			U_LOG_W(
			    "stereo camera (uvc) \"%s\": calibration file \"%s\" rejected (%s) — using the NOMINAL "
			    "model (uncalibrated)",
			    c->dev.name, e->calibration, err);
		} else {
			c->calibrated = true;
			c->baseline_known = true;
			c->hfov_known = true;
			U_LOG_W(
			    "stereo camera (uvc) \"%s\": calibration from \"%s\" (%ux%u per eye, fx %.1f, |T| %.1f mm)",
			    c->dev.name, e->calibration, c->calib.image_width, c->calib.image_height, c->calib.k[0][0],
			    sqrt(c->calib.translation_right_from_left_mm[0] *
			             c->calib.translation_right_from_left_mm[0] +
			         c->calib.translation_right_from_left_mm[1] *
			             c->calib.translation_right_from_left_mm[1] +
			         c->calib.translation_right_from_left_mm[2] *
			             c->calib.translation_right_from_left_mm[2]));
		}
		free(text);
		if (c->calibrated) {
			return;
		}
	}
	c->calib.struct_size = (uint32_t)sizeof(c->calib);
	u_stereo_uvc_nominal_calibration(
	    c->eye_w, c->eye_h, c->hfov_known ? e->hfov_deg : U_STEREO_UVC_DEFAULT_HFOV_DEG,
	    c->baseline_known ? e->baseline_mm : U_STEREO_UVC_DEFAULT_BASELINE_MM, &c->calib);
}

struct u_stereo_uvc *
u_stereo_uvc_create(const struct u_stereo_uvc_config *cfg,
                    const struct u_stereo_uvc_backend *backend,
                    const char *const *exclude_hints,
                    uint32_t exclude_count)
{
	struct u_stereo_uvc *u = (struct u_stereo_uvc *)calloc(1, sizeof(*u));
	if (u == NULL || cfg == NULL) {
		return u;
	}
	if (cfg->fake) {
		// The synthetic device REPLACES OS capture: no real camera is considered.
		u->fake = cfg->fake_params;
		u_stereo_uvc_fake_backend_init(&u->backend, &u->fake);
	} else if (backend != NULL) {
		u->backend = *backend;
	}
	if (u->backend.enumerate == NULL || u->backend.open == NULL || u->backend.read == NULL ||
	    u->backend.close == NULL) {
		if (cfg->count > 0) {
			U_LOG_W("stereo camera (uvc): %u configured device(s) but no capture backend on this platform",
			        cfg->count);
		}
		memset(&u->backend, 0, sizeof(u->backend));
		return u;
	}
	struct u_stereo_uvc_device devs[U_STEREO_UVC_MAX_DEVICES];
	memset(devs, 0, sizeof(devs));
	uint32_t nd = u->backend.enumerate(u->backend.ctx, devs, U_STEREO_UVC_MAX_DEVICES);
	nd = nd > U_STEREO_UVC_MAX_DEVICES ? U_STEREO_UVC_MAX_DEVICES : nd;
	bool used[U_STEREO_UVC_MAX_DEVICES] = {0};
	for (uint32_t i = 0; i < nd; i++) {
		devs[i].name[sizeof(devs[i].name) - 1] = '\0';
		devs[i].id[sizeof(devs[i].id) - 1] = '\0';
		if (!devs[i].has_vid_pid) {
			devs[i].has_vid_pid = u_stereo_uvc_parse_vid_pid(devs[i].id, &devs[i].vid, &devs[i].pid);
		}
		for (uint32_t h = 0; h < exclude_count; h++) {
			if (u_stereo_uvc_device_claimed_by_hint(&devs[i], exclude_hints[h])) {
				used[i] = true; // a plug-in's camera (the tracker's): never ours
				U_LOG_I("stereo camera (uvc): \"%s\" is a display plug-in's camera — never claimed",
				        devs[i].name);
			}
		}
	}
	int32_t assigned[U_STEREO_UVC_MAX_ENTRIES];
	u_stereo_uvc_assign(cfg, devs, nd, used, assigned);
	for (uint32_t e = 0; e < cfg->count && u->count < U_STEREO_UVC_MAX_ENTRIES; e++) {
		const struct u_stereo_uvc_entry *ent = &cfg->entries[e];
		const int32_t hit = assigned[e];
		if (hit == U_STEREO_UVC_ASSIGN_AMBIGUOUS) {
			U_LOG_W(
			    "stereo camera (uvc): config entry %u (match \"%s\") matches SEVERAL capture devices — "
			    "none claimed; make \"match\" more specific or add \"vid_pid\" "
			    "(`displayxr-cli camera uvc-devices`)",
			    e, ent->name_contains);
			continue;
		}
		if (hit < 0) {
			U_LOG_W("stereo camera (uvc): config entry %u (match \"%s\"%s) matches no free capture device",
			        e, ent->name_contains, ent->match_vid_pid ? " + vid_pid" : "");
			continue;
		}
		struct uvc_cam *c = &u->cams[u->count];
		memset(c, 0, sizeof(*c));
		c->entry = *ent;
		c->dev = devs[hit];
		if (!pick_mode(u, ent, c->dev.id, &c->mode)) {
			U_LOG_W(
			    "stereo camera (uvc) \"%s\": the device lists no usable mode — set \"mode\" in the config",
			    c->dev.name);
			continue;
		}
		if (!u_stereo_uvc_eye_size(ent->layout, c->mode.width, c->mode.height, ent->eye_width, ent->eye_height,
		                           &c->eye_w, &c->eye_h)) {
			U_LOG_W("stereo camera (uvc) \"%s\": mode %ux%u cannot be split into two eyes", c->dev.name,
			        c->mode.width, c->mode.height);
			continue;
		}
		used[hit] = true;
		resolve_calibration(c);
		char vp[24] = "no VID:PID";
		if (c->dev.has_vid_pid) {
			snprintf(vp, sizeof(vp), "VID:PID %04x:%04x", c->dev.vid, c->dev.pid);
		}
		U_LOG_W("stereo camera (uvc): CLAIMED \"%s\" (%s) — %s %ux%u@%.1f, eyes %s, %ux%u per eye out, %s",
		        c->dev.name, vp, ent->layout == U_STEREO_UVC_LAYOUT_SBS_HALF ? "half-SBS" : "full-SBS",
		        c->mode.width, c->mode.height, c->mode.fps, ent->swap_eyes ? "rl" : "lr", c->eye_w, c->eye_h,
		        c->calibrated ? "CALIBRATED (file)" : "uncalibrated (nominal pinhole + online row alignment)");
		u->count++;
	}
	return u;
}

void
u_stereo_uvc_destroy(struct u_stereo_uvc **uvc_ptr)
{
	if (uvc_ptr == NULL || *uvc_ptr == NULL) {
		return;
	}
	free(*uvc_ptr);
	*uvc_ptr = NULL;
}

uint32_t
u_stereo_uvc_enumerate(struct u_stereo_uvc *uvc, uint32_t capacity, struct xrt_plugin_stereo_camera_info *out)
{
	if (uvc == NULL) {
		return 0;
	}
	for (uint32_t i = 0; i < uvc->count && i < capacity && out != NULL; i++) {
		const struct uvc_cam *c = &uvc->cams[i];
		uint32_t sz = out[i].struct_size;
		struct xrt_plugin_stereo_camera_info info;
		memset(&info, 0, sizeof(info));
		info.struct_size = (uint32_t)sizeof(info);
		snprintf(info.display_name, sizeof(info.display_name), "%s",
		         c->entry.display_name[0] != '\0' ? c->entry.display_name : c->dev.name);
		snprintf(info.device_identity, sizeof(info.device_identity), "uvc:%.120s", c->dev.id);
		snprintf(info.platform_device_hint, sizeof(info.platform_device_hint), "%s", c->dev.id);
		info.flags = XRT_PLUGIN_STEREO_CAMERA_USER_FACING;
		if (c->calibrated) {
			info.flags |= XRT_PLUGIN_STEREO_CAMERA_CALIBRATED;
		}
		info.eye_width = c->eye_w;
		info.eye_height = c->eye_h;
		info.max_frame_rate = c->mode.fps; // the negotiated mode's rate; the service measures the real one
		info.native_format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
		size_t n = (sz == 0 || sz > sizeof(info)) ? sizeof(info) : sz;
		memcpy(&out[i], &info, n);
		out[i].struct_size = sz;
	}
	return uvc->count;
}

xrt_result_t
u_stereo_uvc_get_calibration(struct u_stereo_uvc *uvc,
                             uint32_t index,
                             struct xrt_plugin_stereo_camera_calibration *out,
                             bool *out_nominal,
                             bool *out_baseline_known,
                             bool *out_hfov_known)
{
	if (uvc == NULL || index >= uvc->count || out == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const struct uvc_cam *c = &uvc->cams[index];
	uint32_t sz = out->struct_size;
	size_t n = (sz == 0 || sz > sizeof(c->calib)) ? sizeof(c->calib) : sz;
	memcpy(out, &c->calib, n);
	out->struct_size = sz == 0 ? (uint32_t)sizeof(c->calib) : sz;
	if (out_nominal != NULL) {
		*out_nominal = !c->calibrated;
	}
	if (out_baseline_known != NULL) {
		*out_baseline_known = c->baseline_known;
	}
	if (out_hfov_known != NULL) {
		*out_hfov_known = c->hfov_known;
	}
	return XRT_SUCCESS;
}

bool
u_stereo_uvc_get_mode(struct u_stereo_uvc *uvc, uint32_t index, struct u_stereo_uvc_mode *out, uint32_t *out_layout)
{
	if (uvc == NULL || index >= uvc->count) {
		return false;
	}
	if (out != NULL) {
		*out = uvc->cams[index].mode;
	}
	if (out_layout != NULL) {
		*out_layout = uvc->cams[index].entry.layout;
	}
	return true;
}

xrt_result_t
u_stereo_uvc_open(struct u_stereo_uvc *uvc, uint32_t index, struct u_stereo_uvc_stream **out_stream)
{
	*out_stream = NULL;
	if (uvc == NULL || index >= uvc->count || uvc->backend.open == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const struct uvc_cam *c = &uvc->cams[index];
	struct u_stereo_uvc_stream *s = (struct u_stereo_uvc_stream *)calloc(1, sizeof(*s));
	if (s == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	s->uvc = uvc;
	s->index = index;
	if (!u_stereo_camera_layout(XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12, 2 * c->eye_w, c->eye_h, &s->layout)) {
		free(s);
		return XRT_ERROR_ALLOCATION;
	}
	s->buf = (uint8_t *)malloc((size_t)s->layout.size);
	if (s->buf == NULL) {
		free(s);
		return XRT_ERROR_ALLOCATION;
	}
	// Hint: the SBS frame we resample to, so a GPU backend can decode + scale there.
	if (!uvc->backend.open(uvc->backend.ctx, c->dev.id, &c->mode, 2 * c->eye_w, c->eye_h, &s->handle)) {
		U_LOG_W(
		    "stereo camera (uvc) \"%s\": could not open %ux%u@%.1f (in use by another app, unplugged, or "
		    "the mode is not offered)",
		    c->dev.name, c->mode.width, c->mode.height, c->mode.fps);
		free(s->buf);
		free(s);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	*out_stream = s;
	return XRT_SUCCESS;
}

uint32_t
u_stereo_uvc_wait_frame(struct u_stereo_uvc_stream *s, int64_t timeout_ns, struct xrt_plugin_stereo_camera_frame *out)
{
	struct u_stereo_uvc *uvc = s->uvc;
	const struct uvc_cam *c = &uvc->cams[s->index];
	struct u_stereo_uvc_raw_frame raw;
	memset(&raw, 0, sizeof(raw));
	uint32_t r = uvc->backend.read(s->handle, timeout_ns, &raw);
	if (r == U_STEREO_UVC_READ_TIMEOUT) {
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT;
	}
	if (r != U_STEREO_UVC_READ_OK) {
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR;
	}
	if ((raw.width != c->mode.width || raw.height != c->mode.height) && !s->size_warned) {
		s->size_warned = true;
		U_LOG_W(
		    "stereo camera (uvc) \"%s\": device delivers %ux%u, not the requested %ux%u — resampled to the "
		    "advertised %ux%u per eye",
		    c->dev.name, raw.width, raw.height, c->mode.width, c->mode.height, c->eye_w, c->eye_h);
	}
	if (!u_stereo_uvc_split(&raw, c->entry.swap_eyes, c->eye_w, c->eye_h, s->buf, &s->layout)) {
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT; // an unsplittable frame is dropped, not fatal
	}
	memset(out, 0, sizeof(*out));
	out->sequence = ++s->seq;
	out->time_ns = raw.time_ns > 0 ? raw.time_ns : os_monotonic_get_ns();
	out->time_is_exposure = raw.time_is_exposure;
	out->width = 2 * c->eye_w;
	out->height = c->eye_h;
	out->format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
	out->planes[0] = s->buf + s->layout.offset[0];
	out->planes[1] = s->buf + s->layout.offset[1];
	out->pitches[0] = s->layout.pitch[0];
	out->pitches[1] = s->layout.pitch[1];
	return XRT_PLUGIN_STEREO_CAMERA_WAIT_OK;
}

void
u_stereo_uvc_release_frame(struct u_stereo_uvc_stream *s)
{
	(void)s; // the split buffer is reused by the next wait_frame
}

void
u_stereo_uvc_close(struct u_stereo_uvc_stream *s)
{
	if (s == NULL) {
		return;
	}
	if (s->handle != NULL) {
		s->uvc->backend.close(s->handle);
	}
	free(s->buf);
	free(s);
}
