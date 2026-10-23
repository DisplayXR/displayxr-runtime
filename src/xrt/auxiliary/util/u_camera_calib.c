// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-owned camera calibration store (see u_camera_calib.h).
 * @ingroup aux_util
 */

#include "util/u_camera_calib.h"
#include "util/u_file.h"

#include "xrt/xrt_config_os.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define CALIB_FILE "camera-calibration.json"
#define VIEWER_FILE "viewer.json"
#define CALIB_FILE_VERSION 1

#ifdef XRT_OS_MACOS
#define MACHINE_CALIB_FILE "/Library/Application Support/DisplayXR/" CALIB_FILE
#else
#define MACHINE_CALIB_FILE "/etc/displayxr/" CALIB_FILE
#endif


/*
 *
 * Names.
 *
 */

static const char *const g_source_names[] = {"none", "default-prior", "placement-hint", "user-calibration",
                                             "display-declared"};

const char *
u_camera_calib_source_str(uint32_t source)
{
	return source < sizeof(g_source_names) / sizeof(g_source_names[0]) ? g_source_names[source] : "none";
}

bool
u_camera_calib_source_parse(const char *s, uint32_t *out)
{
	if (s == NULL) {
		return false;
	}
	for (uint32_t i = 1; i < sizeof(g_source_names) / sizeof(g_source_names[0]); i++) {
		if (strcmp(s, g_source_names[i]) == 0) {
			*out = i;
			return true;
		}
	}
	return false;
}

static const char *const g_edge_names[] = {"top", "bottom", "left", "right"};

const char *
u_camera_calib_edge_str(uint32_t edge)
{
	return edge < 4 ? g_edge_names[edge] : "top";
}

bool
u_camera_calib_edge_parse(const char *s, uint32_t *out)
{
	if (s == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < 4; i++) {
		if (strcmp(s, g_edge_names[i]) == 0) {
			*out = i;
			return true;
		}
	}
	return false;
}


/*
 *
 * Pure geometry + precedence.
 *
 */

static void
identity(double q[4])
{
	q[0] = q[1] = q[2] = 0.0;
	q[3] = 1.0;
}

bool
u_camera_calib_default_prior(double panel_h_m, double t[3], double q[4])
{
	if (!(panel_h_m > 0.0)) {
		return false;
	}
	t[0] = 0.0;
	t[1] = panel_h_m * 0.5 + XRT_CAMERA_POSE_BEZEL_MARGIN_M;
	t[2] = 0.0;
	identity(q);
	return true;
}

bool
u_camera_calib_placement_pose(
    uint32_t edge, double offset_mm, double margin_mm, double panel_w_m, double panel_h_m, double t[3], double q[4])
{
	const double m = XRT_CAMERA_POSE_BEZEL_MARGIN_M + (isfinite(margin_mm) ? margin_mm : 0.0) * 1e-3;
	const double o = (isfinite(offset_mm) ? offset_mm : 0.0) * 1e-3;
	switch (edge) {
	case XRT_CAMERA_PLACEMENT_EDGE_TOP:
	case XRT_CAMERA_PLACEMENT_EDGE_BOTTOM:
		if (!(panel_h_m > 0.0)) {
			return false;
		}
		t[0] = o;
		t[1] = (edge == XRT_CAMERA_PLACEMENT_EDGE_TOP ? 1.0 : -1.0) * (panel_h_m * 0.5 + m);
		break;
	case XRT_CAMERA_PLACEMENT_EDGE_LEFT:
	case XRT_CAMERA_PLACEMENT_EDGE_RIGHT:
		if (!(panel_w_m > 0.0)) {
			return false;
		}
		t[0] = (edge == XRT_CAMERA_PLACEMENT_EDGE_RIGHT ? 1.0 : -1.0) * (panel_w_m * 0.5 + m);
		t[1] = o;
		break;
	default: return false;
	}
	t[2] = 0.0;
	identity(q);
	return true;
}

static void
copy_key(char *dst, const char *src)
{
	snprintf(dst, U_CAMERA_CALIB_KEY_MAX, "%s", src != NULL ? src : "");
}

bool
u_camera_calib_resolve(const struct u_camera_calib_record *declared,
                       const struct u_camera_calib_record *calibration,
                       const struct u_camera_calib_record *hint,
                       const char *camera_key,
                       const char *screen_key,
                       double panel_w_m,
                       double panel_h_m,
                       struct u_camera_calib_record *out)
{
	memset(out, 0, sizeof(*out));
	// TODO(#2296): `declared` is the display's own camera pose (the planned
	// ADR-043 field). Nothing fills it yet; the host passes it when it does.
	if (declared != NULL) {
		*out = *declared;
		out->source = XRT_CAMERA_POSE_SOURCE_DISPLAY_DECLARED;
		if (!(out->focal_scale > 0.0)) {
			out->focal_scale = 1.0;
		}
		return true;
	}
	if (calibration != NULL) {
		*out = *calibration;
		out->source = XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION;
		if (!(out->focal_scale > 0.0)) {
			out->focal_scale = 1.0;
		}
		return true;
	}
	if (hint != NULL) {
		struct u_camera_calib_record r = *hint;
		if (u_camera_calib_placement_pose(r.edge, r.offset_mm, r.margin_mm, panel_w_m, panel_h_m, r.t_m, r.q)) {
			r.source = XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT;
			r.focal_scale = 1.0;
			r.rms_mm = 0.0;
			r.sample_count = 0;
			r.solver_mode[0] = '\0';
			*out = r;
			return true;
		}
		// The panel dimension the edge needs is unknown: fall through to the prior.
	}
	if (!u_camera_calib_default_prior(panel_h_m, out->t_m, out->q)) {
		memset(out, 0, sizeof(*out));
		return false;
	}
	copy_key(out->camera_key, camera_key);
	copy_key(out->screen_key, screen_key);
	out->source = XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR;
	out->focal_scale = 1.0;
	return true;
}

static uint64_t
fnv(uint64_t h, const void *data, size_t n)
{
	const unsigned char *p = (const unsigned char *)data;
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

uint64_t
u_camera_calib_answer_generation(const struct u_camera_calib_record *a, double viewer_iris_scale)
{
	uint64_t h = 1469598103934665603ull;
	h = fnv(h, &a->source, sizeof(a->source));
	h = fnv(h, a->t_m, sizeof(a->t_m));
	h = fnv(h, a->q, sizeof(a->q));
	h = fnv(h, &a->focal_scale, sizeof(a->focal_scale));
	h = fnv(h, &a->rms_mm, sizeof(a->rms_mm));
	h = fnv(h, &a->timestamp_unix, sizeof(a->timestamp_unix));
	h = fnv(h, &a->edge, sizeof(a->edge));
	h = fnv(h, &viewer_iris_scale, sizeof(viewer_iris_scale));
	h = fnv(h, a->camera_key, strlen(a->camera_key));
	h = fnv(h, a->screen_key, strlen(a->screen_key));
	return h == 0 ? 1 : h; // 0 reads as "no answer" to a provider
}


/*
 *
 * Record <-> JSON (the POSIX files, and the test backend on every platform).
 *
 */

static bool
json_vec(const cJSON *a, double *out, int n)
{
	if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != n) {
		return false;
	}
	for (int i = 0; i < n; i++) {
		const cJSON *v = cJSON_GetArrayItem(a, i);
		if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) {
			return false;
		}
		out[i] = v->valuedouble;
	}
	return true;
}

static double
json_num(const cJSON *o, const char *key, double fallback)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : fallback;
}

static void
normalize_q(double q[4])
{
	const double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (n > 1e-12) {
		for (int i = 0; i < 4; i++) {
			q[i] /= n;
		}
	} else {
		identity(q);
	}
}

//! A record object -> @p out. False on a malformed / unknown-kind entry (skipped, never fatal).
static bool
record_from_json(const cJSON *o, struct u_camera_calib_record *out)
{
	memset(out, 0, sizeof(*out));
	const cJSON *cam = cJSON_GetObjectItemCaseSensitive(o, "camera");
	const cJSON *scr = cJSON_GetObjectItemCaseSensitive(o, "screen");
	const cJSON *src = cJSON_GetObjectItemCaseSensitive(o, "source");
	if (!cJSON_IsString(cam) || !cJSON_IsString(scr) || !cJSON_IsString(src) ||
	    !u_camera_calib_source_parse(src->valuestring, &out->source)) {
		return false;
	}
	copy_key(out->camera_key, cam->valuestring);
	copy_key(out->screen_key, scr->valuestring);
	out->timestamp_unix = (int64_t)json_num(o, "timestamp_unix", 0.0);
	out->focal_scale = 1.0;
	identity(out->q);
	if (out->source == XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION) {
		if (!json_vec(cJSON_GetObjectItemCaseSensitive(o, "t_m"), out->t_m, 3) ||
		    !json_vec(cJSON_GetObjectItemCaseSensitive(o, "q"), out->q, 4)) {
			return false;
		}
		normalize_q(out->q);
		out->focal_scale = json_num(o, "focal_scale", 1.0);
		if (!(out->focal_scale > 0.0)) {
			out->focal_scale = 1.0;
		}
		out->rms_mm = json_num(o, "rms_mm", 0.0);
		const double sc = json_num(o, "sample_count", 0.0);
		out->sample_count = sc > 0.0 ? (uint32_t)sc : 0u;
		const cJSON *mode = cJSON_GetObjectItemCaseSensitive(o, "solver_mode");
		if (cJSON_IsString(mode)) {
			snprintf(out->solver_mode, sizeof(out->solver_mode), "%s", mode->valuestring);
		}
		return true;
	}
	if (out->source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT) {
		const cJSON *pl = cJSON_GetObjectItemCaseSensitive(o, "placement");
		const cJSON *edge = cJSON_GetObjectItemCaseSensitive(pl, "edge");
		if (!cJSON_IsString(edge) || !u_camera_calib_edge_parse(edge->valuestring, &out->edge)) {
			return false;
		}
		out->offset_mm = json_num(pl, "offset_mm", 0.0);
		out->margin_mm = json_num(pl, "margin_mm", 0.0);
		return true;
	}
	return false; // a stored record is a calibration or a hint, never a prior / declaration
}

static cJSON *
vec_to_json(const double *v, int n)
{
	cJSON *a = cJSON_CreateArray();
	for (int i = 0; i < n; i++) {
		cJSON_AddItemToArray(a, cJSON_CreateNumber(v[i]));
	}
	return a;
}

static cJSON *
record_to_json(const struct u_camera_calib_record *r)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "camera", r->camera_key);
	cJSON_AddStringToObject(o, "screen", r->screen_key);
	cJSON_AddStringToObject(o, "source", u_camera_calib_source_str(r->source));
	if (r->source == XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION) {
		cJSON_AddItemToObject(o, "t_m", vec_to_json(r->t_m, 3));
		cJSON_AddItemToObject(o, "q", vec_to_json(r->q, 4));
		cJSON_AddNumberToObject(o, "focal_scale", r->focal_scale > 0.0 ? r->focal_scale : 1.0);
		cJSON_AddNumberToObject(o, "rms_mm", r->rms_mm);
		cJSON_AddNumberToObject(o, "sample_count", r->sample_count);
		cJSON_AddStringToObject(o, "solver_mode", r->solver_mode);
	} else {
		cJSON *pl = cJSON_AddObjectToObject(o, "placement");
		cJSON_AddStringToObject(pl, "edge", u_camera_calib_edge_str(r->edge));
		cJSON_AddNumberToObject(pl, "offset_mm", r->offset_mm);
		cJSON_AddNumberToObject(pl, "margin_mm", r->margin_mm);
	}
	cJSON_AddNumberToObject(o, "timestamp_unix", (double)r->timestamp_unix);
	return o;
}


/*
 *
 * Files.
 *
 */

static char g_test_user[1024];
static char g_test_machine[1024];
static char g_test_viewer[1024];
static bool g_test_paths = false;

void
u_camera_calib_store_set_test_paths(const char *user_file, const char *machine_file, const char *viewer_file)
{
	g_test_paths = user_file != NULL || machine_file != NULL || viewer_file != NULL;
	snprintf(g_test_user, sizeof(g_test_user), "%s", user_file != NULL ? user_file : "");
	snprintf(g_test_machine, sizeof(g_test_machine), "%s", machine_file != NULL ? machine_file : "");
	snprintf(g_test_viewer, sizeof(g_test_viewer), "%s", viewer_file != NULL ? viewer_file : "");
}

static bool
json_backend(void)
{
#ifdef XRT_OS_WINDOWS
	return g_test_paths;
#else
	return true;
#endif
}

static bool
file_path(bool machine, bool viewer, char *out, size_t cap)
{
	if (g_test_paths) {
		const char *p = viewer ? g_test_viewer : machine ? g_test_machine : g_test_user;
		if (p[0] == '\0') {
			return false;
		}
		snprintf(out, cap, "%s", p);
		return true;
	}
#ifdef XRT_OS_WINDOWS
	(void)machine;
	(void)viewer;
	(void)out;
	(void)cap;
	return false;
#else
	if (machine) {
		if (viewer) {
			return false; // the iris scale is per user only
		}
		snprintf(out, cap, "%s", MACHINE_CALIB_FILE);
		return true;
	}
	const int n = u_file_get_path_in_config_dir(viewer ? VIEWER_FILE : CALIB_FILE, out, cap);
	return n > 0 && (size_t)n < cap;
#endif
}

//! mkdir -p of @p file's directory (best-effort; the open reports the failure).
static void
make_parent_dirs(const char *file)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", file);
	char *last = NULL;
	for (char *p = tmp; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			last = p;
		}
	}
	if (last == NULL || last == tmp) {
		return;
	}
	*last = '\0';
	for (char *p = tmp + 1; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			const char c = *p;
			*p = '\0';
#ifdef XRT_OS_WINDOWS
			if (!(p == tmp + 2 && tmp[1] == ':')) { // skip a bare drive "C:"
				(void)_mkdir(tmp);
			}
#else
			(void)mkdir(tmp, 0755);
#endif
			*p = c;
		}
	}
#ifdef XRT_OS_WINDOWS
	(void)_mkdir(tmp);
#else
	(void)mkdir(tmp, 0755);
#endif
}

static cJSON *
load_json(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	char *buf = NULL;
	size_t len = 0, cap = 0;
	char chunk[4096];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
		if (len + n + 1 > cap) {
			cap = (len + n + 1) * 2;
			char *nb = (char *)realloc(buf, cap);
			if (nb == NULL) {
				free(buf);
				fclose(f);
				return NULL;
			}
			buf = nb;
		}
		memcpy(buf + len, chunk, n);
		len += n;
	}
	fclose(f);
	if (buf == NULL) {
		return NULL;
	}
	buf[len] = '\0';
	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (root != NULL && !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		root = NULL;
	}
	return root;
}

static enum u_camera_calib_store_result
save_json(const char *path, cJSON *root)
{
	make_parent_dirs(path);
	char *text = cJSON_Print(root);
	if (text == NULL) {
		return U_CAMERA_CALIB_STORE_FAILED;
	}
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		const int e = errno;
		cJSON_free(text);
		return (e == EACCES || e == EPERM || e == EROFS) ? U_CAMERA_CALIB_STORE_DENIED
		                                                 : U_CAMERA_CALIB_STORE_FAILED;
	}
	const bool ok = fputs(text, f) >= 0 && fputs("\n", f) >= 0;
	cJSON_free(text);
	return (fclose(f) == 0 && ok) ? U_CAMERA_CALIB_STORE_OK : U_CAMERA_CALIB_STORE_FAILED;
}

static int
json_list_layer(bool machine, struct u_camera_calib_record *out, int max)
{
	char path[1024];
	if (max <= 0 || !file_path(machine, false, path, sizeof(path))) {
		return 0;
	}
	cJSON *root = load_json(path);
	if (root == NULL) {
		return 0;
	}
	int n = 0;
	const cJSON *it = NULL;
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "records"))
	{
		if (n < max && record_from_json(it, &out[n])) {
			out[n].machine = machine;
			n++;
		}
	}
	cJSON_Delete(root);
	return n;
}

static bool
same_identity(const struct u_camera_calib_record *r, const char *camera, const char *screen, uint32_t source)
{
	return (camera == NULL || strcmp(r->camera_key, camera) == 0) &&
	       (screen == NULL || strcmp(r->screen_key, screen) == 0) && (source == 0 || r->source == source);
}

//! Rewrite one layer's file keeping the records @p keep says yes to, then append @p add (may be NULL).
static enum u_camera_calib_store_result
json_rewrite(bool machine,
             const char *camera,
             const char *screen,
             uint32_t source,
             const struct u_camera_calib_record *add,
             int *out_removed)
{
	char path[1024];
	if (!file_path(machine, false, path, sizeof(path))) {
		return U_CAMERA_CALIB_STORE_UNSUPPORTED;
	}
	cJSON *root = load_json(path);
	if (root == NULL) {
		if (add == NULL) {
			if (out_removed != NULL) {
				*out_removed = 0;
			}
			return U_CAMERA_CALIB_STORE_OK; // nothing to remove from an absent file
		}
		root = cJSON_CreateObject();
	}
	cJSON_DeleteItemFromObjectCaseSensitive(root, "version");
	cJSON_AddNumberToObject(root, "version", CALIB_FILE_VERSION);
	cJSON *recs = cJSON_GetObjectItemCaseSensitive(root, "records");
	if (!cJSON_IsArray(recs)) {
		cJSON_DeleteItemFromObjectCaseSensitive(root, "records");
		recs = cJSON_AddArrayToObject(root, "records");
	}
	int removed = 0;
	for (int i = cJSON_GetArraySize(recs) - 1; i >= 0; i--) {
		struct u_camera_calib_record r;
		if (record_from_json(cJSON_GetArrayItem(recs, i), &r) && same_identity(&r, camera, screen, source)) {
			cJSON_DeleteItemFromArray(recs, i);
			removed++;
		}
	}
	if (add != NULL) {
		cJSON_AddItemToArray(recs, record_to_json(add));
	}
	if (out_removed != NULL) {
		*out_removed = removed;
	}
	enum u_camera_calib_store_result res = U_CAMERA_CALIB_STORE_OK;
	if (add != NULL || removed > 0) {
		res = save_json(path, root);
	}
	cJSON_Delete(root);
	return res;
}


/*
 *
 * Windows registry backend.
 *
 */

#ifdef XRT_OS_WINDOWS

#define REG_CAMERAS "Software\\DisplayXR\\Cameras"
#define REG_VIEWER "Software\\DisplayXR\\Viewer"

static const char *
reg_kind_sub(uint32_t source)
{
	return source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT ? REG_CAMERAS "\\Placement"
	                                                       : REG_CAMERAS "\\Calibration";
}

static bool
reg_get_str(HKEY k, const char *name, char *out, DWORD cap)
{
	DWORD type = 0, size = cap - 1;
	if (RegQueryValueExA(k, name, NULL, &type, (LPBYTE)out, &size) != ERROR_SUCCESS || type != REG_SZ) {
		out[0] = '\0';
		return false;
	}
	out[size < cap ? size : cap - 1] = '\0';
	return true;
}

static double
reg_get_num(HKEY k, const char *name, double fallback)
{
	char v[64];
	if (!reg_get_str(k, name, v, sizeof(v))) {
		return fallback;
	}
	char *end = NULL;
	const double d = strtod(v, &end);
	return (end != v && isfinite(d)) ? d : fallback;
}

static bool
reg_parse_csv(const char *s, double *out, int n)
{
	const char *c = s;
	for (int i = 0; i < n; i++) {
		char *end = NULL;
		out[i] = strtod(c, &end);
		if (end == c || !isfinite(out[i])) {
			return false;
		}
		c = end;
		if (i < n - 1) {
			if (*c != ',') {
				return false;
			}
			c++;
		}
	}
	return *c == '\0';
}

static bool
reg_read_record(HKEY k, uint32_t source, struct u_camera_calib_record *out)
{
	memset(out, 0, sizeof(*out));
	out->source = source;
	out->focal_scale = 1.0;
	identity(out->q);
	if (!reg_get_str(k, "Camera", out->camera_key, sizeof(out->camera_key)) ||
	    !reg_get_str(k, "Screen", out->screen_key, sizeof(out->screen_key))) {
		return false;
	}
	out->timestamp_unix = (int64_t)reg_get_num(k, "Timestamp", 0.0);
	if (source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT) {
		char e[16];
		if (!reg_get_str(k, "Edge", e, sizeof(e)) || !u_camera_calib_edge_parse(e, &out->edge)) {
			return false;
		}
		out->offset_mm = reg_get_num(k, "OffsetMm", 0.0);
		out->margin_mm = reg_get_num(k, "MarginMm", 0.0);
		return true;
	}
	char pose[256];
	double v[7];
	if (!reg_get_str(k, "Pose", pose, sizeof(pose)) || !reg_parse_csv(pose, v, 7)) {
		return false;
	}
	memcpy(out->t_m, v, sizeof(out->t_m));
	memcpy(out->q, v + 3, sizeof(out->q));
	normalize_q(out->q);
	out->focal_scale = reg_get_num(k, "FocalScale", 1.0);
	if (!(out->focal_scale > 0.0)) {
		out->focal_scale = 1.0;
	}
	out->rms_mm = reg_get_num(k, "RmsMm", 0.0);
	const double sc = reg_get_num(k, "SampleCount", 0.0);
	out->sample_count = sc > 0.0 ? (uint32_t)sc : 0u;
	reg_get_str(k, "SolverMode", out->solver_mode, sizeof(out->solver_mode));
	return true;
}

static int
reg_list_kind(HKEY root, uint32_t source, bool machine, struct u_camera_calib_record *out, int max)
{
	HKEY k;
	if (max <= 0 || RegOpenKeyExA(root, reg_kind_sub(source), 0, KEY_READ, &k) != ERROR_SUCCESS) {
		return 0;
	}
	int n = 0;
	for (DWORD i = 0; n < max; i++) {
		char name[256];
		DWORD len = sizeof(name);
		const LSTATUS rc = RegEnumKeyExA(k, i, name, &len, NULL, NULL, NULL, NULL);
		if (rc == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (rc != ERROR_SUCCESS) {
			continue;
		}
		HKEY sub;
		if (RegOpenKeyExA(k, name, 0, KEY_READ, &sub) == ERROR_SUCCESS) {
			if (reg_read_record(sub, source, &out[n])) {
				out[n].machine = machine;
				n++;
			}
			RegCloseKey(sub);
		}
	}
	RegCloseKey(k);
	return n;
}

static void
reg_subkey_name(const struct u_camera_calib_record *r, char *out, size_t cap)
{
	snprintf(out, cap, "%s@%s", r->camera_key, r->screen_key);
	for (char *p = out; *p != '\0'; p++) {
		if (*p == '\\') {
			*p = '#';
		}
	}
}

static enum u_camera_calib_store_result
reg_err(LSTATUS rc)
{
	return rc == ERROR_ACCESS_DENIED ? U_CAMERA_CALIB_STORE_DENIED : U_CAMERA_CALIB_STORE_FAILED;
}

static LSTATUS
reg_set_str(HKEY k, const char *name, const char *v)
{
	return RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)strlen(v) + 1);
}

static LSTATUS
reg_set_num(HKEY k, const char *name, double v)
{
	char b[64];
	snprintf(b, sizeof(b), "%.9g", v);
	return reg_set_str(k, name, b);
}

static int
reg_remove(HKEY root, const char *camera, const char *screen, uint32_t source, enum u_camera_calib_store_result *res)
{
	int removed = 0;
	*res = U_CAMERA_CALIB_STORE_OK;
	const uint32_t kinds[2] = {XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT};
	for (int ki = 0; ki < 2; ki++) {
		if (source != 0 && source != kinds[ki]) {
			continue;
		}
		HKEY k;
		LSTATUS rc = RegOpenKeyExA(root, reg_kind_sub(kinds[ki]), 0, KEY_READ | KEY_WRITE, &k);
		if (rc == ERROR_FILE_NOT_FOUND) {
			continue;
		}
		if (rc != ERROR_SUCCESS) {
			*res = reg_err(rc);
			return -1;
		}
		// Collect first: deleting while enumerating shifts indices.
		char victims[U_CAMERA_CALIB_MAX_RECORDS][256];
		int nv = 0;
		for (DWORD i = 0; nv < U_CAMERA_CALIB_MAX_RECORDS; i++) {
			char name[256];
			DWORD len = sizeof(name);
			rc = RegEnumKeyExA(k, i, name, &len, NULL, NULL, NULL, NULL);
			if (rc == ERROR_NO_MORE_ITEMS) {
				break;
			}
			if (rc != ERROR_SUCCESS) {
				continue;
			}
			HKEY sub;
			struct u_camera_calib_record r;
			if (RegOpenKeyExA(k, name, 0, KEY_READ, &sub) == ERROR_SUCCESS) {
				const bool ok = reg_read_record(sub, kinds[ki], &r);
				RegCloseKey(sub);
				if (ok && same_identity(&r, camera, screen, 0)) {
					snprintf(victims[nv++], 256, "%s", name);
				}
			}
		}
		for (int i = 0; i < nv; i++) {
			rc = RegDeleteKeyA(k, victims[i]);
			if (rc != ERROR_SUCCESS) {
				*res = reg_err(rc);
				RegCloseKey(k);
				return -1;
			}
			removed++;
		}
		RegCloseKey(k);
	}
	return removed;
}

static enum u_camera_calib_store_result
reg_put(HKEY root, const struct u_camera_calib_record *r)
{
	enum u_camera_calib_store_result res;
	if (reg_remove(root, r->camera_key, r->screen_key, r->source, &res) < 0) {
		return res;
	}
	char name[256], sub[512];
	reg_subkey_name(r, name, sizeof(name));
	snprintf(sub, sizeof(sub), "%s\\%s", reg_kind_sub(r->source), name);
	HKEY k;
	LSTATUS rc = RegCreateKeyExA(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
	if (rc != ERROR_SUCCESS) {
		return reg_err(rc);
	}
	rc = reg_set_str(k, "Camera", r->camera_key);
	if (rc == ERROR_SUCCESS) {
		rc = reg_set_str(k, "Screen", r->screen_key);
	}
	if (rc == ERROR_SUCCESS) {
		rc = reg_set_num(k, "Timestamp", (double)r->timestamp_unix);
	}
	if (r->source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT) {
		if (rc == ERROR_SUCCESS) {
			rc = reg_set_str(k, "Edge", u_camera_calib_edge_str(r->edge));
		}
		if (rc == ERROR_SUCCESS) {
			rc = reg_set_num(k, "OffsetMm", r->offset_mm);
		}
		if (rc == ERROR_SUCCESS) {
			rc = reg_set_num(k, "MarginMm", r->margin_mm);
		}
	} else {
		char pose[256];
		snprintf(pose, sizeof(pose), "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g", r->t_m[0], r->t_m[1], r->t_m[2],
		         r->q[0], r->q[1], r->q[2], r->q[3]);
		if (rc == ERROR_SUCCESS) {
			rc = reg_set_str(k, "Pose", pose);
		}
		if (rc == ERROR_SUCCESS) {
			rc = reg_set_num(k, "FocalScale", r->focal_scale > 0.0 ? r->focal_scale : 1.0);
		}
		if (rc == ERROR_SUCCESS) {
			rc = reg_set_num(k, "RmsMm", r->rms_mm);
		}
		if (rc == ERROR_SUCCESS) {
			rc = reg_set_num(k, "SampleCount", (double)r->sample_count);
		}
		if (rc == ERROR_SUCCESS) {
			rc = reg_set_str(k, "SolverMode", r->solver_mode);
		}
	}
	RegCloseKey(k);
	return rc == ERROR_SUCCESS ? U_CAMERA_CALIB_STORE_OK : reg_err(rc);
}

#endif // XRT_OS_WINDOWS


/*
 *
 * The store API.
 *
 */

void
u_camera_calib_store_paths(char *user, size_t user_cap, char *machine, size_t machine_cap)
{
	char p[1024];
	if (user != NULL && user_cap > 0) {
		user[0] = '\0';
		if (file_path(false, false, p, sizeof(p))) {
			snprintf(user, user_cap, "%s", p);
		}
#ifdef XRT_OS_WINDOWS
		else if (!g_test_paths) {
			snprintf(user, user_cap, "%s", "HKCU\\" REG_CAMERAS);
		}
#endif
	}
	if (machine != NULL && machine_cap > 0) {
		machine[0] = '\0';
		if (file_path(true, false, p, sizeof(p))) {
			snprintf(machine, machine_cap, "%s", p);
		}
#ifdef XRT_OS_WINDOWS
		else if (!g_test_paths) {
			snprintf(machine, machine_cap, "%s", "HKLM\\" REG_CAMERAS);
		}
#endif
	}
}

int
u_camera_calib_store_list(struct u_camera_calib_record *out, int max)
{
	if (out == NULL || max <= 0) {
		return 0;
	}
	int n = 0;
	if (json_backend()) {
		n += json_list_layer(false, out + n, max - n);
		n += json_list_layer(true, out + n, max - n);
		return n;
	}
#ifdef XRT_OS_WINDOWS
	n += reg_list_kind(HKEY_CURRENT_USER, XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, false, out + n, max - n);
	n += reg_list_kind(HKEY_CURRENT_USER, XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT, false, out + n, max - n);
	n += reg_list_kind(HKEY_LOCAL_MACHINE, XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, true, out + n, max - n);
	n += reg_list_kind(HKEY_LOCAL_MACHINE, XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT, true, out + n, max - n);
#endif
	return n;
}

bool
u_camera_calib_store_find(const char *camera_key,
                          const char *screen_key,
                          uint32_t source,
                          struct u_camera_calib_record *out)
{
	if (camera_key == NULL || camera_key[0] == '\0') {
		return false;
	}
	const bool any_screen = screen_key == NULL || screen_key[0] == '\0';
	struct u_camera_calib_record *all =
	    (struct u_camera_calib_record *)calloc(U_CAMERA_CALIB_MAX_RECORDS, sizeof(*all));
	if (all == NULL) {
		return false;
	}
	const int n = u_camera_calib_store_list(all, U_CAMERA_CALIB_MAX_RECORDS);
	int best = -1;
	for (int i = 0; i < n; i++) {
		const struct u_camera_calib_record *r = &all[i];
		if (r->source != source || strcmp(r->camera_key, camera_key) != 0 ||
		    (!any_screen && strcmp(r->screen_key, screen_key) != 0)) {
			continue;
		}
		if (best < 0) {
			best = i;
			continue;
		}
		const struct u_camera_calib_record *b = &all[best];
		// The user layer over the machine one; then (any screen) the newest.
		if ((b->machine && !r->machine) ||
		    (b->machine == r->machine && r->timestamp_unix > b->timestamp_unix)) {
			best = i;
		}
	}
	if (best >= 0) {
		*out = all[best];
	}
	free(all);
	return best >= 0;
}

enum u_camera_calib_store_result
u_camera_calib_store_put(const struct u_camera_calib_record *rec, bool machine)
{
	if (rec == NULL || rec->camera_key[0] == '\0' || rec->screen_key[0] == '\0' ||
	    (rec->source != XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION &&
	     rec->source != XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT)) {
		return U_CAMERA_CALIB_STORE_UNSUPPORTED;
	}
	struct u_camera_calib_record r = *rec;
	if (r.timestamp_unix == 0) {
		r.timestamp_unix = (int64_t)time(NULL);
	}
	if (r.source == XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION) {
		normalize_q(r.q);
	}
	if (json_backend()) {
		return json_rewrite(machine, r.camera_key, r.screen_key, r.source, &r, NULL);
	}
#ifdef XRT_OS_WINDOWS
	return reg_put(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, &r);
#else
	return U_CAMERA_CALIB_STORE_UNSUPPORTED;
#endif
}

int
u_camera_calib_store_remove(const char *camera_key,
                            const char *screen_key,
                            uint32_t source,
                            bool machine,
                            enum u_camera_calib_store_result *out_result)
{
	enum u_camera_calib_store_result res = U_CAMERA_CALIB_STORE_OK;
	int removed = 0;
	if (json_backend()) {
		res = json_rewrite(machine, camera_key, screen_key, source, NULL, &removed);
		if (res == U_CAMERA_CALIB_STORE_UNSUPPORTED) {
			res = U_CAMERA_CALIB_STORE_OK; // no store path = nothing stored
		}
	}
#ifdef XRT_OS_WINDOWS
	else {
		removed =
		    reg_remove(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, camera_key, screen_key, source, &res);
	}
#endif
	if (out_result != NULL) {
		*out_result = res;
	}
	return res == U_CAMERA_CALIB_STORE_OK ? removed : -1;
}

bool
u_camera_calib_viewer_iris_scale(double *out)
{
	*out = 1.0;
	double v = 0.0;
	if (json_backend()) {
		char path[1024];
		if (!file_path(false, true, path, sizeof(path))) {
			return false;
		}
		cJSON *root = load_json(path);
		if (root == NULL) {
			return false;
		}
		v = json_num(root, "iris_scale", 0.0);
		cJSON_Delete(root);
	}
#ifdef XRT_OS_WINDOWS
	else {
		HKEY k;
		if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_VIEWER, 0, KEY_READ, &k) != ERROR_SUCCESS) {
			return false;
		}
		v = reg_get_num(k, "IrisScale", 0.0);
		RegCloseKey(k);
	}
#endif
	if (!(v > 0.0)) {
		return false;
	}
	*out = v;
	return true;
}

enum u_camera_calib_store_result
u_camera_calib_set_viewer_iris_scale(double value)
{
	const bool clear = !(value > 0.0) || !isfinite(value);
	if (json_backend()) {
		char path[1024];
		if (!file_path(false, true, path, sizeof(path))) {
			return U_CAMERA_CALIB_STORE_UNSUPPORTED;
		}
		cJSON *root = load_json(path);
		if (root == NULL) {
			if (clear) {
				return U_CAMERA_CALIB_STORE_OK;
			}
			root = cJSON_CreateObject();
		}
		cJSON_DeleteItemFromObjectCaseSensitive(root, "iris_scale");
		if (!clear) {
			cJSON_AddNumberToObject(root, "iris_scale", value);
		}
		const enum u_camera_calib_store_result r = save_json(path, root);
		cJSON_Delete(root);
		return r;
	}
#ifdef XRT_OS_WINDOWS
	HKEY k;
	LSTATUS rc = RegCreateKeyExA(HKEY_CURRENT_USER, REG_VIEWER, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
	if (rc != ERROR_SUCCESS) {
		return reg_err(rc);
	}
	rc = clear ? RegDeleteValueA(k, "IrisScale") : reg_set_num(k, "IrisScale", value);
	RegCloseKey(k);
	return (rc == ERROR_SUCCESS || (clear && rc == ERROR_FILE_NOT_FOUND)) ? U_CAMERA_CALIB_STORE_OK : reg_err(rc);
#else
	return U_CAMERA_CALIB_STORE_UNSUPPORTED;
#endif
}
