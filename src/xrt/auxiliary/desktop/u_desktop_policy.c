// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The desktop-control user store and audit log (XR_DXR_desktop_control
 *         spec §6, ADR-056 D3.6). See u_desktop_policy.h.
 */

#include "u_desktop_policy.h"

#include "xrt/xrt_config_os.h"

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
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
	bool ok =
	    wf != NULL && wt != NULL && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
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
	return env_u8(L"" U_DESKTOP_POLICY_DIR_ENV, out, cap);
#else
	return env_u8(U_DESKTOP_POLICY_DIR_ENV, out, cap);
#endif
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
	bool suspended;
	uint32_t idle_ms;
	cJSON *apps; //!< owned object: exe -> "allow" | "deny"
};

static void
policy_defaults(struct u_desktop_policy *p)
{
	p->suspended = false;
	p->idle_ms = U_DESKTOP_POLICY_DEFAULT_IDLE_MS;
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
	const cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
	if (cJSON_IsObject(apps)) {
		p->apps = cJSON_Duplicate(apps, true);
	}
}

static cJSON *
load_root(const char *path)
{
	char *txt = read_whole(path, NULL);
	if (txt == NULL) {
		return NULL;
	}
	cJSON *root = cJSON_Parse(txt);
	free(txt);
	if (root != NULL && !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		root = NULL;
	}
	return root;
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
	cJSON_AddNumberToObject(root, "version", 1);
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
	cJSON *root = now.exists ? load_root(p->path) : NULL;
	policy_from_json(p, root);
	cJSON_Delete(root);
	return true;
}

bool
u_desktop_policy_is_suspended(const struct u_desktop_policy *p)
{
	return p != NULL && p->suspended;
}

uint32_t
u_desktop_policy_idle_ms(const struct u_desktop_policy *p)
{
	return p != NULL ? p->idle_ms : U_DESKTOP_POLICY_DEFAULT_IDLE_MS;
}

enum u_desktop_app_decision
u_desktop_policy_app(const struct u_desktop_policy *p, const char *exe)
{
	if (p == NULL || p->apps == NULL || exe == NULL) {
		return U_DESKTOP_APP_NONE;
	}
	// A deny anywhere wins over an allow written under another spelling.
	enum u_desktop_app_decision best = U_DESKTOP_APP_NONE;
	const cJSON *it = NULL;
	cJSON_ArrayForEach(it, p->apps)
	{
		if (it->string == NULL || !u_desktop_policy_exe_equal(it->string, exe)) {
			continue;
		}
		enum u_desktop_app_decision d = decision_from_json(it);
		if (d == U_DESKTOP_APP_DENY) {
			return d;
		}
		if (d == U_DESKTOP_APP_ALLOW) {
			best = d;
		}
	}
	return best;
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
			fn(ctx, it->string, decision_from_json(it));
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
	if (exe == NULL || exe[0] == '\0') {
		return false;
	}
	char path[PATH_CAP];
	if (!u_desktop_policy_store_path(path, sizeof(path))) {
		return false;
	}
	cJSON *root = normalised_root(load_root(path));
	if (root == NULL) {
		return false;
	}
	cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");

	// Drop every spelling of this executable, then add the new entry.
	bool removed = false;
	for (cJSON *it = apps->child; it != NULL;) {
		cJSON *next = it->next;
		if (it->string != NULL && u_desktop_policy_exe_equal(it->string, exe)) {
			cJSON_Delete(cJSON_DetachItemViaPointer(apps, it));
			removed = true;
		}
		it = next;
	}
	if (d != U_DESKTOP_APP_NONE) {
		cJSON_AddStringToObject(apps, exe, d == U_DESKTOP_APP_ALLOW ? "allow" : "deny");
	}
	bool ok = save_root_atomic(path, root);
	cJSON_Delete(root);

	if (ok && (d != U_DESKTOP_APP_NONE || removed)) {
		struct u_desktop_audit_record r;
		memset(&r, 0, sizeof(r));
		r.action = d == U_DESKTOP_APP_ALLOW ? "allow" : d == U_DESKTOP_APP_DENY ? "deny" : "forget";
		r.chars = -1;
		r.subject = exe;
		r.by = by;
		r.result = "ok";
		u_desktop_audit_append(&r);
	}
	return ok;
}

bool
u_desktop_policy_set_suspended(bool suspended, const char *by)
{
	char path[PATH_CAP];
	if (!u_desktop_policy_store_path(path, sizeof(path))) {
		return false;
	}
	cJSON *root = normalised_root(load_root(path));
	if (root == NULL) {
		return false;
	}
	bool was = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "suspended"));
	cJSON_DeleteItemFromObjectCaseSensitive(root, "suspended");
	cJSON_AddBoolToObject(root, "suspended", suspended);
	bool ok = save_root_atomic(path, root);
	cJSON_Delete(root);

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
	if (r->has_target) {
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
