// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The desktop-control user store and audit log (XR_DXR_desktop_control
 *         spec §6, ADR-056 D3.6). See u_desktop_policy.h.
 */

#include "u_desktop_policy.h"
#include "u_desktop.h"

#include "xrt/xrt_config_os.h"

#include "util/u_logging.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef XRT_OS_MACOS
#include <mach-o/dyld.h>
#endif

#define STORE_NAME "desktop_control.json"
#define AUDIT_NAME "desktop_control_audit.jsonl"
#define PATH_CAP 1024

#ifdef XRT_OS_WINDOWS
#define SEP '\\'
#else
#define SEP '/'
#endif


/*
 *
 * Portable file helpers (UTF-8 paths everywhere; wide APIs on Windows).
 *
 */

#ifdef XRT_OS_WINDOWS
static wchar_t *
u8_to_w(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0) {
		return NULL;
	}
	wchar_t *w = (wchar_t *)calloc((size_t)n, sizeof(wchar_t));
	if (w == NULL) {
		return NULL;
	}
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}

static bool
env_u8(const wchar_t *name, char *out, size_t cap)
{
	wchar_t w[PATH_CAP];
	DWORD n = GetEnvironmentVariableW(name, w, PATH_CAP);
	if (n == 0 || n >= PATH_CAP) {
		return false;
	}
	return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) > 1;
}
#else
static bool
env_u8(const char *name, char *out, size_t cap)
{
	const char *v = getenv(name);
	if (v == NULL || v[0] == '\0') {
		return false;
	}
	int n = snprintf(out, cap, "%s", v);
	return n > 0 && (size_t)n < cap;
}
#endif

static FILE *
file_open(const char *path, const char *mode)
{
#ifdef XRT_OS_WINDOWS
	if (mode[0] == 'r') {
		// Readers share EVERYTHING, delete included: _wfopen omits FILE_SHARE_DELETE,
		// so an app merely reading the store would make the writer's MoveFileExW
		// rename-over fail with a sharing violation — and the kill switch not land.
		wchar_t *wr = u8_to_w(path);
		if (wr == NULL) {
			return NULL;
		}
		HANDLE h = CreateFileW(wr, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		free(wr);
		if (h == INVALID_HANDLE_VALUE) {
			return NULL;
		}
		int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
		if (fd < 0) {
			CloseHandle(h);
			return NULL;
		}
		FILE *rf = _fdopen(fd, "rb");
		if (rf == NULL) {
			_close(fd); // closes the handle too
		}
		return rf;
	}
	wchar_t *wp = u8_to_w(path);
	wchar_t wm[8];
	size_t i = 0;
	for (; mode[i] != '\0' && i < 7; i++) {
		wm[i] = (wchar_t)mode[i];
	}
	wm[i] = 0;
	FILE *f = wp != NULL ? _wfopen(wp, wm) : NULL;
	free(wp);
	return f;
#else
	// Private files: 0600, whatever the umask.
	int flags = 0;
	if (mode[0] == 'r') {
		return fopen(path, mode);
	} else if (mode[0] == 'a') {
		flags = O_WRONLY | O_CREAT | O_APPEND;
	} else {
		flags = O_WRONLY | O_CREAT | O_TRUNC;
	}
	int fd = open(path, flags, 0600);
	if (fd < 0) {
		return NULL;
	}
	FILE *f = fdopen(fd, mode);
	if (f == NULL) {
		close(fd);
	}
	return f;
#endif
}

static bool
file_rename_replace(const char *from, const char *to)
{
#ifdef XRT_OS_WINDOWS
	wchar_t *wf = u8_to_w(from);
	wchar_t *wt = u8_to_w(to);
	bool ok = false;
	if (wf != NULL && wt != NULL) {
		// Our own readers share delete (file_open), but another process (an older
		// runtime, an antivirus or indexer) may hold the target briefly: retry a
		// sharing violation / access denied for up to ~1 s.
		for (int attempt = 0; attempt < 50; attempt++) {
			if (MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
				ok = true;
				break;
			}
			DWORD e = GetLastError();
			if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED) {
				break;
			}
			Sleep(20);
		}
	}
	free(wf);
	free(wt);
	return ok;
#else
	return rename(from, to) == 0;
#endif
}

static void
file_remove(const char *path)
{
#ifdef XRT_OS_WINDOWS
	wchar_t *w = u8_to_w(path);
	if (w != NULL) {
		DeleteFileW(w);
	}
	free(w);
#else
	unlink(path);
#endif
}

//! Size of @p path in bytes, or -1.
static long long
file_size(const char *path)
{
#ifdef XRT_OS_WINDOWS
	wchar_t *w = u8_to_w(path);
	WIN32_FILE_ATTRIBUTE_DATA a;
	bool ok = w != NULL && GetFileAttributesExW(w, GetFileExInfoStandard, &a);
	free(w);
	if (!ok) {
		return -1;
	}
	return ((long long)a.nFileSizeHigh << 32) | (long long)a.nFileSizeLow;
#else
	struct stat st;
	if (stat(path, &st) != 0) {
		return -1;
	}
	return (long long)st.st_size;
#endif
}

struct file_stamp
{
	bool exists;
	uint64_t a, b, c, d;
};

static struct file_stamp
file_get_stamp(const char *path)
{
	struct file_stamp s;
	memset(&s, 0, sizeof(s));
#ifdef XRT_OS_WINDOWS
	wchar_t *w = u8_to_w(path);
	WIN32_FILE_ATTRIBUTE_DATA a;
	bool ok = w != NULL && GetFileAttributesExW(w, GetFileExInfoStandard, &a);
	free(w);
	if (!ok) {
		return s;
	}
	s.exists = true;
	s.a = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
	// A rename-over carries the temp file's creation time: new on every write.
	s.b = ((uint64_t)a.ftCreationTime.dwHighDateTime << 32) | a.ftCreationTime.dwLowDateTime;
	s.c = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
#else
	struct stat st;
	if (stat(path, &st) != 0) {
		return s;
	}
	s.exists = true;
	s.a = (uint64_t)st.st_mtime;
#if defined(XRT_OS_MACOS)
	s.b = (uint64_t)st.st_mtimespec.tv_nsec;
#elif defined(XRT_OS_LINUX)
	s.b = (uint64_t)st.st_mtim.tv_nsec;
#endif
	s.c = (uint64_t)st.st_size;
	// A rename-over is a new inode: catches two writes inside one mtime tick.
	s.d = (uint64_t)st.st_ino;
#endif
	return s;
}

static bool
stamp_equal(const struct file_stamp *x, const struct file_stamp *y)
{
	return x->exists == y->exists && x->a == y->a && x->b == y->b && x->c == y->c && x->d == y->d;
}

//! mkdir -p of the directory part of @p file_path.
static bool
make_parent_dirs(const char *file_path)
{
	char tmp[PATH_CAP];
	int n = snprintf(tmp, sizeof(tmp), "%s", file_path);
	if (n <= 0 || (size_t)n >= sizeof(tmp)) {
		return false;
	}
	char *last = strrchr(tmp, SEP);
#ifdef XRT_OS_WINDOWS
	char *last2 = strrchr(tmp, '/');
	if (last2 != NULL && (last == NULL || last2 > last)) {
		last = last2;
	}
#endif
	if (last == NULL) {
		return true;
	}
	*last = '\0';
	for (char *p = tmp + 1; *p != '\0'; p++) {
		bool sep = *p == '/' || *p == SEP;
		if (!sep) {
			continue;
		}
#ifdef XRT_OS_WINDOWS
		if (p > tmp && p[-1] == ':') {
			continue; // "C:\"
		}
#endif
		char saved = *p;
		*p = '\0';
#ifdef XRT_OS_WINDOWS
		wchar_t *w = u8_to_w(tmp);
		if (w != NULL) {
			CreateDirectoryW(w, NULL);
		}
		free(w);
#else
		mkdir(tmp, 0700);
#endif
		*p = saved;
	}
#ifdef XRT_OS_WINDOWS
	wchar_t *w = u8_to_w(tmp);
	if (w != NULL) {
		CreateDirectoryW(w, NULL);
	}
	free(w);
#else
	mkdir(tmp, 0700);
#endif
	return true;
}

//! Whole file into a NUL-terminated malloc'd buffer (max 4 MiB), or NULL.
static char *
read_whole(const char *path, size_t *out_len)
{
	FILE *f = file_open(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	size_t cap = 4096, len = 0;
	char *buf = (char *)malloc(cap + 1);
	while (buf != NULL) {
		size_t got = fread(buf + len, 1, cap - len, f);
		len += got;
		if (got == 0) {
			break;
		}
		if (len == cap) {
			if (cap >= 4u * 1024u * 1024u) {
				break;
			}
			char *nb = (char *)realloc(buf, cap * 2 + 1);
			if (nb == NULL) {
				free(buf);
				buf = NULL;
				break;
			}
			buf = nb;
			cap *= 2;
		}
	}
	fclose(f);
	if (buf != NULL) {
		buf[len] = '\0';
		if (out_len != NULL) {
			*out_len = len;
		}
	}
	return buf;
}


/*
 *
 * Paths.
 *
 */

static bool
join(char *out, size_t cap, const char *dir, const char *tail)
{
	int n = snprintf(out, cap, "%s%c%s", dir, SEP, tail);
	return n > 0 && (size_t)n < cap;
}

static bool
override_dir(char *out, size_t cap)
{
#ifdef XRT_OS_WINDOWS
	bool on = env_u8(L"" U_DESKTOP_POLICY_DIR_ENV, out, cap);
#else
	bool on = env_u8(U_DESKTOP_POLICY_DIR_ENV, out, cap);
#endif
	// The override moves the kill switch and every deny somewhere else: say so,
	// once per process (a benign race on the flag at worst logs it twice).
	static bool warned = false;
	if (on && !warned) {
		warned = true;
		U_LOG_W(
		    "desktop_control: %s is set - the desktop-control store and audit log are in %s, not the "
		    "user's (the user's kill switch and denies do not apply to this process)",
		    U_DESKTOP_POLICY_DIR_ENV, out);
	}
	return on;
}

static bool
store_dir(char *out, size_t cap)
{
	if (override_dir(out, cap)) {
		return true;
	}
#if defined(XRT_OS_WINDOWS)
	char base[PATH_CAP];
	if (!env_u8(L"LOCALAPPDATA", base, sizeof(base))) {
		return false;
	}
	return join(out, cap, base, "DisplayXR");
#elif defined(XRT_OS_MACOS)
	char home[PATH_CAP];
	if (!env_u8("HOME", home, sizeof(home))) {
		return false;
	}
	return join(out, cap, home, "Library/Application Support/DisplayXR");
#else
	char base[PATH_CAP];
	if (env_u8("XDG_CONFIG_HOME", base, sizeof(base))) {
		return join(out, cap, base, "displayxr");
	}
	char home[PATH_CAP];
	if (!env_u8("HOME", home, sizeof(home))) {
		return false;
	}
	return join(out, cap, home, ".config/displayxr");
#endif
}

static bool
audit_dir(char *out, size_t cap)
{
#if defined(XRT_OS_LINUX)
	if (override_dir(out, cap)) {
		return true;
	}
	char base[PATH_CAP];
	if (env_u8("XDG_STATE_HOME", base, sizeof(base))) {
		return join(out, cap, base, "displayxr");
	}
	char home[PATH_CAP];
	if (!env_u8("HOME", home, sizeof(home))) {
		return false;
	}
	return join(out, cap, home, ".local/state/displayxr");
#else
	return store_dir(out, cap);
#endif
}

bool
u_desktop_policy_store_path(char *out, size_t cap)
{
	char dir[PATH_CAP];
	return store_dir(dir, sizeof(dir)) && join(out, cap, dir, STORE_NAME);
}

bool
u_desktop_policy_audit_path(char *out, size_t cap)
{
	char dir[PATH_CAP];
	return audit_dir(dir, sizeof(dir)) && join(out, cap, dir, AUDIT_NAME);
}

bool
u_desktop_policy_exe_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL) {
		return false;
	}
#ifdef XRT_OS_WINDOWS
	for (;; a++, b++) {
		char x = *a, y = *b;
		if (x == '/') {
			x = '\\';
		}
		if (y == '/') {
			y = '\\';
		}
		if (x >= 'A' && x <= 'Z') {
			x = (char)(x - 'A' + 'a');
		}
		if (y >= 'A' && y <= 'Z') {
			y = (char)(y - 'A' + 'a');
		}
		if (x != y) {
			return false;
		}
		if (x == '\0') {
			return true;
		}
	}
#else
	return strcmp(a, b) == 0;
#endif
}


/*
 *
 * Store parse / serialise.
 *
 */

struct u_desktop_policy
{
	char path[PATH_CAP];
	bool have_path;
	struct file_stamp stamp;
	bool loaded_once;
	bool have_good;  //!< a good (or missing = defaults) read happened in this process
	bool unreadable; //!< the file exists, cannot be parsed, and there is no good read to keep
	bool suspended;
	uint32_t idle_ms;
	uint64_t kill_count; //!< kill_switch_count: how many times the kill switch was turned on
	cJSON *apps;         //!< owned object: exe -> "allow" | "deny"
};

static void
policy_defaults(struct u_desktop_policy *p)
{
	p->suspended = false;
	p->idle_ms = U_DESKTOP_POLICY_DEFAULT_IDLE_MS;
	p->kill_count = 0;
	cJSON_Delete(p->apps);
	p->apps = NULL;
}

static enum u_desktop_app_decision
decision_from_json(const cJSON *v)
{
	if (!cJSON_IsString(v) || v->valuestring == NULL) {
		return U_DESKTOP_APP_NONE;
	}
	if (strcmp(v->valuestring, "allow") == 0) {
		return U_DESKTOP_APP_ALLOW;
	}
	if (strcmp(v->valuestring, "deny") == 0) {
		return U_DESKTOP_APP_DENY;
	}
	return U_DESKTOP_APP_NONE;
}

//! An entry's answer for ONE scope: a string is the CONTROL answer (v1), an object
//! has one field per scope (v2).
static enum u_desktop_app_decision
decision_for_scope(const cJSON *v, uint32_t scope)
{
	if (cJSON_IsString(v)) {
		return scope == U_DESKTOP_SCOPE_CONTROL ? decision_from_json(v) : U_DESKTOP_APP_NONE;
	}
	if (cJSON_IsObject(v)) {
		const char *key = scope == U_DESKTOP_SCOPE_CAPTURE ? "capture" : "control";
		return decision_from_json(cJSON_GetObjectItemCaseSensitive(v, key));
	}
	return U_DESKTOP_APP_NONE;
}

const char *
u_desktop_scope_str(uint32_t scopes)
{
	uint32_t both = U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE;
	if ((scopes & both) == both) {
		return "all";
	}
	return (scopes & U_DESKTOP_SCOPE_CAPTURE) != 0 ? "capture" : "control";
}

//! Parse @p root (may be NULL) into @p p.
static void
policy_from_json(struct u_desktop_policy *p, const cJSON *root)
{
	policy_defaults(p);
	if (!cJSON_IsObject(root)) {
		return;
	}
	const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "suspended");
	p->suspended = cJSON_IsTrue(s);
	const cJSON *idle = cJSON_GetObjectItemCaseSensitive(root, "idle_ms");
	if (cJSON_IsNumber(idle) && idle->valuedouble >= 0) {
		double v = idle->valuedouble;
		p->idle_ms = v > U_DESKTOP_POLICY_MAX_IDLE_MS ? U_DESKTOP_POLICY_MAX_IDLE_MS : (uint32_t)v;
	}
	const cJSON *kc = cJSON_GetObjectItemCaseSensitive(root, "kill_switch_count");
	if (cJSON_IsNumber(kc) && kc->valuedouble >= 0) {
		p->kill_count = (uint64_t)kc->valuedouble;
	}
	const cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
	if (cJSON_IsObject(apps)) {
		p->apps = cJSON_Duplicate(apps, true);
	}
}

enum load_status
{
	LOAD_MISSING, //!< no file: defaults
	LOAD_OK,
	LOAD_CORRUPT, //!< exists but cannot be read / parsed: NEVER defaults (spec §6.1)
};

static cJSON *
load_root_status(const char *path, enum load_status *out_status)
{
	*out_status = LOAD_MISSING;
	if (!file_get_stamp(path).exists) {
		return NULL;
	}
	char *txt = read_whole(path, NULL);
	if (txt == NULL) {
		*out_status = LOAD_CORRUPT;
		return NULL;
	}
	cJSON *root = cJSON_Parse(txt);
	free(txt);
	if (root == NULL || !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		*out_status = LOAD_CORRUPT;
		return NULL;
	}
	*out_status = LOAD_OK;
	return root;
}


/*
 *
 * Cross-process lock around every read-modify-write: `<store>.lock`.
 *
 */

#ifdef XRT_OS_WINDOWS
typedef HANDLE store_lock_t;
#define STORE_LOCK_NONE INVALID_HANDLE_VALUE
#else
typedef int store_lock_t;
#define STORE_LOCK_NONE (-1)
#endif

static store_lock_t
store_lock(const char *store_path)
{
	char lp[PATH_CAP + 8];
	snprintf(lp, sizeof(lp), "%s.lock", store_path);
	make_parent_dirs(lp);
#ifdef XRT_OS_WINDOWS
	wchar_t *w = u8_to_w(lp);
	if (w == NULL) {
		return STORE_LOCK_NONE;
	}
	HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	free(w);
	if (h == INVALID_HANDLE_VALUE) {
		return STORE_LOCK_NONE;
	}
	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
	if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) {
		CloseHandle(h);
		return STORE_LOCK_NONE;
	}
	return h;
#else
	int fd = open(lp, O_RDWR | O_CREAT, 0600);
	if (fd < 0) {
		return STORE_LOCK_NONE;
	}
	while (flock(fd, LOCK_EX) != 0) {
		if (errno != EINTR) {
			close(fd);
			return STORE_LOCK_NONE;
		}
	}
	return fd;
#endif
}

static void
store_unlock(store_lock_t l)
{
	if (l == STORE_LOCK_NONE) {
		return;
	}
#ifdef XRT_OS_WINDOWS
	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
	UnlockFileEx(l, 0, 1, 0, &ov);
	CloseHandle(l);
#else
	flock(l, LOCK_UN);
	close(l);
#endif
}

static bool
save_root_atomic(const char *path, cJSON *root)
{
	char *txt = cJSON_Print(root);
	if (txt == NULL) {
		return false;
	}
	make_parent_dirs(path);

	char tmp[PATH_CAP + 32];
	snprintf(tmp, sizeof(tmp), "%s.tmp.%u", path, u_desktop_self_pid());
	FILE *f = file_open(tmp, "wb");
	if (f == NULL) {
		cJSON_free(txt);
		return false;
	}
	size_t len = strlen(txt);
	bool ok = fwrite(txt, 1, len, f) == len && fwrite("\n", 1, 1, f) == 1;
	ok = fflush(f) == 0 && ok;
#ifndef XRT_OS_WINDOWS
	ok = fsync(fileno(f)) == 0 && ok;
#endif
	ok = fclose(f) == 0 && ok;
	cJSON_free(txt);
	if (!ok || !file_rename_replace(tmp, path)) {
		file_remove(tmp);
		return false;
	}
	return true;
}

//! Fresh document with the required fields, from @p old (may be NULL).
static cJSON *
normalised_root(cJSON *old)
{
	cJSON *root = old != NULL ? old : cJSON_CreateObject();
	if (root == NULL) {
		return NULL;
	}
	cJSON_DeleteItemFromObjectCaseSensitive(root, "version");
	cJSON_AddNumberToObject(root, "version", 2); // spec v2: entries may be per-scope objects
	if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "suspended"))) {
		cJSON_DeleteItemFromObjectCaseSensitive(root, "suspended");
		cJSON_AddBoolToObject(root, "suspended", false);
	}
	if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "idle_ms"))) {
		cJSON_DeleteItemFromObjectCaseSensitive(root, "idle_ms");
		cJSON_AddNumberToObject(root, "idle_ms", U_DESKTOP_POLICY_DEFAULT_IDLE_MS);
	}
	if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "apps"))) {
		cJSON_DeleteItemFromObjectCaseSensitive(root, "apps");
		cJSON_AddObjectToObject(root, "apps");
	}
	return root;
}


/*
 *
 * Cached reader.
 *
 */

struct u_desktop_policy *
u_desktop_policy_create(void)
{
	struct u_desktop_policy *p = (struct u_desktop_policy *)calloc(1, sizeof(*p));
	if (p == NULL) {
		return NULL;
	}
	policy_defaults(p);
	p->have_path = u_desktop_policy_store_path(p->path, sizeof(p->path));
	u_desktop_policy_refresh(p);
	return p;
}

void
u_desktop_policy_destroy(struct u_desktop_policy **p_ptr)
{
	if (p_ptr == NULL || *p_ptr == NULL) {
		return;
	}
	cJSON_Delete((*p_ptr)->apps);
	free(*p_ptr);
	*p_ptr = NULL;
}

bool
u_desktop_policy_refresh(struct u_desktop_policy *p)
{
	if (p == NULL || !p->have_path) {
		return false;
	}
	struct file_stamp now = file_get_stamp(p->path);
	if (p->loaded_once && stamp_equal(&now, &p->stamp)) {
		return false;
	}
	p->stamp = now;
	p->loaded_once = true;
	enum load_status st;
	cJSON *root = load_root_status(p->path, &st);
	if (st == LOAD_CORRUPT) {
		// Never defaults: that would silently drop the kill switch and every
		// deny. Keep the last good policy; with none, fail closed.
		if (!p->have_good && !p->unreadable) {
			U_LOG_W(
			    "desktop_control: %s cannot be parsed - desktop control is suspended until it is "
			    "fixed or removed",
			    p->path);
		}
		p->unreadable = !p->have_good;
		return true;
	}
	policy_from_json(p, root);
	cJSON_Delete(root);
	p->have_good = true;
	p->unreadable = false;
	return true;
}

bool
u_desktop_policy_is_unreadable(const struct u_desktop_policy *p)
{
	return p != NULL && p->unreadable;
}

uint64_t
u_desktop_policy_kill_switch_count(const struct u_desktop_policy *p)
{
	return p->kill_count;
}

bool
u_desktop_policy_is_suspended(const struct u_desktop_policy *p)
{
	return p != NULL && (p->suspended || p->unreadable);
}

uint32_t
u_desktop_policy_idle_ms(const struct u_desktop_policy *p)
{
	return p != NULL ? p->idle_ms : U_DESKTOP_POLICY_DEFAULT_IDLE_MS;
}

//! Every spelling of @p exe in @p apps, one scope; a deny anywhere wins.
static enum u_desktop_app_decision
apps_decision(const cJSON *apps, const char *exe, uint32_t scope)
{
	enum u_desktop_app_decision best = U_DESKTOP_APP_NONE;
	const cJSON *it = NULL;
	cJSON_ArrayForEach(it, apps)
	{
		if (it->string == NULL || !u_desktop_policy_exe_equal(it->string, exe)) {
			continue;
		}
		enum u_desktop_app_decision d = decision_for_scope(it, scope);
		if (d == U_DESKTOP_APP_DENY) {
			return d;
		}
		if (d == U_DESKTOP_APP_ALLOW) {
			best = d;
		}
	}
	return best;
}

enum u_desktop_app_decision
u_desktop_policy_app_scope(const struct u_desktop_policy *p, const char *exe, uint32_t scope)
{
	if (p == NULL || p->apps == NULL || exe == NULL) {
		return U_DESKTOP_APP_NONE;
	}
	// A deny anywhere wins over an allow written under another spelling.
	return apps_decision(p->apps, exe, scope);
}

enum u_desktop_app_decision
u_desktop_policy_app(const struct u_desktop_policy *p, const char *exe)
{
	return u_desktop_policy_app_scope(p, exe, U_DESKTOP_SCOPE_CONTROL);
}

void
u_desktop_policy_for_each_app(const struct u_desktop_policy *p, u_desktop_policy_app_fn fn, void *ctx)
{
	if (p == NULL || p->apps == NULL || fn == NULL) {
		return;
	}
	const cJSON *it = NULL;
	cJSON_ArrayForEach(it, p->apps)
	{
		if (it->string != NULL) {
			fn(ctx, it->string, decision_for_scope(it, U_DESKTOP_SCOPE_CONTROL));
		}
	}
}

void
u_desktop_policy_for_each_app_scopes(const struct u_desktop_policy *p, u_desktop_policy_app_scopes_fn fn, void *ctx)
{
	if (p == NULL || p->apps == NULL || fn == NULL) {
		return;
	}
	const cJSON *it = NULL;
	cJSON_ArrayForEach(it, p->apps)
	{
		if (it->string != NULL) {
			fn(ctx, it->string, decision_for_scope(it, U_DESKTOP_SCOPE_CONTROL),
			   decision_for_scope(it, U_DESKTOP_SCOPE_CAPTURE));
		}
	}
}


/*
 *
 * Writers.
 *
 */

bool
u_desktop_policy_set_app(const char *exe, enum u_desktop_app_decision d, const char *by)
{
	return u_desktop_policy_set_app_scope(exe, U_DESKTOP_SCOPE_CONTROL, d, by);
}

static const char *
decision_word(enum u_desktop_app_decision d)
{
	return d == U_DESKTOP_APP_ALLOW ? "allow" : "deny";
}

bool
u_desktop_policy_set_app_scope(const char *exe, uint32_t scopes, enum u_desktop_app_decision d, const char *by)
{
	scopes &= U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE;
	if (exe == NULL || exe[0] == '\0' || scopes == 0) {
		return false;
	}
	char path[PATH_CAP];
	if (!u_desktop_policy_store_path(path, sizeof(path))) {
		return false;
	}
	store_lock_t lk = store_lock(path);
	if (lk == STORE_LOCK_NONE) {
		// Never write unlocked: a concurrent `desktop stop` could be overwritten.
		U_LOG_W("desktop_control: could not lock %s.lock - not changing the store", path);
		return false;
	}
	enum load_status st;
	cJSON *loaded = load_root_status(path, &st); // re-read UNDER the lock
	if (st == LOAD_CORRUPT) {
		// Writing over it would turn its kill switch / denies into defaults.
		U_LOG_W("desktop_control: %s cannot be parsed - not changing it (fix or remove the file)", path);
		store_unlock(lk);
		return false;
	}
	cJSON *root = normalised_root(loaded);
	if (root == NULL) {
		store_unlock(lk);
		return false;
	}
	cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");

	// The answers this executable has now (every spelling; a deny wins), with the
	// change applied to the scopes asked for.
	enum u_desktop_app_decision ctl = apps_decision(apps, exe, U_DESKTOP_SCOPE_CONTROL);
	enum u_desktop_app_decision cap = apps_decision(apps, exe, U_DESKTOP_SCOPE_CAPTURE);
	bool had = ctl != U_DESKTOP_APP_NONE || cap != U_DESKTOP_APP_NONE;
	if (scopes & U_DESKTOP_SCOPE_CONTROL) {
		ctl = d;
	}
	if (scopes & U_DESKTOP_SCOPE_CAPTURE) {
		cap = d;
	}

	// Drop every spelling of this executable, then add the merged entry.
	bool removed = false;
	for (cJSON *it = apps->child; it != NULL;) {
		cJSON *next = it->next;
		if (it->string != NULL && u_desktop_policy_exe_equal(it->string, exe)) {
			cJSON_Delete(cJSON_DetachItemViaPointer(apps, it));
			removed = true;
		}
		it = next;
	}
	if (cap == U_DESKTOP_APP_NONE && ctl != U_DESKTOP_APP_NONE) {
		// No CAPTURE answer: the v1 string form, which a v1 runtime still reads.
		cJSON_AddStringToObject(apps, exe, decision_word(ctl));
	} else if (cap != U_DESKTOP_APP_NONE) {
		cJSON *e = cJSON_AddObjectToObject(apps, exe);
		if (e != NULL && ctl != U_DESKTOP_APP_NONE) {
			cJSON_AddStringToObject(e, "control", decision_word(ctl));
		}
		if (e != NULL) {
			cJSON_AddStringToObject(e, "capture", decision_word(cap));
		}
	}
	bool ok = save_root_atomic(path, root);
	cJSON_Delete(root);
	store_unlock(lk);

	if (ok && (d != U_DESKTOP_APP_NONE || (removed && had))) {
		struct u_desktop_audit_record r;
		memset(&r, 0, sizeof(r));
		r.action = d == U_DESKTOP_APP_ALLOW ? "allow" : d == U_DESKTOP_APP_DENY ? "deny" : "forget";
		r.chars = -1;
		r.subject = exe;
		r.by = by;
		r.scope = u_desktop_scope_str(scopes);
		r.result = "ok";
		u_desktop_audit_append(&r);
	}
	return ok;
}

bool
u_desktop_policy_set_suspended(bool suspended, const char *by)
{
	if (suspended) {
		// The kill switch ends every saved OS grant too (spec §6.4): the next request
		// of every app shows the OS dialog again. Done first and whatever happens to
		// the store below — a grant left on disk would outlive a "stop".
		uint32_t n = u_desktop_grant_remove_all();
		if (n > 0) {
			U_LOG_W("desktop_control: kill switch - removed %u saved grant(s)", n);
		}
	}
	char path[PATH_CAP];
	if (!u_desktop_policy_store_path(path, sizeof(path))) {
		return false;
	}
	store_lock_t lk = store_lock(path);
	if (lk == STORE_LOCK_NONE) {
		if (!suspended) {
			U_LOG_W("desktop_control: could not lock %s.lock - not resuming", path);
			return false;
		}
		// The kill switch must always land: proceed unlocked. The worst a racing
		// writer can do is an allow/deny lost to this write, never a resume.
		U_LOG_W("desktop_control: could not lock %s.lock - writing the kill switch unlocked", path);
	}
	enum load_status st;
	cJSON *loaded = load_root_status(path, &st); // re-read UNDER the lock
	if (st == LOAD_CORRUPT) {
		if (!suspended) {
			// Resuming over an unreadable file would mean "defaults": refuse.
			U_LOG_W("desktop_control: %s cannot be parsed - not resuming (fix or remove the file)", path);
			store_unlock(lk);
			return false;
		}
		// The kill switch must always work: keep the damaged file aside for the
		// user and write a fresh, suspended store.
		char aside[PATH_CAP + 16];
		snprintf(aside, sizeof(aside), "%s.corrupt", path);
		file_rename_replace(path, aside);
		U_LOG_W("desktop_control: %s could not be parsed - moved to %s; writing a suspended store", path,
		        aside);
	}
	cJSON *root = normalised_root(loaded);
	if (root == NULL) {
		store_unlock(lk);
		return false;
	}
	bool was = st == LOAD_CORRUPT ? false : cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "suspended"));
	cJSON_DeleteItemFromObjectCaseSensitive(root, "suspended");
	cJSON_AddBoolToObject(root, "suspended", suspended);
	if (suspended) {
		// Every "on" counts (spec §6.4): a running session that sees the count change
		// ends its grant, even if the switch is already off again when it looks.
		const cJSON *kc = cJSON_GetObjectItemCaseSensitive(root, "kill_switch_count");
		double n = cJSON_IsNumber(kc) && kc->valuedouble >= 0 ? kc->valuedouble : 0;
		cJSON_DeleteItemFromObjectCaseSensitive(root, "kill_switch_count");
		cJSON_AddNumberToObject(root, "kill_switch_count", n + 1);
	}
	bool ok = save_root_atomic(path, root);
	cJSON_Delete(root);
	store_unlock(lk);

	if (ok && was != suspended) {
		struct u_desktop_audit_record r;
		memset(&r, 0, sizeof(r));
		r.action = suspended ? "suspended" : "resumed";
		r.chars = -1;
		r.by = by;
		r.result = "ok";
		u_desktop_audit_append(&r);
	}
	return ok;
}


/*
 *
 * Audit.
 *
 */

static void
utc_now(char *out, size_t cap)
{
	time_t t = time(NULL);
	struct tm tmv;
	memset(&tmv, 0, sizeof(tmv));
#ifdef XRT_OS_WINDOWS
	gmtime_s(&tmv, &t);
#else
	gmtime_r(&t, &tmv);
#endif
	strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

void
u_desktop_audit_utc_now(char *out, size_t cap)
{
	if (out != NULL && cap > 0) {
		out[0] = '\0';
		utc_now(out, cap);
	}
}

bool
u_desktop_audit_append(const struct u_desktop_audit_record *r)
{
	if (r == NULL || r->action == NULL) {
		return false;
	}
	char path[PATH_CAP];
	if (!u_desktop_policy_audit_path(path, sizeof(path))) {
		return false;
	}

	char self_exe[PATH_CAP];
	const char *exe = r->exe;
	if (exe == NULL && u_desktop_self_identity(self_exe, sizeof(self_exe), NULL, 0)) {
		exe = self_exe;
	}

	char t[32];
	utc_now(t, sizeof(t));

	cJSON *o = cJSON_CreateObject();
	if (o == NULL) {
		return false;
	}
	cJSON_AddStringToObject(o, "t", t);
	if (exe != NULL) {
		cJSON_AddStringToObject(o, "exe", exe);
	}
	cJSON_AddNumberToObject(o, "pid", r->pid != 0 ? r->pid : u_desktop_self_pid());
	cJSON_AddStringToObject(o, "action", r->action);
	if (r->subject != NULL) {
		cJSON_AddStringToObject(o, "subject", r->subject);
	}
	if (r->scope != NULL) {
		cJSON_AddStringToObject(o, "scope", r->scope);
	}
	if (r->target_kind != NULL) {
		// A capture: what was captured (a region, the desktop, a window's frame).
		cJSON *tg = cJSON_AddObjectToObject(o, "target");
		if (tg != NULL) {
			cJSON_AddStringToObject(tg, "kind", r->target_kind);
			if (r->has_target) {
				char id[24];
				snprintf(id, sizeof(id), "%llu", (unsigned long long)r->target_id);
				cJSON_AddStringToObject(tg, "id", id);
			}
			if (r->target_app != NULL) {
				cJSON_AddStringToObject(tg, "app", r->target_app);
			}
			if (r->target_title != NULL) {
				cJSON_AddStringToObject(tg, "title", r->target_title);
			}
			if (r->has_rect) {
				int rect[4] = {r->rx, r->ry, r->rw, r->rh};
				cJSON_AddItemToObject(tg, "rect", cJSON_CreateIntArray(rect, 4));
			}
		}
	} else if (r->has_target) {
		cJSON *tg = cJSON_AddObjectToObject(o, "target");
		if (tg != NULL) {
			char id[24];
			snprintf(id, sizeof(id), "%llu", (unsigned long long)r->target_id);
			cJSON_AddStringToObject(tg, "id", id);
			if (r->target_app != NULL) {
				cJSON_AddStringToObject(tg, "app", r->target_app);
			}
			if (r->target_title != NULL) {
				cJSON_AddStringToObject(tg, "title", r->target_title);
			}
		}
	}
	if (r->has_point) {
		cJSON_AddNumberToObject(o, "x", r->x);
		cJSON_AddNumberToObject(o, "y", r->y);
	}
	if (r->chars >= 0) {
		cJSON_AddNumberToObject(o, "chars", (double)r->chars);
	}
	if (r->has_size) {
		cJSON_AddNumberToObject(o, "w", r->w);
		cJSON_AddNumberToObject(o, "h", r->h);
	}
	if (r->count > 0) {
		cJSON_AddNumberToObject(o, "count", r->count);
	}
	if (r->first_t != NULL) {
		cJSON_AddStringToObject(o, "first", r->first_t);
	}
	if (r->last_t != NULL) {
		cJSON_AddStringToObject(o, "last", r->last_t);
	}
	if (r->keys != NULL) {
		cJSON_AddStringToObject(o, "keys", r->keys);
	}
	if (r->reason != NULL) {
		cJSON_AddStringToObject(o, "reason", r->reason);
	}
	if (r->by != NULL) {
		cJSON_AddStringToObject(o, "by", r->by);
	}
	if (r->result != NULL) {
		cJSON_AddStringToObject(o, "result", r->result);
	}
	if (r->message != NULL && r->message[0] != '\0') {
		cJSON_AddStringToObject(o, "message", r->message);
	}
	char *line = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (line == NULL) {
		return false;
	}

	make_parent_dirs(path);
	long long sz = file_size(path);
	if (sz >= (long long)U_DESKTOP_AUDIT_ROTATE_BYTES) {
		char rotated[PATH_CAP + 4];
		snprintf(rotated, sizeof(rotated), "%s.1", path);
		file_rename_replace(path, rotated);
	}

	bool ok = false;
	FILE *f = file_open(path, "ab");
	if (f != NULL) {
		// One write per record so concurrent appenders interleave whole lines.
		size_t len = strlen(line);
		char *buf = (char *)malloc(len + 2);
		if (buf != NULL) {
			memcpy(buf, line, len);
			buf[len] = '\n';
			ok = fwrite(buf, 1, len + 1, f) == len + 1;
			free(buf);
		}
		ok = fclose(f) == 0 && ok;
	}
	cJSON_free(line);
	return ok;
}

struct tail_ring
{
	char **lines;
	uint32_t cap, count, head;
};

static void
ring_push(struct tail_ring *r, const char *s, size_t len)
{
	char *copy = (char *)malloc(len + 1);
	if (copy == NULL) {
		return;
	}
	memcpy(copy, s, len);
	copy[len] = '\0';
	uint32_t idx = (r->head + r->count) % r->cap;
	if (r->count == r->cap) {
		free(r->lines[r->head]);
		r->lines[r->head] = copy;
		r->head = (r->head + 1) % r->cap;
	} else {
		r->lines[idx] = copy;
		r->count++;
	}
}

static bool
ring_feed_file(struct tail_ring *r, const char *path)
{
	char *txt = read_whole(path, NULL);
	if (txt == NULL) {
		return false;
	}
	const char *p = txt;
	while (*p != '\0') {
		const char *nl = strchr(p, '\n');
		size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);
		if (len > 0 && p[len - 1] == '\r') {
			len--;
		}
		if (len > 0) {
			ring_push(r, p, len);
		}
		if (nl == NULL) {
			break;
		}
		p = nl + 1;
	}
	free(txt);
	return true;
}

bool
u_desktop_audit_tail(uint32_t n, u_desktop_audit_line_fn fn, void *ctx)
{
	if (fn == NULL || n == 0) {
		return false;
	}
	char path[PATH_CAP];
	if (!u_desktop_policy_audit_path(path, sizeof(path))) {
		return false;
	}
	char rotated[PATH_CAP + 4];
	snprintf(rotated, sizeof(rotated), "%s.1", path);

	struct tail_ring r;
	memset(&r, 0, sizeof(r));
	r.cap = n;
	r.lines = (char **)calloc(n, sizeof(char *));
	if (r.lines == NULL) {
		return false;
	}
	bool any = ring_feed_file(&r, rotated);
	any = ring_feed_file(&r, path) || any;
	for (uint32_t i = 0; i < r.count; i++) {
		uint32_t idx = (r.head + i) % r.cap;
		fn(ctx, r.lines[idx]);
	}
	for (uint32_t i = 0; i < r.cap; i++) {
		free(r.lines[i]);
	}
	free(r.lines);
	return any;
}


/*
 *
 * Saved OS grants (spec §6.4). See u_desktop_policy.h.
 *
 */

bool
u_desktop_grant_dir(char *out, size_t cap)
{
	char dir[PATH_CAP];
	// Next to the audit log: $XDG_STATE_HOME/displayxr on Linux (state, not
	// configuration), the store directory elsewhere, the override when set.
	return audit_dir(dir, sizeof(dir)) && join(out, cap, dir, U_DESKTOP_GRANT_DIR_NAME);
}

bool
u_desktop_grant_key_valid(const char *key)
{
	if (key == NULL || key[0] == '\0' || key[0] == '.') {
		return false;
	}
	size_t n = 0;
	for (const char *p = key; *p != '\0'; p++, n++) {
		char c = *p;
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
		          c == '_' || c == '-';
		if (!ok || n >= U_DESKTOP_GRANT_KEY_MAX) {
			return false;
		}
	}
	return true;
}

bool
u_desktop_grant_path(const char *key, char *out, size_t cap)
{
	char dir[PATH_CAP];
	if (!u_desktop_grant_key_valid(key) || !u_desktop_grant_dir(dir, sizeof(dir))) {
		return false;
	}
	int n = snprintf(out, cap, "%s%c%s.json", dir, SEP, key);
	return n > 0 && (size_t)n < cap;
}

#ifndef XRT_OS_WINDOWS

/*!
 * The grant directory is usable: a real directory of this user that nobody else
 * can write into. With @p create it is made (0700, parents too) when missing and
 * tightened to 0700 when it has any group / other bits.
 */
static bool
grant_dir_private(const char *dir, bool create)
{
	struct stat st;
	if (lstat(dir, &st) != 0) {
		if (!create || errno != ENOENT) {
			return false;
		}
		char probe[PATH_CAP + 4];
		snprintf(probe, sizeof(probe), "%s%cx", dir, SEP);
		make_parent_dirs(probe); // mkdir -p, 0700 for what it creates
		if (lstat(dir, &st) != 0) {
			return false;
		}
	}
	if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
		return false; // a symlink, a file, or someone else's directory
	}
	if ((st.st_mode & 077) != 0) {
		if (!create) {
			return (st.st_mode & 022) == 0; // readable as long as nobody else can write it
		}
		if (chmod(dir, 0700) != 0) {
			return false;
		}
	}
	return true;
}

enum u_desktop_grant_read_result
u_desktop_grant_read(const char *key, char **out_text)
{
	*out_text = NULL;
	char dir[PATH_CAP], path[PATH_CAP];
	if (!u_desktop_grant_dir(dir, sizeof(dir)) || !u_desktop_grant_path(key, path, sizeof(path))) {
		return U_DESKTOP_GRANT_READ_NONE;
	}
	struct stat dst;
	if (lstat(dir, &dst) != 0) {
		return U_DESKTOP_GRANT_READ_NONE; // nothing was ever saved here
	}
	if (!grant_dir_private(dir, false)) {
		return U_DESKTOP_GRANT_READ_UNSAFE;
	}
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return errno == ENOENT ? U_DESKTOP_GRANT_READ_NONE : U_DESKTOP_GRANT_READ_UNSAFE; // ELOOP: a symlink
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0 ||
	    st.st_size < 0 || (unsigned long long)st.st_size > U_DESKTOP_GRANT_MAX_BYTES) {
		close(fd);
		return U_DESKTOP_GRANT_READ_UNSAFE;
	}
	size_t want = (size_t)st.st_size;
	char *buf = (char *)malloc(want + 1);
	if (buf == NULL) {
		close(fd);
		return U_DESKTOP_GRANT_READ_NONE;
	}
	size_t got = 0;
	while (got < want) {
		ssize_t r = read(fd, buf + got, want - got);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			break;
		}
		got += (size_t)r;
	}
	close(fd);
	buf[got] = '\0';
	*out_text = buf;
	return U_DESKTOP_GRANT_READ_OK;
}

bool
u_desktop_grant_write(const char *key, const char *text)
{
	char dir[PATH_CAP], path[PATH_CAP];
	if (text == NULL || !u_desktop_grant_dir(dir, sizeof(dir)) || !u_desktop_grant_path(key, path, sizeof(path)) ||
	    !grant_dir_private(dir, true)) {
		return false;
	}
	char tmp[PATH_CAP + 32];
	snprintf(tmp, sizeof(tmp), "%s.tmp.%u", path, u_desktop_self_pid());
	unlink(tmp); // a leftover of this pid's crash
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		return false;
	}
	size_t len = strlen(text), done = 0;
	bool ok = true;
	while (ok && done < len) {
		ssize_t w = write(fd, text + done, len - done);
		if (w < 0 && errno == EINTR) {
			continue;
		}
		ok = w > 0;
		done += ok ? (size_t)w : 0;
	}
	ok = ok && fsync(fd) == 0;
	ok = close(fd) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

//! @p dir is this user's real directory (not reached through a symlink of its own).
static bool
grant_dir_ours(const char *dir)
{
	struct stat st;
	return lstat(dir, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid();
}

bool
u_desktop_grant_remove(const char *key)
{
	char dir[PATH_CAP], path[PATH_CAP];
	return u_desktop_grant_dir(dir, sizeof(dir)) && grant_dir_ours(dir) &&
	       u_desktop_grant_path(key, path, sizeof(path)) && unlink(path) == 0;
}

bool
u_desktop_grant_exists(const char *key)
{
	char path[PATH_CAP];
	struct stat st;
	return u_desktop_grant_path(key, path, sizeof(path)) && lstat(path, &st) == 0;
}

//! A saved grant ("<key>.json") or a writer's leftover ("<key>.json.tmp.<pid>").
static bool
grant_file_name(const char *name, bool *is_tmp)
{
	size_t len = strlen(name), klen = 0;
	*is_tmp = false;
	if (len > 5 && strcmp(name + len - 5, ".json") == 0) {
		klen = len - 5;
	} else {
		// The LAST ".json.tmp." (a key may contain ".json"), followed by digits only.
		const char *t = NULL;
		for (const char *s = strstr(name, ".json.tmp."); s != NULL; s = strstr(s + 1, ".json.tmp.")) {
			t = s;
		}
		if (t == NULL || t[10] == '\0') {
			return false;
		}
		for (const char *d = t + 10; *d != '\0'; d++) {
			if (*d < '0' || *d > '9') {
				return false;
			}
		}
		klen = (size_t)(t - name);
		*is_tmp = true;
	}
	char key[U_DESKTOP_GRANT_KEY_MAX + 2];
	if (klen == 0 || klen > U_DESKTOP_GRANT_KEY_MAX) {
		return false;
	}
	memcpy(key, name, klen);
	key[klen] = '\0';
	return u_desktop_grant_key_valid(key);
}

//! Count (and with @p remove, unlink) the grant files: saved grants only are counted.
static uint32_t
grant_sweep(bool remove)
{
	char dir[PATH_CAP];
	// This user's real directory (never through a symlink); removing works even
	// where reading would be refused (a directory others can write into).
	if (!u_desktop_grant_dir(dir, sizeof(dir)) || !grant_dir_ours(dir)) {
		return 0;
	}
	DIR *d = opendir(dir);
	if (d == NULL) {
		return 0;
	}
	uint32_t n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		bool is_tmp = false;
		if (!grant_file_name(e->d_name, &is_tmp)) {
			continue;
		}
		char path[PATH_CAP];
		if (!join(path, sizeof(path), dir, e->d_name)) {
			continue;
		}
		struct stat st;
		if (lstat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
			continue;
		}
		if (!remove) {
			n += is_tmp ? 0 : 1;
		} else if (unlink(path) == 0 && !is_tmp) {
			n++;
		}
	}
	closedir(d);
	return n;
}

uint32_t
u_desktop_grant_remove_all(void)
{
	return grant_sweep(true);
}

static int
cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

uint32_t
u_desktop_grant_for_each_key(u_desktop_grant_key_fn fn, void *ctx)
{
	char dir[PATH_CAP];
	if (!u_desktop_grant_dir(dir, sizeof(dir)) || !grant_dir_ours(dir)) {
		return 0;
	}
	DIR *d = opendir(dir);
	if (d == NULL) {
		return 0;
	}
	char *keys[256];
	uint32_t n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL && n < 256) {
		bool is_tmp = false;
		if (!grant_file_name(e->d_name, &is_tmp) || is_tmp) {
			continue;
		}
		size_t len = strlen(e->d_name) - 5; // ".json"
		keys[n] = (char *)malloc(len + 1);
		if (keys[n] == NULL) {
			break;
		}
		memcpy(keys[n], e->d_name, len);
		keys[n][len] = '\0';
		n++;
	}
	closedir(d);
	qsort(keys, n, sizeof(keys[0]), cmp_str);
	for (uint32_t i = 0; i < n; i++) {
		if (fn != NULL) {
			fn(ctx, keys[i]);
		}
		free(keys[i]);
	}
	return n;
}

uint32_t
u_desktop_grant_count(void)
{
	return grant_sweep(false);
}

#else // XRT_OS_WINDOWS: no backend saves a grant there.

enum u_desktop_grant_read_result
u_desktop_grant_read(const char *key, char **out_text)
{
	(void)key;
	*out_text = NULL;
	return U_DESKTOP_GRANT_READ_NONE;
}

bool
u_desktop_grant_write(const char *key, const char *text)
{
	(void)key;
	(void)text;
	return false;
}

bool
u_desktop_grant_remove(const char *key)
{
	(void)key;
	return false;
}

bool
u_desktop_grant_exists(const char *key)
{
	(void)key;
	return false;
}

uint32_t
u_desktop_grant_remove_all(void)
{
	return 0;
}

uint32_t
u_desktop_grant_count(void)
{
	return 0;
}

uint32_t
u_desktop_grant_for_each_key(u_desktop_grant_key_fn fn, void *ctx)
{
	(void)fn;
	(void)ctx;
	return 0;
}

#endif


/*
 *
 * Identity helpers.
 *
 */

uint32_t
u_desktop_self_pid(void)
{
#ifdef XRT_OS_WINDOWS
	return (uint32_t)GetCurrentProcessId();
#else
	return (uint32_t)getpid();
#endif
}

void
u_desktop_display_name_from_exe(const char *exe, char *name, size_t name_cap)
{
	if (name == NULL || name_cap == 0) {
		return;
	}
	name[0] = '\0';
	if (exe == NULL) {
		return;
	}
	// macOS bundle: ".../<Name>.app/Contents/MacOS/<exe>" -> "<Name>".
	const char *app = strstr(exe, ".app/Contents/MacOS/");
	if (app != NULL) {
		const char *start = app;
		while (start > exe && start[-1] != '/') {
			start--;
		}
		size_t len = (size_t)(app - start);
		if (len > 0) {
			snprintf(name, name_cap, "%.*s", (int)len, start);
			return;
		}
	}
	const char *base = exe;
	for (const char *p = exe; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	size_t len = strlen(base);
	if (len > 4) {
		const char *ext = base + len - 4;
		if ((ext[0] == '.') && (ext[1] == 'e' || ext[1] == 'E') && (ext[2] == 'x' || ext[2] == 'X') &&
		    (ext[3] == 'e' || ext[3] == 'E')) {
			len -= 4;
		}
	}
	snprintf(name, name_cap, "%.*s", (int)len, base);
}

bool
u_desktop_self_identity(char *exe, size_t exe_cap, char *name, size_t name_cap)
{
	char buf[PATH_CAP];
	buf[0] = '\0';
	bool ok = false;
#if defined(XRT_OS_WINDOWS)
	wchar_t w[PATH_CAP];
	DWORD n = GetModuleFileNameW(NULL, w, PATH_CAP);
	if (n > 0 && n < PATH_CAP) {
		ok = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof(buf), NULL, NULL) > 1;
	}
#elif defined(XRT_OS_MACOS)
	char tmp[PATH_CAP];
	uint32_t sz = sizeof(tmp);
	if (_NSGetExecutablePath(tmp, &sz) == 0) {
		char *resolved = realpath(tmp, NULL);
		snprintf(buf, sizeof(buf), "%s", resolved != NULL ? resolved : tmp);
		free(resolved);
		ok = true;
	}
#else
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		ok = true;
	}
#endif
	if (!ok) {
		return false;
	}
	if (exe != NULL && exe_cap > 0) {
		snprintf(exe, exe_cap, "%s", buf);
	}
	if (name != NULL && name_cap > 0) {
		u_desktop_display_name_from_exe(buf, name, name_cap);
	}
	return true;
}
