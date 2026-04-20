// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR shell app-discovery scanner implementation.
 *
 * Walks filesystem paths, finds .exe files with a matching .displayxr.json
 * sidecar, parses the sidecar via cJSON, and populates an array of
 * shell_scanned_app structs.
 *
 * @ingroup shell
 */

#include "shell_app_scan.h"
#include "shell_pe_scan.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define LOG_W(...) fprintf(stderr, "[shell_scan] " __VA_ARGS__)
#define LOG_I(...) fprintf(stdout, "[shell_scan] " __VA_ARGS__)

#ifdef _WIN32

#include <windows.h>

// -----------------------------------------------------------------------------
// Sidecar parsing
// -----------------------------------------------------------------------------

static void
copy_str(char *dst, size_t dst_size, const char *src)
{
	if (!dst || dst_size == 0) return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dst_size, "%s", src);
}

// Read the entire file into a heap buffer. Caller frees. Returns NULL on error.
static char *
read_file_text(const char *path, long max_bytes)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (len <= 0 || len > max_bytes) {
		fclose(f);
		return NULL;
	}

	char *data = (char *)malloc((size_t)len + 1);
	if (!data) {
		fclose(f);
		return NULL;
	}

	size_t got = fread(data, 1, (size_t)len, f);
	fclose(f);

	if (got != (size_t)len) {
		free(data);
		return NULL;
	}

	data[len] = '\0';
	return data;
}

// Resolve an icon path from the sidecar — relative paths are resolved against
// sidecar_dir. Writes "" to out if the resolved file doesn't exist.
static void
resolve_icon_path(const char *sidecar_dir, const char *rel, char *out, size_t out_size)
{
	if (!rel || !*rel) {
		if (out_size) out[0] = '\0';
		return;
	}

	char candidate[SHELL_PATH_MAX];
	// Treat the sidecar value as relative unless it already looks absolute.
	if (rel[0] == '\\' || rel[0] == '/' || (rel[0] && rel[1] == ':')) {
		snprintf(candidate, sizeof(candidate), "%s", rel);
	} else {
		snprintf(candidate, sizeof(candidate), "%s\\%s", sidecar_dir, rel);
	}

	DWORD attrs = GetFileAttributesA(candidate);
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		if (out_size) out[0] = '\0';
		return;
	}

	copy_str(out, out_size, candidate);
}

// Validate and populate a scanned_app from a parsed sidecar JSON object.
// Returns true on success.
static bool
parse_sidecar(const cJSON *root, const char *exe_path, const char *sidecar_dir,
              struct shell_scanned_app *app)
{
	memset(app, 0, sizeof(*app));

	const cJSON *j_schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
	if (!cJSON_IsNumber(j_schema) || j_schema->valueint != 1) {
		LOG_W("rejecting %s: schema_version missing or unsupported\n", exe_path);
		return false;
	}

	const cJSON *j_name = cJSON_GetObjectItemCaseSensitive(root, "name");
	if (!cJSON_IsString(j_name) || !j_name->valuestring || !*j_name->valuestring) {
		LOG_W("rejecting %s: name missing\n", exe_path);
		return false;
	}
	if (strlen(j_name->valuestring) >= SHELL_APP_NAME_MAX) {
		LOG_W("rejecting %s: name too long\n", exe_path);
		return false;
	}
	copy_str(app->name, sizeof(app->name), j_name->valuestring);

	const cJSON *j_type = cJSON_GetObjectItemCaseSensitive(root, "type");
	if (!cJSON_IsString(j_type) || !j_type->valuestring) {
		LOG_W("rejecting %s: type missing\n", exe_path);
		return false;
	}
	if (strcmp(j_type->valuestring, "3d") != 0 && strcmp(j_type->valuestring, "2d") != 0) {
		LOG_W("rejecting %s: type must be '3d' or '2d' (got '%s')\n", exe_path,
		      j_type->valuestring);
		return false;
	}
	copy_str(app->type, sizeof(app->type), j_type->valuestring);

	// Optional: category (default "app")
	const cJSON *j_cat = cJSON_GetObjectItemCaseSensitive(root, "category");
	if (cJSON_IsString(j_cat) && j_cat->valuestring) {
		copy_str(app->category, sizeof(app->category), j_cat->valuestring);
	} else {
		copy_str(app->category, sizeof(app->category), "app");
	}

	// Optional: display_mode (default "auto")
	const cJSON *j_mode = cJSON_GetObjectItemCaseSensitive(root, "display_mode");
	if (cJSON_IsString(j_mode) && j_mode->valuestring) {
		copy_str(app->display_mode, sizeof(app->display_mode), j_mode->valuestring);
	} else {
		copy_str(app->display_mode, sizeof(app->display_mode), "auto");
	}

	// Optional: description
	const cJSON *j_desc = cJSON_GetObjectItemCaseSensitive(root, "description");
	if (cJSON_IsString(j_desc) && j_desc->valuestring) {
		copy_str(app->description, sizeof(app->description), j_desc->valuestring);
	}

	// Optional: icon (2D)
	const cJSON *j_icon = cJSON_GetObjectItemCaseSensitive(root, "icon");
	if (cJSON_IsString(j_icon) && j_icon->valuestring) {
		resolve_icon_path(sidecar_dir, j_icon->valuestring, app->icon_path,
		                  sizeof(app->icon_path));
		if (!app->icon_path[0]) {
			LOG_W("rejecting %s: icon file not found ('%s')\n", exe_path,
			      j_icon->valuestring);
			return false;
		}
	}

	// Optional: icon_3d (stereo) — requires icon to also be set.
	const cJSON *j_icon3d = cJSON_GetObjectItemCaseSensitive(root, "icon_3d");
	if (cJSON_IsString(j_icon3d) && j_icon3d->valuestring) {
		if (!app->icon_path[0]) {
			LOG_W("rejecting %s: icon_3d requires icon to also be set\n", exe_path);
			return false;
		}
		resolve_icon_path(sidecar_dir, j_icon3d->valuestring, app->icon_3d_path,
		                  sizeof(app->icon_3d_path));
		if (!app->icon_3d_path[0]) {
			LOG_W("rejecting %s: icon_3d file not found ('%s')\n", exe_path,
			      j_icon3d->valuestring);
			return false;
		}

		const cJSON *j_layout = cJSON_GetObjectItemCaseSensitive(root, "icon_3d_layout");
		const char *layout = "sbs-lr"; // default
		if (cJSON_IsString(j_layout) && j_layout->valuestring) {
			layout = j_layout->valuestring;
		}
		if (strcmp(layout, "sbs-lr") != 0 && strcmp(layout, "sbs-rl") != 0 &&
		    strcmp(layout, "tb") != 0 && strcmp(layout, "bt") != 0) {
			LOG_W("rejecting %s: icon_3d_layout must be sbs-lr|sbs-rl|tb|bt (got '%s')\n",
			      exe_path, layout);
			return false;
		}
		copy_str(app->icon_3d_layout, sizeof(app->icon_3d_layout), layout);
	}

	copy_str(app->exe_path, sizeof(app->exe_path), exe_path);
	return true;
}

// -----------------------------------------------------------------------------
// Sidecar lookup per executable
// -----------------------------------------------------------------------------

// Given an exe path, look for "<basename>.displayxr.json" next to it.
// On success, parses and writes into @p app. Returns true on success.
static bool
try_load_app_from_exe(const char *exe_path, struct shell_scanned_app *app)
{
	// Derive sidecar_path by stripping .exe and appending .displayxr.json.
	char sidecar_path[SHELL_PATH_MAX];
	snprintf(sidecar_path, sizeof(sidecar_path), "%s", exe_path);

	size_t len = strlen(sidecar_path);
	if (len > 4 && _stricmp(sidecar_path + len - 4, ".exe") == 0) {
		sidecar_path[len - 4] = '\0';
	}
	if (strlen(sidecar_path) + strlen(".displayxr.json") + 1 > sizeof(sidecar_path)) {
		return false;
	}
	strcat(sidecar_path, ".displayxr.json");

	DWORD attrs = GetFileAttributesA(sidecar_path);
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		// No sidecar — silently skip.
		return false;
	}

	char *text = read_file_text(sidecar_path, 64 * 1024);
	if (!text) {
		LOG_W("failed to read sidecar: %s\n", sidecar_path);
		return false;
	}

	cJSON *root = cJSON_Parse(text);
	free(text);

	if (!root) {
		LOG_W("invalid JSON in sidecar: %s\n", sidecar_path);
		return false;
	}

	// Derive sidecar_dir for resolving relative icon paths.
	char sidecar_dir[SHELL_PATH_MAX];
	copy_str(sidecar_dir, sizeof(sidecar_dir), sidecar_path);
	char *last_sep = strrchr(sidecar_dir, '\\');
	if (!last_sep) last_sep = strrchr(sidecar_dir, '/');
	if (last_sep) *last_sep = '\0';

	bool ok = parse_sidecar(root, exe_path, sidecar_dir, app);
	cJSON_Delete(root);

	if (!ok) return false;

	// Sanity: for "3d" apps, warn if the PE doesn't import openxr_loader.dll.
	// This is not fatal — the sidecar is authoritative — but a mismatch
	// usually means the sidecar was placed next to the wrong binary.
	if (strcmp(app->type, "3d") == 0) {
		if (!shell_pe_exe_imports(exe_path, "openxr_loader.dll")) {
			LOG_W("warning: %s has type=3d but does not import openxr_loader.dll\n",
			      exe_path);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
// Directory walks
// -----------------------------------------------------------------------------

// Enumerate .exe files in a directory (non-recursive). For each, try to load
// the sidecar and append a scanned_app to @p out.
static void
scan_dir_for_exes(const char *dir, struct shell_scanned_app *out, int max_out, int *count)
{
	if (*count >= max_out) return;

	char glob[SHELL_PATH_MAX];
	snprintf(glob, sizeof(glob), "%s\\*.exe", dir);

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(glob, &fd);
	if (h == INVALID_HANDLE_VALUE) return;

	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

		char exe_path[SHELL_PATH_MAX];
		snprintf(exe_path, sizeof(exe_path), "%s\\%s", dir, fd.cFileName);

		if (*count >= max_out) break;

		if (try_load_app_from_exe(exe_path, &out[*count])) {
			(*count)++;
		}
	} while (FindNextFileA(h, &fd));

	FindClose(h);
}

// Enumerate immediate subdirectories of @p parent and scan each child's build/
// directory for executables. Matches the test_apps/*/build/ and demos/*/build/
// repo layouts.
static void
scan_parent_build_dirs(const char *parent, struct shell_scanned_app *out, int max_out,
                       int *count)
{
	if (*count >= max_out) return;

	char glob[SHELL_PATH_MAX];
	snprintf(glob, sizeof(glob), "%s\\*", parent);

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(glob, &fd);
	if (h == INVALID_HANDLE_VALUE) return;

	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;

		char build_dir[SHELL_PATH_MAX];
		snprintf(build_dir, sizeof(build_dir), "%s\\%s\\build", parent, fd.cFileName);

		DWORD attrs = GetFileAttributesA(build_dir);
		if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			continue;
		}

		scan_dir_for_exes(build_dir, out, max_out, count);

		if (*count >= max_out) break;
	} while (FindNextFileA(h, &fd));

	FindClose(h);
}

// -----------------------------------------------------------------------------
// Repo-root resolution
// -----------------------------------------------------------------------------

// Starting from @p start, walk up the directory tree (up to max_levels) looking
// for a directory that contains a "test_apps" subdirectory. Writes the match
// into @p out and returns true. Returns false if not found.
static bool
find_repo_root(const char *start, int max_levels, char *out, size_t out_size)
{
	char cur[SHELL_PATH_MAX];
	copy_str(cur, sizeof(cur), start);

	for (int i = 0; i <= max_levels; i++) {
		char probe[SHELL_PATH_MAX];
		snprintf(probe, sizeof(probe), "%s\\test_apps", cur);

		DWORD attrs = GetFileAttributesA(probe);
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			copy_str(out, out_size, cur);
			return true;
		}

		// Walk up one level.
		char *last_sep = strrchr(cur, '\\');
		if (!last_sep) last_sep = strrchr(cur, '/');
		if (!last_sep) return false;
		*last_sep = '\0';
		if (!cur[0]) return false;
	}
	return false;
}

// -----------------------------------------------------------------------------
// Public entry point
// -----------------------------------------------------------------------------

int
shell_scan_apps(const char *shell_exe_dir, struct shell_scanned_app *out, int max_out)
{
	if (!out || max_out <= 0) return 0;

	int count = 0;

	// 1) Dev-repo layout — walk up from the shell exe dir looking for test_apps/.
	char repo_root[SHELL_PATH_MAX] = {0};
	bool have_repo = false;
	if (shell_exe_dir && *shell_exe_dir) {
		have_repo = find_repo_root(shell_exe_dir, 6, repo_root, sizeof(repo_root));
	}
	// Also honor an explicit env override.
	const char *env_root = getenv("DISPLAYXR_REPO_ROOT");
	if (env_root && *env_root) {
		copy_str(repo_root, sizeof(repo_root), env_root);
		have_repo = true;
	}

	if (have_repo) {
		LOG_I("scanning repo root: %s\n", repo_root);

		char test_apps[SHELL_PATH_MAX];
		snprintf(test_apps, sizeof(test_apps), "%s\\test_apps", repo_root);
		scan_parent_build_dirs(test_apps, out, max_out, &count);

		char demos[SHELL_PATH_MAX];
		snprintf(demos, sizeof(demos), "%s\\demos", repo_root);
		scan_parent_build_dirs(demos, out, max_out, &count);

		char pkg_bin[SHELL_PATH_MAX];
		snprintf(pkg_bin, sizeof(pkg_bin), "%s\\_package\\bin", repo_root);
		scan_dir_for_exes(pkg_bin, out, max_out, &count);
	} else {
		LOG_I("no repo root found — skipping dev paths\n");
	}

	// 2) Production install path.
	const char *pf = getenv("ProgramFiles");
	if (pf && *pf) {
		char prod[SHELL_PATH_MAX];
		snprintf(prod, sizeof(prod), "%s\\DisplayXR\\apps", pf);
		DWORD attrs = GetFileAttributesA(prod);
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			LOG_I("scanning production path: %s\n", prod);
			// v1: flat layout — exes directly under DisplayXR\apps\. Future
			// installer shape (per-app subdirectories) can be added when it
			// exists.
			scan_dir_for_exes(prod, out, max_out, &count);
		}
	}

	LOG_I("scan complete: %d app(s) found\n", count);
	return count;
}

#else // !_WIN32

int
shell_scan_apps(const char *shell_exe_dir, struct shell_scanned_app *out, int max_out)
{
	(void)shell_exe_dir;
	(void)out;
	(void)max_out;
	return 0;
}

#endif
