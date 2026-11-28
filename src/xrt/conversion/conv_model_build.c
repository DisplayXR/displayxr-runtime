// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Locally built models: the §5.6.3 build sequence (see conv_model_build.h).
 * @ingroup aux_conversion
 */

#include "conv_model_build.h"
#include "conv_model_store_internal.h"
#include "conv_host_paths.h"

#include "os/os_time.h"
#include "util/u_json.h"
#include "util/u_logging.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(XRT_OS_ANDROID)
#include "os/os_process.h"
#endif

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID)
#include <sys/prctl.h>
#endif
#if defined(XRT_OS_MACOS)
#include <sys/sysctl.h>
#endif
extern char **environ;
#endif

#define BUILD_DEFAULT_STALL_MS (10u * 60u * 1000u)
#define BUILD_DEFAULT_GRACE_MS 10000u
#define BUILD_DEFAULT_DESCRIBE_MS 60000u
#define BUILD_LINE_MAX 16384
#define BUILD_MAX_MODELS 16

static void
cpy(char *dst, size_t n, const char *src)
{
	if (dst != NULL && n > 0) {
		(void)snprintf(dst, n, "%s", src != NULL ? src : "");
	}
}

static uint64_t
now_ms(void)
{
	return os_monotonic_get_ns() / 1000000ull;
}

static void
sleep_ms(uint32_t ms)
{
	os_nanosleep((int64_t)ms * 1000000);
}

const char *
conv_build_result_str(enum conv_build_result r)
{
	switch (r) {
	case CONV_BUILD_BUILT: return "built";
	case CONV_BUILD_PRESENT: return "ok";
	case CONV_BUILD_NOT_BUILDABLE: return "not-buildable";
	case CONV_BUILD_POLICY: return "policy";
	case CONV_BUILD_POLICY_LOCKED: return "policy-locked";
	case CONV_BUILD_LICENSE_TEXT: return "build-license-text";
	case CONV_BUILD_LICENSE: return "build-license";
	case CONV_BUILD_TOOL_MISSING: return "build-tool-missing";
	case CONV_BUILD_TOOL_OLD: return "build-tool-old";
	case CONV_BUILD_TOOL_UNTRUSTED: return "build-tool-untrusted";
	case CONV_BUILD_TOOL_MISMATCH: return "build-tool-mismatch";
	case CONV_BUILD_DISK: return "build-disk";
	case CONV_BUILD_MEMORY: return "build-memory";
	case CONV_BUILD_UNSUPPORTED: return "build-unsupported";
	case CONV_BUILD_BUSY: return "busy";
	case CONV_BUILD_DOWNLOAD: return "build-download";
	case CONV_BUILD_BAD_DOWNLOAD: return "bad-download";
	case CONV_BUILD_SOURCE_MISMATCH: return "build-source-mismatch";
	case CONV_BUILD_OUTPUT_MISMATCH: return "build-output-mismatch";
	case CONV_BUILD_CANCELLED: return "build-cancelled";
	case CONV_BUILD_STALLED: return "build-stalled";
	case CONV_BUILD_FAILED: return "build-failed";
	case CONV_BUILD_IO: return "io";
	default: return "unknown";
	}
}

void
conv_build_options_defaults(struct conv_build_options *opt)
{
	memset(opt, 0, sizeof(*opt));
}

enum conv_build_result
conv_build_result_of_exit(int32_t code)
{
	switch (code) {
	case 0: return CONV_BUILD_BUILT;
	case 10: return CONV_BUILD_LICENSE;
	case 11: return CONV_BUILD_DOWNLOAD;
	case 12: return CONV_BUILD_SOURCE_MISMATCH;
	case 13: return CONV_BUILD_OUTPUT_MISMATCH;
	case 14: return CONV_BUILD_MEMORY;
	case 15: return CONV_BUILD_DISK;
	case 16: return CONV_BUILD_UNSUPPORTED;
	case 17: return CONV_BUILD_CANCELLED;
	default: return CONV_BUILD_FAILED;
	}
}

bool
conv_build_dev_env_name(const char *tool_id, char *out, size_t out_size)
{
	const int n = snprintf(out, out_size, "DXR_MODEL_BUILDER_%s", tool_id != NULL ? tool_id : "");
	if (n <= 0 || (size_t)n >= out_size || tool_id == NULL || tool_id[0] == '\0') {
		return false;
	}
	for (char *c = out; *c != '\0'; c++) {
		*c = *c == '-' || *c == '.' ? '_' : (char)toupper((unsigned char)*c);
	}
	return true;
}


/*
 *
 * Pure helpers.
 *
 */

int
conv_build_version_cmp(const char *a, const char *b)
{
	a = a != NULL ? a : "";
	b = b != NULL ? b : "";
	while (*a != '\0' || *b != '\0') {
		unsigned long x = 0, y = 0;
		while (*a >= '0' && *a <= '9') {
			x = x * 10 + (unsigned long)(*a++ - '0');
		}
		while (*b >= '0' && *b <= '9') {
			y = y * 10 + (unsigned long)(*b++ - '0');
		}
		if (x != y) {
			return x < y ? -1 : 1;
		}
		// Skip to the next part ('.', or anything that ended this one: "1.0.0-rc1" compares 1.0.0).
		const bool a_dot = *a == '.', b_dot = *b == '.';
		if (*a != '\0' && !a_dot) {
			a = "";
		}
		if (*b != '\0' && !b_dot) {
			b = "";
		}
		a += a_dot ? 1 : 0;
		b += b_dot ? 1 : 0;
	}
	return 0;
}

bool
conv_build_os_matches(const char *entry, const char *os, const char *version)
{
	if (entry == NULL || os == NULL) {
		return false;
	}
	char name[32];
	cpy(name, sizeof(name), entry);
	const char *min = NULL;
	char *ge = strstr(name, ">=");
	if (ge != NULL) {
		*ge = '\0';
		min = entry + (ge - name) + 2;
	}
	if (strcmp(name, os) != 0) {
		return false;
	}
	return min == NULL || conv_build_version_cmp(version != NULL ? version : "", min) >= 0;
}

static bool
path_absolute(const char *p)
{
	if (p == NULL || p[0] == '\0') {
		return false;
	}
	if (p[0] == '/') {
		return true;
	}
	if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
	    (p[2] == '\\' || p[2] == '/')) {
		return true;
	}
	return p[0] == '\\' && p[1] == '\\';
}

bool
conv_build_tool_manifest_parse(const char *json, const char *want_id, struct conv_build_tool *out)
{
	memset(out, 0, sizeof(*out));
	cJSON *root = json != NULL ? cJSON_Parse(json) : NULL;
	bool ok = false;
	if (cJSON_IsObject(root)) {
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
		const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
		const cJSON *bin = cJSON_GetObjectItemCaseSensitive(root, "binary");
		if (cJSON_IsString(id) && cJSON_IsString(ver) && cJSON_IsString(bin) && want_id != NULL &&
		    strcmp(id->valuestring, want_id) == 0 && path_absolute(bin->valuestring)) {
			cpy(out->id, sizeof(out->id), id->valuestring);
			cpy(out->version, sizeof(out->version), ver->valuestring);
			cpy(out->binary, sizeof(out->binary), bin->valuestring);
			const cJSON *dn = cJSON_GetObjectItemCaseSensitive(root, "display_name");
			cpy(out->display_name, sizeof(out->display_name),
			    cJSON_IsString(dn) ? dn->valuestring : id->valuestring);
			ok = out->version[0] != '\0';
		}
	}
	cJSON_Delete(root);
	return ok;
}

static const char *
jget_str(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) && v->valuestring != NULL ? v->valuestring : NULL;
}

bool
conv_build_describe_check(const char *json,
                          const struct conv_pack_build *b,
                          const char *const *variants,
                          uint32_t variant_count,
                          char *why,
                          size_t why_size)
{
	cpy(why, why_size, "");
	cJSON *root = json != NULL ? cJSON_Parse(json) : NULL;
	bool ok = false;
	if (!cJSON_IsObject(root)) {
		cpy(why, why_size, "describe --json did not print a JSON object");
		goto done;
	}
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
	if (!cJSON_IsNumber(v) || v->valueint != 1) {
		cpy(why, why_size, "describe --json: \"v\" is not 1");
		goto done;
	}
	const char *tool = jget_str(root, "tool");
	const char *version = jget_str(root, "version");
	if (tool == NULL || strcmp(tool, b->tool_id) != 0) {
		(void)snprintf(why, why_size, "the builder says it is '%s', the pack wants '%s'",
		               tool != NULL ? tool : "", b->tool_id);
		goto done;
	}
	if (version == NULL || conv_build_version_cmp(version, b->tool_min_version) < 0) {
		(void)snprintf(why, why_size, "the builder is version %s, the pack needs %s or later",
		               version != NULL ? version : "?", b->tool_min_version);
		goto done;
	}
	const cJSON *recipes = cJSON_GetObjectItemCaseSensitive(root, "recipes");
	const cJSON *rec = NULL, *r = NULL;
	cJSON_ArrayForEach(r, recipes)
	{
		const char *id = jget_str(r, "recipe");
		if (id != NULL && strcmp(id, b->recipe) == 0) {
			rec = r;
			break;
		}
	}
	if (rec == NULL) {
		(void)snprintf(why, why_size, "the builder has no recipe '%s'", b->recipe);
		goto done;
	}
	const cJSON *vars = cJSON_GetObjectItemCaseSensitive(rec, "variants");
	for (uint32_t i = 0; i < variant_count; i++) {
		bool found = false;
		const cJSON *x = NULL;
		cJSON_ArrayForEach(x, vars)
		{
			found = found || (cJSON_IsString(x) && strcmp(x->valuestring, variants[i]) == 0);
		}
		if (!found) {
			(void)snprintf(why, why_size, "recipe '%s' has no variant '%s' in this builder", b->recipe,
			               variants[i]);
			goto done;
		}
	}
	const cJSON *srcs = cJSON_GetObjectItemCaseSensitive(rec, "sources");
	uint32_t seen = 0;
	const cJSON *sx = NULL;
	cJSON_ArrayForEach(sx, srcs)
	{
		const char *role = jget_str(sx, "role");
		const char *sha = jget_str(sx, "sha256");
		const char *lic = jget_str(sx, "license_id");
		const cJSON *size = cJSON_GetObjectItemCaseSensitive(sx, "size");
		const struct conv_pack_source *ps = NULL;
		for (uint32_t k = 0; role != NULL && k < b->source_count; k++) {
			if (strcmp(b->sources[k].role, role) == 0) {
				ps = &b->sources[k];
			}
		}
		if (ps == NULL) {
			(void)snprintf(why, why_size, "the builder pins a source '%s' the pack does not have",
			               role != NULL ? role : "?");
			goto done;
		}
		char lower[CONV_STORE_SHA_HEX];
		cpy(lower, sizeof(lower), sha);
		for (char *c = lower; *c != '\0'; c++) {
			*c = (char)tolower((unsigned char)*c);
		}
		if (sha == NULL || strcmp(lower, ps->sha256) != 0 || !cJSON_IsNumber(size) ||
		    (uint64_t)size->valuedouble != ps->size || lic == NULL || strcmp(lic, ps->license.id) != 0) {
			(void)snprintf(why, why_size,
			               "source '%s': the builder's pin (sha256, size, licence) is not the pack's",
			               ps->role);
			goto done;
		}
		seen++;
	}
	if (seen != b->source_count) {
		cpy(why, why_size, "the builder does not pin every source the pack lists");
		goto done;
	}
	ok = true;
done:
	cJSON_Delete(root);
	return ok;
}


/*
 *
 * Android: no builder (§5.6.7).
 *
 */

#if defined(XRT_OS_ANDROID)

bool
conv_build_machine_read(const char *root, struct conv_build_machine *out)
{
	(void)root;
	memset(out, 0, sizeof(*out));
	cpy(out->os, sizeof(out->os), "android");
	return false;
}

uint32_t
conv_model_store_build(struct conv_model_store *s,
                       const struct conv_pack *p,
                       const char *const *models,
                       uint32_t count,
                       const struct conv_build_ops *ops,
                       const struct conv_build_options *opt,
                       struct conv_build_model_result *out)
{
	(void)s;
	(void)p;
	(void)ops;
	(void)opt;
	for (uint32_t i = 0; i < count; i++) {
		memset(&out[i], 0, sizeof(out[i]));
		cpy(out[i].name, sizeof(out[i].name), models[i]);
		out[i].result = CONV_BUILD_UNSUPPORTED;
		out[i].exit_code = -1;
		cpy(out[i].hint, sizeof(out[i].hint), "no model builder on Android");
	}
	return count;
}

bool
conv_model_store_build_cancel(struct conv_model_store *s, const char *pack, const char *model)
{
	(void)s;
	(void)pack;
	(void)model;
	return false;
}

uint32_t
conv_model_store_clean_staging(struct conv_model_store *s, uint64_t max_age_s)
{
	(void)s;
	(void)max_age_s;
	return 0;
}

#else // !XRT_OS_ANDROID


/*
 *
 * Directories: list, age, owner, recursive delete.
 *
 */

#ifdef XRT_OS_WINDOWS
static bool
widen(const char *s, wchar_t *out, int n)
{
	return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) > 0;
}

static bool
narrow(const wchar_t *s, char *out, int n)
{
	return WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL) > 0;
}
#endif

typedef void (*dir_fn)(void *user, const char *name, bool is_dir);

static void
dir_each(const char *dir, dir_fn fn, void *user)
{
#ifdef XRT_OS_WINDOWS
	char pat[CONV_STORE_PATH_MAX];
	conv_store_fs_join(pat, sizeof(pat), dir, "*");
	wchar_t w[CONV_STORE_PATH_MAX];
	if (!widen(pat, w, CONV_STORE_PATH_MAX)) {
		return;
	}
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(w, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
			continue;
		}
		char name[260 * 3];
		if (narrow(fd.cFileName, name, (int)sizeof(name))) {
			const bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
			               (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
			fn(user, name, d);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir);
	if (d == NULL) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		char p[CONV_STORE_PATH_MAX];
		conv_store_fs_join(p, sizeof(p), dir, e->d_name);
		struct stat st;
		// lstat: a symlink is removed as a link, never followed out of staging.
		const bool is_dir = lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
		fn(user, e->d_name, is_dir);
	}
	closedir(d);
#endif
}

static bool
dir_remove_empty(const char *path)
{
#ifdef XRT_OS_WINDOWS
	wchar_t w[CONV_STORE_PATH_MAX];
	return widen(path, w, CONV_STORE_PATH_MAX) && RemoveDirectoryW(w);
#else
	return rmdir(path) == 0;
#endif
}

struct rm_ctx
{
	const char *dir;
	int depth;
};

static void
rmtree_at(const char *path, int depth);

static void
rm_fn(void *user, const char *name, bool is_dir)
{
	const struct rm_ctx *c = (const struct rm_ctx *)user;
	char p[CONV_STORE_PATH_MAX];
	conv_store_fs_join(p, sizeof(p), c->dir, name);
	if (is_dir) {
		rmtree_at(p, c->depth + 1);
	} else {
		(void)conv_store_fs_remove(p);
	}
}

static void
rmtree_at(const char *path, int depth)
{
	if (depth > 16) {
		return;
	}
	struct rm_ctx c = {path, depth};
	dir_each(path, rm_fn, &c);
	(void)dir_remove_empty(path);
}

//! Delete @p path and everything under it (symlinks are removed, never followed).
static void
rmtree(const char *path)
{
	rmtree_at(path, 0);
}

//! Seconds since @p path was last written (UINT64_MAX when it cannot be read).
static uint64_t
age_s(const char *path)
{
#ifdef XRT_OS_WINDOWS
	wchar_t w[CONV_STORE_PATH_MAX];
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!widen(path, w, CONV_STORE_PATH_MAX) || !GetFileAttributesExW(w, GetFileExInfoStandard, &a)) {
		return UINT64_MAX;
	}
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	const uint64_t t = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
	const uint64_t n = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
	return n > t ? (n - t) / 10000000ull : 0;
#else
	struct stat st;
	if (stat(path, &st) != 0) {
		return UINT64_MAX;
	}
	const time_t now = time(NULL);
	return now > st.st_mtime ? (uint64_t)(now - st.st_mtime) : 0;
#endif
}

//! Was @p path created by this user (not merely a trusted writer)?
static bool
owned_by_self(const char *path)
{
#ifdef XRT_OS_WINDOWS
	wchar_t w[CONV_STORE_PATH_MAX];
	if (!widen(path, w, CONV_STORE_PATH_MAX)) {
		return false;
	}
	PSID owner = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	if (GetNamedSecurityInfoW(w, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd) !=
	        ERROR_SUCCESS ||
	    owner == NULL) {
		if (sd != NULL) {
			LocalFree(sd);
		}
		return false;
	}
	bool ok = false;
	HANDLE tok = NULL;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		union {
			TOKEN_USER tu;
			BYTE raw[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
		} buf;
		DWORD n = 0;
		if (GetTokenInformation(tok, TokenUser, &buf, sizeof(buf), &n)) {
			ok = EqualSid(owner, buf.tu.User.Sid) != FALSE;
		}
		// An elevated token's default owner for what it creates is
		// BUILTIN\Administrators, not the user: that is "this user" too.
		union {
			TOKEN_OWNER to;
			BYTE raw[sizeof(TOKEN_OWNER) + SECURITY_MAX_SID_SIZE];
		} own;
		if (!ok && GetTokenInformation(tok, TokenOwner, &own, sizeof(own), &n)) {
			ok = EqualSid(owner, own.to.Owner) != FALSE;
		}
		CloseHandle(tok);
	}
	LocalFree(sd);
	return ok;
#else
	struct stat st;
	return lstat(path, &st) == 0 && st.st_uid == geteuid();
#endif
}


/*
 *
 * The machine (step 5).
 *
 */

bool
conv_build_machine_read(const char *root, struct conv_build_machine *out)
{
	memset(out, 0, sizeof(*out));
	// Free space at the nearest existing ancestor of the root (it may not exist yet).
	char dir[CONV_STORE_PATH_MAX];
	cpy(dir, sizeof(dir), root != NULL ? root : "");
	for (int guard = 0; guard < 64 && dir[0] != '\0' && !conv_store_fs_exists(dir); guard++) {
		char *last = NULL;
		for (char *c = dir; *c != '\0'; c++) {
			if (*c == '/' || *c == '\\') {
				last = c;
			}
		}
		if (last == NULL) {
			dir[0] = '\0';
		} else if (last == dir || (last == dir + 2 && dir[1] == ':')) {
			last[1] = '\0'; // "/" or "C:\"
			break;
		} else {
			*last = '\0';
		}
	}
#ifdef XRT_OS_WINDOWS
	cpy(out->os, sizeof(out->os), "windows");
	MEMORYSTATUSEX ms;
	ms.dwLength = sizeof(ms);
	if (GlobalMemoryStatusEx(&ms)) {
		out->phys_known = true;
		out->phys_mb = ms.ullTotalPhys / (1024ull * 1024ull);
	}
	wchar_t w[CONV_STORE_PATH_MAX];
	ULARGE_INTEGER avail;
	if (dir[0] != '\0' && widen(dir, w, CONV_STORE_PATH_MAX) && GetDiskFreeSpaceExW(w, &avail, NULL, NULL)) {
		out->free_known = true;
		out->free_mb = avail.QuadPart / (1024ull * 1024ull);
	}
	// RtlGetVersion: GetVersionEx lies to an unmanifested process.
	typedef LONG(WINAPI * rtl_get_version_fn)(PRTL_OSVERSIONINFOW);
	HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	rtl_get_version_fn rgv = nt != NULL ? (rtl_get_version_fn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
	RTL_OSVERSIONINFOW vi;
	memset(&vi, 0, sizeof(vi));
	vi.dwOSVersionInfoSize = sizeof(vi);
	if (rgv != NULL && rgv(&vi) == 0) {
		(void)snprintf(out->os_version, sizeof(out->os_version), "%lu.%lu.%lu",
		               (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion,
		               (unsigned long)vi.dwBuildNumber);
	}
#else
#if defined(XRT_OS_MACOS)
	cpy(out->os, sizeof(out->os), "macos");
	uint64_t mem = 0;
	size_t len = sizeof(mem);
	if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0) {
		out->phys_known = true;
		out->phys_mb = mem / (1024ull * 1024ull);
	}
	char ver[32] = {0};
	len = sizeof(ver) - 1;
	if (sysctlbyname("kern.osproductversion", ver, &len, NULL, 0) == 0) {
		cpy(out->os_version, sizeof(out->os_version), ver);
	}
#else
	cpy(out->os, sizeof(out->os), "linux");
	const long pages = sysconf(_SC_PHYS_PAGES);
	const long page = sysconf(_SC_PAGESIZE);
	if (pages > 0 && page > 0) {
		out->phys_known = true;
		out->phys_mb = (uint64_t)pages * (uint64_t)page / (1024ull * 1024ull);
	}
	struct utsname u;
	if (uname(&u) == 0) {
		cpy(out->os_version, sizeof(out->os_version), u.release);
	}
#endif
	struct statvfs sv;
	if (dir[0] != '\0' && statvfs(dir, &sv) == 0) {
		out->free_known = true;
		out->free_mb = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize / (1024ull * 1024ull);
	}
#endif
	return true;
}


/*
 *
 * The builder process: argv (no shell), a minimal environment, a working
 * directory, stdout on a pipe, stderr to the log, a job object with
 * kill-on-close (Windows) / its own process group (POSIX).
 *
 */

struct bproc
{
#ifdef XRT_OS_WINDOWS
	HANDLE process;
	HANDLE job;
	HANDLE out_rd;
#else
	pid_t pid;
	int out_fd;
	bool reaped;
	int status;
#endif
	bool exited;
	int32_t code;
};

#ifdef XRT_OS_WINDOWS
//! Append @p arg to @p cmd with the MSVC quoting rules (CommandLineToArgvW reads it back exactly).
static bool
win_append_arg(wchar_t *cmd, size_t cap, size_t *len, const wchar_t *arg)
{
	const bool quote = arg[0] == L'\0' || wcspbrk(arg, L" \t\n\v\"") != NULL;
#define PUT(ch)                                                                                                        \
	do {                                                                                                           \
		if (*len + 2 >= cap) {                                                                                 \
			return false;                                                                                  \
		}                                                                                                      \
		cmd[(*len)++] = (ch);                                                                                  \
	} while (0)
	if (*len > 0) {
		PUT(L' ');
	}
	if (!quote) {
		for (const wchar_t *c = arg; *c != L'\0'; c++) {
			PUT(*c);
		}
		cmd[*len] = L'\0';
		return true;
	}
	PUT(L'"');
	for (const wchar_t *c = arg;; c++) {
		size_t bs = 0;
		while (*c == L'\\') {
			bs++;
			c++;
		}
		if (*c == L'\0') {
			for (size_t i = 0; i < bs * 2; i++) {
				PUT(L'\\');
			}
			break;
		}
		if (*c == L'"') {
			for (size_t i = 0; i < bs * 2 + 1; i++) {
				PUT(L'\\');
			}
			PUT(L'"');
		} else {
			for (size_t i = 0; i < bs; i++) {
				PUT(L'\\');
			}
			PUT(*c);
		}
	}
	PUT(L'"');
	cmd[*len] = L'\0';
	return true;
#undef PUT
}

static int
env_cmp(const void *a, const void *b)
{
	return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}
#endif

/*!
 * Start @p exe with @p args (after argv[0]) in @p cwd. @p home is where
 * HOME / TMP / TEMP / TMPDIR (and on Windows USERPROFILE / APPDATA /
 * LOCALAPPDATA) point: the build's work directory.
 */
static bool
bproc_spawn(const char *exe,
            const char *const *args,
            uint32_t n,
            const char *cwd,
            const char *home,
            const char *log_path,
            struct bproc *out)
{
	memset(out, 0, sizeof(*out));
	out->code = -1;
#ifdef XRT_OS_WINDOWS
	out->process = NULL;
	wchar_t wexe[CONV_STORE_PATH_MAX], wcwd[CONV_STORE_PATH_MAX], whome[CONV_STORE_PATH_MAX];
	if (!widen(exe, wexe, CONV_STORE_PATH_MAX) || !widen(cwd, wcwd, CONV_STORE_PATH_MAX) ||
	    !widen(home, whome, CONV_STORE_PATH_MAX)) {
		return false;
	}
	const size_t cap = 32768;
	wchar_t *cmd = calloc(cap, sizeof(wchar_t));
	wchar_t *wa = calloc(CONV_STORE_PATH_MAX * 2, sizeof(wchar_t));
	bool ok = cmd != NULL && wa != NULL;
	size_t len = 0;
	ok = ok && win_append_arg(cmd, cap, &len, wexe);
	for (uint32_t i = 0; ok && i < n; i++) {
		ok = widen(args[i], wa, CONV_STORE_PATH_MAX * 2) && win_append_arg(cmd, cap, &len, wa);
	}
	free(wa);
	// The minimal environment, sorted (CreateProcess expects a sorted block).
	wchar_t sysroot[MAX_PATH] = L"C:\\Windows", sysdrive[16] = L"C:", ncpu[16] = L"1", arch[32] = L"AMD64";
	(void)GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH);
	(void)GetEnvironmentVariableW(L"SystemDrive", sysdrive, 16);
	(void)GetEnvironmentVariableW(L"NUMBER_OF_PROCESSORS", ncpu, 16);
	(void)GetEnvironmentVariableW(L"PROCESSOR_ARCHITECTURE", arch, 32);
	wchar_t vars[14][CONV_STORE_PATH_MAX + 64];
	int nv = 0;
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"APPDATA=%ls", whome);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"HOME=%ls", whome);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"LOCALAPPDATA=%ls", whome);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"NUMBER_OF_PROCESSORS=%ls", ncpu);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"PATH=%ls\\System32;%ls", sysroot, sysroot);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"PROCESSOR_ARCHITECTURE=%ls", arch);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"SystemDrive=%ls", sysdrive);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"SystemRoot=%ls", sysroot);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"TEMP=%ls", whome);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"TMP=%ls", whome);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"USERPROFILE=%ls", whome);
	(void)_snwprintf(vars[nv++], CONV_STORE_PATH_MAX + 64, L"windir=%ls", sysroot);
	const wchar_t *sorted[14];
	for (int i = 0; i < nv; i++) {
		sorted[i] = vars[i];
	}
	qsort(sorted, (size_t)nv, sizeof(sorted[0]), env_cmp);
	wchar_t *env = calloc((size_t)nv * (CONV_STORE_PATH_MAX + 64) + 2, sizeof(wchar_t));
	ok = ok && env != NULL;
	size_t el = 0;
	for (int i = 0; ok && i < nv; i++) {
		const size_t l = wcslen(sorted[i]);
		memcpy(env + el, sorted[i], l * sizeof(wchar_t));
		el += l;
		env[el++] = L'\0';
	}
	if (ok) {
		env[el++] = L'\0';
	}

	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE rd = NULL, wr = NULL, nul = INVALID_HANDLE_VALUE, logh = INVALID_HANDLE_VALUE;
	ok = ok && CreatePipe(&rd, &wr, &sa, 0) && SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	if (ok) {
		nul =
		    CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
		wchar_t wlog[CONV_STORE_PATH_MAX];
		if (log_path != NULL && widen(log_path, wlog, CONV_STORE_PATH_MAX)) {
			logh = CreateFileW(wlog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
			                   FILE_ATTRIBUTE_NORMAL, NULL);
		}
		if (logh == INVALID_HANDLE_VALUE) {
			logh = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
			                   OPEN_EXISTING, 0, NULL);
		}
		ok = nul != INVALID_HANDLE_VALUE && logh != INVALID_HANDLE_VALUE;
	}
	// Only these three handles are inherited.
	SIZE_T attr_size = 0;
	(void)InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = ok ? (LPPROC_THREAD_ATTRIBUTE_LIST)calloc(1, attr_size) : NULL;
	HANDLE inherit[3] = {nul, wr, logh};
	ok = ok && attrs != NULL && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
	     UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), NULL,
	                               NULL);
	HANDLE job = ok ? CreateJobObjectW(NULL, NULL) : NULL;
	if (job != NULL) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
		memset(&li, 0, sizeof(li));
		li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li)) != FALSE;
	} else {
		ok = false;
	}
	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));
	if (ok) {
		STARTUPINFOEXW si;
		memset(&si, 0, sizeof(si));
		si.StartupInfo.cb = sizeof(si);
		si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		si.StartupInfo.hStdInput = nul;
		si.StartupInfo.hStdOutput = wr;
		si.StartupInfo.hStdError = logh;
		si.lpAttributeList = attrs;
		ok = CreateProcessW(wexe, cmd, NULL, NULL, TRUE,
		                    CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
		                        CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
		                    env, wcwd, &si.StartupInfo, &pi) != FALSE;
		if (!ok) {
			U_LOG_W("[conv-build] could not start %s (error %lu)", exe, (unsigned long)GetLastError());
		}
	}
	if (ok && !AssignProcessToJobObject(job, pi.hProcess)) {
		U_LOG_W("[conv-build] could not put the builder in a job object (error %lu)",
		        (unsigned long)GetLastError());
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		ok = false;
	}
	if (ok) {
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);
		out->process = pi.hProcess;
		out->job = job;
		out->out_rd = rd;
		job = NULL;
		rd = NULL;
	}
	if (attrs != NULL) {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
	}
	if (job != NULL) {
		CloseHandle(job);
	}
	if (rd != NULL) {
		CloseHandle(rd);
	}
	if (wr != NULL) {
		CloseHandle(wr);
	}
	if (nul != INVALID_HANDLE_VALUE) {
		CloseHandle(nul);
	}
	if (logh != INVALID_HANDLE_VALUE) {
		CloseHandle(logh);
	}
	free(env);
	free(cmd);
	return ok;
#else
	char *argv[64];
	if (n + 2 > sizeof(argv) / sizeof(argv[0])) {
		return false;
	}
	argv[0] = (char *)exe;
	for (uint32_t i = 0; i < n; i++) {
		argv[i + 1] = (char *)args[i];
	}
	argv[n + 1] = NULL;
	char e_home[CONV_STORE_PATH_MAX + 8], e_tmpdir[CONV_STORE_PATH_MAX + 8], e_tmp[CONV_STORE_PATH_MAX + 8],
	    e_temp[CONV_STORE_PATH_MAX + 8];
	(void)snprintf(e_home, sizeof(e_home), "HOME=%s", home);
	(void)snprintf(e_tmpdir, sizeof(e_tmpdir), "TMPDIR=%s", home);
	(void)snprintf(e_tmp, sizeof(e_tmp), "TMP=%s", home);
	(void)snprintf(e_temp, sizeof(e_temp), "TEMP=%s", home);
	char *envp[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin",
	                "LANG=C.UTF-8",
	                "LC_ALL=C.UTF-8",
	                e_home,
	                e_tmpdir,
	                e_tmp,
	                e_temp,
	                NULL};
	int fds[2];
	if (pipe(fds) != 0) {
		return false;
	}
	const int nul = open("/dev/null", O_RDONLY);
	const int logfd = log_path != NULL ? open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
	const int errfd = logfd >= 0 ? logfd : open("/dev/null", O_WRONLY);
	const pid_t pid = fork();
	if (pid == 0) {
		// Child: only async-signal-safe calls until execve.
		(void)setpgid(0, 0);
#if defined(XRT_OS_LINUX)
		(void)prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
		if (nul >= 0) {
			(void)dup2(nul, 0);
		}
		(void)dup2(fds[1], 1);
		if (errfd >= 0) {
			(void)dup2(errfd, 2);
		}
		for (int fd = 3; fd < 1024; fd++) {
			(void)close(fd);
		}
		if (chdir(cwd) != 0) {
			_exit(126);
		}
		execve(exe, argv, envp);
		_exit(127);
	}
	(void)close(fds[1]);
	if (nul >= 0) {
		(void)close(nul);
	}
	if (errfd >= 0) {
		(void)close(errfd);
	}
	if (pid < 0) {
		(void)close(fds[0]);
		return false;
	}
	(void)setpgid(pid, pid); // the parent too, so the group exists before any kill
	(void)fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	(void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	out->pid = pid;
	out->out_fd = fds[0];
	return true;
#endif
}

//! > 0 bytes read, 0 = nothing within @p timeout_ms, -1 = end of output.
static int
bproc_read(struct bproc *p, char *buf, size_t cap, uint32_t timeout_ms)
{
#ifdef XRT_OS_WINDOWS
	const uint64_t end = now_ms() + timeout_ms;
	for (;;) {
		DWORD avail = 0;
		if (!PeekNamedPipe(p->out_rd, NULL, 0, NULL, &avail, NULL)) {
			return -1; // ERROR_BROKEN_PIPE: every writer closed
		}
		if (avail > 0) {
			DWORD got = 0;
			const DWORD want = avail < (DWORD)cap ? avail : (DWORD)cap;
			if (!ReadFile(p->out_rd, buf, want, &got, NULL)) {
				return -1;
			}
			return (int)got;
		}
		if (now_ms() >= end) {
			return 0;
		}
		Sleep(20);
	}
#else
	struct pollfd pfd = {p->out_fd, POLLIN, 0};
	const int r = poll(&pfd, 1, (int)timeout_ms);
	if (r == 0) {
		return 0;
	}
	if (r < 0) {
		return errno == EINTR ? 0 : -1;
	}
	const ssize_t n = read(p->out_fd, buf, cap);
	if (n > 0) {
		return (int)n;
	}
	if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
		return 0;
	}
	return -1;
#endif
}

//! True when the builder exited (its code in p->code).
static bool
bproc_wait(struct bproc *p, uint32_t timeout_ms)
{
	if (p->exited) {
		return true;
	}
#ifdef XRT_OS_WINDOWS
	if (WaitForSingleObject(p->process, timeout_ms) != WAIT_OBJECT_0) {
		return false;
	}
	DWORD code = 1;
	(void)GetExitCodeProcess(p->process, &code);
	p->code = (int32_t)code;
	p->exited = true;
	return true;
#else
	const uint64_t end = now_ms() + timeout_ms;
	for (;;) {
		int st = 0;
		const pid_t r = waitpid(p->pid, &st, WNOHANG);
		if (r == p->pid) {
			p->exited = true;
			p->code = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
			return true;
		}
		if (r < 0 && errno != EINTR) {
			p->exited = true;
			p->code = 1;
			return true;
		}
		if (now_ms() >= end) {
			return false;
		}
		sleep_ms(10);
	}
#endif
}

//! Kill the builder and everything it started.
static void
bproc_kill(struct bproc *p)
{
#ifdef XRT_OS_WINDOWS
	if (p->job != NULL) {
		(void)TerminateJobObject(p->job, 1);
	}
#else
	if (p->pid > 0) {
		(void)kill(-p->pid, SIGKILL);
		(void)kill(p->pid, SIGKILL);
	}
#endif
}

static void
bproc_close(struct bproc *p)
{
#ifdef XRT_OS_WINDOWS
	if (p->out_rd != NULL) {
		CloseHandle(p->out_rd);
	}
	if (p->process != NULL) {
		CloseHandle(p->process);
	}
	if (p->job != NULL) {
		CloseHandle(p->job); // kill-on-close: nothing of the build survives this
	}
	memset(p, 0, sizeof(*p));
#else
	if (p->out_fd > 0) {
		(void)close(p->out_fd);
	}
	if (p->pid > 0 && !p->exited) {
		bproc_kill(p);
		(void)bproc_wait(p, 5000);
	}
	if (p->pid > 0) {
		(void)kill(-p->pid, SIGKILL); // grandchildren left in the group
	}
	memset(p, 0, sizeof(*p));
#endif
}


/*
 *
 * The run.
 *
 */

struct group
{
	const struct conv_pack *p;
	const struct conv_pack_build *b; //!< the first model's block (recipe + tool)
	uint32_t n;
	uint32_t idx[BUILD_MAX_MODELS]; //!< into the caller's models[] / out[]
	const struct conv_pack_model *m[BUILD_MAX_MODELS];
	const struct conv_pack_build *mb[BUILD_MAX_MODELS];
};

struct run_ctx
{
	struct conv_model_store *s;
	const struct conv_build_ops *ops;
	const struct conv_build_options *opt;
	char staging[CONV_STORE_PATH_MAX];
	char cancel_file[CONV_STORE_PATH_MAX];
	uint64_t cancel_poll_ms;
	bool cancel_seen;
};

static void
emit(const struct run_ctx *c, const char *line)
{
	if (c->ops != NULL && c->ops->on_event != NULL) {
		c->ops->on_event(c->ops->user, line);
	}
}

static bool
interrupted(const struct run_ctx *c)
{
	return c->opt != NULL && c->opt->interrupt != NULL && *c->opt->interrupt != 0;
}

//! The store's cancel hook during a build: the interrupt, or the cancel file (polled at 4 Hz).
static bool
run_cancel_fn(void *user)
{
	struct run_ctx *c = (struct run_ctx *)user;
	if (c->cancel_seen || interrupted(c)) {
		c->cancel_seen = true;
		return true;
	}
	const uint64_t t = now_ms();
	if (c->cancel_file[0] != '\0' && t - c->cancel_poll_ms >= 250) {
		c->cancel_poll_ms = t;
		if (conv_store_fs_exists(c->cancel_file)) {
			c->cancel_seen = true;
		}
	}
	return c->cancel_seen;
}

static void
set_all(struct group *g, struct conv_build_model_result *out, enum conv_build_result r, const char *hint)
{
	for (uint32_t i = 0; i < g->n; i++) {
		out[g->idx[i]].result = r;
		cpy(out[g->idx[i]].hint, sizeof(out[g->idx[i]].hint), hint);
	}
}

struct dl_progress
{
	const struct run_ctx *c;
};

static void
dl_progress_fn(void *user, const char *sha, uint64_t done, uint64_t total)
{
	(void)sha;
	const struct run_ctx *c = (const struct run_ctx *)user;
	char line[160];
	(void)snprintf(line, sizeof(line),
	               "{\"v\":1,\"event\":\"progress\",\"phase\":\"download-source\",\"done\":%llu,\"total\":%llu}",
	               (unsigned long long)done, (unsigned long long)total);
	emit(c, line);
}

/* The lock: <root>/staging/<recipe>.lock = "<pid> <nonce> <pack>/<model> ...\n". */

static bool
lock_read(const char *path, long *pid, char *nonce, size_t nonce_size, char **rest)
{
	char *t = conv_store_fs_read_all(path, 4096);
	if (t == NULL) {
		return false;
	}
	char nb[96] = {0};
	long p = 0;
	int consumed = 0;
	const bool ok = sscanf(t, "%ld %95s %n", &p, nb, &consumed) >= 2 && p > 0;
	if (ok) {
		*pid = p;
		cpy(nonce, nonce_size, nb);
		if (rest != NULL) {
			*rest = t;
			memmove(t, t + consumed, strlen(t + consumed) + 1);
			return true;
		}
	}
	free(t);
	return ok;
}

static bool
lock_create(const char *path, const char *text)
{
#ifdef XRT_OS_WINDOWS
	wchar_t w[CONV_STORE_PATH_MAX];
	if (!widen(path, w, CONV_STORE_PATH_MAX)) {
		return false;
	}
	HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_NEW,
	                       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	DWORD wrote = 0;
	const bool ok = WriteFile(h, text, (DWORD)strlen(text), &wrote, NULL) && wrote == strlen(text);
	CloseHandle(h);
	return ok;
#else
	const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		return false;
	}
	const size_t n = strlen(text);
	const bool ok = write(fd, text, n) == (ssize_t)n;
	(void)close(fd);
	return ok;
#endif
}

/*!
 * Take the recipe's lock: 1 = ours, 0 = held by a live process (busy), -1 =
 * cannot write. A lock of a dead process is stale and taken over.
 */
static int
lock_try(const char *path, const char *text)
{
	for (int attempt = 0; attempt < 3; attempt++) {
		if (lock_create(path, text)) {
			return 1;
		}
		long pid = 0;
		char nonce[96];
		if (lock_read(path, &pid, nonce, sizeof(nonce), NULL) && os_process_is_alive(pid)) {
			return 0;
		}
		if (!conv_store_fs_exists(path)) {
			continue; // released meanwhile
		}
		// Unreadable (a writer mid-write) or dead: give a writer a moment, then take it over.
		sleep_ms(50);
		if (lock_read(path, &pid, nonce, sizeof(nonce), NULL) && os_process_is_alive(pid)) {
			return 0;
		}
		U_LOG_W("[conv-build] stale build lock %s (process %ld is gone): taken over", path, pid);
		(void)conv_store_fs_remove(path);
	}
	return -1;
}

struct stale_ctx
{
	const char *staging;
	uint64_t max_age_s;
	uint32_t deleted;
	char live[16][96]; //!< nonces of live locks
	uint32_t live_n;
};

static void
stale_lock_fn(void *user, const char *name, uint64_t size)
{
	(void)size;
	struct stale_ctx *c = (struct stale_ctx *)user;
	const size_t n = strlen(name);
	if (n < 6 || strcmp(name + n - 5, ".lock") != 0) {
		return;
	}
	char p[CONV_STORE_PATH_MAX];
	conv_store_fs_join(p, sizeof(p), c->staging, name);
	long pid = 0;
	char nonce[96];
	if (lock_read(p, &pid, nonce, sizeof(nonce), NULL) && os_process_is_alive(pid)) {
		if (c->live_n < 16) {
			cpy(c->live[c->live_n++], sizeof(c->live[0]), nonce);
		}
	} else if (owned_by_self(p) && age_s(p) > 5) {
		(void)conv_store_fs_remove(p); // its process is gone
	}
}

static void
stale_dir_fn(void *user, const char *name, bool is_dir)
{
	struct stale_ctx *c = (struct stale_ctx *)user;
	if (!is_dir) {
		return;
	}
	for (uint32_t i = 0; i < c->live_n; i++) {
		if (strcmp(c->live[i], name) == 0) {
			return;
		}
	}
	char p[CONV_STORE_PATH_MAX];
	conv_store_fs_join(p, sizeof(p), c->staging, name);
	if (!owned_by_self(p) || age_s(p) < c->max_age_s) {
		return;
	}
	U_LOG_W("[conv-build] deleting stale staging directory %s", p);
	rmtree(p);
	c->deleted++;
}

uint32_t
conv_model_store_clean_staging(struct conv_model_store *s, uint64_t max_age_s)
{
	if (s == NULL) {
		return 0;
	}
	struct stale_ctx *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return 0;
	}
	char staging[CONV_STORE_PATH_MAX];
	conv_store_fs_join(staging, sizeof(staging), conv_model_store_root(s), "staging");
	c->staging = staging;
	c->max_age_s = max_age_s;
	conv_store_fs_list(staging, stale_lock_fn, c);
	dir_each(staging, stale_dir_fn, c);
	const uint32_t n = c->deleted;
	free(c);
	return n;
}

struct cancel_ctx
{
	const char *staging;
	const char *want; //!< "<pack>/<model>"
	bool found;
};

static void
cancel_lock_fn(void *user, const char *name, uint64_t size)
{
	(void)size;
	struct cancel_ctx *c = (struct cancel_ctx *)user;
	const size_t n = strlen(name);
	if (c->found || n < 6 || strcmp(name + n - 5, ".lock") != 0) {
		return;
	}
	char p[CONV_STORE_PATH_MAX];
	conv_store_fs_join(p, sizeof(p), c->staging, name);
	long pid = 0;
	char nonce[96];
	char *rest = NULL;
	if (!lock_read(p, &pid, nonce, sizeof(nonce), &rest)) {
		return;
	}
	bool hit = false;
	for (char *tok = strtok(rest, " \t\r\n"); tok != NULL && !hit; tok = strtok(NULL, " \t\r\n")) {
		hit = strcmp(tok, c->want) == 0;
	}
	free(rest);
	if (!hit || !os_process_is_alive(pid)) {
		return;
	}
	char dir[CONV_STORE_PATH_MAX], cancel[CONV_STORE_PATH_MAX];
	conv_store_fs_join(dir, sizeof(dir), c->staging, nonce);
	conv_store_fs_join(cancel, sizeof(cancel), dir, "cancel");
	c->found = conv_store_fs_write_text(cancel, "cancel\n");
}

bool
conv_model_store_build_cancel(struct conv_model_store *s, const char *pack, const char *model)
{
	if (s == NULL || pack == NULL || model == NULL) {
		return false;
	}
	char staging[CONV_STORE_PATH_MAX], want[160];
	conv_store_fs_join(staging, sizeof(staging), conv_model_store_root(s), "staging");
	(void)snprintf(want, sizeof(want), "%s/%s", pack, model);
	struct cancel_ctx c = {staging, want, false};
	conv_store_fs_list(staging, cancel_lock_fn, &c);
	return c.found;
}

/* Builder output. */

struct line_state
{
	char buf[BUILD_LINE_MAX];
	size_t len;
	bool overlong;
	char error_code[48];
	char error_msg[256];
};

static void
handle_line(const struct run_ctx *c, struct line_state *ls, const char *line)
{
	cJSON *o = cJSON_Parse(line);
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "v");
	const char *ev = cJSON_IsObject(o) ? jget_str(o, "event") : NULL;
	if (!cJSON_IsNumber(v) || v->valueint != 1 || ev == NULL) {
		// §5.6.2: nothing but JSON event lines on stdout. Dropped, never forwarded.
		U_LOG_W("[conv-build] builder printed a non-event line on stdout (dropped)");
		cJSON_Delete(o);
		return;
	}
	if (strcmp(ev, "error") == 0) {
		cpy(ls->error_code, sizeof(ls->error_code), jget_str(o, "code"));
		cpy(ls->error_msg, sizeof(ls->error_msg), jget_str(o, "message"));
	}
	// `output` lines are not trusted: ingest hashes out/ itself (step 9).
	cJSON_Delete(o);
	emit(c, line);
}

static void
feed(const struct run_ctx *c, struct line_state *ls, const char *data, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		const char ch = data[i];
		if (ch == '\n') {
			if (!ls->overlong && ls->len > 0) {
				if (ls->buf[ls->len - 1] == '\r') {
					ls->len--;
				}
				ls->buf[ls->len] = '\0';
				handle_line(c, ls, ls->buf);
			}
			ls->len = 0;
			ls->overlong = false;
			continue;
		}
		if (ls->len + 1 >= sizeof(ls->buf)) {
			ls->overlong = true;
			continue;
		}
		ls->buf[ls->len++] = ch;
	}
}

/*!
 * Run the builder to its end. Returns the run's result (BUILT = exit 0) and
 * the exit code (-1 when it was killed / never started).
 */
static enum conv_build_result
run_builder(struct run_ctx *c,
            const char *exe,
            const char *const *args,
            uint32_t n,
            const char *work,
            const char *log_path,
            int32_t *exit_code,
            char *hint,
            size_t hint_size)
{
	*exit_code = -1;
	struct bproc bp;
	if (!bproc_spawn(exe, args, n, work, work, log_path, &bp)) {
		cpy(hint, hint_size, "the builder could not be started");
		return CONV_BUILD_FAILED;
	}
	const uint32_t stall = c->opt->stall_ms != 0 ? c->opt->stall_ms : BUILD_DEFAULT_STALL_MS;
	const uint32_t grace = c->opt->cancel_grace_ms != 0 ? c->opt->cancel_grace_ms : BUILD_DEFAULT_GRACE_MS;
	struct line_state *ls = calloc(1, sizeof(*ls));
	char chunk[4096];
	uint64_t last_out = now_ms(), cancel_at = 0, exited_at = 0;
	bool eof = false, killed = false, stalled = false, cancelled = false;
	while (ls != NULL) {
		if (!eof) {
			const int r = bproc_read(&bp, chunk, sizeof(chunk), 200);
			if (r > 0) {
				last_out = now_ms();
				feed(c, ls, chunk, (size_t)r);
			}
			eof = r < 0;
		} else {
			sleep_ms(50);
		}
		// Every pass checks exit, cancel and the watchdog: a chatty builder
		// that ignores its cancel file is still killed after the grace period.
		const uint64_t t = now_ms();
		if (bproc_wait(&bp, 0)) {
			if (exited_at == 0) {
				exited_at = t;
			}
			// Drain what it printed before exiting; a grandchild that keeps the
			// pipe open does not hold the run past 2 s.
			if (eof || t - exited_at >= 2000) {
				break;
			}
			continue;
		}
		if (!cancelled && run_cancel_fn(c)) {
			cancelled = true;
			cancel_at = t;
			// An interrupt reaches the builder the documented way: its cancel file.
			(void)conv_store_fs_write_text(c->cancel_file, "cancel\n");
		}
		if (cancelled && !killed && t - cancel_at >= grace) {
			U_LOG_W("[conv-build] the builder did not exit %u ms after cancel: killing it", grace);
			bproc_kill(&bp);
			killed = true;
		}
		if (!cancelled && !killed && t - last_out >= stall) {
			U_LOG_W("[conv-build] the builder printed nothing for %u ms: killing it (build-stalled)",
			        stall);
			bproc_kill(&bp);
			killed = true;
			stalled = true;
		}
	}
	(void)bproc_wait(&bp, 10000);
	if (ls != NULL && ls->error_msg[0] != '\0') {
		(void)snprintf(hint, hint_size, "%s%s%s", ls->error_code, ls->error_code[0] != '\0' ? ": " : "",
		               ls->error_msg);
	}
	free(ls);
	const bool exited_itself = bp.exited && !killed;
	const int32_t code = bp.code;
	bproc_close(&bp);
	if (stalled) {
		cpy(hint, hint_size, "the builder printed nothing for the watchdog period and was stopped");
		return CONV_BUILD_STALLED;
	}
	if (cancelled) {
		*exit_code = exited_itself ? code : -1;
		return CONV_BUILD_CANCELLED;
	}
	if (!exited_itself) {
		return CONV_BUILD_FAILED;
	}
	*exit_code = code;
	return conv_build_result_of_exit(code);
}

//! `describe --json` into @p out (NUL-terminated). False when it did not exit 0 in time.
static bool
run_describe(const char *exe, const char *work, uint32_t timeout_ms, char *out, size_t cap)
{
	const char *args[] = {"describe", "--json"};
	struct bproc bp;
	out[0] = '\0';
	if (!bproc_spawn(exe, args, 2, work, work, NULL, &bp)) {
		return false;
	}
	size_t len = 0;
	const uint64_t end = now_ms() + timeout_ms;
	bool eof = false;
	while (!eof && now_ms() < end) {
		char chunk[4096];
		const int r = bproc_read(&bp, chunk, sizeof(chunk), 100);
		if (r > 0) {
			const size_t take = len + (size_t)r < cap - 1 ? (size_t)r : cap - 1 - len;
			memcpy(out + len, chunk, take);
			len += take;
		}
		eof = r < 0;
	}
	out[len] = '\0';
	bool ok = eof && bproc_wait(&bp, 2000) && bp.code == 0;
	if (!eof) {
		bproc_kill(&bp);
		ok = false;
	}
	bproc_close(&bp);
	return ok;
}

/* Ingest. */

struct out_file
{
	char path[CONV_STORE_PATH_MAX];
	char sha[CONV_STORE_SHA_HEX];
	uint64_t size;
	bool moved;
};

struct out_list
{
	struct conv_model_store *s;
	const char *dir;
	struct out_file f[32];
	uint32_t n;
};

static void
out_list_fn(void *user, const char *name, uint64_t size)
{
	struct out_list *l = (struct out_list *)user;
	if (l->n >= 32) {
		return;
	}
	struct out_file *f = &l->f[l->n];
	conv_store_fs_join(f->path, sizeof(f->path), l->dir, name);
	uint64_t got = 0;
	if (conv_store_i_hash_file(l->s, f->path, f->sha, &got) && got == size) {
		f->size = size;
		l->n++;
	}
}

static const struct out_file *
out_find(const struct out_list *l, const char *sha, uint64_t size)
{
	for (uint32_t i = 0; i < l->n; i++) {
		if (l->f[i].size == size && strcmp(l->f[i].sha, sha) == 0) {
			return &l->f[i];
		}
	}
	return NULL;
}

//! Every file of @p m is in @p l (or already a stamped blob).
static bool
model_matched(struct conv_model_store *s, const struct out_list *l, const struct conv_pack_model *m)
{
	for (uint32_t v = 0; v < m->variant_count; v++) {
		for (uint32_t f = 0; f < m->variants[v].file_count; f++) {
			const struct conv_pack_file *pf = &m->variants[v].files[f];
			if (out_find(l, pf->sha256, pf->size) != NULL) {
				continue;
			}
			struct conv_store_blob_disk d;
			conv_model_store_blob_disk(s, pf->sha256, &d);
			if (!(d.present && d.stamped && d.size == pf->size)) {
				return false;
			}
		}
	}
	return true;
}

//! Move @p m's files (data roles first, then model) into the store.
static enum conv_build_result
model_ingest(struct conv_model_store *s, struct out_list *l, const struct conv_pack_model *m)
{
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t v = 0; v < m->variant_count; v++) {
			for (uint32_t f = 0; f < m->variants[v].file_count; f++) {
				const struct conv_pack_file *pf = &m->variants[v].files[f];
				const bool model_role = strcmp(pf->role, "model") == 0;
				if ((pass == 0) == model_role) {
					continue;
				}
				struct out_file *of = (struct out_file *)out_find(l, pf->sha256, pf->size);
				if (of == NULL || of->moved) {
					continue; // already a stamped blob (checked by model_matched), or moved for
					          // another model
				}
				const enum conv_store_blob_result r =
				    conv_store_i_ingest(s, of->path, of->sha, of->size);
				if (r != CONV_STORE_BLOB_OK) {
					U_LOG_W("[conv-build] could not store %.12s…: %s", of->sha,
					        conv_store_blob_result_str(r));
					return CONV_BUILD_IO;
				}
				of->moved = true;
			}
		}
	}
	return CONV_BUILD_BUILT;
}

//! The builder's stderr log: <log_dir>/<recipe>-<nonce>.log.
static void
log_path_of(const struct conv_build_options *opt, const char *recipe, const char *nonce, char *out, size_t n)
{
	char dir[CONV_STORE_PATH_MAX];
	out[0] = '\0';
	if (opt->log_dir[0] != '\0') {
		cpy(dir, sizeof(dir), opt->log_dir);
		(void)conv_store_fs_mkdir_p(dir);
	} else if (!conv_scratch_dir("model-builder", dir, sizeof(dir))) {
		return;
	}
	char name[160];
	(void)snprintf(name, sizeof(name), "%s-%s.log", recipe, nonce);
	conv_store_fs_join(out, n, dir, name);
}

//! The build of one group (same recipe, same tool): steps 1-9.
static void
build_group(struct conv_model_store *s,
            struct group *g,
            const struct conv_build_ops *ops,
            const struct conv_build_options *opt,
            struct conv_build_model_result *out)
{
	const struct conv_pack *p = g->p;
	const struct conv_pack_build *b = g->b;
	char hint[320];

	// 1. Policy.
	bool research = false;
	for (uint32_t i = 0; i < g->n; i++) {
		research |= !g->m[i]->commercial;
	}
	if (research && !ops->allow_research_only) {
		if (ops->research_locked_off) {
			set_all(g, out, CONV_BUILD_POLICY_LOCKED,
			        "your administrator has turned off research-only models (HKLM Policy\\Lock)");
		} else {
			set_all(g, out, CONV_BUILD_POLICY,
			        "research-only: needs AllowResearchOnly (`conv research on`) or --allow-research-only");
		}
		return;
	}

	// 2. Licence: the text is the pinned one, and this user accepted it.
	for (uint32_t i = 0; i < g->n; i++) {
		for (uint32_t k = 0; k < g->mb[i]->source_count; k++) {
			const struct conv_pack_source_license *l = &g->mb[i]->sources[k].license;
			char err[256];
			if (!conv_pack_license_text_verify(p, l, err, sizeof(err))) {
				set_all(g, out, CONV_BUILD_LICENSE_TEXT, err);
				return;
			}
			bool asked = false;
			for (uint32_t a = 0; opt->accept_licences != NULL && a < opt->accept_count; a++) {
				asked |= opt->accept_licences[a] != NULL && strcmp(opt->accept_licences[a], l->id) == 0;
			}
			if (asked) {
				if (ops->licence_record == NULL ||
				    !ops->licence_record(ops->user, l->id, l->text_sha256)) {
					(void)snprintf(hint, sizeof(hint),
					               "could not record the acceptance of licence '%s'", l->id);
					set_all(g, out, CONV_BUILD_IO, hint);
					return;
				}
				continue;
			}
			if (ops->licence_accepted == NULL || !ops->licence_accepted(ops->user, l->id, l->text_sha256)) {
				char text[CONV_STORE_PATH_MAX];
				(void)conv_pack_license_text_path(p, l, text, sizeof(text));
				(void)snprintf(hint, sizeof(hint),
				               "accept licence '%s' (%s, %s; text: %s) with --accept-license %s", l->id,
				               l->name, l->link, text, l->id);
				set_all(g, out, CONV_BUILD_LICENSE, hint);
				return;
			}
		}
	}

	// Already in the store: nothing to build (the licence is still this user's to accept).
	bool all_present = true;
	for (uint32_t i = 0; i < g->n; i++) {
		const bool on = conv_store_i_model_on_disk(s, g->m[i]);
		all_present &= on;
		if (on) {
			out[g->idx[i]].result = CONV_BUILD_PRESENT;
		}
	}
	if (all_present) {
		return;
	}

	// 3. The builder.
	struct conv_build_tool tool;
	memset(&tool, 0, sizeof(tool));
	if (ops->find_tool == NULL || !ops->find_tool(ops->user, b->tool_id, &tool)) {
		(void)snprintf(hint, sizeof(hint), "install the model builder '%s' from the Catalog (%s)", b->tool_id,
		               b->tool_catalog_id[0] != '\0' ? b->tool_catalog_id : b->tool_id);
		set_all(g, out, CONV_BUILD_TOOL_MISSING, hint);
		return;
	}
	if (tool.version[0] != '\0' && conv_build_version_cmp(tool.version, b->tool_min_version) < 0) {
		(void)snprintf(hint, sizeof(hint), "the model builder is version %s; this model needs %s or later (%s)",
		               tool.version, b->tool_min_version, b->tool_catalog_id);
		set_all(g, out, CONV_BUILD_TOOL_OLD, hint);
		return;
	}

	// 4. Provenance, then describe must agree with the pack.
	struct conv_build_trust trust;
	memset(&trust, 0, sizeof(trust));
	if (ops->tool_trusted == NULL || !ops->tool_trusted(ops->user, &tool, &trust)) {
		(void)snprintf(hint, sizeof(hint), "%s: %s", tool.binary,
		               trust.reason[0] != '\0' ? trust.reason : "its provenance was not accepted");
		set_all(g, out, CONV_BUILD_TOOL_UNTRUSTED, hint);
		return;
	}
	for (uint32_t i = 0; i < g->n; i++) {
		struct conv_build_model_result *r = &out[g->idx[i]];
		cpy(r->tool, sizeof(r->tool), tool.id);
		cpy(r->tool_version, sizeof(r->tool_version), tool.version);
		cpy(r->provenance, sizeof(r->provenance), trust.provenance);
	}
	const char *variants[BUILD_MAX_MODELS];
	uint32_t nvar = 0;
	for (uint32_t i = 0; i < g->n; i++) {
		if (out[g->idx[i]].result == CONV_BUILD_PRESENT) {
			continue;
		}
		bool dup = false;
		for (uint32_t k = 0; k < nvar && !dup; k++) {
			dup = strcmp(variants[k], g->mb[i]->variant) == 0;
		}
		if (!dup) {
			variants[nvar++] = g->mb[i]->variant;
		}
	}
	{
		// describe runs in a scratch working dir of its own: nothing is staged yet.
		char dwork[CONV_STORE_PATH_MAX];
		if (!conv_scratch_dir("model-builder", dwork, sizeof(dwork))) {
			cpy(dwork, sizeof(dwork), conv_model_store_root(s));
		}
		char *dj = malloc(65536);
		const bool ran = dj != NULL && run_describe(tool.binary, dwork,
		                                            opt->describe_timeout_ms != 0 ? opt->describe_timeout_ms
		                                                                          : BUILD_DEFAULT_DESCRIBE_MS,
		                                            dj, 65536);
		char why[256];
		const bool agree = ran && conv_build_describe_check(dj, b, variants, nvar, why, sizeof(why));
		if (!ran) {
			cpy(why, sizeof(why), "`describe --json` did not exit 0");
		}
		free(dj);
		if (!agree) {
			set_all(g, out, CONV_BUILD_TOOL_MISMATCH, why);
			return;
		}
	}

	// 5. Resources, before any download.
	struct conv_build_machine mc;
	memset(&mc, 0, sizeof(mc));
	if (ops->machine != NULL) {
		(void)ops->machine(ops->user, conv_model_store_root(s), &mc);
	} else {
		(void)conv_build_machine_read(conv_model_store_root(s), &mc);
	}
	uint64_t need_disk = 0, need_ram = 0;
	bool os_ok = true;
	for (uint32_t i = 0; i < g->n; i++) {
		const struct conv_pack_build *mb = g->mb[i];
		need_disk = mb->needs_disk_mb > need_disk ? mb->needs_disk_mb : need_disk;
		need_ram = mb->needs_ram_mb > need_ram ? mb->needs_ram_mb : need_ram;
		bool any = mb->os_count == 0;
		for (uint32_t k = 0; k < mb->os_count; k++) {
			any |= conv_build_os_matches(mb->os[k], mc.os, mc.os_version);
		}
		os_ok &= any;
	}
	if (mc.free_known && mc.free_mb < need_disk) {
		(void)snprintf(hint, sizeof(hint), "building needs %llu MB free on the store's drive; %llu MB is free",
		               (unsigned long long)need_disk, (unsigned long long)mc.free_mb);
		set_all(g, out, CONV_BUILD_DISK, hint);
		return;
	}
	if (mc.phys_known && mc.phys_mb < need_ram) {
		(void)snprintf(hint, sizeof(hint), "building needs %llu MB of memory; this machine has %llu MB",
		               (unsigned long long)need_ram, (unsigned long long)mc.phys_mb);
		set_all(g, out, CONV_BUILD_MEMORY, hint);
		return;
	}
	if (!os_ok) {
		(void)snprintf(hint, sizeof(hint), "this model cannot be built on %s %s", mc.os, mc.os_version);
		set_all(g, out, CONV_BUILD_UNSUPPORTED, hint);
		return;
	}

	// 6. Staging: one build per recipe per store.
	struct run_ctx c;
	memset(&c, 0, sizeof(c));
	c.s = s;
	c.ops = ops;
	c.opt = opt;
	if (!conv_model_store_ensure_root(s)) {
		set_all(g, out, CONV_BUILD_IO, "cannot create the model store");
		return;
	}
	char staging_root[CONV_STORE_PATH_MAX];
	conv_store_fs_join(staging_root, sizeof(staging_root), conv_model_store_root(s), "staging");
	(void)conv_store_fs_mkdir_p(staging_root);
	(void)conv_model_store_clean_staging(s, 24ull * 3600ull);
	char nonce[64];
	conv_store_i_nonce(s, nonce, sizeof(nonce));
	char lock_name[96], lock_path[CONV_STORE_PATH_MAX];
	(void)snprintf(lock_name, sizeof(lock_name), "%s.lock", b->recipe);
	conv_store_fs_join(lock_path, sizeof(lock_path), staging_root, lock_name);
	char lock_text[1024];
	int lt = snprintf(lock_text, sizeof(lock_text), "%ld %s", os_process_current_pid(), nonce);
	for (uint32_t i = 0; i < g->n && lt > 0 && (size_t)lt < sizeof(lock_text); i++) {
		lt += snprintf(lock_text + lt, sizeof(lock_text) - (size_t)lt, " %s/%s", p->pack, g->m[i]->name);
	}
	if (lt > 0 && (size_t)lt < sizeof(lock_text) - 2) {
		lock_text[lt++] = '\n';
		lock_text[lt] = '\0';
	}
	for (;;) {
		const int lr = lock_try(lock_path, lock_text);
		if (lr == 1) {
			break;
		}
		if (lr < 0) {
			set_all(g, out, CONV_BUILD_IO, "cannot write the build lock");
			return;
		}
		if (opt->no_wait) {
			(void)snprintf(hint, sizeof(hint), "another process is building recipe '%s' in this store",
			               b->recipe);
			set_all(g, out, CONV_BUILD_BUSY, hint);
			return;
		}
		if (interrupted(&c)) {
			set_all(g, out, CONV_BUILD_CANCELLED, "cancelled while waiting for another build");
			return;
		}
		sleep_ms(opt->lock_poll_ms != 0 ? opt->lock_poll_ms : 1000);
	}
	conv_store_fs_join(c.staging, sizeof(c.staging), staging_root, nonce);
	conv_store_fs_join(c.cancel_file, sizeof(c.cancel_file), c.staging, "cancel");
	char src_dir[CONV_STORE_PATH_MAX], work[CONV_STORE_PATH_MAX], outd[CONV_STORE_PATH_MAX];
	conv_store_fs_join(src_dir, sizeof(src_dir), c.staging, "source");
	conv_store_fs_join(work, sizeof(work), c.staging, "work");
	conv_store_fs_join(outd, sizeof(outd), c.staging, "out");
	char log_path[CONV_STORE_PATH_MAX];
	log_path_of(opt, b->recipe, nonce, log_path, sizeof(log_path));
	for (uint32_t i = 0; i < g->n; i++) {
		struct conv_build_model_result *r = &out[g->idx[i]];
		if (r->result == CONV_BUILD_PRESENT) {
			continue;
		}
		r->ran = true;
		cpy(r->staging, sizeof(r->staging), c.staging);
		cpy(r->log, sizeof(r->log), log_path);
	}
	enum conv_build_result res = CONV_BUILD_BUILT;
	int32_t code = -1;
	hint[0] = '\0';
	if (!conv_store_fs_mkdir_p(src_dir) || !conv_store_fs_mkdir_p(work) || !conv_store_fs_mkdir_p(outd)) {
		res = CONV_BUILD_IO;
		cpy(hint, sizeof(hint), "cannot create the staging directory");
	}
	conv_store_i_set_cancel(s, run_cancel_fn, &c);

	// 7. Sources (the union over the group's models: one download serves the run).
	const struct conv_pack_source *srcs[CONV_STORE_MAX_SOURCES * BUILD_MAX_MODELS];
	uint32_t nsrc = 0;
	for (uint32_t i = 0; i < g->n && res == CONV_BUILD_BUILT; i++) {
		for (uint32_t k = 0; k < g->mb[i]->source_count; k++) {
			const struct conv_pack_source *sc = &g->mb[i]->sources[k];
			bool dup = false;
			for (uint32_t j = 0; j < nsrc; j++) {
				if (strcmp(srcs[j]->role, sc->role) == 0) {
					dup = true;
					if (strcmp(srcs[j]->sha256, sc->sha256) != 0) {
						res = CONV_BUILD_TOOL_MISMATCH;
						(void)snprintf(hint, sizeof(hint),
						               "two models of recipe '%s' pin different '%s' sources",
						               b->recipe, sc->role);
					}
				}
			}
			if (!dup) {
				srcs[nsrc++] = sc;
			}
		}
	}
	char src_paths[CONV_STORE_MAX_SOURCES * BUILD_MAX_MODELS][CONV_STORE_PATH_MAX];
	for (uint32_t j = 0; j < nsrc && res == CONV_BUILD_BUILT; j++) {
		char line[200];
		(void)snprintf(
		    line, sizeof(line),
		    "{\"v\":1,\"event\":\"phase\",\"phase\":\"download-source\",\"variant\":null,\"step\":%u,"
		    "\"steps\":%u}",
		    j + 1, nsrc);
		emit(&c, line);
		conv_store_fs_join(src_paths[j], sizeof(src_paths[j]), src_dir, srcs[j]->role);
		const enum conv_store_blob_result dr = conv_store_i_download(
		    s, srcs[j]->url, &srcs[j]->auth, srcs[j]->sha256, srcs[j]->size, src_paths[j], dl_progress_fn, &c);
		if (dr == CONV_STORE_BLOB_OK) {
			continue;
		}
		if (dr == CONV_STORE_BLOB_CANCELLED || run_cancel_fn(&c)) {
			res = CONV_BUILD_CANCELLED;
			cpy(hint, sizeof(hint), "cancelled while downloading the official file");
		} else if (dr == CONV_STORE_BLOB_BAD_DOWNLOAD) {
			res = CONV_BUILD_BAD_DOWNLOAD;
			(void)snprintf(hint, sizeof(hint), "source '%s' was not the pinned file (size or sha256)",
			               srcs[j]->role);
		} else if (dr == CONV_STORE_BLOB_IO) {
			res = CONV_BUILD_IO;
			cpy(hint, sizeof(hint), "cannot write the staging directory");
		} else {
			res = CONV_BUILD_DOWNLOAD;
			(void)snprintf(hint, sizeof(hint), "source '%s' could not be downloaded (%s): %s",
			               srcs[j]->role, conv_store_blob_result_str(dr), srcs[j]->url);
		}
	}

	// 8. Run.
	if (res == CONV_BUILD_BUILT) {
		static const uint32_t max_args = 60;
		const char *args[60];
		char src_args[CONV_STORE_MAX_SOURCES * BUILD_MAX_MODELS][CONV_STORE_PATH_MAX + 40];
		uint32_t n = 0;
		args[n++] = "build";
		args[n++] = "--recipe";
		args[n++] = b->recipe;
		for (uint32_t k = 0; k < nvar && n + 2 < max_args; k++) {
			args[n++] = "--variant";
			args[n++] = variants[k];
		}
		for (uint32_t j = 0; j < nsrc && n + 2 < max_args; j++) {
			(void)snprintf(src_args[j], sizeof(src_args[j]), "%s=%s", srcs[j]->role, src_paths[j]);
			args[n++] = "--source";
			args[n++] = src_args[j];
		}
		args[n++] = "--out";
		args[n++] = outd;
		args[n++] = "--work";
		args[n++] = work;
		args[n++] = "--cancel-file";
		args[n++] = c.cancel_file;
		for (uint32_t j = 0; j < nsrc && n + 2 < max_args; j++) {
			bool dup = false;
			for (uint32_t k = 0; k < j && !dup; k++) {
				dup = strcmp(srcs[k]->license.id, srcs[j]->license.id) == 0;
			}
			if (!dup) {
				args[n++] = "--accept-license";
				args[n++] = srcs[j]->license.id;
			}
		}
		U_LOG_W("[conv-build] building %s/%s with %s %s (%s), staging %s", p->pack, b->recipe, tool.binary,
		        tool.version, trust.provenance, c.staging);
		res = run_builder(&c, tool.binary, args, n, work, log_path[0] != '\0' ? log_path : NULL, &code, hint,
		                  sizeof(hint));
		if (res == CONV_BUILD_FAILED && hint[0] == '\0') {
			(void)snprintf(hint, sizeof(hint), "the builder exited with code %d; the log is in %s",
			               (int)code, log_path);
		}
	}
	for (uint32_t i = 0; i < g->n; i++) {
		if (out[g->idx[i]].result != CONV_BUILD_PRESENT) {
			out[g->idx[i]].exit_code = code;
		}
	}

	// 9. Ingest: the store's own hash of out/ decides, never the builder's `output` lines.
	if (res == CONV_BUILD_BUILT) {
		emit(&c, "{\"v\":1,\"event\":\"phase\",\"phase\":\"ingest\",\"variant\":null,\"step\":1,\"steps\":1}");
		struct out_list *ol = calloc(1, sizeof(*ol));
		if (ol == NULL) {
			res = CONV_BUILD_IO;
		} else {
			ol->s = s;
			ol->dir = outd;
			conv_store_fs_list(outd, out_list_fn, ol);
			bool any = false;
			for (uint32_t i = 0; i < g->n; i++) {
				struct conv_build_model_result *r = &out[g->idx[i]];
				if (r->result == CONV_BUILD_PRESENT) {
					continue;
				}
				if (!model_matched(s, ol, g->m[i])) {
					r->result = CONV_BUILD_OUTPUT_MISMATCH;
					cpy(r->hint, sizeof(r->hint),
					    "the builder's output does not hash to this model's files; nothing was "
					    "stored");
					continue;
				}
				r->result = model_ingest(s, ol, g->m[i]);
				if (r->result != CONV_BUILD_BUILT) {
					cpy(r->hint, sizeof(r->hint), "cannot move the built files into the store");
				}
				any |= r->result == CONV_BUILD_BUILT;
			}
			free(ol);
			if (any) {
				conv_store_i_record_pack(s, p);
			}
		}
		if (res != CONV_BUILD_BUILT) {
			set_all(g, out, res, "cannot read the build output");
		}
	} else {
		for (uint32_t i = 0; i < g->n; i++) {
			if (out[g->idx[i]].result != CONV_BUILD_PRESENT) {
				out[g->idx[i]].result = res;
				cpy(out[g->idx[i]].hint, sizeof(out[g->idx[i]].hint), hint);
			}
		}
	}

	conv_store_i_set_cancel(s, NULL, NULL);
	rmtree(c.staging);
	(void)conv_store_fs_remove(lock_path);
}

uint32_t
conv_model_store_build(struct conv_model_store *s,
                       const struct conv_pack *p,
                       const char *const *models,
                       uint32_t count,
                       const struct conv_build_ops *ops,
                       const struct conv_build_options *opt,
                       struct conv_build_model_result *out)
{
	struct conv_build_ops no_ops;
	memset(&no_ops, 0, sizeof(no_ops));
	struct conv_build_options no_opt;
	conv_build_options_defaults(&no_opt);
	ops = ops != NULL ? ops : &no_ops;
	opt = opt != NULL ? opt : &no_opt;
	for (uint32_t i = 0; i < count; i++) {
		memset(&out[i], 0, sizeof(out[i]));
		cpy(out[i].name, sizeof(out[i].name), models[i]);
		out[i].exit_code = -1;
		out[i].result = CONV_BUILD_NOT_BUILDABLE;
	}
	if (s == NULL || p == NULL) {
		return count;
	}
	bool done[BUILD_MAX_MODELS] = {false};
	for (uint32_t i = 0; i < count && i < BUILD_MAX_MODELS; i++) {
		const struct conv_pack_model *m = conv_pack_find_model(p, models[i]);
		if (m == NULL) {
			(void)snprintf(out[i].hint, sizeof(out[i].hint), "pack '%s' has no model '%s'", p->pack,
			               models[i]);
			done[i] = true;
		} else if (conv_pack_model_build(p, m) == NULL) {
			cpy(out[i].hint, sizeof(out[i].hint), "its local_build names no builder tool");
			done[i] = true;
		}
	}
	struct group *g = calloc(1, sizeof(*g));
	if (g == NULL) {
		return count;
	}
	for (uint32_t i = 0; i < count && i < BUILD_MAX_MODELS; i++) {
		if (done[i]) {
			continue;
		}
		memset(g, 0, sizeof(*g));
		g->p = p;
		for (uint32_t j = i; j < count && j < BUILD_MAX_MODELS; j++) {
			if (done[j]) {
				continue;
			}
			const struct conv_pack_model *m = conv_pack_find_model(p, models[j]);
			const struct conv_pack_build *b = conv_pack_model_build(p, m);
			if (g->n > 0 &&
			    (strcmp(b->recipe, g->b->recipe) != 0 || strcmp(b->tool_id, g->b->tool_id) != 0)) {
				continue;
			}
			bool dup = false;
			for (uint32_t k = 0; k < g->n && !dup; k++) {
				dup = g->m[k] == m;
			}
			if (g->n == 0) {
				g->b = b;
			}
			done[j] = true;
			if (dup) {
				out[j].result = CONV_BUILD_NOT_BUILDABLE;
				cpy(out[j].hint, sizeof(out[j].hint), "named twice");
				continue;
			}
			g->idx[g->n] = j;
			g->m[g->n] = m;
			g->mb[g->n] = b;
			g->n++;
		}
		build_group(s, g, ops, opt, out);
	}
	free(g);
	uint32_t failed = 0;
	for (uint32_t i = 0; i < count; i++) {
		failed += out[i].result == CONV_BUILD_BUILT || out[i].result == CONV_BUILD_PRESENT ? 0u : 1u;
	}
	return failed;
}

#endif // !XRT_OS_ANDROID

uint32_t
conv_model_store_remove_model(struct conv_model_store *s, const char *pack, const char *model, uint64_t *bytes_out)
{
	uint32_t deleted = 0;
	uint64_t bytes = 0;
	struct conv_pack *p = calloc(1, sizeof(*p));
	const uint32_t np = conv_model_store_pack_count(s);
	for (uint32_t i = 0; p != NULL && i < np; i++) {
		if (!conv_model_store_get_pack(s, i, p) || strcmp(p->pack, pack) != 0) {
			continue;
		}
		const struct conv_pack_model *m = conv_pack_find_model(p, model);
		for (uint32_t v = 0; m != NULL && v < m->variant_count; v++) {
			for (uint32_t f = 0; f < m->variants[v].file_count; f++) {
				const struct conv_pack_file *pf = &m->variants[v].files[f];
				if (conv_store_i_referenced_elsewhere(s, pack, model, pf->sha256)) {
					continue;
				}
				char blob[CONV_STORE_PATH_MAX], stamp[CONV_STORE_PATH_MAX];
				conv_store_i_blob_paths(s, pf->sha256, blob, stamp);
				uint64_t size = 0;
				bool is_dir = false;
				if (!conv_store_fs_stat(blob, &is_dir, &size) || is_dir) {
					(void)conv_store_fs_remove(stamp);
					continue;
				}
				// The stamp first: a half-removed blob is never READY.
				(void)conv_store_fs_remove(stamp);
				if (conv_store_fs_remove(blob)) {
					deleted++;
					bytes += size;
				}
				conv_store_i_forget_blob(s, pf->sha256);
			}
		}
		break;
	}
	free(p);
	if (bytes_out != NULL) {
		*bytes_out = bytes;
	}
	return deleted;
}
