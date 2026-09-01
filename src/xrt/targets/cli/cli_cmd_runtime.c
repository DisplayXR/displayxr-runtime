// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `runtime` subcommand — inspect / switch the active OpenXR runtime.
 *
 *   runtime status     Resolve the FULL loader precedence chain (env var, HKCU,
 *                      HKLM, 64-bit and 32-bit views), name the winner, and
 *                      flag every conflict that can make an app pick up a
 *                      runtime you did not intend.
 *   runtime list       Enumerate every OpenXR runtime on the box — including
 *                      ones that never registered in AvailableRuntimes, which
 *                      a registry-only switcher cannot see.
 *   runtime activate   Point ActiveRuntime at DisplayXR (or at an explicit
 *                      manifest) in EVERY view the loader consults, and clear
 *                      the shadows that would otherwise silently win.
 *   runtime restore    Put back the value the last `activate` replaced.
 *
 * Supersedes the retired DisplayXRSwitcher (#378). That tool listed only the
 * runtimes it found in the Khronos `AvailableRuntimes` key, so a runtime that
 * simply overwrote `ActiveRuntime` without registering itself was invisible —
 * including, on the box that motivated this rewrite, the very runtime that was
 * actively holding the key. `list` therefore sweeps the filesystem too, and
 * `status` always reports the live `ActiveRuntime` value whether or not the
 * thing it points at is a runtime we know about.
 *
 * Windows-only (ActiveRuntime is a Khronos-on-Windows registry concept); a
 * no-op note on other platforms.
 *
 * @author David Fattal
 */

#include "cli_common.h"

#include "xrt/xrt_config_os.h"

#include <stdio.h>
#include <string.h>

#define P(...) printf(__VA_ARGS__)

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

static const wchar_t *KHRONOS_KEY = L"Software\\Khronos\\OpenXR\\1";
static const wchar_t *AVAILABLE_KEY = L"Software\\Khronos\\OpenXR\\1\\AvailableRuntimes";
static const wchar_t *DXR_KEY = L"Software\\DisplayXR\\Runtime";
static const wchar_t *DEFAULT_MANIFEST = L"C:\\Program Files\\DisplayXR\\Runtime\\DisplayXR_win64.json";

//! Budget for the `list` filesystem sweep. A runtime manifest is a few hundred
//! bytes, so the size gate below prunes essentially every other .json on disk;
//! these caps only bound pathological trees (huge Steam libraries, etc.).
#define SCAN_MAX_DEPTH 6
#define SCAN_MAX_MS 10000
#define SCAN_MAX_MANIFEST_BYTES 8192

#define MAX_RUNTIMES 64

/*!
 * One discovered runtime. @p path is the manifest; everything else is derived
 * from it or from where we found it.
 */
struct rt_entry
{
	wchar_t path[MAX_PATH];
	char path_u8[MAX_PATH * 2];
	char name[128];    //!< "name" from the manifest, or "" if unreadable.
	char lib_u8[1024]; //!< Resolved library_path.
	char sources[512]; //!< Comma-separated list of where we saw it.
	bool file_exists;
	bool lib_exists;
	bool is_dxr;
	bool winner_64; //!< What a 64-bit app resolves to.
	bool winner_32; //!< What a 32-bit app resolves to.
};

static struct rt_entry g_rt[MAX_RUNTIMES];
static int g_rt_count = 0;
static bool g_scan_truncated = false;


/*
 *
 * Small helpers.
 *
 */

static void
to_u8(const wchar_t *w, char *out, int cap)
{
	out[0] = '\0';
	WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
	out[cap - 1] = '\0';
}

//! Read a REG_SZ, null-terminating defensively (RegQueryValueEx need not).
static bool
reg_read_sz(HKEY root, const wchar_t *subkey, REGSAM view, const wchar_t *value, wchar_t *out, DWORD cap_chars)
{
	out[0] = L'\0';
	HKEY k;
	if (RegOpenKeyExW(root, subkey, 0, KEY_READ | view, &k) != ERROR_SUCCESS) {
		return false;
	}
	DWORD bytes = cap_chars * (DWORD)sizeof(wchar_t);
	DWORD type = 0;
	LSTATUS rc = RegQueryValueExW(k, value, NULL, &type, (LPBYTE)out, &bytes);
	RegCloseKey(k);
	if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
		out[0] = L'\0';
		return false;
	}
	DWORD chars = bytes / (DWORD)sizeof(wchar_t);
	if (chars >= cap_chars) {
		chars = cap_chars - 1;
	}
	out[chars] = L'\0';
	return out[0] != L'\0';
}

/*!
 * Pull a string value out of a flat JSON object. Manifests are tiny and
 * fixed-shape, so this beats linking a parser into the CLI; it understands the
 * one escape (`\\`) that actually shows up in a Windows `library_path`.
 */
static bool
json_str(const char *json, const char *key, char *out, int cap)
{
	out[0] = '\0';
	char pat[64];
	_snprintf_s(pat, sizeof(pat), _TRUNCATE, "\"%s\"", key);
	const char *p = strstr(json, pat);
	if (p == NULL) {
		return false;
	}
	p += strlen(pat);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	if (*p != ':') {
		return false;
	}
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	if (*p != '"') {
		return false;
	}
	p++;
	int n = 0;
	while (*p != '\0' && *p != '"' && n < cap - 1) {
		if (*p == '\\' && p[1] != '\0') {
			p++; // Collapse the escape; `\\` -> `\`, and nothing else occurs here.
		}
		out[n++] = *p++;
	}
	out[n] = '\0';
	return n > 0;
}

//! True if @p path has a drive letter or is a UNC path.
static bool
is_abs_path(const char *path)
{
	if (path[0] == '\\' && path[1] == '\\') {
		return true;
	}
	return path[0] != '\0' && path[1] == ':';
}

/*!
 * Read a manifest and fill in name / library_path / whether the DLL is really
 * there. A manifest pointing at a deleted DLL is a distinct failure from a
 * missing manifest — both surface to the user as "Failed to initialize
 * OpenXR", so the tool has to tell them apart.
 */
static void
probe_manifest(struct rt_entry *e)
{
	e->file_exists = GetFileAttributesW(e->path) != INVALID_FILE_ATTRIBUTES;
	e->name[0] = '\0';
	e->lib_u8[0] = '\0';
	e->lib_exists = false;
	if (!e->file_exists) {
		return;
	}

	HANDLE f = CreateFileW(e->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		return;
	}
	char buf[SCAN_MAX_MANIFEST_BYTES + 1];
	DWORD got = 0;
	BOOL ok = ReadFile(f, buf, SCAN_MAX_MANIFEST_BYTES, &got, NULL);
	CloseHandle(f);
	if (!ok) {
		return;
	}
	buf[got] = '\0';

	json_str(buf, "name", e->name, (int)sizeof(e->name));

	char lib[512];
	if (!json_str(buf, "library_path", lib, (int)sizeof(lib))) {
		return;
	}
	if (is_abs_path(lib)) {
		_snprintf_s(e->lib_u8, sizeof(e->lib_u8), _TRUNCATE, "%s", lib);
	} else {
		// Relative to the manifest's own directory, per the loader spec.
		char dir[MAX_PATH * 2];
		to_u8(e->path, dir, (int)sizeof(dir));
		char *slash = strrchr(dir, '\\');
		if (slash != NULL) {
			*slash = '\0';
		}
		const char *rel = lib;
		if (rel[0] == '.' && rel[1] == '\\') {
			rel += 2;
		}
		_snprintf_s(e->lib_u8, sizeof(e->lib_u8), _TRUNCATE, "%s\\%s", dir, rel);
	}

	wchar_t wlib[MAX_PATH * 2];
	MultiByteToWideChar(CP_UTF8, 0, e->lib_u8, -1, wlib, MAX_PATH * 2);
	e->lib_exists = GetFileAttributesW(wlib) != INVALID_FILE_ATTRIBUTES;
}


/*
 *
 * DisplayXR's own manifest.
 *
 */

//! Prefer the installer-written InstallPath, then the default location.
static bool
find_displayxr_manifest(wchar_t *out, int cap_chars)
{
	wchar_t install[MAX_PATH];
	if (reg_read_sz(HKEY_LOCAL_MACHINE, DXR_KEY, KEY_WOW64_64KEY, L"InstallPath", install, MAX_PATH)) {
		_snwprintf_s(out, cap_chars, _TRUNCATE, L"%s\\DisplayXR_win64.json", install);
		if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) {
			return true;
		}
	}
	_snwprintf_s(out, cap_chars, _TRUNCATE, L"%s", DEFAULT_MANIFEST);
	return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

/*!
 * Is this manifest DisplayXR's?
 *
 * Deliberately NOT a `strstr(path, "DisplayXR")` test — that was the old
 * check, and it says yes to any runtime that happens to sit under a directory
 * with our name in it. Match the resolved install manifest by path, or trust
 * the manifest's declared name.
 */
static bool
entry_is_dxr(const struct rt_entry *e)
{
	wchar_t dxr[MAX_PATH];
	if (find_displayxr_manifest(dxr, MAX_PATH) && _wcsicmp(dxr, e->path) == 0) {
		return true;
	}
	return _stricmp(e->name, "DisplayXR") == 0;
}


/*
 *
 * The runtime table.
 *
 */

//! Add @p path, or fold @p source into the existing row if we already have it.
static struct rt_entry *
add_runtime(const wchar_t *path, const char *source)
{
	if (path == NULL || path[0] == L'\0') {
		return NULL;
	}
	for (int i = 0; i < g_rt_count; i++) {
		if (_wcsicmp(g_rt[i].path, path) == 0) {
			if (source != NULL && strstr(g_rt[i].sources, source) == NULL) {
				size_t used = strlen(g_rt[i].sources);
				_snprintf_s(g_rt[i].sources + used, sizeof(g_rt[i].sources) - used, _TRUNCATE, "%s%s",
				            used > 0 ? ", " : "", source);
			}
			return &g_rt[i];
		}
	}
	if (g_rt_count >= MAX_RUNTIMES) {
		return NULL;
	}
	struct rt_entry *e = &g_rt[g_rt_count++];
	memset(e, 0, sizeof(*e));
	_snwprintf_s(e->path, MAX_PATH, _TRUNCATE, L"%s", path);
	to_u8(e->path, e->path_u8, (int)sizeof(e->path_u8));
	if (source != NULL) {
		_snprintf_s(e->sources, sizeof(e->sources), _TRUNCATE, "%s", source);
	}
	probe_manifest(e);
	e->is_dxr = entry_is_dxr(e);
	return e;
}

//! Fold in every path listed under an `AvailableRuntimes` key.
static void
add_available_runtimes(HKEY root, REGSAM view, const char *label)
{
	HKEY k;
	if (RegOpenKeyExW(root, AVAILABLE_KEY, 0, KEY_READ | view, &k) != ERROR_SUCCESS) {
		return;
	}
	for (DWORD i = 0;; i++) {
		wchar_t name[MAX_PATH];
		DWORD name_chars = MAX_PATH;
		if (RegEnumValueW(k, i, name, &name_chars, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
			break;
		}
		// The value NAME is the manifest path; the DWORD data is 0 for enabled.
		add_runtime(name, label);
	}
	RegCloseKey(k);
}


/*
 *
 * Filesystem sweep — the part a registry-only switcher was missing.
 *
 */

static bool
name_is_noise(const wchar_t *n)
{
	static const wchar_t *suffixes[] = {L".runtimeconfig.json", L".deps.json"};
	static const wchar_t *exact[] = {L"package.json", L"package-lock.json", L"tsconfig.json", L"composer.json",
	                                 L"asconfig.json"};
	size_t len = wcslen(n);
	for (int i = 0; i < (int)(sizeof(suffixes) / sizeof(suffixes[0])); i++) {
		size_t sl = wcslen(suffixes[i]);
		if (len >= sl && _wcsicmp(n + len - sl, suffixes[i]) == 0) {
			return true;
		}
	}
	for (int i = 0; i < (int)(sizeof(exact) / sizeof(exact[0])); i++) {
		if (_wcsicmp(n, exact[i]) == 0) {
			return true;
		}
	}
	return false;
}

static bool
dir_is_noise(const wchar_t *n)
{
	static const wchar_t *skip[] = {L"node_modules", L".git",     L"WinSxS",   L"assembly", L"Package Cache",
	                                L"Installer",    L"dotnet",   L"Fonts",    L"INF",      L"servicing",
	                                L"SysWOW64",     L"System32", L"Temp",     L"cache",    L"cache2",
	                                L"CrashReports", L"Logs"};
	for (int i = 0; i < (int)(sizeof(skip) / sizeof(skip[0])); i++) {
		if (_wcsicmp(n, skip[i]) == 0) {
			return true;
		}
	}
	return false;
}

/*!
 * Recursive sweep for OpenXR runtime manifests. The cheap gate is the file
 * size straight out of the directory entry — a real manifest is a few hundred
 * bytes — so we only ever open a handful of candidates per tree.
 */
static void
scan_dir(const wchar_t *dir, int depth, ULONGLONG deadline)
{
	if (depth > SCAN_MAX_DEPTH) {
		return;
	}
	if (GetTickCount64() > deadline) {
		g_scan_truncated = true;
		return;
	}

	wchar_t glob[MAX_PATH];
	_snwprintf_s(glob, MAX_PATH, _TRUNCATE, L"%s\\*", dir);
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(glob, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}

	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
			continue;
		}
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
			continue; // Never follow junctions; they loop.
		}

		wchar_t full[MAX_PATH];
		if (_snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName) < 0) {
			continue; // Path too long to be a real manifest location.
		}

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (!dir_is_noise(fd.cFileName)) {
				scan_dir(full, depth + 1, deadline);
			}
			continue;
		}

		size_t len = wcslen(fd.cFileName);
		if (len < 6 || _wcsicmp(fd.cFileName + len - 5, L".json") != 0) {
			continue;
		}
		if (fd.nFileSizeHigh != 0 || fd.nFileSizeLow > SCAN_MAX_MANIFEST_BYTES) {
			continue;
		}
		if (name_is_noise(fd.cFileName)) {
			continue;
		}

		// Cheap content gate before committing a table slot: a runtime manifest
		// declares a library_path inside a "runtime" object.
		HANDLE f = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (f == INVALID_HANDLE_VALUE) {
			continue;
		}
		char buf[SCAN_MAX_MANIFEST_BYTES + 1];
		DWORD got = 0;
		BOOL ok = ReadFile(f, buf, SCAN_MAX_MANIFEST_BYTES, &got, NULL);
		CloseHandle(f);
		if (!ok) {
			continue;
		}
		buf[got] = '\0';
		if (strstr(buf, "\"library_path\"") == NULL || strstr(buf, "\"runtime\"") == NULL) {
			continue;
		}

		add_runtime(full, "disk");
	} while (FindNextFileW(h, &fd));

	FindClose(h);
}

static void
scan_filesystem(void)
{
	static const wchar_t *vars[] = {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData",
	                                L"LOCALAPPDATA", L"APPDATA",           L"USERPROFILE"};
	ULONGLONG deadline = GetTickCount64() + SCAN_MAX_MS;
	for (int i = 0; i < (int)(sizeof(vars) / sizeof(vars[0])); i++) {
		wchar_t root[MAX_PATH];
		DWORD n = GetEnvironmentVariableW(vars[i], root, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) {
			continue;
		}
		scan_dir(root, 0, deadline);
	}
}


/*
 *
 * Loader precedence.
 *
 */

/*!
 * The places the Khronos loader looks, in the order it looks. The env var
 * wins; then HKCU shadows HKLM; and a 32-bit app reads the WOW6432Node view of
 * whichever hive won. Getting this wrong is exactly how a box ends up
 * "configured correctly" and still running someone else's runtime.
 */
struct precedence
{
	wchar_t env[MAX_PATH];
	wchar_t hkcu64[MAX_PATH];
	wchar_t hkcu32[MAX_PATH];
	wchar_t hklm64[MAX_PATH];
	wchar_t hklm32[MAX_PATH];
	bool has_env, has_hkcu64, has_hkcu32, has_hklm64, has_hklm32;
	const wchar_t *win64; //!< What a 64-bit app gets.
	const wchar_t *win32; //!< What a 32-bit app gets.
	const char *win64_src;
	const char *win32_src;
};

static void
resolve_precedence(struct precedence *p)
{
	memset(p, 0, sizeof(*p));

	DWORD n = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", p->env, MAX_PATH);
	p->has_env = n > 0 && n < MAX_PATH;

	p->has_hkcu64 =
	    reg_read_sz(HKEY_CURRENT_USER, KHRONOS_KEY, KEY_WOW64_64KEY, L"ActiveRuntime", p->hkcu64, MAX_PATH);
	p->has_hkcu32 =
	    reg_read_sz(HKEY_CURRENT_USER, KHRONOS_KEY, KEY_WOW64_32KEY, L"ActiveRuntime", p->hkcu32, MAX_PATH);
	p->has_hklm64 =
	    reg_read_sz(HKEY_LOCAL_MACHINE, KHRONOS_KEY, KEY_WOW64_64KEY, L"ActiveRuntime", p->hklm64, MAX_PATH);
	p->has_hklm32 =
	    reg_read_sz(HKEY_LOCAL_MACHINE, KHRONOS_KEY, KEY_WOW64_32KEY, L"ActiveRuntime", p->hklm32, MAX_PATH);

	if (p->has_env) {
		p->win64 = p->env;
		p->win64_src = "XR_RUNTIME_JSON";
		p->win32 = p->env;
		p->win32_src = "XR_RUNTIME_JSON";
		return;
	}
	if (p->has_hkcu64) {
		p->win64 = p->hkcu64;
		p->win64_src = "HKCU (64-bit view)";
	} else if (p->has_hklm64) {
		p->win64 = p->hklm64;
		p->win64_src = "HKLM (64-bit view)";
	}
	if (p->has_hkcu32) {
		p->win32 = p->hkcu32;
		p->win32_src = "HKCU (WOW6432Node)";
	} else if (p->has_hklm32) {
		p->win32 = p->hklm32;
		p->win32_src = "HKLM (WOW6432Node)";
	}
}

//! Short human label for a manifest path, for the one-line summaries.
static void
describe(const wchar_t *path, char *out, int cap)
{
	if (path == NULL || path[0] == L'\0') {
		_snprintf_s(out, cap, _TRUNCATE, "<unset>");
		return;
	}
	struct rt_entry e;
	memset(&e, 0, sizeof(e));
	_snwprintf_s(e.path, MAX_PATH, _TRUNCATE, L"%s", path);
	to_u8(e.path, e.path_u8, (int)sizeof(e.path_u8));
	probe_manifest(&e);
	if (!e.file_exists) {
		_snprintf_s(out, cap, _TRUNCATE, "MISSING FILE");
		return;
	}
	_snprintf_s(out, cap, _TRUNCATE, "%s%s", e.name[0] != '\0' ? e.name : "unnamed",
	            e.lib_exists ? "" : " (library missing!)");
}

//! True if the manifest at @p path is DisplayXR's.
static bool
path_is_dxr(const wchar_t *path)
{
	if (path == NULL || path[0] == L'\0') {
		return false;
	}
	struct rt_entry e;
	memset(&e, 0, sizeof(e));
	_snwprintf_s(e.path, MAX_PATH, _TRUNCATE, L"%s", path);
	probe_manifest(&e);
	return entry_is_dxr(&e);
}

static void
print_slot(const char *label, bool has, const wchar_t *value, bool is_winner)
{
	char v[MAX_PATH * 2] = "<unset>";
	if (has) {
		to_u8(value, v, (int)sizeof(v));
	}
	P("  %-26s %s\n", label, v);
	if (has) {
		char who[192];
		describe(value, who, (int)sizeof(who));
		P("  %-26s   -> %s%s\n", "", who, is_winner ? "   [WINNER]" : "");
	}
}


/*
 *
 * status
 *
 */

static int
cmd_status(void)
{
	struct precedence p;
	resolve_precedence(&p);

	P("OpenXR loader precedence (first match wins):\n\n");
	print_slot("XR_RUNTIME_JSON (env)", p.has_env, p.env, p.has_env);
	print_slot("HKCU  ...\\OpenXR\\1", p.has_hkcu64, p.hkcu64, !p.has_env && p.has_hkcu64);
	print_slot("HKLM  ...\\OpenXR\\1", p.has_hklm64, p.hklm64, !p.has_env && !p.has_hkcu64 && p.has_hklm64);
	P("\n  -- 32-bit apps read the WOW6432Node view instead --\n");
	print_slot("HKCU  ...WOW6432Node", p.has_hkcu32, p.hkcu32, !p.has_env && p.has_hkcu32);
	print_slot("HKLM  ...WOW6432Node", p.has_hklm32, p.hklm32, !p.has_env && !p.has_hkcu32 && p.has_hklm32);

	char who64[192], who32[192];
	describe(p.win64, who64, (int)sizeof(who64));
	describe(p.win32, who32, (int)sizeof(who32));
	bool dxr64 = path_is_dxr(p.win64);

	P("\nEffective runtime:\n");
	P("  64-bit apps : %s   [via %s]\n", who64, p.win64_src != NULL ? p.win64_src : "nothing set");
	P("  32-bit apps : %s   [via %s]\n", who32, p.win32_src != NULL ? p.win32_src : "nothing set");
	P("\n  DisplayXR %s the active runtime for 64-bit apps.\n", dxr64 ? "IS" : "is NOT");

	// Conflicts. Each of these has bitten a real box, so name them explicitly
	// rather than leaving the reader to diff two long paths by eye.
	if (p.has_env) {
		P("\n  ! XR_RUNTIME_JSON is set and overrides BOTH registry views.\n");
		P("    The loader ignores it in ELEVATED processes, so an admin app and a normal\n");
		P("    app on this box can be running two different runtimes right now.\n");
	}
	if (p.has_hkcu64 && p.has_hklm64 && _wcsicmp(p.hkcu64, p.hklm64) != 0) {
		P("\n  ! HKCU shadows HKLM (64-bit): the per-user value wins over the machine one,\n");
		P("    so a machine-wide installer appears to have had no effect.\n");
	}
	if (p.win64 != NULL && p.win32 != NULL && _wcsicmp(p.win64, p.win32) != 0) {
		P("\n  ! 64-bit and 32-bit views DISAGREE. 64-bit apps get '%s'; 32-bit apps get\n", who64);
		P("    '%s'. An installer that wrote only one view leaves the box like this.\n", who32);
	}
	if (p.win64 == NULL) {
		P("\n  ! No active runtime is set at all — every OpenXR app will fail to start.\n");
		P("    Some uninstallers clear the key instead of restoring the previous value.\n");
	} else if (strstr(who64, "MISSING") != NULL || strstr(who64, "library missing") != NULL) {
		P("\n  ! The active manifest is broken (file or library missing) — apps will fail\n");
		P("    with 'Failed to initialize OpenXR'.\n");
	}
	if (!dxr64 && p.win64 != NULL) {
		P("\n  Another runtime holds the key. To take it back:\n");
		P("      displayxr-cli runtime activate      (elevated)\n");
		P("  To see everything installed on this box:\n");
		P("      displayxr-cli runtime list\n");
	}

	return dxr64 ? 0 : 1;
}


/*
 *
 * list
 *
 */

static void
collect(bool do_scan)
{
	struct precedence p;
	resolve_precedence(&p);

	// The live values first, so the active runtime is always in the table even
	// when it registered nowhere — the blind spot that motivated this rewrite.
	if (p.has_env) {
		add_runtime(p.env, "XR_RUNTIME_JSON");
	}
	if (p.has_hkcu64) {
		add_runtime(p.hkcu64, "HKCU active");
	}
	if (p.has_hkcu32) {
		add_runtime(p.hkcu32, "HKCU active (32)");
	}
	if (p.has_hklm64) {
		add_runtime(p.hklm64, "HKLM active");
	}
	if (p.has_hklm32) {
		add_runtime(p.hklm32, "HKLM active (32)");
	}

	add_available_runtimes(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, "registered");
	add_available_runtimes(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, "registered (32)");
	add_available_runtimes(HKEY_CURRENT_USER, KEY_WOW64_64KEY, "registered (user)");
	add_available_runtimes(HKEY_CURRENT_USER, KEY_WOW64_32KEY, "registered (user,32)");

	wchar_t dxr[MAX_PATH];
	if (find_displayxr_manifest(dxr, MAX_PATH)) {
		add_runtime(dxr, "installed");
	}

	if (do_scan) {
		scan_filesystem();
	}

	for (int i = 0; i < g_rt_count; i++) {
		if (p.win64 != NULL && _wcsicmp(g_rt[i].path, p.win64) == 0) {
			g_rt[i].winner_64 = true;
		}
		if (p.win32 != NULL && _wcsicmp(g_rt[i].path, p.win32) == 0) {
			g_rt[i].winner_32 = true;
		}
	}
}

static void
json_escape(const char *in, char *out, int cap)
{
	int n = 0;
	for (const char *p = in; *p != '\0' && n < cap - 8; p++) {
		if (*p == '\\' || *p == '"') {
			out[n++] = '\\';
			out[n++] = *p;
		} else if ((unsigned char)*p < 0x20) {
			n += _snprintf_s(out + n, cap - n, _TRUNCATE, "\\u%04x", (unsigned char)*p);
		} else {
			out[n++] = *p;
		}
	}
	out[n] = '\0';
}

static int
cmd_list(int argc, const char **argv)
{
	bool as_json = cli_has_flag(argc, argv, "--json");
	bool do_scan = !cli_has_flag(argc, argv, "--no-scan");

	collect(do_scan);

	if (as_json) {
		char esc[MAX_PATH * 3];
		P("{\n  \"runtimes\": [\n");
		for (int i = 0; i < g_rt_count; i++) {
			const struct rt_entry *e = &g_rt[i];
			P("    {");
			json_escape(e->path_u8, esc, (int)sizeof(esc));
			P("\"manifest\": \"%s\", ", esc);
			json_escape(e->name, esc, (int)sizeof(esc));
			P("\"name\": \"%s\", ", esc);
			json_escape(e->sources, esc, (int)sizeof(esc));
			P("\"sources\": \"%s\", ", esc);
			P("\"manifest_exists\": %s, ", e->file_exists ? "true" : "false");
			P("\"library_exists\": %s, ", e->lib_exists ? "true" : "false");
			P("\"is_displayxr\": %s, ", e->is_dxr ? "true" : "false");
			P("\"active_64\": %s, ", e->winner_64 ? "true" : "false");
			P("\"active_32\": %s}%s\n", e->winner_32 ? "true" : "false", i + 1 < g_rt_count ? "," : "");
		}
		P("  ],\n  \"scanned\": %s,\n  \"scan_truncated\": %s\n}\n", do_scan ? "true" : "false",
		  g_scan_truncated ? "true" : "false");
		return 0;
	}

	P("OpenXR runtimes on this machine%s:\n\n", do_scan ? "" : " (registry only, --no-scan)");
	if (g_rt_count == 0) {
		P("  none found.\n");
		return 1;
	}
	for (int i = 0; i < g_rt_count; i++) {
		const struct rt_entry *e = &g_rt[i];
		P("%s [%d] %s%s\n", e->winner_64 ? "*" : " ", i, e->name[0] != '\0' ? e->name : "(unnamed)",
		  e->is_dxr ? "   <- DisplayXR" : "");
		P("       manifest : %s\n", e->path_u8);
		if (!e->file_exists) {
			P("       status   : MISSING — the manifest file is gone\n");
		} else if (!e->lib_exists) {
			P("       status   : BROKEN — library_path does not exist (%s)\n", e->lib_u8);
		} else {
			P("       status   : ok\n");
		}
		P("       seen in  : %s\n", e->sources[0] != '\0' ? e->sources : "?");
		if (e->winner_64 || e->winner_32) {
			P("       ACTIVE   : %s%s%s\n", e->winner_64 ? "64-bit apps" : "",
			  (e->winner_64 && e->winner_32) ? " + " : "", e->winner_32 ? "32-bit apps" : "");
		}
		P("\n");
	}
	P("  '*' = what a 64-bit OpenXR app resolves to right now.\n");
	P("  A row seen only in 'disk' never registered in AvailableRuntimes, so a\n");
	P("  registry-only switcher cannot see it — including, potentially, the one\n");
	P("  currently holding the key.\n");
	if (g_scan_truncated) {
		P("\n  ! The filesystem sweep hit its %d s budget and may be incomplete.\n", SCAN_MAX_MS / 1000);
	}
	P("\n  Switch with:  displayxr-cli runtime activate [<manifest>]   (elevated)\n");
	return 0;
}


/*
 *
 * activate / restore
 *
 */

static bool
write_active(HKEY root, REGSAM view, const wchar_t *manifest)
{
	HKEY k;
	if (RegCreateKeyExW(root, KHRONOS_KEY, 0, NULL, 0, KEY_SET_VALUE | view, NULL, &k, NULL) != ERROR_SUCCESS) {
		return false;
	}
	DWORD bytes = (DWORD)((wcslen(manifest) + 1) * sizeof(wchar_t));
	LSTATUS rc = RegSetValueExW(k, L"ActiveRuntime", 0, REG_SZ, (const BYTE *)manifest, bytes);
	RegCloseKey(k);
	return rc == ERROR_SUCCESS;
}

static void
delete_active(HKEY root, REGSAM view)
{
	HKEY k;
	if (RegOpenKeyExW(root, KHRONOS_KEY, 0, KEY_SET_VALUE | view, &k) != ERROR_SUCCESS) {
		return;
	}
	RegDeleteValueW(k, L"ActiveRuntime");
	RegCloseKey(k);
}

//! Advertise a manifest in AvailableRuntimes so OTHER vendors' switchers list it.
static void
register_available(const wchar_t *manifest, REGSAM view)
{
	HKEY k;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, AVAILABLE_KEY, 0, NULL, 0, KEY_SET_VALUE | view, NULL, &k, NULL) !=
	    ERROR_SUCCESS) {
		return;
	}
	DWORD zero = 0; // 0 == enabled, per the loader's convention.
	RegSetValueExW(k, manifest, 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
	RegCloseKey(k);
}

static int
cmd_activate(int argc, const char **argv)
{
	wchar_t manifest[MAX_PATH];

	// An explicit manifest makes this a real switcher: it can hand the key to
	// another runtime, which is what you want after testing against ours.
	const char *want = NULL;
	for (int i = 3; i < argc; i++) {
		if (argv[i][0] != '-') {
			want = argv[i];
			break;
		}
	}
	if (want != NULL) {
		wchar_t rel[MAX_PATH];
		MultiByteToWideChar(CP_UTF8, 0, want, -1, rel, MAX_PATH);
		if (GetFullPathNameW(rel, MAX_PATH, manifest, NULL) == 0) {
			P("FAIL: cannot resolve '%s'.\n", want);
			return 1;
		}
		if (GetFileAttributesW(manifest) == INVALID_FILE_ATTRIBUTES) {
			char u8[MAX_PATH * 2];
			to_u8(manifest, u8, (int)sizeof(u8));
			P("FAIL: no such manifest: %s\n", u8);
			return 1;
		}
	} else if (!find_displayxr_manifest(manifest, MAX_PATH)) {
		P("FAIL: could not find DisplayXR's manifest (DisplayXR_win64.json) — is the runtime installed?\n");
		return 1;
	}

	// Refuse to point the key at something that cannot load. Otherwise the
	// failure surfaces much later as an unexplained "Failed to initialize
	// OpenXR" in whatever app the user tries next.
	struct rt_entry target;
	memset(&target, 0, sizeof(target));
	_snwprintf_s(target.path, MAX_PATH, _TRUNCATE, L"%s", manifest);
	to_u8(target.path, target.path_u8, (int)sizeof(target.path_u8));
	probe_manifest(&target);
	if (!target.lib_exists) {
		P("FAIL: %s declares a library that does not exist:\n      %s\n", target.path_u8, target.lib_u8);
		P("      Refusing to activate a runtime that cannot load.\n");
		return 1;
	}

	// Save what we are replacing so `runtime restore` can undo it.
	wchar_t prev[MAX_PATH];
	if (reg_read_sz(HKEY_LOCAL_MACHINE, KHRONOS_KEY, KEY_WOW64_64KEY, L"ActiveRuntime", prev, MAX_PATH) &&
	    _wcsicmp(prev, manifest) != 0) {
		HKEY k;
		if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, DXR_KEY, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &k,
		                    NULL) == ERROR_SUCCESS) {
			DWORD bytes = (DWORD)((wcslen(prev) + 1) * sizeof(wchar_t));
			RegSetValueExW(k, L"PrevActiveRuntime", 0, REG_SZ, (const BYTE *)prev, bytes);
			RegCloseKey(k);
		}
	}

	if (!write_active(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, manifest)) {
		P("FAIL: cannot write HKLM ActiveRuntime — run from an ELEVATED terminal.\n");
		return 1;
	}
	// Write the 32-bit view too. Leaving it stale is how a box ends up running
	// one runtime for 64-bit apps and a different one for 32-bit apps.
	bool ok32 = write_active(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, manifest);

	// A per-user value silently outranks everything we just wrote.
	struct precedence p;
	resolve_precedence(&p);
	bool cleared_hkcu = false;
	if ((p.has_hkcu64 && _wcsicmp(p.hkcu64, manifest) != 0) ||
	    (p.has_hkcu32 && _wcsicmp(p.hkcu32, manifest) != 0)) {
		delete_active(HKEY_CURRENT_USER, KEY_WOW64_64KEY);
		delete_active(HKEY_CURRENT_USER, KEY_WOW64_32KEY);
		cleared_hkcu = true;
	}

	register_available(manifest, KEY_WOW64_64KEY);
	register_available(manifest, KEY_WOW64_32KEY);

	P("Active OpenXR runtime set:\n  %s\n", target.path_u8);
	P("  HKLM 64-bit view : ok\n");
	P("  HKLM 32-bit view : %s\n", ok32 ? "ok" : "FAILED (32-bit apps may still use the old runtime)");
	if (cleared_hkcu) {
		P("  HKCU shadow      : cleared (a per-user value was overriding the machine one)\n");
	}
	P("  AvailableRuntimes: registered (other vendors' switchers can now see it)\n");

	// Re-resolve: the env var can still beat everything we just did.
	resolve_precedence(&p);
	if (p.has_env) {
		char envu8[MAX_PATH * 2];
		to_u8(p.env, envu8, (int)sizeof(envu8));
		P("\n  ! XR_RUNTIME_JSON is set to %s and OVERRIDES what was just written for\n", envu8);
		P("    non-elevated processes. Clear it if you want the registry to win.\n");
	}
	return 0;
}

static int
cmd_restore(void)
{
	wchar_t prev[MAX_PATH];
	if (!reg_read_sz(HKEY_LOCAL_MACHINE, DXR_KEY, KEY_WOW64_64KEY, L"PrevActiveRuntime", prev, MAX_PATH)) {
		P("Nothing to restore — no previous ActiveRuntime was recorded.\n");
		return 1;
	}
	if (!write_active(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, prev)) {
		P("FAIL: cannot write HKLM ActiveRuntime — run from an ELEVATED terminal.\n");
		return 1;
	}
	write_active(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, prev);

	char u8[MAX_PATH * 2];
	to_u8(prev, u8, (int)sizeof(u8));
	P("Restored the previous active OpenXR runtime:\n  %s\n", u8);

	HKEY k;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, DXR_KEY, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
		RegDeleteValueW(k, L"PrevActiveRuntime");
		RegCloseKey(k);
	}
	return 0;
}

int
cli_cmd_runtime(int argc, const char **argv)
{
	if (argc >= 3 && strcmp(argv[2], "activate") == 0) {
		return cmd_activate(argc, argv);
	}
	if (argc >= 3 && strcmp(argv[2], "status") == 0) {
		return cmd_status();
	}
	if (argc >= 3 && strcmp(argv[2], "list") == 0) {
		return cmd_list(argc, argv);
	}
	if (argc >= 3 && strcmp(argv[2], "restore") == 0) {
		return cmd_restore();
	}
	P("Usage: displayxr-cli runtime <status|list|activate|restore>\n");
	P("  status                  Resolve the full loader precedence chain (env var, HKCU,\n");
	P("                          HKLM, 64- and 32-bit views), name the winner, flag conflicts.\n");
	P("  list [--json]           Enumerate every OpenXR runtime on this machine, including\n");
	P("       [--no-scan]        ones that never registered in AvailableRuntimes.\n");
	P("  activate [<manifest>]   Set DisplayXR (or the given manifest) active in every view\n");
	P("                          the loader consults. Needs admin.\n");
	P("  restore                 Put back the runtime the last 'activate' replaced.\n");
	return 1;
}

#else /* !XRT_OS_WINDOWS */

int
cli_cmd_runtime(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	P("'runtime' (ActiveRuntime switching) is Windows-only; on this platform set\n");
	P("XR_RUNTIME_JSON or the per-user active-runtime file instead.\n");
	return 1;
}

#endif
