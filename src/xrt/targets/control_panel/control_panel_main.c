// Copyright 2026, Leia Inc.
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
 *
 * @author David Fattal
 */

#include "glad/gl.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui/cimgui.h"
#include "cimgui/cimgui_impl.h"

#include <cjson/cJSON.h>

#include <SDL2/SDL.h>

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
refresh_all(struct panel_state *s)
{
	refresh_info(s);
	refresh_dp(s);
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
}


/*
 *
 * UI.
 *
 */

static const ImVec4 COL_GREEN = {0.30f, 0.85f, 0.40f, 1.0f};
static const ImVec4 COL_RED = {0.95f, 0.35f, 0.35f, 1.0f};

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

	if (!s->have_info) {
		igSpacing();
		igTextColored(COL_RED, "%s", s->info_err[0] ? s->info_err : "No runtime info.");
		igEnd();
		return;
	}

	// ---- Runtime ----
	igSeparatorText("Runtime");
	igText("Version : %s", s->rt_desc);
	igText("Git tag : %s", s->rt_tag);
	igText("Plug-in ABI : v%d", s->rt_abi);
	if (s->ar_queried) {
		igText("Active OpenXR runtime : %s", s->ar_set ? s->ar_value : "<unset>");
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
		igText("Eye-tracking : %s (0x%x), default %s",
		       s->et_supported_label[0] ? s->et_supported_label : "?", (unsigned)s->et_modes,
		       s->et_default_label[0] ? s->et_default_label : "?");
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
	igText("PreferredPlugin : %s", s->have_preferred ? s->preferred : "<unset>");
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

	igEnd();
}


/*
 *
 * SDL2 + OpenGL3 host.
 *
 */

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

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		igNewFrame();

		draw_panel(&state);

		igRender();
		glViewport(0, 0, (int)io->DisplaySize.x, (int)io->DisplaySize.y);
		glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
		SDL_GL_SwapWindow(win);

		if (!did_initial) {
			refresh_all(&state);
			did_initial = true;
		}
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	igDestroyContext(NULL);
	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
