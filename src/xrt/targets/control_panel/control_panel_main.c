// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR Control Panel — an ImGui + SDL2 GUI over `displayxr-cli`.
 *
 * The panel is deliberately "dumb": it spawns the sibling `displayxr-cli`
 * with `--json`, parses stdout with cJSON, and renders. All runtime / plug-in
 * knowledge stays in the CLI (single source of truth), and because the CLI
 * runs as a separate process the panel links zero vendor symbols (ADR-019).
 *
 *   Tier 0  runtime / plug-in / display dashboard + self-test + copy diagnostics
 *   Tier 1  display-processor switch via the PreferredPlugin override
 *   #918    GPU topology — does the weave cross adapters to reach the panel?
 *   #1252   Performance: Target GPU, Mode, Diagnostics (via `displayxr-cli perf`)
 *
 * @author David Fattal
 */

#include "glad/gl.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui/cimgui.h"
#include "cimgui/cimgui_impl.h"

#include <cjson/cJSON.h>

#include <SDL2/SDL.h>
#ifdef _WIN32
#include <SDL2/SDL_syswm.h> // real HWND, to size the ImGui window from the client rect
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


/*
 *
 * Shell out to displayxr-cli.
 *
 */

/*!
 * Run the sibling `displayxr-cli <args>` and capture its stdout into @p out
 * (NUL-terminated, truncated to @p cap). stderr (the noisy plug-in WARN
 * lines) is discarded so stdout stays clean JSON. Returns true if the
 * process launched and was waited on.
 */
static bool
run_cli(const char *args, char *out, size_t cap)
{
	if (cap == 0) {
		return false;
	}
	out[0] = '\0';

#ifdef _WIN32
	// Resolve displayxr-cli.exe next to our own executable.
	char dir[MAX_PATH];
	DWORD len = GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir));
	if (len == 0 || len >= sizeof(dir)) {
		return false;
	}
	char *slash = strrchr(dir, '\\');
	if (slash != NULL) {
		*(slash + 1) = '\0';
	} else {
		dir[0] = '\0';
	}

	char cmd[2048];
	snprintf(cmd, sizeof(cmd), "\"%sdisplayxr-cli.exe\" %s", dir, args);

	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE rd = NULL, wr = NULL;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		return false;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
	                         OPEN_EXISTING, 0, NULL);

	STARTUPINFOA si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = wr;
	si.hStdError = nul;
	si.hStdInput = nul;

	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));

	BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	CloseHandle(wr); // parent must close its write end so ReadFile sees EOF
	if (!ok) {
		CloseHandle(rd);
		if (nul != INVALID_HANDLE_VALUE) {
			CloseHandle(nul);
		}
		return false;
	}

	size_t total = 0;
	char buf[1024];
	DWORD n = 0;
	while (ReadFile(rd, buf, (DWORD)sizeof(buf), &n, NULL) && n > 0) {
		size_t room = (total < cap - 1) ? (cap - 1 - total) : 0;
		size_t take = (n < room) ? n : room;
		if (take > 0) {
			memcpy(out + total, buf, take);
			total += take;
		}
		if (take < (size_t)n) {
			break; // buffer full
		}
	}
	out[total] = '\0';

	WaitForSingleObject(pi.hProcess, INFINITE);
	CloseHandle(rd);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (nul != INVALID_HANDLE_VALUE) {
		CloseHandle(nul);
	}
	return true;
#else
	char cmd[2048];
	snprintf(cmd, sizeof(cmd), "displayxr-cli %s 2>/dev/null", args);
	FILE *f = popen(cmd, "r");
	if (f == NULL) {
		return false;
	}
	size_t total = fread(out, 1, cap - 1, f);
	out[total] = '\0';
	pclose(f);
	return true;
#endif
}


/*
 *
 * State + JSON parsing.
 *
 */

#define MAX_CHECKS 8
#define MAX_DPS 8

struct dp_row
{
	char id[64];
	char name[128];
	int order;
	bool active;
	bool preferred;
};

struct check_row
{
	char name[40];
	bool ok;
	char detail[256];
};

struct disp_row
{
	char mfr[8];
	char product[8];
	int pw, ph, hz, left, top;
	bool primary;
};

//! One resolved monitor→DP binding (from `displays --claims --json`, #793).
struct claim_row
{
	char monitor_id[24]; // "0x0123456789abcdef"
	char plugin_id[64];
	char confidence[16]; // FALLBACK / EDID / VERIFIED
	char apis[64];       // "vk|d3d11|d3d12"
	char serial[32];
	int pw, ph, left, top;
};

//! One hardware adapter from the #918 GPU-topology probe (`info --json` → `gpu.adapters`).
struct gpu_row
{
	char name[128];
	char luid[32]; // "00000000:00024f0b"
	int vram_mb;
};

#define MAX_GPUS 8

/*!
 * One allow-listed performance lever (`info --json` → `performance.levers`).
 *
 * `source` is carried, never dropped: "env" means the value came from the
 * environment of the `displayxr-cli` child this panel spawned — which inherits
 * the panel's own environment and says nothing about any other process —
 * whereas "user" / "machine" / "default" are machine-wide and DO describe what
 * a newly launched app will see. The UI states which.
 */
struct setting_row
{
	char name[64];
	char value[128]; // empty = unset
	bool set;
	char source[16];
};

#define MAX_SETTINGS 16

struct panel_state
{
	// info
	bool have_info;
	char info_err[256];
	char rt_desc[256], rt_tag[128];
	int rt_abi;
	bool ar_queried, ar_set;
	char ar_value[1024];
	bool have_plugin;
	char pl_id[64], pl_name[128], pl_vendor[64], pl_ver[64];
	char device[256];
	bool have_display;
	double w_m, h_m;
	int px, py;
	double vx, vy, vz;
	int et_modes, et_def;
	char et_supported_label[64], et_default_label[32];

	// GPU topology (#918). Machine facts…
	bool gpu_probed;
	char gpu_note[128];
	char gpu_verdict[192];
	bool gpu_split_applies;
	int n_gpus;
	struct gpu_row gpus[MAX_GPUS];
	bool gpu_scanout_resolved, gpu_render_resolved, gpu_ingest_resolved;
	char gpu_scanout_name[128], gpu_scanout_luid[32];
	char gpu_render_name[128], gpu_render_luid[32];
	char gpu_ingest_name[128], gpu_ingest_luid[32], gpu_ingest_provenance[64];
	// …and, kept separate on purpose, the CONFIGURED half: resolved through
	// the settings chain, so each carries a source. "env" means the CLI
	// child's environment (which is this panel's) and describes no other
	// process; the rest are machine-wide. The GPU section states which.
	char gpu_weave[64]; // empty = unset
	bool gpu_weave_set;
	char gpu_weave_source[16]; // env / user / machine / default
	char gpu_ingress[32];
	char gpu_service_split[160];

	// Performance settings (#1252) — what the three controls below read and
	// write. Each row carries its provenance.
	int n_settings;
	struct setting_row settings[MAX_SETTINGS];
	char settings_user_file[512];
	char settings_user_written[32];

	// selftest
	bool have_selftest;
	char verdict[16];
	int result_code;
	int n_checks;
	struct check_row checks[MAX_CHECKS];

	// dp list
	int n_dp;
	bool have_preferred;
	char preferred[64];
	struct dp_row dps[MAX_DPS];

	// connected displays (EDID)
	int n_displays;
	struct disp_row displays[MAX_DPS];

	// resolved per-display DP binding (display → DP the compositor weaves with)
	int n_claims;
	int claims_monitor_count; // total monitors enumerated (claimed or not)
	struct claim_row claims[MAX_DPS];

	// last dp use/reset feedback
	char last_action[512];
};

static void
cpy_str(char *dst, size_t cap, const cJSON *parent, const char *key)
{
	dst[0] = '\0';
	const cJSON *n = cJSON_GetObjectItemCaseSensitive(parent, key);
	if (cJSON_IsString(n) && n->valuestring != NULL) {
		snprintf(dst, cap, "%s", n->valuestring);
	}
}

static double
get_num(const cJSON *parent, const char *key)
{
	const cJSON *n = cJSON_GetObjectItemCaseSensitive(parent, key);
	return cJSON_IsNumber(n) ? n->valuedouble : 0.0;
}

static void
refresh_info(struct panel_state *s)
{
	s->have_info = false;
	s->have_plugin = false;
	s->have_display = false;
	s->ar_queried = false;
	s->info_err[0] = '\0';
	// Refresh is idempotent: n_gpus must be cleared or every click appends
	// another copy of the adapter list until MAX_GPUS.
	s->gpu_probed = false;
	s->n_gpus = 0;
	s->gpu_scanout_resolved = false;
	s->gpu_render_resolved = false;
	s->gpu_ingest_resolved = false;
	s->gpu_weave_set = false;
	s->gpu_weave[0] = '\0';
	s->gpu_weave_source[0] = '\0';
	s->gpu_ingress[0] = '\0';
	s->gpu_service_split[0] = '\0';
	s->n_settings = 0;
	s->settings_user_written[0] = '\0';
	s->gpu_note[0] = '\0';
	s->gpu_verdict[0] = '\0';

	char out[16384];
	if (!run_cli("info --json", out, sizeof(out)) || out[0] == '\0') {
		snprintf(s->info_err, sizeof(s->info_err), "Could not run displayxr-cli (not found alongside the panel?)");
		return;
	}
	cJSON *root = cJSON_Parse(out);
	if (root == NULL) {
		snprintf(s->info_err, sizeof(s->info_err), "Failed to parse 'info --json' output.");
		return;
	}
	s->have_info = true;

	const cJSON *rt = cJSON_GetObjectItemCaseSensitive(root, "runtime");
	if (rt != NULL) {
		cpy_str(s->rt_desc, sizeof(s->rt_desc), rt, "description");
		cpy_str(s->rt_tag, sizeof(s->rt_tag), rt, "git_tag");
		s->rt_abi = (int)get_num(rt, "plugin_abi_version");
	}

	const cJSON *ar = cJSON_GetObjectItemCaseSensitive(root, "active_openxr_runtime");
	if (ar != NULL) {
		s->ar_queried = true;
		const cJSON *set = cJSON_GetObjectItemCaseSensitive(ar, "set");
		s->ar_set = cJSON_IsTrue(set);
		cpy_str(s->ar_value, sizeof(s->ar_value), ar, "value");
	}

	const cJSON *pl = cJSON_GetObjectItemCaseSensitive(root, "plugin");
	if (cJSON_IsObject(pl)) {
		s->have_plugin = true;
		cpy_str(s->pl_id, sizeof(s->pl_id), pl, "id");
		cpy_str(s->pl_name, sizeof(s->pl_name), pl, "display_name");
		cpy_str(s->pl_vendor, sizeof(s->pl_vendor), pl, "vendor");
		cpy_str(s->pl_ver, sizeof(s->pl_ver), pl, "version");
	}

	cpy_str(s->device, sizeof(s->device), root, "device");

	const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "display");
	if (cJSON_IsObject(d)) {
		s->have_display = true;
		s->w_m = get_num(d, "physical_width_m");
		s->h_m = get_num(d, "physical_height_m");
		s->px = (int)get_num(d, "pixel_width");
		s->py = (int)get_num(d, "pixel_height");
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(d, "viewer_m");
		if (v != NULL) {
			s->vx = get_num(v, "x");
			s->vy = get_num(v, "y");
			s->vz = get_num(v, "z");
		}
		const cJSON *et = cJSON_GetObjectItemCaseSensitive(d, "eye_tracking");
		if (et != NULL) {
			s->et_modes = (int)get_num(et, "supported_modes");
			s->et_def = (int)get_num(et, "default_mode");
			cpy_str(s->et_supported_label, sizeof(s->et_supported_label), et, "supported_label");
			cpy_str(s->et_default_label, sizeof(s->et_default_label), et, "default_label");
		}
	}

	// #918 GPU topology. Absent off-Windows, and `probed` false when DXGI
	// could not answer — both render as one line rather than an empty table.
	const cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "gpu");
	if (cJSON_IsObject(g)) {
		s->gpu_probed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "probed"));
		cpy_str(s->gpu_note, sizeof(s->gpu_note), g, "note");
		cpy_str(s->gpu_verdict, sizeof(s->gpu_verdict), g, "verdict");
		s->gpu_split_applies = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "split_applies"));

		const cJSON *arr = cJSON_GetObjectItemCaseSensitive(g, "adapters");
		const cJSON *it = NULL;
		cJSON_ArrayForEach(it, arr)
		{
			if (s->n_gpus >= MAX_GPUS) {
				break;
			}
			struct gpu_row *row = &s->gpus[s->n_gpus++];
			cpy_str(row->name, sizeof(row->name), it, "name");
			cpy_str(row->luid, sizeof(row->luid), it, "luid");
			row->vram_mb = (int)get_num(it, "dedicated_vram_mb");
		}

		const cJSON *sc = cJSON_GetObjectItemCaseSensitive(g, "scanout");
		if (sc != NULL) {
			s->gpu_scanout_resolved = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sc, "resolved"));
			cpy_str(s->gpu_scanout_name, sizeof(s->gpu_scanout_name), sc, "name");
			cpy_str(s->gpu_scanout_luid, sizeof(s->gpu_scanout_luid), sc, "luid");
		}
		const cJSON *rd = cJSON_GetObjectItemCaseSensitive(g, "render");
		if (rd != NULL) {
			s->gpu_render_resolved = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rd, "resolved"));
			cpy_str(s->gpu_render_name, sizeof(s->gpu_render_name), rd, "name");
			cpy_str(s->gpu_render_luid, sizeof(s->gpu_render_luid), rd, "luid");
		}
		const cJSON *ig = cJSON_GetObjectItemCaseSensitive(g, "service_ingest");
		if (ig != NULL) {
			s->gpu_ingest_resolved = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ig, "resolved"));
			cpy_str(s->gpu_ingest_name, sizeof(s->gpu_ingest_name), ig, "name");
			cpy_str(s->gpu_ingest_luid, sizeof(s->gpu_ingest_luid), ig, "luid");
			cpy_str(s->gpu_ingest_provenance, sizeof(s->gpu_ingest_provenance), ig, "provenance");
		}
		const cJSON *sp = cJSON_GetObjectItemCaseSensitive(g, "split");
		if (sp != NULL) {
			s->gpu_weave_set = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sp, "weave_on_scanout_set"));
			cpy_str(s->gpu_weave, sizeof(s->gpu_weave), sp, "weave_on_scanout");
			cpy_str(s->gpu_weave_source, sizeof(s->gpu_weave_source), sp, "weave_on_scanout_source");
			cpy_str(s->gpu_ingress, sizeof(s->gpu_ingress), sp, "ingress");
			cpy_str(s->gpu_service_split, sizeof(s->gpu_service_split), sp, "service_split");
		}
	}

	// #1252 performance levers, each with its provenance.
	const cJSON *pf = cJSON_GetObjectItemCaseSensitive(root, "performance");
	if (cJSON_IsObject(pf)) {
		cpy_str(s->settings_user_file, sizeof(s->settings_user_file), pf, "user_file");
		cpy_str(s->settings_user_written, sizeof(s->settings_user_written), pf, "user_written");
		const cJSON *arr = cJSON_GetObjectItemCaseSensitive(pf, "levers");
		const cJSON *it = NULL;
		cJSON_ArrayForEach(it, arr)
		{
			if (s->n_settings >= MAX_SETTINGS) {
				break;
			}
			struct setting_row *row = &s->settings[s->n_settings++];
			cpy_str(row->name, sizeof(row->name), it, "name");
			cpy_str(row->value, sizeof(row->value), it, "value");
			cpy_str(row->source, sizeof(row->source), it, "source");
			row->set = row->value[0] != '\0';
		}
	}

	cJSON_Delete(root);
}

static void
refresh_selftest(struct panel_state *s)
{
	s->have_selftest = false;
	s->n_checks = 0;

	char out[16384];
	if (!run_cli("selftest --json", out, sizeof(out)) || out[0] == '\0') {
		return;
	}
	cJSON *root = cJSON_Parse(out);
	if (root == NULL) {
		return;
	}
	s->have_selftest = true;
	cpy_str(s->verdict, sizeof(s->verdict), root, "verdict");
	s->result_code = (int)get_num(root, "result_code");

	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "checks");
	if (cJSON_IsArray(arr)) {
		const cJSON *c = NULL;
		cJSON_ArrayForEach(c, arr)
		{
			if (s->n_checks >= MAX_CHECKS) {
				break;
			}
			struct check_row *r = &s->checks[s->n_checks++];
			cpy_str(r->name, sizeof(r->name), c, "name");
			r->ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "ok"));
			cpy_str(r->detail, sizeof(r->detail), c, "detail");
		}
	}
	cJSON_Delete(root);
}

static void
refresh_dp(struct panel_state *s)
{
	s->n_dp = 0;
	s->have_preferred = false;
	s->preferred[0] = '\0';

	char out[16384];
	if (!run_cli("dp list --json", out, sizeof(out)) || out[0] == '\0') {
		return;
	}
	cJSON *root = cJSON_Parse(out);
	if (root == NULL) {
		return;
	}
	const cJSON *pref = cJSON_GetObjectItemCaseSensitive(root, "preferred");
	if (cJSON_IsString(pref) && pref->valuestring != NULL) {
		s->have_preferred = true;
		snprintf(s->preferred, sizeof(s->preferred), "%s", pref->valuestring);
	}

	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "plugins");
	if (cJSON_IsArray(arr)) {
		const cJSON *p = NULL;
		cJSON_ArrayForEach(p, arr)
		{
			if (s->n_dp >= MAX_DPS) {
				break;
			}
			struct dp_row *r = &s->dps[s->n_dp++];
			cpy_str(r->id, sizeof(r->id), p, "id");
			cpy_str(r->name, sizeof(r->name), p, "display_name");
			r->order = (int)get_num(p, "probe_order");
			r->active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "active"));
			r->preferred = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "preferred"));
		}
	}
	cJSON_Delete(root);
}

static void
refresh_displays(struct panel_state *s)
{
	s->n_displays = 0;
	char out[16384];
	if (!run_cli("displays --json", out, sizeof(out)) || out[0] == '\0') {
		return;
	}
	cJSON *root = cJSON_Parse(out);
	if (root == NULL) {
		return;
	}
	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "displays");
	if (cJSON_IsArray(arr)) {
		const cJSON *m = NULL;
		cJSON_ArrayForEach(m, arr)
		{
			if (s->n_displays >= MAX_DPS) {
				break;
			}
			struct disp_row *d = &s->displays[s->n_displays++];
			cpy_str(d->mfr, sizeof(d->mfr), m, "manufacturer");
			cpy_str(d->product, sizeof(d->product), m, "product");
			d->pw = (int)get_num(m, "pixel_width");
			d->ph = (int)get_num(m, "pixel_height");
			d->hz = (int)get_num(m, "refresh_hz");
			d->left = (int)get_num(m, "screen_left");
			d->top = (int)get_num(m, "screen_top");
			d->primary = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "primary"));
		}
	}
	cJSON_Delete(root);
}

//! Resolved monitor→DP registry (#793): which DP the compositor binds to each
//! display. Same `target_plugin_resolve_displays` data the CLI prints; the panel
//! stays a dumb JSON client (ADR-019) by shelling `displays --claims --json`.
static void
refresh_claims(struct panel_state *s)
{
	s->n_claims = 0;
	s->claims_monitor_count = 0;
	char out[16384];
	if (!run_cli("displays --claims --json", out, sizeof(out)) || out[0] == '\0') {
		return;
	}
	cJSON *root = cJSON_Parse(out);
	if (root == NULL) {
		return;
	}
	s->claims_monitor_count = (int)get_num(root, "monitor_count");
	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "claims");
	if (cJSON_IsArray(arr)) {
		const cJSON *c = NULL;
		cJSON_ArrayForEach(c, arr)
		{
			if (s->n_claims >= MAX_DPS) {
				break;
			}
			struct claim_row *r = &s->claims[s->n_claims++];
			cpy_str(r->monitor_id, sizeof(r->monitor_id), c, "monitor_id");
			cpy_str(r->plugin_id, sizeof(r->plugin_id), c, "plugin_id");
			cpy_str(r->confidence, sizeof(r->confidence), c, "confidence");
			cpy_str(r->apis, sizeof(r->apis), c, "supported_apis");
			cpy_str(r->serial, sizeof(r->serial), c, "serial");
			r->pw = (int)get_num(c, "pixel_width");
			r->ph = (int)get_num(c, "pixel_height");
			r->left = (int)get_num(c, "screen_left");
			r->top = (int)get_num(c, "screen_top");
		}
	}
	cJSON_Delete(root);
}

static void
refresh_all(struct panel_state *s)
{
	refresh_info(s);
	refresh_dp(s);
	refresh_displays(s);
	refresh_claims(s);
}

static void
dp_action(struct panel_state *s, const char *args)
{
	char out[2048];
	if (run_cli(args, out, sizeof(out))) {
		// first line is the human-readable result
		char *nl = strchr(out, '\n');
		if (nl != NULL) {
			*nl = '\0';
		}
		snprintf(s->last_action, sizeof(s->last_action), "%s", out[0] ? out : "(done)");
	} else {
		snprintf(s->last_action, sizeof(s->last_action), "Failed to run: displayxr-cli %s", args);
	}
	refresh_dp(s);
	refresh_claims(s); // the override changed which DP binds each display (#793)
}

/*!
 * Write one performance lever through `displayxr-cli perf` (#1252).
 *
 * The panel deliberately does not write the settings file itself: one writer,
 * the same shape as `dp use` / `dp reset`, and the GUI keeps no runtime
 * knowledge (#378). `refresh_info` afterwards re-reads the resolved state, so
 * what the UI shows is always what the chain resolved — never what we assumed
 * the click did.
 */
static void
perf_action(struct panel_state *s, const char *args)
{
	char out[2048];
	if (run_cli(args, out, sizeof(out))) {
		char *nl = strchr(out, '\n');
		if (nl != NULL) {
			*nl = '\0';
		}
		snprintf(s->last_action, sizeof(s->last_action), "%s", out[0] ? out : "(done)");
	} else {
		snprintf(s->last_action, sizeof(s->last_action), "Failed to run: displayxr-cli %s", args);
	}
	refresh_info(s);
}

//! Resolved value of one lever, or "" when nothing set it.
static const char *
setting_value(const struct panel_state *s, const char *name)
{
	for (int i = 0; i < s->n_settings; i++) {
		if (strcmp(s->settings[i].name, name) == 0) {
			return s->settings[i].value;
		}
	}
	return "";
}

//! Provenance of one lever ("env"/"user"/"machine"/"default"), or "".
static const char *
setting_source(const struct panel_state *s, const char *name)
{
	for (int i = 0; i < s->n_settings; i++) {
		if (strcmp(s->settings[i].name, name) == 0) {
			return s->settings[i].source;
		}
	}
	return "";
}

//! Is any lever set by something other than the runtime's own default?
static bool
any_setting_non_default(const struct panel_state *s)
{
	for (int i = 0; i < s->n_settings; i++) {
		if (s->settings[i].set) {
			return true;
		}
	}
	return false;
}

/*!
 * Is Compatibility mode in force?
 *
 * Derived from the resolved values rather than stored as its own key. A preset
 * that stored its own name would drift from what the levers actually say the
 * moment anything else wrote one of them; deriving it means the UI can never
 * claim a mode the runtime is not in, and "Custom" falls out for free.
 */
static bool
compat_mode_on(const struct panel_state *s)
{
	const char *split = setting_value(s, "DXR_WEAVE_ON_SCANOUT");
	const char *repaint = setting_value(s, "DXR_WEAVE_REPAINT");
	return split[0] == '0' && repaint[0] == '0';
}


/*
 *
 * UI.
 *
 */

static const ImVec4 COL_GREEN = {0.30f, 0.85f, 0.40f, 1.0f};
static const ImVec4 COL_RED = {0.95f, 0.35f, 0.35f, 1.0f};
static const ImVec4 COL_AMBER = {0.98f, 0.75f, 0.25f, 1.0f}; // override-forced binding (#793)

static void
draw_panel(struct panel_state *s)
{
	ImGuiIO *io = igGetIO();
	igSetNextWindowPos((ImVec2){0, 0}, ImGuiCond_Always, (ImVec2){0, 0});
	igSetNextWindowSize(io->DisplaySize, ImGuiCond_Always);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
	                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
	                         ImGuiWindowFlags_NoSavedSettings;
	igBegin("DisplayXR Control Panel", NULL, flags);

	// Wrap all text at the window's right edge so long values (device
	// strings, ActiveRuntime path, self-test details) reflow instead of
	// clipping. Pos 0.0f tracks the content-region width, so it follows
	// the window live on resize. Popped before every igEnd() below.
	igPushTextWrapPos(0.0f);

	if (igButton("Refresh", (ImVec2){0, 0})) {
		refresh_all(s);
	}
	igSameLine(0.0f, -1.0f);
	if (igButton("Copy diagnostics", (ImVec2){0, 0})) {
		char dump[16384];
		if (run_cli("info", dump, sizeof(dump))) {
			SDL_SetClipboardText(dump);
			snprintf(s->last_action, sizeof(s->last_action),
			         "Copied diagnostics to clipboard (logs: %%LOCALAPPDATA%%\\DisplayXR).");
		}
	}

	// ---- Persistent-override banner (#793 Phase 2) ----
	// `dp use` writes HKLM PreferredPlugin and PERSISTS across reboots — a known
	// footgun: a box left pinned (e.g. to sim_display for a test) keeps using that
	// DP for every app and every reboot until someone notices. Surface it loudly,
	// above everything else, whenever an override is active, with one-click reset.
	// Shown before the have_info early-return so a mis-pinned DP that breaks the
	// runtime is still self-evidently the cause.
	if (s->have_preferred) {
		igSpacing();
		igTextColored(COL_AMBER,
		              "[!] DP OVERRIDE ACTIVE - '%s' is forced for every app and persists across "
		              "reboots until reset.",
		              s->preferred);
		const ImVec4 red = {0.60f, 0.12f, 0.12f, 1.0f};
		const ImVec4 red_hi = {0.78f, 0.18f, 0.18f, 1.0f};
		igPushStyleColor_Vec4(ImGuiCol_Button, red);
		igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, red_hi);
		igPushStyleColor_Vec4(ImGuiCol_ButtonActive, red_hi);
		if (igButton("Reset DP override now", (ImVec2){0, 0})) {
			dp_action(s, "dp reset");
		}
		igPopStyleColor(3);
		igSpacing();
		igSeparator();
	}

	if (!s->have_info) {
		igSpacing();
		igTextColored(COL_RED, "%s", s->info_err[0] ? s->info_err : "No runtime info.");
		// The banner's reset button is reachable on this path, but the usual
		// last_action readout below is not — it lives past this early-return.
		// Without this the button reports nothing when it fails (e.g. `dp reset`
		// deletes an HKLM value, so a non-elevated panel is denied): the banner
		// just stays put and the click looks ignored. This IS the mis-pinned-DP
		// case the banner exists for, so it is the one place feedback matters most.
		if (s->last_action[0] != '\0') {
			igSpacing();
			igSeparator();
			igTextWrapped("%s", s->last_action);
		}
		igPopTextWrapPos();
		igEnd();
		return;
	}

	// ---- Runtime ----
	igSeparatorText("Runtime");
	igText("Version : %s", s->rt_desc);
	igText("Git tag : %s", s->rt_tag);
	igText("Plug-in ABI : v%d", s->rt_abi);
	if (s->ar_queried) {
		bool is_dxr = s->ar_set && strstr(s->ar_value, "DisplayXR") != NULL;
		igText("Active OpenXR runtime :");
		igSameLine(0.0f, -1.0f);
		igTextColored(is_dxr ? COL_GREEN : COL_RED, "%s", s->ar_set ? s->ar_value : "<unset>");
		if (!is_dxr) {
			if (igButton("Set DisplayXR as active OpenXR runtime", (ImVec2){0, 0})) {
				char out[2048];
				if (run_cli("runtime activate", out, sizeof(out))) {
					char *nl = strchr(out, '\n');
					if (nl != NULL) {
						*nl = '\0';
					}
					snprintf(s->last_action, sizeof(s->last_action), "%s",
					         out[0] ? out : "(done)");
				}
				refresh_info(s);
			}
		}
	}

	// ---- Display processor ----
	igSeparatorText("Display processor");
	if (!s->have_plugin) {
		igTextColored(COL_RED, "No active vendor plug-in.");
	} else {
		igText("Plug-in : %s  (%s)", s->pl_id, s->pl_name[0] ? s->pl_name : "?");
		igText("Vendor  : %s", s->pl_vendor[0] ? s->pl_vendor : "?");
		igText("Version : %s", s->pl_ver[0] ? s->pl_ver : "?");
		igTextColored(COL_GREEN, "ABI v%d (loader-verified match)", s->rt_abi);
		igText("Device  : %s", s->device);
	}

	// ---- Display ----
	if (s->have_display) {
		igSeparatorText("Display");
		igText("Physical : %.4f m x %.4f m", s->w_m, s->h_m);
		igText("Pixels   : %d x %d", s->px, s->py);
		igText("Viewer   : (%.3f, %.3f, %.3f) m", s->vx, s->vy, s->vz);
		if (igIsItemHovered(0)) {
			igSetTooltip("Nominal viewer (eye) position relative to the display centre, "
			             "in metres (x=right, y=up, z=out toward the user). Drives the "
			             "Kooima projection default when the app has no head tracking.");
		}
		igText("Eye-tracking : %s (0x%x), default %s",
		       s->et_supported_label[0] ? s->et_supported_label : "?", (unsigned)s->et_modes,
		       s->et_default_label[0] ? s->et_default_label : "?");
	}

	// ---- Connected displays (EDID, vendor-neutral) ----
	igSeparatorText("Connected displays (EDID)");
	if (s->n_displays == 0) {
		igTextDisabled("(none enumerated — Windows-only)");
	}
	for (int i = 0; i < s->n_displays; i++) {
		struct disp_row *d = &s->displays[i];
		igText("[%d] %s %s  %dx%d @ %dHz  pos (%d,%d)%s", i, d->mfr, d->product, d->pw, d->ph, d->hz,
		       d->left, d->top, d->primary ? "  [primary]" : "");
	}

	// ---- Resolved display → DP binding (#793) ----
	igSeparatorText("Display -> DP binding (resolved)");
	if (igIsItemHovered(0)) {
		igSetTooltip("Which display processor the runtime bound to each monitor, from the per-monitor "
		             "registry — by EDID claim confidence, or forced by the PreferredPlugin override "
		             "below. This is the DP the compositor actually weaves with.");
	}
	if (s->n_claims == 0) {
		igTextDisabled("(no monitor claimed by any plug-in%s)",
		               s->claims_monitor_count > 0 ? "" : " — Windows-only");
	}
	for (int i = 0; i < s->n_claims; i++) {
		struct claim_row *r = &s->claims[i];
		bool forced = s->have_preferred && strcmp(r->plugin_id, s->preferred) == 0;
		igText("%s  %dx%d @ (%d,%d)", r->monitor_id, r->pw, r->ph, r->left, r->top);
		igTextColored(forced ? COL_AMBER : COL_GREEN, "    -> %s", r->plugin_id);
		igSameLine(0.0f, -1.0f);
		igTextDisabled("[%s]  apis=%s%s%s%s", r->confidence, r->apis, r->serial[0] ? "  serial=" : "",
		               r->serial, forced ? "   (forced by override)" : "");
	}

	// ---- GPU topology (#918) ----
	//
	// Two kinds of fact, and the section keeps them visibly apart. The adapter
	// list, the scanout/render/ingest adapters and the verdict are properties
	// of the MACHINE. `DXR_WEAVE_ON_SCANOUT`, the ingress policy and the
	// "service split" line are read from the environment of the displayxr-cli
	// CHILD THIS PANEL SPAWNED, which inherits the panel's environment and has
	// nothing to do with the environment a running app or the DisplayXR service
	// was started in. Rendering the second kind as machine state is the trap
	// docs/roadmap/control-panel-performance-settings.md exists to prevent — so
	// it is drawn dimmed, under its own labelled sub-heading, and never in the
	// same visual weight as the adapter list.
	igSeparatorText("GPU topology");
	if (igIsItemHovered(0)) {
		igSetTooltip(
		    "Does the woven frame have to cross adapters to reach the panel? On a hybrid "
		    "laptop the panel is often scanned out by the integrated GPU while the app "
		    "renders on the discrete one (#918 / ADR-037).");
	}
	if (!s->gpu_probed) {
		igTextDisabled("(not probed — %s)", s->gpu_note[0] ? s->gpu_note : "Windows-only");
	} else if (s->n_gpus <= 1) {
		// One adapter: the whole question is moot. A table here would be noise.
		igTextColored(COL_GREEN, "Single adapter — the weave never crosses GPUs.");
		if (s->n_gpus == 1) {
			igTextDisabled("%s  LUID=%s", s->gpus[0].name, s->gpus[0].luid);
		}
	} else {
		for (int i = 0; i < s->n_gpus; i++) {
			struct gpu_row *g = &s->gpus[i];
			bool is_scanout = s->gpu_scanout_resolved && strcmp(g->luid, s->gpu_scanout_luid) == 0;
			bool is_render = s->gpu_render_resolved && strcmp(g->luid, s->gpu_render_luid) == 0;
			igText("[%d] %s", i, g->name);
			igTextDisabled("    LUID=%s  %d MB dedicated%s%s", g->luid, g->vram_mb,
			               is_scanout ? "   <- panel scanout" : "",
			               is_render ? "   <- render (default)" : "");
		}
		igTextColored(s->gpu_split_applies ? COL_AMBER : COL_GREEN, "%s", s->gpu_verdict);
		if (s->gpu_ingest_resolved) {
			igTextDisabled("service ingest: %s (%s)", s->gpu_ingest_name, s->gpu_ingest_provenance);
		}
	}
	if (s->gpu_probed) {
		// The configured half, dimmed and with its provenance stated. "env"
		// means THIS panel's environment, which says nothing about another
		// process; every other source is machine-wide and does.
		const bool weave_from_env = strcmp(s->gpu_weave_source, "env") == 0;
		igTextDisabled("DXR_WEAVE_ON_SCANOUT=%s [%s]%s   ingress=%s",
		               s->gpu_weave_set ? s->gpu_weave : "<unset>",
		               s->gpu_weave_source[0] ? s->gpu_weave_source : "?",
		               weave_from_env ? "  <- this panel's environment only" : "",
		               s->gpu_ingress[0] ? s->gpu_ingress : "?");
		if (s->gpu_service_split[0] != '\0') {
			igTextDisabled("%s", s->gpu_service_split);
		}
		igTextDisabled(
		    "A running app's real placement is the 'weave placement:' line in its log "
		    "(%%LOCALAPPDATA%%\\DisplayXR\\DisplayXR_<exe>.*.log).");
	}

	// ---- Performance (#1252) ----
	//
	// Three controls, deliberately. There is no quality-vs-performance dial in
	// this runtime — the defaults ARE the tuned configuration — so a
	// Quality/Balanced/Performance menu would be fiction. What users actually
	// have is three unrelated needs: which GPU, "is the pipeline the problem",
	// and "I am filing a bug". They are separate axes, so they are separate
	// controls; folding them into one dropdown would produce a combinatorial
	// menu that is both larger and less clear.
	//
	// Every write goes through `displayxr-cli perf`, and the state shown is
	// re-read from the resolved chain afterwards — never assumed from the click.
	igSeparatorText("Performance");
	if (igIsItemHovered(0)) {
		igSetTooltip(
		    "Settings the runtime reads inside each app's own process. They apply to apps "
		    "started AFTER the change — the panel cannot reach into a running app.");
	}

	if (!s->have_info) {
		igTextDisabled("(unavailable — displayxr-cli did not report)");
	} else {
		// -- 1. Target GPU. Only a real choice on a multi-adapter box; on a
		//    single-GPU machine there is nothing to choose, so don't offer it.
		if (s->n_gpus > 1) {
			const char *gpu_now = setting_value(s, "DXR_D3D_FORCE_GPU");
			igText("Target GPU");
			igTextDisabled(
			    "    Which adapter apps render on. 'Panel's adapter' keeps the weave local "
			    "to the display.");
			struct
			{
				const char *label;
				const char *value; // "" = Auto (clear the setting)
			} gpu_opts[] = {
			    {"Auto (recommended)", ""},
			    {"Panel's display adapter", "scanout"},
			    {"High performance (discrete)", "dgpu"},
			    {"Power saving (integrated)", "igpu"},
			};
			for (int i = 0; i < (int)(sizeof(gpu_opts) / sizeof(gpu_opts[0])); i++) {
				const bool active = (gpu_opts[i].value[0] == '\0')
				                        ? (gpu_now[0] == '\0')
				                        : (strcmp(gpu_now, gpu_opts[i].value) == 0);
				char id[96];
				snprintf(id, sizeof(id), "%s##gpu%d", gpu_opts[i].label, i);
				if (igRadioButton_Bool(id, active) && !active) {
					char args[160];
					if (gpu_opts[i].value[0] == '\0') {
						// Both variables, so a mixed D3D/VK state cannot linger.
						perf_action(s, "perf reset DXR_D3D_FORCE_GPU");
						perf_action(s, "perf reset DXR_VK_FORCE_GPU");
					} else {
						snprintf(args, sizeof(args), "perf set DXR_D3D_FORCE_GPU %s",
						         gpu_opts[i].value);
						perf_action(s, args);
						snprintf(args, sizeof(args), "perf set DXR_VK_FORCE_GPU %s",
						         gpu_opts[i].value);
						perf_action(s, args);
					}
				}
			}
			const char *gpu_src = setting_source(s, "DXR_D3D_FORCE_GPU");
			if (strcmp(gpu_src, "env") == 0) {
				igTextColored(COL_AMBER,
				              "    An environment variable is setting this and OUTRANKS the panel.");
			}
			igSpacing();
		}

		// -- 2. Mode. Compatibility turns off the two levers that change what
		//    the DISPLAY PROCESSOR is asked to do — the scanout split and the
		//    repaint. Late weave is deliberately NOT in the bundle: it only
		//    changes when we present on our own swapchain, it already
		//    self-disables where the platform gives no present-timing feedback,
		//    and it is the single largest latency win (96->17 ms on VK), so
		//    bundling it would charge every compatibility click a 5x latency
		//    regression on the lever least likely to be the culprit.
		const bool compat = compat_mode_on(s);
		igText("Mode");
		if (igRadioButton_Bool("Balanced (default)##mode", !compat) && compat) {
			perf_action(s, "perf reset DXR_WEAVE_ON_SCANOUT");
			perf_action(s, "perf reset DXR_WEAVE_REPAINT");
		}
		if (igRadioButton_Bool("Compatibility##mode", compat) && !compat) {
			perf_action(s, "perf set DXR_WEAVE_ON_SCANOUT 0");
			perf_action(s, "perf set DXR_WEAVE_REPAINT 0");
		}
		igTextDisabled("    Compatibility turns off the cross-adapter weave split and the repaint —");
		igTextDisabled("    for 'the 3D looks wrong, is it the pipeline?'. It COSTS latency; it is not");
		igTextDisabled("    a faster setting. Leave it on Balanced unless you are diagnosing.");
		igSpacing();

		// -- 3. Diagnostics. Pure observers: they change no behaviour, which is
		//    exactly what makes them safe to hand a user.
		const bool diag = setting_value(s, "DXR_FRAME_WITNESS")[0] != '\0' ||
		                  setting_value(s, "DXR_FRAME_STAGE_TIMING")[0] != '\0';
		igText("Diagnostics");
		if (igRadioButton_Bool("Off##diag", !diag) && diag) {
			perf_action(s, "perf reset DXR_FRAME_WITNESS");
			perf_action(s, "perf reset DXR_FRAME_STAGE_TIMING");
		}
		if (igRadioButton_Bool("On (for bug reports)##diag", diag) && !diag) {
			perf_action(s, "perf set DXR_FRAME_WITNESS 5");
			perf_action(s, "perf set DXR_FRAME_STAGE_TIMING 1");
		}
		igTextDisabled("    Adds frame/weave/present counters to each app's log. Changes no behaviour.");

		// -- The anti-stale banner. A setting nobody remembers making is the
		//    failure mode this whole surface has to defend against, so it is
		//    stated loudly with the date and a one-click way out.
		if (any_setting_non_default(s)) {
			igSpacing();
			igTextColored(COL_AMBER, "[!] Non-default performance settings are in force%s%s.",
			              s->settings_user_written[0] ? " since " : "",
			              s->settings_user_written[0] ? s->settings_user_written : "");
			for (int i = 0; i < s->n_settings; i++) {
				if (s->settings[i].set) {
					igTextDisabled("      %s = %s [%s]", s->settings[i].name, s->settings[i].value,
					               s->settings[i].source);
				}
			}
			if (igButton("Reset all performance settings", (ImVec2){0, 0})) {
				perf_action(s, "perf reset");
			}
			igTextDisabled("    Applies to apps started after the change. Values shown as [env] come");
			igTextDisabled("    from an environment variable and cannot be reset from here.");
		}
	}

	// ---- Self-test ----
	igSeparatorText("Self-test");
	if (igButton("Run self-test", (ImVec2){0, 0})) {
		refresh_selftest(s);
	}
	if (s->have_selftest) {
		igSameLine(0.0f, -1.0f);
		bool pass = (s->result_code == 0);
		igTextColored(pass ? COL_GREEN : COL_RED, "%s (rc=%d)", s->verdict, s->result_code);
		for (int i = 0; i < s->n_checks; i++) {
			igTextColored(s->checks[i].ok ? COL_GREEN : COL_RED, "  [%s] %s",
			              s->checks[i].ok ? "PASS" : "FAIL", s->checks[i].name);
			igSameLine(0.0f, -1.0f);
			igTextDisabled("- %s", s->checks[i].detail);
		}
	}

	// ---- Tier 1: DP switch ----
	igSeparatorText("Display-processor switch (PreferredPlugin override)");
	igText("PreferredPlugin : %s", s->have_preferred ? s->preferred : "<unset> (auto - by probe order)");
	if (igIsItemHovered(0)) {
		igSetTooltip("Optional manual override that forces a specific display processor. "
		             "When unset (the normal state), the runtime auto-selects by probe order; "
		             "the active plug-in is shown under 'Display processor' above.");
	}
	for (int i = 0; i < s->n_dp; i++) {
		struct dp_row *r = &s->dps[i];
		igText("%s %s id='%s' (ProbeOrder %d)%s", r->active ? "*" : " ",
		       r->preferred ? "[preferred]" : "          ", r->id, r->order,
		       r->name[0] ? "" : "");
		igSameLine(0.0f, -1.0f);
		char btn[96];
		snprintf(btn, sizeof(btn), "Use##%d", i);
		if (igButton(btn, (ImVec2){0, 0})) {
			char args[128];
			snprintf(args, sizeof(args), "dp use %s", r->id);
			dp_action(s, args);
		}
	}
	if (igButton("Reset to default discovery", (ImVec2){0, 0})) {
		dp_action(s, "dp reset");
	}
	igTextDisabled("Switching takes effect on the next process — restart the service or relaunch your app.");

	if (s->last_action[0] != '\0') {
		igSpacing();
		igSeparator();
		igTextWrapped("%s", s->last_action);
	}

	igPopTextWrapPos();
	igEnd();
}


/*
 *
 * SDL2 + OpenGL3 host.
 *
 */

//! Everything needed to paint one frame. Passed to the resize event watch so
//! the panel can repaint *during* Windows' modal move/size loop — that loop
//! blocks the normal main loop, so without this the content only reflows on
//! mouse release (it appears to "stretch" mid-drag, then snap-wrap).
struct frame_ctx
{
	SDL_Window *win;
	ImGuiIO *io;
	struct panel_state *state;
	void *hwnd; // HWND on Windows, NULL elsewhere
};

//! Paint exactly one frame: new ImGui frame (sized from the live client rect on
//! Windows), the panel, then present.
static void
render_frame(struct frame_ctx *c)
{
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
#ifdef _WIN32
	// Override the (possibly stale) backend size with the true client rect so
	// the full-window panel + its wrap edge track live resizes.
	if (c->hwnd != NULL) {
		RECT rc;
		if (GetClientRect((HWND)c->hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
			c->io->DisplaySize.x = (float)(rc.right - rc.left);
			c->io->DisplaySize.y = (float)(rc.bottom - rc.top);
			c->io->DisplayFramebufferScale.x = 1.0f;
			c->io->DisplayFramebufferScale.y = 1.0f;
		}
	}
#endif
	igNewFrame();

	draw_panel(c->state);

	igRender();
	glViewport(0, 0, (int)c->io->DisplaySize.x, (int)c->io->DisplaySize.y);
	glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
	SDL_GL_SwapWindow(c->win);
}

//! SDL event watch: fires synchronously as events are *sent*, including from
//! inside Windows' modal resize loop (where the main loop is blocked).
//! Repainting on every size change is what makes the panel reflow live while
//! the user drags the window edge.
static int SDLCALL
resize_event_watch(void *userdata, SDL_Event *e)
{
	if (e->type == SDL_WINDOWEVENT &&
	    (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_EXPOSED)) {
		render_frame((struct frame_ctx *)userdata);
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		return 1;
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

	SDL_Window *win = SDL_CreateWindow("DisplayXR Control Panel", SDL_WINDOWPOS_CENTERED,
	                                   SDL_WINDOWPOS_CENTERED, 760, 820,
	                                   SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
	                                       SDL_WINDOW_ALLOW_HIGHDPI);
	if (win == NULL) {
		SDL_Quit();
		return 1;
	}
	SDL_GLContext ctx = SDL_GL_CreateContext(win);
	if (ctx == NULL) {
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}
	SDL_GL_MakeCurrent(win, ctx);
	SDL_GL_SetSwapInterval(1);

	if (gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress) == 0) {
		SDL_GL_DeleteContext(ctx);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	// Scale the UI to the display's DPI (96 = 100%), like u_debug_gui.
	float gui_scale = 1.0f;
	int disp_idx = SDL_GetWindowDisplayIndex(win);
	if (disp_idx >= 0) {
		float dpi = 96.0f;
		if (SDL_GetDisplayDPI(disp_idx, &dpi, NULL, NULL) == 0) {
			gui_scale = dpi / 96.0f;
			if (gui_scale < 1.0f) {
				gui_scale = 1.0f;
			}
			if (gui_scale > 3.0f) {
				gui_scale = 3.0f;
			}
		}
	}

	igCreateContext(NULL);
	ImGuiIO *io = igGetIO();
	io->IniFilename = NULL; // don't litter an imgui.ini
	io->FontGlobalScale = gui_scale;
	igStyleColorsDark(NULL);
	ImGuiStyle_ScaleAllSizes(igGetStyle(), gui_scale);

	ImGui_ImplSDL2_InitForOpenGL(win, ctx);
	ImGui_ImplOpenGL3_Init(NULL);

	struct panel_state state;
	memset(&state, 0, sizeof(state));
	snprintf(state.info_err, sizeof(state.info_err), "Loading… (querying displayxr-cli)");

	// The first query (and every Refresh) spawns displayxr-cli, which loads
	// the vendor plug-in — up to a second or two. Present one frame first so
	// the window paints immediately instead of freezing black on launch.
	bool did_initial = false;

#ifdef _WIN32
	// On this HighDPI Win32 path SDL's cached window size doesn't reliably
	// follow OS resizes, which pinned the ImGui window — and thus the text
	// wrap edge — to the launch width (text clipped instead of reflowing).
	// Drive the window size from the real client rect each frame instead.
	HWND panel_hwnd = NULL;
	{
		SDL_SysWMinfo wmi;
		SDL_VERSION(&wmi.version);
		if (SDL_GetWindowWMInfo(win, &wmi)) {
			panel_hwnd = wmi.info.win.window;
		}
	}
#endif

	struct frame_ctx fctx;
	fctx.win = win;
	fctx.io = io;
	fctx.state = &state;
#ifdef _WIN32
	fctx.hwnd = panel_hwnd;
#else
	fctx.hwnd = NULL;
#endif
	// Repaint during the modal move/size loop (live resize).
	SDL_AddEventWatch(resize_event_watch, &fctx);

	bool running = true;
	while (running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			ImGui_ImplSDL2_ProcessEvent(&e);
			if (e.type == SDL_QUIT) {
				running = false;
			}
			if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
			    e.window.windowID == SDL_GetWindowID(win)) {
				running = false;
			}
		}

		render_frame(&fctx);

		if (!did_initial) {
			refresh_all(&state);
			did_initial = true;
		}
	}

	SDL_DelEventWatch(resize_event_watch, &fctx);
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	igDestroyContext(NULL);
	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
