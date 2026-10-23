// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli camera calibration` — the runtime-owned camera
 *         calibration store (where a camera sits relative to a screen),
 *         edited LOCALLY: no service, no IPC.
 *
 *   camera calibration list  [--camera K] [--screen S] [--json]
 *       Every stored record (user layer, then machine) + the per-user viewer iris scale.
 *   camera calibration show  --camera K [--screen S] [--panel-mm WxH] [--json]
 *       The answer a provider gets from `host->get_camera_pose`: display-declared >
 *       user-calibration > placement-hint > default-prior, with its source. The panel
 *       size (for a hint / the prior) comes from --panel-mm, else the screen's row in
 *       the headless status snapshot (the runtime-default screen without --screen).
 *   camera calibration set-placement --camera K --screen S --edge top|bottom|left|right
 *                      [--offset-mm X] [--margin-mm X] [--machine] [--json]
 *       Say where an uncalibrated camera sits (e.g. a webcam on the bottom bezel).
 *   camera calibration reset [--camera K] [--screen S] [--source user-calibration|placement-hint]
 *                      [--iris] [--machine] [--json]
 *       Remove matching records (no filter = every record of the layer); --iris also clears
 *       the viewer iris scale.
 *
 * Contract: docs/specs/runtime/tracking-provider-discovery.md §6.
 */

#include "cli_common.h"

#include "xrt/xrt_camera_pose.h"
#include "xrt/xrt_display_status.h"
#include "util/u_camera_calib.h"

#include "target_camera_pose.h"

#include <cjson/cJSON.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define P(...) printf(__VA_ARGS__)

static const char *
opt(int argc, const char **argv, const char *name)
{
	for (int i = 3; i + 1 < argc; i++) {
		if (strcmp(argv[i], name) == 0) {
			return argv[i + 1];
		}
	}
	return NULL;
}

static bool
num(const char *s, double *out)
{
	if (s == NULL) {
		return false;
	}
	char *end = NULL;
	const double d = strtod(s, &end);
	if (end == s || *end != '\0' || !isfinite(d)) {
		return false;
	}
	*out = d;
	return true;
}

static void
print_json(cJSON *o)
{
	char *s = cJSON_Print(o);
	if (s != NULL) {
		printf("%s\n", s);
		cJSON_free(s);
	}
	fflush(stdout);
}

static cJSON *
vec(const double *v, int n)
{
	cJSON *a = cJSON_CreateArray();
	for (int i = 0; i < n; i++) {
		cJSON_AddItemToArray(a, cJSON_CreateNumber(v[i]));
	}
	return a;
}

/*!
 * The one-line label the Dashboard shows for an answer:
 * "calibrated (rms 2.1 mm)", "uncalibrated (assumed top centre)",
 * "uncalibrated (placed on the bottom edge)", "declared by the display".
 */
static void
label_of(const struct u_camera_calib_record *r, char *out, size_t cap)
{
	switch (r->source) {
	case XRT_CAMERA_POSE_SOURCE_DISPLAY_DECLARED: snprintf(out, cap, "declared by the display"); break;
	case XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION:
		if (r->sample_count == 0 && r->rms_mm == 0.0) {
			snprintf(out, cap, "calibrated"); // committed without the solve's statistics
		} else {
			snprintf(out, cap, "calibrated (rms %.1f mm)", r->rms_mm);
		}
		break;
	case XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT:
		snprintf(out, cap, "uncalibrated (placed on the %s edge)", u_camera_calib_edge_str(r->edge));
		break;
	case XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR: snprintf(out, cap, "uncalibrated (assumed top centre)"); break;
	default: snprintf(out, cap, "unknown"); break;
	}
}

/*!
 * One record as JSON. With a panel size (> 0), a placement hint also carries
 * the pose it turns into on that panel. @p resolved marks an answer (adds
 * `label`), not a stored row (adds `layer`).
 */
static cJSON *
record_json(const struct u_camera_calib_record *r, double panel_w_m, double panel_h_m, bool resolved)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "camera", r->camera_key);
	cJSON_AddStringToObject(o, "screen", r->screen_key);
	cJSON_AddStringToObject(o, "source", u_camera_calib_source_str(r->source));
	char label[96];
	label_of(r, label, sizeof(label));
	cJSON_AddStringToObject(o, "label", label);
	if (!resolved) {
		cJSON_AddStringToObject(o, "layer", r->machine ? "machine" : "user");
	}
	double t[3], q[4];
	bool have_pose = true;
	memcpy(t, r->t_m, sizeof(t));
	memcpy(q, r->q, sizeof(q));
	if (!resolved && r->source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT) {
		have_pose =
		    u_camera_calib_placement_pose(r->edge, r->offset_mm, r->margin_mm, panel_w_m, panel_h_m, t, q);
	}
	if (have_pose) {
		cJSON_AddItemToObject(o, "t_m", vec(t, 3));
		cJSON_AddItemToObject(o, "q", vec(q, 4));
	} else {
		cJSON_AddNullToObject(o, "t_m"); // the hint's pose needs the panel size
		cJSON_AddNullToObject(o, "q");
	}
	cJSON_AddNumberToObject(o, "focal_scale", r->focal_scale > 0.0 ? r->focal_scale : 1.0);
	if (r->source == XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION) {
		cJSON_AddNumberToObject(o, "rms_mm", r->rms_mm);
		cJSON_AddNumberToObject(o, "sample_count", r->sample_count);
		cJSON_AddStringToObject(o, "solver_mode", r->solver_mode);
	} else {
		cJSON_AddNullToObject(o, "rms_mm");
	}
	if (r->source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT) {
		cJSON *pl = cJSON_AddObjectToObject(o, "placement");
		cJSON_AddStringToObject(pl, "edge", u_camera_calib_edge_str(r->edge));
		cJSON_AddNumberToObject(pl, "offset_mm", r->offset_mm);
		cJSON_AddNumberToObject(pl, "margin_mm", r->margin_mm);
	}
	if (r->timestamp_unix > 0) {
		cJSON_AddNumberToObject(o, "timestamp_unix", (double)r->timestamp_unix);
		const int64_t age = (int64_t)time(NULL) - r->timestamp_unix;
		cJSON_AddNumberToObject(o, "age_s", (double)(age > 0 ? age : 0));
	} else {
		cJSON_AddNullToObject(o, "timestamp_unix");
		cJSON_AddNullToObject(o, "age_s");
	}
	return o;
}

static void
age_str(int64_t ts, char *out, size_t cap)
{
	if (ts <= 0) {
		snprintf(out, cap, "-");
		return;
	}
	const int64_t a = (int64_t)time(NULL) - ts;
	if (a < 120) {
		snprintf(out, cap, "%llds", (long long)(a > 0 ? a : 0));
	} else if (a < 7200) {
		snprintf(out, cap, "%lldm", (long long)(a / 60));
	} else if (a < 172800) {
		snprintf(out, cap, "%lldh", (long long)(a / 3600));
	} else {
		snprintf(out, cap, "%lldd", (long long)(a / 86400));
	}
}

static bool
matches(const struct u_camera_calib_record *r, const char *camera, const char *screen)
{
	return (camera == NULL || strcmp(r->camera_key, camera) == 0) &&
	       (screen == NULL || strcmp(r->screen_key, screen) == 0);
}

//! Panel metres of @p screen_key (or the runtime-default screen when NULL) from the headless snapshot.
static bool
panel_from_snapshot(const char *screen_key, double *w_m, double *h_m, char *found_key, size_t cap)
{
	struct xrt_status_snapshot *snap = (struct xrt_status_snapshot *)calloc(1, sizeof(*snap));
	if (snap == NULL) {
		return false;
	}
	cli_status_build_headless(snap);
	bool ok = false;
	for (uint32_t i = 0; i < snap->screen_count && i < XRT_STATUS_MAX_SCREENS; i++) {
		const struct xrt_status_screen *s = &snap->screens[i];
		const bool pick = screen_key != NULL ? strcmp(s->key, screen_key) == 0 : s->roles.runtime_default;
		if (!pick) {
			continue;
		}
		double w = s->layout.width_m, h = s->layout.height_m;
		if (!(w > 0.0 && h > 0.0)) {
			w = s->physical.width_mm * 1e-3;
			h = s->physical.height_mm * 1e-3;
		}
		if (w > 0.0 && h > 0.0) {
			*w_m = w;
			*h_m = h;
			snprintf(found_key, cap, "%s", s->key);
			ok = true;
		}
		break;
	}
	free(snap);
	return ok;
}

static void
store_paths_json(cJSON *o)
{
	char user[1024], machine[1024];
	u_camera_calib_store_paths(user, sizeof(user), machine, sizeof(machine));
	cJSON *st = cJSON_AddObjectToObject(o, "store");
	cJSON_AddStringToObject(st, "user", user);
	cJSON_AddStringToObject(st, "machine", machine);
}

static void
viewer_json(cJSON *o)
{
	double iris = 1.0;
	const bool have = u_camera_calib_viewer_iris_scale(&iris);
	cJSON *v = cJSON_AddObjectToObject(o, "viewer");
	cJSON_AddNumberToObject(v, "iris_scale", iris);
	cJSON_AddStringToObject(v, "iris_scale_source", have ? "user" : "default");
}

static const char *
result_str(enum u_camera_calib_store_result r, bool machine)
{
	switch (r) {
	case U_CAMERA_CALIB_STORE_OK: return "ok";
	case U_CAMERA_CALIB_STORE_DENIED:
		return machine ? "denied: the machine layer needs an elevated terminal / root" : "denied";
	case U_CAMERA_CALIB_STORE_UNSUPPORTED: return "no store path (HOME / config dir unset?)";
	default: return "store write failed";
	}
}


/*
 *
 * Verbs.
 *
 */

static int
cmd_list(int argc, const char **argv, bool json)
{
	const char *camera = opt(argc, argv, "--camera");
	const char *screen = opt(argc, argv, "--screen");
	struct u_camera_calib_record *all =
	    (struct u_camera_calib_record *)calloc(U_CAMERA_CALIB_MAX_RECORDS, sizeof(*all));
	if (all == NULL) {
		return 1;
	}
	const int n = u_camera_calib_store_list(all, U_CAMERA_CALIB_MAX_RECORDS);
	if (json) {
		cJSON *o = cJSON_CreateObject();
		store_paths_json(o);
		viewer_json(o);
		cJSON *arr = cJSON_AddArrayToObject(o, "records");
		for (int i = 0; i < n; i++) {
			if (matches(&all[i], camera, screen)) {
				cJSON_AddItemToArray(arr, record_json(&all[i], 0.0, 0.0, false));
			}
		}
		print_json(o);
		cJSON_Delete(o);
	} else {
		char user[1024], machine[1024];
		u_camera_calib_store_paths(user, sizeof(user), machine, sizeof(machine));
		P("Camera calibration store: user %s, machine %s\n", user[0] ? user : "-", machine[0] ? machine : "-");
		double iris = 1.0;
		const bool have = u_camera_calib_viewer_iris_scale(&iris);
		P("Viewer iris scale: %.4f (%s)\n", iris, have ? "per user" : "default");
		int shown = 0;
		for (int i = 0; i < n; i++) {
			const struct u_camera_calib_record *r = &all[i];
			if (!matches(r, camera, screen)) {
				continue;
			}
			char label[96], age[16];
			label_of(r, label, sizeof(label));
			age_str(r->timestamp_unix, age, sizeof(age));
			P("  %-28s @ %-32s %-17s %-7s age %-5s %s\n", r->camera_key, r->screen_key,
			  u_camera_calib_source_str(r->source), r->machine ? "machine" : "user", age, label);
			shown++;
		}
		if (shown == 0) {
			P("  (no stored records: every camera uses the default prior, top centre of the bezel)\n");
		}
	}
	free(all);
	return 0;
}

static int
cmd_show(int argc, const char **argv, bool json)
{
	const char *camera = opt(argc, argv, "--camera");
	const char *screen = opt(argc, argv, "--screen");
	if (camera == NULL) {
		P("error: 'camera calibration show' needs --camera <key>.\n");
		return 1;
	}
	double w = 0.0, h = 0.0;
	char panel_from[160] = "unknown";
	const char *pm = opt(argc, argv, "--panel-mm");
	if (pm != NULL) {
		double pw = 0.0, ph = 0.0;
		if (sscanf(pm, "%lfx%lf", &pw, &ph) != 2 || !(pw > 0.0 && ph > 0.0)) {
			P("error: --panel-mm takes WxH in millimetres (e.g. 344x194).\n");
			return 1;
		}
		w = pw * 1e-3;
		h = ph * 1e-3;
		snprintf(panel_from, sizeof(panel_from), "--panel-mm");
	} else {
		char key[XRT_STATUS_NAME_MAX] = {0};
		if (panel_from_snapshot(screen, &w, &h, key, sizeof(key))) {
			snprintf(panel_from, sizeof(panel_from), "screen %s", key);
		}
	}
	struct u_camera_calib_record r;
	double iris = 1.0;
	const bool ok = target_camera_pose_resolve(camera, screen, (float)w, (float)h, &r, &iris);
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "ok", ok);
		if (ok) {
			cJSON *a = record_json(&r, w, h, true);
			cJSON_AddItemToObject(o, "answer", a);
		} else {
			cJSON_AddNullToObject(o, "answer");
			cJSON_AddStringToObject(o, "reason",
			                        "no stored record and the panel size is unknown (--panel-mm)");
		}
		cJSON *pn = cJSON_AddObjectToObject(o, "panel");
		cJSON_AddNumberToObject(pn, "width_m", w);
		cJSON_AddNumberToObject(pn, "height_m", h);
		cJSON_AddStringToObject(pn, "from", panel_from);
		viewer_json(o);
		print_json(o);
		cJSON_Delete(o);
	} else if (ok) {
		char label[96];
		label_of(&r, label, sizeof(label));
		P("camera '%s' on screen '%s': %s — source %s\n", camera, screen != NULL ? screen : "(served)", label,
		  u_camera_calib_source_str(r.source));
		P("\tT = (%.2f, %.2f, %.2f) mm   q = (%.6f, %.6f, %.6f, %.6f)   focal scale %.4f\n", r.t_m[0] * 1e3,
		  r.t_m[1] * 1e3, r.t_m[2] * 1e3, r.q[0], r.q[1], r.q[2], r.q[3], r.focal_scale);
		P("\tpanel %.4f x %.4f m (%s); viewer iris scale %.4f\n", w, h, panel_from, iris);
	} else {
		P("No answer: nothing stored for '%s' and the panel size is unknown (pass --panel-mm WxH).\n", camera);
	}
	return ok ? 0 : 2;
}

static int
cmd_set_placement(int argc, const char **argv, bool json)
{
	const bool machine = cli_has_flag(argc, argv, "--machine");
	const char *camera = opt(argc, argv, "--camera");
	const char *screen = opt(argc, argv, "--screen");
	const char *edge = opt(argc, argv, "--edge");
	struct u_camera_calib_record r;
	memset(&r, 0, sizeof(r));
	if (camera == NULL || screen == NULL || !u_camera_calib_edge_parse(edge, &r.edge)) {
		P("error: 'camera calibration set-placement' needs --camera <key> --screen <key> "
		  "--edge top|bottom|left|right [--offset-mm X] [--margin-mm X].\n");
		return 1;
	}
	const char *os = opt(argc, argv, "--offset-mm");
	const char *ms = opt(argc, argv, "--margin-mm");
	if ((os != NULL && !num(os, &r.offset_mm)) || (ms != NULL && !num(ms, &r.margin_mm))) {
		P("error: --offset-mm / --margin-mm must be numbers (mm).\n");
		return 1;
	}
	if (strlen(camera) >= U_CAMERA_CALIB_KEY_MAX || strlen(screen) >= U_CAMERA_CALIB_KEY_MAX) {
		P("error: keys are limited to %d bytes.\n", U_CAMERA_CALIB_KEY_MAX - 1);
		return 1;
	}
	snprintf(r.camera_key, sizeof(r.camera_key), "%s", camera);
	snprintf(r.screen_key, sizeof(r.screen_key), "%s", screen);
	r.source = XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT;
	r.focal_scale = 1.0;
	const enum u_camera_calib_store_result w = u_camera_calib_store_put(&r, machine);
	const bool ok = w == U_CAMERA_CALIB_STORE_OK;
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "ok", ok);
		struct u_camera_calib_record stored = r;
		(void)u_camera_calib_store_find(camera, screen, XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT, &stored);
		cJSON_AddItemToObject(o, "record", record_json(&stored, 0.0, 0.0, false));
		store_paths_json(o);
		if (!ok) {
			cJSON_AddStringToObject(o, "reason", result_str(w, machine));
		}
		print_json(o);
		cJSON_Delete(o);
	} else if (ok) {
		P("Camera '%s' on screen '%s': placed on the %s edge, offset %.1f mm, extra margin %.1f mm (%s "
		  "layer).\n",
		  camera, screen, u_camera_calib_edge_str(r.edge), r.offset_mm, r.margin_mm,
		  machine ? "machine" : "user");
		P("A calibration of the same camera + screen still wins over it.\n");
	} else {
		P("FAIL: %s.\n", result_str(w, machine));
	}
	return ok ? 0 : 1;
}

static int
cmd_reset(int argc, const char **argv, bool json)
{
	const bool machine = cli_has_flag(argc, argv, "--machine");
	const bool iris = cli_has_flag(argc, argv, "--iris");
	const char *camera = opt(argc, argv, "--camera");
	const char *screen = opt(argc, argv, "--screen");
	const char *ss = opt(argc, argv, "--source");
	uint32_t source = 0;
	if (ss != NULL &&
	    (!u_camera_calib_source_parse(ss, &source) ||
	     (source != XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION && source != XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT))) {
		P("error: --source is user-calibration or placement-hint.\n");
		return 1;
	}
	enum u_camera_calib_store_result res = U_CAMERA_CALIB_STORE_OK;
	const int removed = u_camera_calib_store_remove(camera, screen, source, machine, &res);
	enum u_camera_calib_store_result ires = U_CAMERA_CALIB_STORE_OK;
	if (iris && !machine) {
		ires = u_camera_calib_set_viewer_iris_scale(0.0);
	}
	const bool ok = removed >= 0 && ires == U_CAMERA_CALIB_STORE_OK;
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "ok", ok);
		cJSON_AddNumberToObject(o, "removed", removed > 0 ? removed : 0);
		cJSON_AddBoolToObject(o, "iris_cleared", iris && !machine && ires == U_CAMERA_CALIB_STORE_OK);
		cJSON_AddStringToObject(o, "layer", machine ? "machine" : "user");
		if (!ok) {
			cJSON_AddStringToObject(o, "reason", result_str(removed < 0 ? res : ires, machine));
		}
		print_json(o);
		cJSON_Delete(o);
	} else if (ok) {
		P("Removed %d record(s) from the %s layer%s.\n", removed, machine ? "machine" : "user",
		  iris && !machine ? "; viewer iris scale cleared" : "");
	} else {
		P("FAIL: %s.\n", result_str(removed < 0 ? res : ires, machine));
	}
	return ok ? 0 : 1;
}

static int
usage(void)
{
	P("usage: displayxr-cli camera calibration list [--camera K] [--screen S] [--json]\n"
	  "       camera calibration show --camera K [--screen S] [--panel-mm WxH] [--json]\n"
	  "       camera calibration set-placement --camera K --screen S --edge top|bottom|left|right\n"
	  "                                        [--offset-mm X] [--margin-mm X] [--machine] [--json]\n"
	  "       camera calibration reset [--camera K] [--screen S] [--source user-calibration|placement-hint]\n"
	  "                                [--iris] [--machine] [--json]\n"
	  "Precedence: display-declared > user-calibration > placement-hint > default-prior (top centre).\n");
	return 1;
}

int
cli_cmd_camera_calibration(int argc, const char **argv)
{
	// argv: displayxr-cli camera calibration <verb> ...
	const char *verb = argc >= 4 ? argv[3] : "list";
	const bool json = cli_has_flag(argc, argv, "--json");
	if (verb[0] == '-') {
		verb = "list"; // `camera calibration --json`
	}
	if (strcmp(verb, "list") == 0) {
		return cmd_list(argc, argv, json);
	}
	if (strcmp(verb, "show") == 0) {
		return cmd_show(argc, argv, json);
	}
	if (strcmp(verb, "set-placement") == 0) {
		return cmd_set_placement(argc, argv, json);
	}
	if (strcmp(verb, "reset") == 0) {
		return cmd_reset(argc, argv, json);
	}
	return usage();
}


/*
 *
 * status / info JSON.
 *
 */

void
cli_camera_calibration_annotate_status(cJSON *root)
{
	if (root == NULL) {
		return;
	}
	struct u_camera_calib_record *all =
	    (struct u_camera_calib_record *)calloc(U_CAMERA_CALIB_MAX_RECORDS, sizeof(*all));
	if (all == NULL) {
		return;
	}
	const int n = u_camera_calib_store_list(all, U_CAMERA_CALIB_MAX_RECORDS);
	cJSON *screen = NULL;
	cJSON_ArrayForEach(screen, cJSON_GetObjectItemCaseSensitive(root, "screens"))
	{
		const cJSON *key = cJSON_GetObjectItemCaseSensitive(screen, "key");
		const cJSON *lay = cJSON_GetObjectItemCaseSensitive(screen, "layout");
		const cJSON *wj = cJSON_GetObjectItemCaseSensitive(lay, "width_m");
		const cJSON *hj = cJSON_GetObjectItemCaseSensitive(lay, "height_m");
		const double w = cJSON_IsNumber(wj) ? wj->valuedouble : 0.0;
		const double h = cJSON_IsNumber(hj) ? hj->valuedouble : 0.0;
		cJSON *arr = cJSON_AddArrayToObject(screen, "camera_calibrations");
		if (!cJSON_IsString(key)) {
			continue;
		}
		for (int i = 0; i < n; i++) {
			if (strcmp(all[i].screen_key, key->valuestring) == 0) {
				cJSON_AddItemToArray(arr, record_json(&all[i], w, h, false));
			}
		}
	}
	cJSON *cc = cJSON_AddObjectToObject(root, "camera_calibration");
	store_paths_json(cc);
	viewer_json(cc);
	free(all);
}

void
cli_camera_calibration_annotate_info(cJSON *root)
{
	if (root == NULL) {
		return;
	}
	struct u_camera_calib_record *all =
	    (struct u_camera_calib_record *)calloc(U_CAMERA_CALIB_MAX_RECORDS, sizeof(*all));
	if (all == NULL) {
		return;
	}
	const int n = u_camera_calib_store_list(all, U_CAMERA_CALIB_MAX_RECORDS);
	cJSON *cc = cJSON_AddObjectToObject(root, "camera_calibration");
	store_paths_json(cc);
	viewer_json(cc);
	cJSON *arr = cJSON_AddArrayToObject(cc, "records");
	for (int i = 0; i < n; i++) {
		cJSON_AddItemToArray(arr, record_json(&all[i], 0.0, 0.0, false));
	}
	free(all);
}
