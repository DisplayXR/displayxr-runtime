// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `tracking calibrate` — mirror-alignment camera-pose calibration for
 *         camera-based viewer-tracking providers (the CLI half; the Dashboard
 *         owns the full-screen UI and drives these verbs).
 *
 *   tracking calibrate --screen <key> [--watch] [--json] [--provider <id>]
 *                      [--count N] [--interval-ms N]
 *       Hosts the provider headlessly in-process (the selftest pattern),
 *       starts it, and streams its samples — the camera-space block (camera
 *       key, per-eye unit direction + estimated distance) beside the
 *       display-space eyes. `--json`: one NDJSON line per sample after a
 *       "hello" line (which also carries the camera's current pose answer,
 *       the prior the Dashboard sizes its target layout from); `--watch` keeps going until Ctrl-C / stdin EOF (the
 *       Dashboard's stop) or `--count` samples; without it, one sample.
 *
 *   tracking calibrate solve --in <file|-> [--mode a|b|c] [--eye mid|left|right]
 *                      [--focal] [--reject-mm X] [--tz-mm X] [--json]
 *       Runs the solver (math/m_mirror_calib.h) on {dot_m, sample} pairs.
 *
 *   tracking calibrate apply --screen <key> --camera <key> --pose tx,ty,tz,qx,qy,qz,qw
 *                      [--depth-scale X] [--focal-scale X] [--rms-mm X] [--samples N]
 *                      [--mode a|b|c] [--machine] [--json]
 *       Commits the result into the runtime's camera calibration store, keyed
 *       by (camera, screen) and shared by every provider of every class that
 *       reads that camera (util/u_camera_calib.h). --depth-scale is the solved
 *       iris scale: a per-USER value, stored apart. --provider / --slot (the
 *       #2303 shape) are accepted and ignored.
 *
 * Contract: docs/specs/runtime/tracking-provider-discovery.md, "Camera pose calibration".
 */

#include "cli_common.h"
#include "cli_query.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_tracking_plugin.h"

#include "math/m_mirror_calib.h"
#include "os/os_threading.h"
#include "util/u_camera_calib.h"
#include "os/os_time.h"
#include "util/u_time.h"

#include "target_camera_pose.h"
#include "target_component_loader.h"
#include "target_tracking_plugin_loader.h"

#include <cjson/cJSON.h>

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#define cal_isatty _isatty
#else
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#define cal_isatty isatty
#endif

#define P(...) printf(__VA_ARGS__)
#define CAL_MAX_PROVIDERS 16

//! Value of `--name <v>` anywhere in argv[2..], or NULL.
static const char *
opt_value(int argc, const char **argv, const char *name)
{
	for (int i = 2; i + 1 < argc; i++) {
		if (strcmp(argv[i], name) == 0) {
			return argv[i + 1];
		}
	}
	return NULL;
}

static bool
parse_double(const char *s, double *out)
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
print_json(cJSON *root, bool compact)
{
	char *s = compact ? cJSON_PrintUnformatted(root) : cJSON_Print(root);
	if (s != NULL) {
		printf("%s\n", s);
		cJSON_free(s);
	}
	fflush(stdout);
}

static cJSON *
vec_json(const double *v, int n)
{
	cJSON *a = cJSON_CreateArray();
	for (int i = 0; i < n; i++) {
		cJSON_AddItemToArray(a, cJSON_CreateNumber(v[i]));
	}
	return a;
}

static cJSON *
fvec_json(const float *v, int n)
{
	double d[4];
	for (int i = 0; i < n; i++) {
		d[i] = (double)v[i];
	}
	return vec_json(d, n);
}


/*
 *
 * Watch: host the provider and stream its samples.
 *
 */

static volatile sig_atomic_t g_stop = 0;

static void
on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

//! True once a piped stdin reached EOF (the parent closed it: the Dashboard's stop).
static bool
stdin_closed(void)
{
#ifdef XRT_OS_WINDOWS
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	if (h == NULL || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_PIPE) {
		return false;
	}
	DWORD avail = 0;
	if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
		return GetLastError() == ERROR_BROKEN_PIPE;
	}
	while (avail > 0) {
		char buf[256];
		DWORD got = 0;
		if (!ReadFile(h, buf, avail < sizeof(buf) ? avail : (DWORD)sizeof(buf), &got, NULL) || got == 0) {
			return true;
		}
		avail -= got;
	}
	return false;
#else
	// Only a pipe / socket parent can "close" us: a console, /dev/null or a file never does.
	struct stat st;
	if (cal_isatty(STDIN_FILENO) || fstat(STDIN_FILENO, &st) != 0 ||
	    !(S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) {
		return false;
	}
	struct pollfd p = {STDIN_FILENO, POLLIN, 0};
	if (poll(&p, 1, 0) <= 0) {
		return false;
	}
	if ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 && (p.revents & POLLIN) == 0) {
		return true;
	}
	if ((p.revents & POLLIN) != 0) {
		char buf[256];
		return read(STDIN_FILENO, buf, sizeof(buf)) == 0;
	}
	return false;
#endif
}

static struct
{
	struct os_mutex mutex;
	uint64_t seq;
	struct xrt_tracking_viewer_sample sample;
} g_latest;

static void
watch_sink_push(const struct xrt_tracking_sink *sink, const struct xrt_tracking_viewer_sample *s)
{
	(void)sink;
	if (s == NULL) {
		return;
	}
	os_mutex_lock(&g_latest.mutex);
	memset(&g_latest.sample, 0, sizeof(g_latest.sample));
	size_t n = s->struct_size < sizeof(g_latest.sample) ? s->struct_size : sizeof(g_latest.sample);
	if (n < offsetof(struct xrt_tracking_viewer_sample, capture_ns)) {
		n = 0;
	}
	memcpy(&g_latest.sample, s, n);
	// The camera block is only meaningful when the provider's struct reaches it.
	if (n < offsetof(struct xrt_tracking_viewer_sample, camera) + sizeof(s->camera)) {
		g_latest.sample.flags &= ~XRT_TRACKING_SAMPLE_CAMERA_VALID;
	}
	g_latest.seq++;
	os_mutex_unlock(&g_latest.mutex);
}

static void
emit_error(bool json, const char *code, const char *reason)
{
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "type", "error");
		cJSON_AddStringToObject(o, "error", code);
		cJSON_AddStringToObject(o, "reason", reason);
		print_json(o, true);
		cJSON_Delete(o);
	} else {
		P("error: %s\n", reason);
	}
}

static void
emit_sample(bool json, uint64_t seq, const struct xrt_tracking_viewer_sample *s)
{
	const bool cam = (s->flags & XRT_TRACKING_SAMPLE_CAMERA_VALID) != 0;
	if (!json) {
		P("#%llu t=%lld %s  L(%.4f %.4f %.4f) R(%.4f %.4f %.4f)", (unsigned long long)seq,
		  (long long)s->capture_ns, (s->flags & XRT_TRACKING_SAMPLE_TRACKING) ? "tracking" : "lost",
		  s->left_eye[0], s->left_eye[1], s->left_eye[2], s->right_eye[0], s->right_eye[1], s->right_eye[2]);
		if (cam) {
			P("  cam '%s' Ldir(%.4f %.4f %.4f)@%.4f Rdir(%.4f %.4f %.4f)@%.4f", s->camera.camera_key,
			  s->camera.left_dir[0], s->camera.left_dir[1], s->camera.left_dir[2],
			  s->camera.left_distance_m, s->camera.right_dir[0], s->camera.right_dir[1],
			  s->camera.right_dir[2], s->camera.right_distance_m);
		} else {
			P("  (no camera-space block)");
		}
		P("\n");
		fflush(stdout);
		return;
	}
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "type", "sample");
	cJSON_AddNumberToObject(o, "seq", (double)seq);
	cJSON_AddNumberToObject(o, "capture_ns", (double)s->capture_ns);
	cJSON_AddBoolToObject(o, "tracking", (s->flags & XRT_TRACKING_SAMPLE_TRACKING) != 0);
	cJSON_AddNumberToObject(o, "confidence", s->confidence);
	cJSON *d = cJSON_AddObjectToObject(o, "display");
	cJSON_AddItemToObject(d, "left_eye", fvec_json(s->left_eye, 3));
	cJSON_AddItemToObject(d, "right_eye", fvec_json(s->right_eye, 3));
	if (cam) {
		cJSON *c = cJSON_AddObjectToObject(o, "camera");
		char key[XRT_TRACKING_CAMERA_KEY_MAX];
		snprintf(key, sizeof(key), "%.*s", (int)sizeof(key) - 1, s->camera.camera_key);
		cJSON_AddStringToObject(c, "key", key);
		cJSON_AddItemToObject(c, "left_dir", fvec_json(s->camera.left_dir, 3));
		cJSON_AddNumberToObject(c, "left_distance_m", s->camera.left_distance_m);
		cJSON_AddItemToObject(c, "right_dir", fvec_json(s->camera.right_dir, 3));
		cJSON_AddNumberToObject(c, "right_distance_m", s->camera.right_distance_m);
	} else {
		cJSON_AddNullToObject(o, "camera");
	}
	print_json(o, true);
	cJSON_Delete(o);
}

static int
cmd_watch(int argc, const char **argv)
{
	const bool json = cli_has_flag(argc, argv, "--json");
	const bool watch = cli_has_flag(argc, argv, "--watch");
	const char *screen = opt_value(argc, argv, "--screen");
	const char *want = opt_value(argc, argv, "--provider");
	long count = watch ? 0 : 1;
	const char *cs = opt_value(argc, argv, "--count");
	if (cs != NULL) {
		count = strtol(cs, NULL, 10);
	}
	long interval_ms = 0;
	const char *is = opt_value(argc, argv, "--interval-ms");
	if (is != NULL) {
		interval_ms = strtol(is, NULL, 10);
	}
	if (screen == NULL) {
		P("error: 'tracking calibrate' needs --screen <key> (displayxr-cli status / displays --claims).\n");
		return 1;
	}

	// Headless host, as `selftest` does: discovery + display info + the viewer arbitration.
	struct cli_query_result *r = calloc(1, sizeof(*r));
	if (r == NULL) {
		return 1;
	}
	struct cli_query_handles h;
	memset(&h, 0, sizeof(h));
	cli_query_fill(r, &h, NULL);
	if (r->display_info_ok) {
		struct xrt_tracking_host_display_geometry geo = {0};
		geo.struct_size = (uint32_t)sizeof(geo);
		geo.display_width_m = r->display_info.display_width_m;
		geo.display_height_m = r->display_info.display_height_m;
		geo.nominal_viewer_x_m = r->display_info.nominal_viewer_x_m;
		geo.nominal_viewer_y_m = r->display_info.nominal_viewer_y_m;
		geo.nominal_viewer_z_m = r->display_info.nominal_viewer_z_m;
		target_tracking_plugin_set_display_geometry(&geo);
	}

	int rc = 0;
	const int n = target_tracking_plugin_get_count();
	int index = -1;
	for (int i = 0; i < n; i++) {
		if (want == NULL || strcmp(target_tracking_plugin_get_row_id(i), want) == 0) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		char why[256];
		if (want != NULL) {
			snprintf(why, sizeof(why),
			         "provider '%s' is not resident (not registered, disabled, or its probe "
			         "found no usable source) - see 'displayxr-cli tracking list'",
			         want);
		} else {
			snprintf(why, sizeof(why),
			         "no viewer-tracking provider is resident (none registered + enabled, or "
			         "every probe declined) - see 'displayxr-cli tracking list'");
		}
		emit_error(json, "no_provider", why);
		cli_query_teardown(&h);
		free(r);
		return 3;
	}

	const struct xrt_tracking_plugin_iface *iface = target_tracking_plugin_get_iface(index);
	struct xrt_tracking_plugin_instance *inst = target_tracking_plugin_get_instance(index);
	struct xrt_tracking_provider_info info;
	memset(&info, 0, sizeof(info));
	info.struct_size = (uint32_t)sizeof(info);
	if (iface->describe != NULL) {
		(void)iface->describe(inst, &info);
	}
	const bool camera_space = (info.tracks & XRT_TRACKING_TRACKS_CAMERA_SPACE) != 0;
	const char *row_id = target_tracking_plugin_get_row_id(index);

	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "type", "hello");
		cJSON_AddStringToObject(o, "screen", screen);
		cJSON_AddStringToObject(o, "provider", row_id);
		cJSON_AddStringToObject(o, "source", info.source);
		cJSON_AddStringToObject(o, "camera_key", info.camera_key);
		cJSON_AddBoolToObject(o, "tracks_camera_space", camera_space);
		cJSON_AddStringToObject(o, "viewer_source", r->tracking_viewer_source);
		cJSON_AddStringToObject(o, "viewer_reason", r->tracking_viewer_reason);
		if (r->display_info_ok) {
			cJSON *d = cJSON_AddObjectToObject(o, "display");
			cJSON_AddNumberToObject(d, "width_m", r->display_info.display_width_m);
			cJSON_AddNumberToObject(d, "height_m", r->display_info.display_height_m);
			cJSON_AddNumberToObject(d, "pixel_width", r->display_info.display_pixel_width);
			cJSON_AddNumberToObject(d, "pixel_height", r->display_info.display_pixel_height);
		} else {
			cJSON_AddNullToObject(o, "display");
		}
		// The pose the camera has now (display-declared > user-calibration > placement hint >
		// default prior): the prior the Dashboard lays its targets out from, so each dot keeps
		// the face inside the camera's view.
		struct u_camera_calib_record pose;
		const float pw = r->display_info_ok ? (float)r->display_info.display_width_m : 0.0f;
		const float ph = r->display_info_ok ? (float)r->display_info.display_height_m : 0.0f;
		if (info.camera_key[0] != '\0' &&
		    target_camera_pose_resolve(info.camera_key, screen, pw, ph, &pose, NULL)) {
			cJSON *cp = cJSON_AddObjectToObject(o, "camera_pose");
			cJSON_AddItemToObject(cp, "t_m", vec_json(pose.t_m, 3));
			cJSON_AddItemToObject(cp, "q", vec_json(pose.q, 4));
			cJSON_AddStringToObject(cp, "source", u_camera_calib_source_str(pose.source));
		} else {
			cJSON_AddNullToObject(o, "camera_pose");
		}
		cJSON_AddStringToObject(o, "note",
		                        "providers serve the primary screen only in this release; --screen is echoed, "
		                        "not routed");
		print_json(o, true);
		cJSON_Delete(o);
	} else {
		P(" :: tracking calibrate — provider '%s' (%s), camera '%s'%s, screen %s\n", row_id, info.source,
		  info.camera_key, camera_space ? "" : " — NO camera-space samples (nothing to calibrate)", screen);
		P("    viewer source on the primary screen: %s%s%s\n", r->tracking_viewer_source,
		  r->tracking_viewer_reason[0] ? " — " : "", r->tracking_viewer_reason);
	}

	os_mutex_init(&g_latest.mutex);
	g_latest.seq = 0;
	struct xrt_tracking_sink sink;
	memset(&sink, 0, sizeof(sink));
	sink.struct_size = (uint32_t)sizeof(sink);
	sink.push = watch_sink_push;
	const xrt_result_t xret = iface->start(inst, &sink);
	if (xret != XRT_SUCCESS) {
		char why[128];
		snprintf(why, sizeof(why), "provider '%s' failed to start (xrt_result %d)", row_id, (int)xret);
		emit_error(json, "start_failed", why);
		os_mutex_destroy(&g_latest.mutex);
		cli_query_teardown(&h);
		free(r);
		return 3;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	uint64_t last_seq = 0;
	uint64_t last_emit_ns = 0;
	long emitted = 0;
	const uint64_t deadline_ns = os_monotonic_get_ns() + (uint64_t)5 * U_TIME_1S_IN_NS; // first sample
	while (!g_stop) {
		struct xrt_tracking_viewer_sample s;
		uint64_t seq;
		os_mutex_lock(&g_latest.mutex);
		seq = g_latest.seq;
		s = g_latest.sample;
		os_mutex_unlock(&g_latest.mutex);
		const uint64_t now = os_monotonic_get_ns();
		if (seq != last_seq &&
		    (interval_ms <= 0 || now - last_emit_ns >= (uint64_t)interval_ms * U_TIME_1MS_IN_NS)) {
			last_seq = seq;
			last_emit_ns = now;
			emit_sample(json, seq, &s);
			emitted++;
			if (count > 0 && emitted >= count) {
				break;
			}
		}
		if (seq == 0 && now > deadline_ns) {
			emit_error(json, "no_samples", "the provider started but pushed no sample within 5 s");
			rc = 3;
			break;
		}
		if (stdin_closed()) {
			break;
		}
		os_nanosleep(5 * U_TIME_1MS_IN_NS);
	}

	iface->stop(inst);
	os_mutex_destroy(&g_latest.mutex);
	cli_query_teardown(&h);
	free(r);
	return rc;
}


/*
 *
 * Solve.
 *
 */

static char *
read_all(const char *path)
{
	FILE *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	while (buf != NULL) {
		const size_t got = fread(buf + len, 1, cap - len - 1, f);
		len += got;
		if (got == 0) {
			break;
		}
		if (len + 1 >= cap) {
			char *nb = realloc(buf, cap * 2);
			if (nb == NULL) {
				free(buf);
				buf = NULL;
				break;
			}
			buf = nb;
			cap *= 2;
		}
	}
	if (f != stdin) {
		fclose(f);
	}
	if (buf != NULL) {
		buf[len] = '\0';
	}
	return buf;
}

static bool
json_vec(const cJSON *obj, const char *name, double *out, int n)
{
	const cJSON *a = cJSON_GetObjectItemCaseSensitive(obj, name);
	if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != n) {
		return false;
	}
	for (int i = 0; i < n; i++) {
		const cJSON *v = cJSON_GetArrayItem(a, i);
		if (!cJSON_IsNumber(v)) {
			return false;
		}
		out[i] = v->valuedouble;
	}
	return true;
}

static double
json_num(const cJSON *obj, const char *name, double fallback)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
	return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

/*!
 * One input pair into a solver sample. Accepted shapes:
 *   {"dot_m":[x,y], "sample": <a `calibrate --watch --json` sample line>}
 *   {"dot_m":[x,y], "sample": <its "camera" object>}       (or "camera": {...})
 *   {"dot_m":[x,y], "dir":[x,y,z], "distance_m": d}        (an already-chosen eye point)
 */
static bool
parse_pair(const cJSON *item,
           enum m_mirror_calib_eye eye,
           struct m_mirror_calib_sample *out,
           char *camera_key,
           size_t key_cap,
           char *why,
           size_t why_cap)
{
	memset(out, 0, sizeof(*out));
	if (!json_vec(item, "dot_m", out->dot_m, 2)) {
		snprintf(why, why_cap, "missing \"dot_m\": [x, y] (metres, display space)");
		return false;
	}
	if (json_vec(item, "dir", out->dir, 3)) {
		out->est_distance_m = json_num(item, "distance_m", 0.0);
		return true;
	}
	const cJSON *cam = cJSON_GetObjectItemCaseSensitive(item, "camera");
	const cJSON *smp = cJSON_GetObjectItemCaseSensitive(item, "sample");
	if (cJSON_IsObject(smp)) {
		const cJSON *inner = cJSON_GetObjectItemCaseSensitive(smp, "camera");
		cam = cJSON_IsObject(inner) ? inner : smp;
	}
	if (!cJSON_IsObject(cam)) {
		snprintf(why, why_cap,
		         "no camera-space block (\"sample\".\"camera\") — the provider reports no "
		         "camera-space samples");
		return false;
	}
	double l[3], r[3];
	if (!json_vec(cam, "left_dir", l, 3) || !json_vec(cam, "right_dir", r, 3)) {
		snprintf(why, why_cap, "camera block lacks left_dir / right_dir");
		return false;
	}
	const float lf[3] = {(float)l[0], (float)l[1], (float)l[2]};
	const float rf[3] = {(float)r[0], (float)r[1], (float)r[2]};
	const float ld = (float)json_num(cam, "left_distance_m", 0.0);
	const float rd = (float)json_num(cam, "right_distance_m", 0.0);
	if (!m_mirror_calib_eye_point(eye, lf, ld, rf, rd, out->dir, &out->est_distance_m)) {
		snprintf(why, why_cap, "zero eye direction");
		return false;
	}
	const cJSON *k = cJSON_GetObjectItemCaseSensitive(cam, "key");
	if (cJSON_IsString(k) && camera_key[0] == '\0') {
		snprintf(camera_key, key_cap, "%s", k->valuestring);
	}
	return true;
}

static int
cmd_solve(int argc, const char **argv)
{
	const bool json = cli_has_flag(argc, argv, "--json");
	const char *in = opt_value(argc, argv, "--in");
	if (in == NULL) {
		P("error: 'tracking calibrate solve' needs --in <file|->.\n");
		return 1;
	}
	char *text = read_all(in);
	if (text == NULL) {
		P("error: cannot read '%s'.\n", in);
		return 1;
	}
	cJSON *root = cJSON_Parse(text);
	free(text);
	if (root == NULL) {
		P("error: '%s' is not JSON.\n", in);
		return 1;
	}

	struct m_mirror_calib_options opt;
	m_mirror_calib_options_init(&opt);
	enum m_mirror_calib_eye eye = M_MIRROR_CALIB_EYE_MID;
	char camera_key[XRT_TRACKING_CAMERA_KEY_MAX] = {0};
	char screen[256] = {0};
	const cJSON *list = root;
	if (cJSON_IsObject(root)) {
		const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "mode");
		if (cJSON_IsString(m)) {
			(void)m_mirror_calib_parse_mode(m->valuestring, &opt.mode);
		}
		const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "eye");
		if (cJSON_IsString(e)) {
			(void)m_mirror_calib_parse_eye(e->valuestring, &eye);
		}
		const cJSON *ck = cJSON_GetObjectItemCaseSensitive(root, "camera_key");
		if (cJSON_IsString(ck)) {
			snprintf(camera_key, sizeof(camera_key), "%s", ck->valuestring);
		}
		const cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "screen");
		if (cJSON_IsString(sc)) {
			snprintf(screen, sizeof(screen), "%s", sc->valuestring);
		}
		list = cJSON_GetObjectItemCaseSensitive(root, "samples");
	}
	// Flags override the file.
	const char *ms = opt_value(argc, argv, "--mode");
	if (ms != NULL && !m_mirror_calib_parse_mode(ms, &opt.mode)) {
		P("error: --mode must be a, b or c.\n");
		cJSON_Delete(root);
		return 1;
	}
	const char *es = opt_value(argc, argv, "--eye");
	if (es != NULL && !m_mirror_calib_parse_eye(es, &eye)) {
		P("error: --eye must be mid, left or right.\n");
		cJSON_Delete(root);
		return 1;
	}
	opt.solve_focal_scale = cli_has_flag(argc, argv, "--focal");
	double v;
	if (parse_double(opt_value(argc, argv, "--reject-mm"), &v)) {
		opt.reject_threshold_m = v / 1000.0;
	}
	if (parse_double(opt_value(argc, argv, "--tz-mm"), &v)) {
		opt.assumed_tz_m = v / 1000.0;
	}
	if (!cJSON_IsArray(list)) {
		P("error: expected an array of {dot_m, sample} (or {\"samples\": [...]}).\n");
		cJSON_Delete(root);
		return 1;
	}
	const int n = cJSON_GetArraySize(list);
	if (n > M_MIRROR_CALIB_MAX_SAMPLES) {
		P("error: %d samples; at most %d per solve.\n", n, M_MIRROR_CALIB_MAX_SAMPLES);
		cJSON_Delete(root);
		return 1;
	}
	struct m_mirror_calib_sample samples[M_MIRROR_CALIB_MAX_SAMPLES];
	for (int i = 0; i < n; i++) {
		char why[160];
		if (!parse_pair(cJSON_GetArrayItem(list, i), eye, &samples[i], camera_key, sizeof(camera_key), why,
		                sizeof(why))) {
			if (json) {
				cJSON *o = cJSON_CreateObject();
				cJSON_AddBoolToObject(o, "ok", false);
				cJSON_AddStringToObject(o, "status", "invalid_input");
				char reason[200];
				snprintf(reason, sizeof(reason), "sample %d: %s", i, why);
				cJSON_AddStringToObject(o, "reason", reason);
				print_json(o, false);
				cJSON_Delete(o);
			} else {
				P("error: sample %d: %s\n", i, why);
			}
			cJSON_Delete(root);
			return 1;
		}
	}
	cJSON_Delete(root);

	struct m_mirror_calib_result *res = calloc(1, sizeof(*res));
	if (res == NULL) {
		return 1;
	}
	const bool ok = m_mirror_calib_solve(samples, n, &opt, res);
	char setting[160];
	snprintf(setting, sizeof(setting), "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f", res->t_m[0], res->t_m[1], res->t_m[2],
	         res->q[0], res->q[1], res->q[2], res->q[3]);
	const char *eye_s = eye == M_MIRROR_CALIB_EYE_LEFT ? "left" : eye == M_MIRROR_CALIB_EYE_RIGHT ? "right" : "mid";

	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "ok", ok);
		cJSON_AddStringToObject(o, "status", m_mirror_calib_status_str(res->status));
		cJSON_AddStringToObject(o, "reason", res->reason);
		cJSON_AddStringToObject(o, "mode", m_mirror_calib_mode_str(opt.mode));
		cJSON_AddStringToObject(o, "eye", eye_s);
		cJSON_AddStringToObject(o, "camera_key", camera_key);
		cJSON_AddStringToObject(o, "screen", screen);
		if (ok) {
			cJSON *p = cJSON_AddObjectToObject(o, "pose");
			cJSON_AddItemToObject(p, "t_m", vec_json(res->t_m, 3));
			cJSON_AddItemToObject(p, "q", vec_json(res->q, 4));
			cJSON_AddNumberToObject(p, "pitch_deg", res->pitch_deg);
			cJSON_AddNumberToObject(p, "yaw_deg", res->yaw_deg);
			cJSON_AddNumberToObject(p, "roll_deg", res->roll_deg);
			cJSON_AddNumberToObject(p, "focal_scale", res->focal_scale);
			cJSON_AddBoolToObject(p, "tz_observable", false);
			cJSON_AddStringToObject(o, "setting", setting);
			cJSON_AddNumberToObject(o, "rms_mm", res->rms_m * 1000.0);
			cJSON_AddNumberToObject(o, "max_mm", res->max_m * 1000.0);
			cJSON_AddNumberToObject(o, "iris_scale", res->iris_scale);
		} else {
			cJSON_AddNullToObject(o, "pose");
		}
		cJSON_AddNumberToObject(o, "params", res->params);
		cJSON_AddNumberToObject(o, "rank", res->rank);
		cJSON_AddNumberToObject(o, "condition", isfinite(res->condition) ? res->condition : -1.0);
		cJSON_AddBoolToObject(o, "ill_conditioned", res->ill_conditioned);
		cJSON_AddNumberToObject(o, "iterations", res->iterations);
		cJSON_AddNumberToObject(o, "sample_count", res->sample_count);
		cJSON_AddNumberToObject(o, "used_count", res->used_count);
		cJSON_AddNumberToObject(o, "distinct_dots", res->distinct_dots);
		cJSON *arr = cJSON_AddArrayToObject(o, "samples");
		for (int i = 0; ok && i < n; i++) {
			cJSON *s = cJSON_CreateObject();
			cJSON_AddNumberToObject(s, "index", i);
			cJSON_AddItemToObject(s, "dot_m", vec_json(samples[i].dot_m, 2));
			cJSON_AddNumberToObject(s, "residual_mm", res->residual_m[i] * 1000.0);
			cJSON_AddNumberToObject(s, "distance_m", res->distance_m[i]);
			cJSON_AddNumberToObject(s, "estimated_distance_m", samples[i].est_distance_m);
			cJSON_AddBoolToObject(s, "rejected", res->rejected[i]);
			cJSON_AddItemToArray(arr, s);
		}
		print_json(o, false);
		cJSON_Delete(o);
	} else if (ok) {
		P(" :: camera pose (mode %s, eye %s, %d/%d samples, %d dots)\n", m_mirror_calib_mode_str(opt.mode),
		  eye_s, res->used_count, res->sample_count, res->distinct_dots);
		P("\tT  = (%.2f, %.2f, %.2f) mm   (T_z assumed: not observable from the mirror)\n", res->t_m[0] * 1e3,
		  res->t_m[1] * 1e3, res->t_m[2] * 1e3);
		P("\tR  = pitch %.3f, yaw %.3f, roll %.3f deg;  q = (%.6f, %.6f, %.6f, %.6f)\n", res->pitch_deg,
		  res->yaw_deg, res->roll_deg, res->q[0], res->q[1], res->q[2], res->q[3]);
		P("\tfocal scale %.4f   iris scale %.4f\n", res->focal_scale, res->iris_scale);
		P("\tRMS %.2f mm, max %.2f mm; rank %d/%d, condition %.3g%s\n", res->rms_m * 1e3, res->max_m * 1e3,
		  res->rank, res->params, res->condition, res->ill_conditioned ? " (ILL-CONDITIONED)" : "");
		for (int i = 0; i < n; i++) {
			P("\t  #%-2d dot (%7.2f, %7.2f) mm  residual %6.2f mm  distance %.3f m (est %.3f)%s\n", i,
			  samples[i].dot_m[0] * 1e3, samples[i].dot_m[1] * 1e3, res->residual_m[i] * 1e3,
			  res->distance_m[i], samples[i].est_distance_m, res->rejected[i] ? "  REJECTED" : "");
		}
		P("\tsetting: %s\n", setting);
		P("\tcommit:  displayxr-cli tracking calibrate apply --screen %s --camera %s --pose %s --rms-mm %.3f "
		  "--samples %d --mode %s --depth-scale %.4f\n",
		  screen[0] ? screen : "<key>", camera_key[0] ? camera_key : "<key>", setting, res->rms_m * 1e3,
		  res->used_count, m_mirror_calib_mode_str(opt.mode), res->iris_scale);
	} else {
		P("refused (%s): %s\n", m_mirror_calib_status_str(res->status), res->reason);
	}
	free(res);
	return ok ? 0 : 2;
}


/*
 *
 * Apply: commit into the runtime's camera calibration store.
 *
 */

//! Parse "tx,ty,tz,qx,qy,qz,qw" into t / q; false on anything else (or a zero quaternion).
static bool
pose_parse(const char *s, double t[3], double q[4])
{
	if (s == NULL) {
		return false;
	}
	const char *c = s;
	double v[7];
	double q2 = 0.0;
	for (int i = 0; i < 7; i++) {
		char *end = NULL;
		v[i] = strtod(c, &end);
		if (end == c || !isfinite(v[i])) {
			return false;
		}
		if (i >= 3) {
			q2 += v[i] * v[i];
		}
		c = end;
		if (i < 6) {
			if (*c != ',') {
				return false;
			}
			c++;
		}
	}
	if (*c != '\0' || !(q2 > 1e-9)) {
		return false;
	}
	memcpy(t, v, 3 * sizeof(double));
	memcpy(q, v + 3, 4 * sizeof(double));
	return true;
}

static bool
pose_valid(const char *s)
{
	double t[3], q[4];
	return pose_parse(s, t, q);
}

static const char *
store_result_str(enum u_camera_calib_store_result w, bool machine)
{
	switch (w) {
	case U_CAMERA_CALIB_STORE_OK: return "ok";
	case U_CAMERA_CALIB_STORE_DENIED:
		return machine ? "denied: the machine layer needs an elevated terminal / root" : "denied";
	case U_CAMERA_CALIB_STORE_UNSUPPORTED: return "no store path (HOME / config dir unset?)";
	default: return "store write failed";
	}
}

static int
cmd_apply(int argc, const char **argv)
{
	const bool json = cli_has_flag(argc, argv, "--json");
	const bool machine = cli_has_flag(argc, argv, "--machine");
	const char *screen = opt_value(argc, argv, "--screen");
	const char *camera = opt_value(argc, argv, "--camera");
	const char *pose = opt_value(argc, argv, "--pose");
	if (pose == NULL && argc > 4 && argv[argc - 1][0] != '-' && pose_valid(argv[argc - 1])) {
		pose = argv[argc - 1]; // positional, as the spec's "apply --screen <k> --camera <k> <pose>"
	}
	struct u_camera_calib_record rec;
	memset(&rec, 0, sizeof(rec));
	if (screen == NULL || camera == NULL || !pose_parse(pose, rec.t_m, rec.q)) {
		P("error: 'tracking calibrate apply' needs --screen <key> --camera <key> --pose tx,ty,tz,qx,qy,qz,qw\n"
		  "       (metres, display space; the \"setting\" field of 'calibrate solve --json').\n");
		return 1;
	}
	if (strlen(camera) >= U_CAMERA_CALIB_KEY_MAX || strlen(screen) >= U_CAMERA_CALIB_KEY_MAX) {
		P("error: --camera / --screen keys are limited to %d bytes.\n", U_CAMERA_CALIB_KEY_MAX - 1);
		return 1;
	}
	// #2303 compatibility: the Dashboard still passes --provider (and may pass --slot). The pose is
	// no longer a provider's setting — it belongs to the camera + screen — so both are accepted and ignored.
	const char *provider = opt_value(argc, argv, "--provider");
	char note[256] = {0};
	if (provider != NULL || opt_value(argc, argv, "--slot") != NULL) {
		snprintf(note, sizeof(note),
		         "--provider / --slot ignored: the camera pose is stored per camera + screen, shared by every "
		         "provider");
		fprintf(stderr, "note: %s.\n", note);
	}
	double depth_scale = 0.0, focal_scale = 1.0, rms_mm = 0.0, samples = 0.0;
	const char *ds = opt_value(argc, argv, "--depth-scale");
	const char *fs = opt_value(argc, argv, "--focal-scale");
	const char *rs = opt_value(argc, argv, "--rms-mm");
	const char *ns = opt_value(argc, argv, "--samples");
	const char *ms = opt_value(argc, argv, "--mode");
	if ((ds != NULL && (!parse_double(ds, &depth_scale) || depth_scale <= 0.0)) ||
	    (fs != NULL && (!parse_double(fs, &focal_scale) || focal_scale <= 0.0)) ||
	    (rs != NULL && (!parse_double(rs, &rms_mm) || rms_mm < 0.0)) ||
	    (ns != NULL && (!parse_double(ns, &samples) || samples < 0.0))) {
		P("error: --depth-scale / --focal-scale must be positive, --rms-mm / --samples non-negative.\n");
		return 1;
	}
	if (ms != NULL && !(strcmp(ms, "a") == 0 || strcmp(ms, "b") == 0 || strcmp(ms, "c") == 0)) {
		P("error: --mode must be a, b or c.\n");
		return 1;
	}

	snprintf(rec.camera_key, sizeof(rec.camera_key), "%s", camera);
	snprintf(rec.screen_key, sizeof(rec.screen_key), "%s", screen);
	rec.source = XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION;
	rec.focal_scale = focal_scale;
	rec.rms_mm = rms_mm;
	rec.sample_count = (uint32_t)samples;
	snprintf(rec.solver_mode, sizeof(rec.solver_mode), "%s", ms != NULL ? ms : "");
	const enum u_camera_calib_store_result w = u_camera_calib_store_put(&rec, machine);
	enum u_camera_calib_store_result wi = U_CAMERA_CALIB_STORE_OK;
	if (w == U_CAMERA_CALIB_STORE_OK && ds != NULL) {
		// The iris scale belongs to the USER, not the camera: one per-user value.
		wi = u_camera_calib_set_viewer_iris_scale(depth_scale);
	}
	char user_path[1024], machine_path[1024];
	u_camera_calib_store_paths(user_path, sizeof(user_path), machine_path, sizeof(machine_path));
	const char *store = machine ? machine_path : user_path;
	const bool ok = w == U_CAMERA_CALIB_STORE_OK && wi == U_CAMERA_CALIB_STORE_OK;
	struct u_camera_calib_record stored = rec;
	(void)u_camera_calib_store_find(camera, screen, XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, &stored);
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "ok", ok);
		cJSON_AddStringToObject(o, "camera", camera);
		cJSON_AddStringToObject(o, "screen", screen);
		cJSON_AddStringToObject(o, "source", u_camera_calib_source_str(rec.source));
		cJSON_AddStringToObject(o, "layer", machine ? "machine" : "user");
		cJSON_AddStringToObject(o, "store", store);
		cJSON_AddItemToObject(o, "t_m", vec_json(stored.t_m, 3));
		cJSON_AddItemToObject(o, "q", vec_json(stored.q, 4));
		cJSON_AddNumberToObject(o, "focal_scale", stored.focal_scale);
		cJSON_AddNumberToObject(o, "rms_mm", stored.rms_mm);
		cJSON_AddNumberToObject(o, "sample_count", stored.sample_count);
		cJSON_AddStringToObject(o, "solver_mode", stored.solver_mode);
		cJSON_AddNumberToObject(o, "timestamp_unix", (double)stored.timestamp_unix);
		if (ds != NULL) {
			cJSON_AddNumberToObject(o, "viewer_iris_scale", depth_scale);
		}
		if (provider != NULL) {
			cJSON_AddStringToObject(o, "provider", provider); // echoed, not used
		}
		if (note[0] != '\0') {
			cJSON_AddStringToObject(o, "note", note);
		}
		if (!ok) {
			cJSON_AddStringToObject(o, "failed",
			                        w != U_CAMERA_CALIB_STORE_OK ? "camera_pose" : "iris_scale");
			cJSON_AddStringToObject(o, "reason",
			                        store_result_str(w != U_CAMERA_CALIB_STORE_OK ? w : wi, machine));
		}
		print_json(o, false);
		cJSON_Delete(o);
	} else if (ok) {
		P("Calibrated camera '%s' on screen '%s': %s (%s layer, %s).\n", camera, screen, pose,
		  machine ? "machine" : "user", store);
		if (ds != NULL) {
			P("Viewer iris scale %.4f (per user).\n", depth_scale);
		}
		P("Every provider that reads this camera picks it up at its next start.\n");
	} else {
		P("FAIL: writing the %s (%s).\n", w != U_CAMERA_CALIB_STORE_OK ? "camera pose" : "iris scale",
		  store_result_str(w != U_CAMERA_CALIB_STORE_OK ? w : wi, machine));
	}
	return ok ? 0 : 1;
}

static int
usage(void)
{
	P("Usage: displayxr-cli tracking calibrate ...\n");
	P("  --screen <key> [--watch] [--json] [--provider <id>] [--count N] [--interval-ms N]\n");
	P("        Host the provider in-process and stream its samples (camera-space block + display-space\n");
	P("        eyes); NDJSON with --json (a \"hello\" line, then one \"sample\" line each).\n");
	P("  solve --in <file|-> [--mode a|b|c] [--eye mid|left|right] [--focal] [--reject-mm X] [--tz-mm X] "
	  "[--json]\n");
	P("        Solve the camera pose from {\"dot_m\":[x,y], \"sample\":<a sample line>} pairs.\n");
	P("        a: R = I (T_x, T_y; >= 2 dots)  b: + pitch (>= 3 samples, two distances)  c: full R (>= 7).\n");
	P("  apply --screen <key> --camera <key> --pose tx,ty,tz,qx,qy,qz,qw [--depth-scale X]\n");
	P("        [--focal-scale X] [--rms-mm X] [--samples N] [--mode a|b|c] [--machine] [--json]\n");
	P("        Commit into the runtime's camera calibration store (per camera + screen, shared by every\n");
	P("        provider; 'displayxr-cli camera calibration list'). --depth-scale = the per-user iris scale.\n");
	return 1;
}

int
cli_cmd_tracking_calibrate(int argc, const char **argv)
{
	// argv: displayxr-cli tracking calibrate [solve|apply] ...
	if (argc >= 4 && strcmp(argv[3], "solve") == 0) {
		return cmd_solve(argc, argv);
	}
	if (argc >= 4 && strcmp(argv[3], "apply") == 0) {
		return cmd_apply(argc, argv);
	}
	if (argc >= 4 && (strcmp(argv[3], "help") == 0 || strcmp(argv[3], "--help") == 0)) {
		return usage();
	}
	if (argc >= 4 && argv[3][0] != '-') {
		P("error: unknown 'tracking calibrate' verb '%s'.\n\n", argv[3]);
		return usage();
	}
	return cmd_watch(argc, argv);
}
