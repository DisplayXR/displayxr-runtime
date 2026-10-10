// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Persisted runtime settings — implementation.
 * @ingroup aux_util
 *
 * **This file must not log and must not call back into `u_debug`.** It sits
 * underneath `debug_get_*_option()`, which is itself used by the logging setup,
 * so any call in the other direction risks recursing through the one-time init.
 * Plain file I/O, cJSON and the Win32 registry only.
 */

#include "xrt/xrt_config_os.h"

#include "util/u_setting.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#include <shlobj.h> // SHGetFolderPathA
#else
#include <sys/stat.h> // mkdir — needed on Android too, which also takes the
                      // POSIX path for the per-user file.
#ifdef XRT_OS_ANDROID
#include <sys/system_properties.h>
#endif
#endif


/*
 *
 * The allow-list.
 *
 */

/*!
 * Names the persisted stores may set. Everything else is environment-only.
 *
 * This is exactly the set the Control Panel's three user-facing controls drive
 * (#1252 Phase 1). It is deliberately not "every tuning lever": each entry is a
 * name a GUI can now pin for every app on the machine, so adding one is a
 * decision about a support surface, not a convenience. The developer section
 * (Phase 2) extends this list; the Tier-4 names in
 * `docs/roadmap/control-panel-performance-settings.md` must never appear here.
 */
static const char *const managed_names[] = {
    // Target GPU — the documented supported contract (#845).
    "DXR_D3D_FORCE_GPU",
    "DXR_VK_FORCE_GPU",
    // Mode: Balanced / Compatibility. Both change what the DISPLAY PROCESSOR is
    // asked to do, which is where compatibility problems actually live.
    "DXR_WEAVE_ON_SCANOUT",
    "DXR_WEAVE_REPAINT",
    // Diagnostics. Pure observers — they change no behaviour, which is what
    // makes them safe to expose to a user at all.
    "DXR_FRAME_WITNESS",
    "DXR_FRAME_STAGE_TIMING",
};

#define MANAGED_COUNT ((uint32_t)(sizeof(managed_names) / sizeof(managed_names[0])))

//! Longest value we will carry out of a store.
#define SETTING_VALUE_MAX 128

#define USER_FILENAME "settings.json"
#define WRITTEN_KEY "_written"

#ifdef XRT_OS_WINDOWS
#define MACHINE_KEY_PATH L"Software\\DisplayXR\\Settings"
#endif


/*
 *
 * Cache.
 *
 */

struct entry
{
	bool user_set;
	bool machine_set;
	char user[SETTING_VALUE_MAX];
	char machine[SETTING_VALUE_MAX];
};

static struct entry s_entries[MANAGED_COUNT];
static char s_written[32];
static bool s_loaded = false;
//! The per-screen preference cache (bottom of this file) is loaded.
static bool s_screen_loaded;

#ifdef XRT_OS_WINDOWS
static CRITICAL_SECTION s_lock;
static INIT_ONCE s_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK
init_lock(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once;
	(void)param;
	(void)ctx;
	InitializeCriticalSection(&s_lock);
	return TRUE;
}

static void
lock(void)
{
	InitOnceExecuteOnce(&s_lock_once, init_lock, NULL, NULL);
	EnterCriticalSection(&s_lock);
}

static void
unlock(void)
{
	LeaveCriticalSection(&s_lock);
}
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static void
lock(void)
{
	pthread_mutex_lock(&s_lock);
}

static void
unlock(void)
{
	pthread_mutex_unlock(&s_lock);
}
#endif

//! Index of @p name in @ref managed_names, or -1.
static int
managed_index(const char *name)
{
	if (name == NULL) {
		return -1;
	}
	for (uint32_t i = 0; i < MANAGED_COUNT; i++) {
		if (strcmp(managed_names[i], name) == 0) {
			return (int)i;
		}
	}
	return -1;
}


/*
 *
 * Step 1 — the environment. These are the bodies that used to live in
 * u_debug.c's get_option_raw(), moved here so every consumer of the chain gets
 * identical platform behaviour (notably the Android system-property namespace).
 *
 */

#ifdef XRT_OS_ANDROID
struct android_read_arg
{
	char *chars;
	size_t char_count;
};

static void
android_on_property_read(void *cookie, const char *name, const char *value, uint32_t serial)
{
	(void)name;
	(void)serial;
	struct android_read_arg *a = (struct android_read_arg *)cookie;
	snprintf(a->chars, a->char_count, "%s", value);
}
#endif

static const char *
env_get_raw(char *chars, size_t char_count, const char *name)
{
#if defined XRT_OS_WINDOWS
	size_t required_size = 0;
	getenv_s(&required_size, chars, char_count, name);
	if (required_size == 0) {
		return NULL;
	}
	return chars;

#elif defined XRT_OS_ANDROID
	// Android has always had an out-of-process channel for these; it is a
	// system property rather than an environment variable.
	//
	// The CALLBACK form, not __system_property_read: the latter writes up to
	// PROP_VALUE_MAX into the caller's buffer with no way to bound it, and
	// several call sites here pass a 64-byte buffer. This mirrors what
	// u_debug.c did before the read moved into this file.
	char prefixed[1024];
	snprintf(prefixed, sizeof(prefixed), "debug.xrt.%s", name);

	const struct prop_info *pi = __system_property_find(prefixed);
	if (pi == NULL) {
		return NULL;
	}

	struct android_read_arg a = {.chars = chars, .char_count = char_count};
	__system_property_read_callback(pi, &android_on_property_read, &a);

	return chars;

#else
	const char *raw = getenv(name);
	if (raw == NULL) {
		return NULL;
	}
	snprintf(chars, char_count, "%s", raw);
	return chars;
#endif
}


/*
 *
 * Step 2 — the per-user file.
 *
 */

static bool
user_path(char *buf, size_t cap)
{
#ifdef XRT_OS_WINDOWS
	char appdata[MAX_PATH];
	if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata))) {
		return false;
	}
	snprintf(buf, cap, "%s\\DisplayXR\\" USER_FILENAME, appdata);
#else
	const char *config_home = getenv("XDG_CONFIG_HOME");
	if (config_home != NULL && config_home[0] != '\0') {
		snprintf(buf, cap, "%s/displayxr/" USER_FILENAME, config_home);
	} else {
		const char *home = getenv("HOME");
		if (home == NULL || home[0] == '\0') {
			return false;
		}
		snprintf(buf, cap, "%s/.config/displayxr/" USER_FILENAME, home);
	}
#endif
	return true;
}

static void
ensure_parent_dir(const char *filepath)
{
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", filepath);

	char *last = strrchr(dir, '/');
#ifdef XRT_OS_WINDOWS
	char *back = strrchr(dir, '\\');
	if (back != NULL && (last == NULL || back > last)) {
		last = back;
	}
#endif
	if (last == NULL) {
		return;
	}
	*last = '\0';

#ifdef XRT_OS_WINDOWS
	CreateDirectoryA(dir, NULL); // already-exists is fine
#else
	mkdir(dir, 0755);
#endif
}

//! Read the whole file. Caller frees. NULL on any failure — never fails loudly:
//! a low-integrity or AppContainer process may simply not be allowed to look.
static char *
read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 64 * 1024) {
		fclose(f);
		return NULL;
	}

	char *buf = (char *)malloc((size_t)len + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

//! Parse the user file into a fresh cJSON object, or an empty one.
static cJSON *
user_load_json(void)
{
	char path[512];
	if (!user_path(path, sizeof(path))) {
		return cJSON_CreateObject();
	}
	char *text = read_file(path);
	if (text == NULL) {
		return cJSON_CreateObject();
	}
	cJSON *root = cJSON_Parse(text);
	free(text);
	if (root == NULL || !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return cJSON_CreateObject();
	}
	return root;
}

static bool
user_save_json(cJSON *root)
{
	char path[512];
	if (!user_path(path, sizeof(path))) {
		return false;
	}
	ensure_parent_dir(path);

	// Stamp the write date so a UI can say how long a setting has been in
	// force. A setting nobody remembers making is the failure mode this whole
	// surface has to defend against.
	char stamp[32] = {0};
	time_t now = time(NULL);
	struct tm tm_buf;
#ifdef XRT_OS_WINDOWS
	if (gmtime_s(&tm_buf, &now) == 0) {
#else
	if (gmtime_r(&now, &tm_buf) != NULL) {
#endif
		strftime(stamp, sizeof(stamp), "%Y-%m-%d", &tm_buf);
	}
	cJSON_DeleteItemFromObjectCaseSensitive(root, WRITTEN_KEY);
	if (stamp[0] != '\0') {
		cJSON_AddStringToObject(root, WRITTEN_KEY, stamp);
	}

	char *text = cJSON_Print(root);
	if (text == NULL) {
		return false;
	}
	FILE *f = fopen(path, "w");
	if (f == NULL) {
		free(text);
		return false;
	}
	fputs(text, f);
	fclose(f);
	free(text);
	return true;
}


/*
 *
 * Step 3 — the machine store (Windows only; HKLM needs admin to write, which is
 * why the panel writes the per-user file instead).
 *
 */

#ifdef XRT_OS_WINDOWS
static bool
machine_get(const char *name, char *buf, size_t cap)
{
	wchar_t wname[128];
	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, (int)(sizeof(wname) / sizeof(wname[0]))) <= 0) {
		return false;
	}

	wchar_t wval[SETTING_VALUE_MAX];
	DWORD bytes = sizeof(wval);
	// 64-bit view, matching every other HKLM\Software\DisplayXR reader.
	LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, MACHINE_KEY_PATH, wname, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
	                          NULL, wval, &bytes);
	if (rc != ERROR_SUCCESS) {
		return false;
	}
	if (WideCharToMultiByte(CP_UTF8, 0, wval, -1, buf, (int)cap, NULL, NULL) <= 0) {
		return false;
	}
	return buf[0] != '\0';
}
#endif


/*
 *
 * Load.
 *
 */

//! Caller holds the lock.
static void
load_locked(void)
{
	if (s_loaded) {
		return;
	}
	s_loaded = true;
	memset(s_entries, 0, sizeof(s_entries));
	s_written[0] = '\0';

	cJSON *root = user_load_json();
	if (root != NULL) {
		const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, WRITTEN_KEY);
		if (cJSON_IsString(w) && w->valuestring != NULL) {
			snprintf(s_written, sizeof(s_written), "%s", w->valuestring);
		}
		for (uint32_t i = 0; i < MANAGED_COUNT; i++) {
			const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, managed_names[i]);
			if (cJSON_IsString(n) && n->valuestring != NULL && n->valuestring[0] != '\0') {
				snprintf(s_entries[i].user, sizeof(s_entries[i].user), "%s", n->valuestring);
				s_entries[i].user_set = true;
			}
		}
		cJSON_Delete(root);
	}

#ifdef XRT_OS_WINDOWS
	for (uint32_t i = 0; i < MANAGED_COUNT; i++) {
		if (machine_get(managed_names[i], s_entries[i].machine, sizeof(s_entries[i].machine))) {
			s_entries[i].machine_set = true;
		}
	}
#endif
}


/*
 *
 * Public API.
 *
 */

const char *
u_setting_source_str(enum u_setting_source source)
{
	switch (source) {
	case U_SETTING_SOURCE_ENV: return "env";
	case U_SETTING_SOURCE_USER: return "user";
	case U_SETTING_SOURCE_MACHINE: return "machine";
	case U_SETTING_SOURCE_DEFAULT:
	default: return "default";
	}
}

const char *
u_setting_get_raw(const char *name, char *buf, size_t cap, enum u_setting_source *out_source)
{
	enum u_setting_source src = U_SETTING_SOURCE_DEFAULT;
	const char *ret = NULL;

	if (name == NULL || buf == NULL || cap == 0) {
		goto out;
	}

	// 1. The environment always wins — see the header for why that is not
	//    negotiable.
	ret = env_get_raw(buf, cap, name);
	if (ret != NULL) {
		src = U_SETTING_SOURCE_ENV;
		goto out;
	}

	// 2/3. Persisted stores, allow-listed names only.
	{
		int idx = managed_index(name);
		if (idx < 0) {
			goto out;
		}

		lock();
		load_locked();
		const struct entry *e = &s_entries[idx];
		if (e->user_set) {
			snprintf(buf, cap, "%s", e->user);
			src = U_SETTING_SOURCE_USER;
			ret = buf;
		} else if (e->machine_set) {
			snprintf(buf, cap, "%s", e->machine);
			src = U_SETTING_SOURCE_MACHINE;
			ret = buf;
		}
		unlock();
	}

out:
	if (out_source != NULL) {
		*out_source = src;
	}
	return ret;
}

bool
u_setting_is_managed(const char *name)
{
	return managed_index(name) >= 0;
}

uint32_t
u_setting_managed_count(void)
{
	return MANAGED_COUNT;
}

const char *
u_setting_managed_name(uint32_t index)
{
	return index < MANAGED_COUNT ? managed_names[index] : NULL;
}

bool
u_setting_user_set(const char *name, const char *value)
{
	if (!u_setting_is_managed(name) || value == NULL) {
		return false;
	}

	cJSON *root = user_load_json();
	if (root == NULL) {
		return false;
	}
	cJSON_DeleteItemFromObjectCaseSensitive(root, name);
	cJSON_AddStringToObject(root, name, value);

	bool ok = user_save_json(root);
	cJSON_Delete(root);
	if (ok) {
		u_setting_reload();
	}
	return ok;
}

bool
u_setting_user_clear(const char *name)
{
	if (name == NULL) {
		return false;
	}
	cJSON *root = user_load_json();
	if (root == NULL) {
		return false;
	}
	cJSON_DeleteItemFromObjectCaseSensitive(root, name);

	bool ok = user_save_json(root);
	cJSON_Delete(root);
	if (ok) {
		u_setting_reload();
	}
	return ok;
}

bool
u_setting_user_clear_all(void)
{
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return false;
	}
	// The per-screen display-processor preferences are a display assignment,
	// not a tuning option: `perf reset` keeps them (`dp reset --screen all`
	// clears them).
	cJSON *old = user_load_json();
	cJSON *screens =
	    old != NULL ? cJSON_DetachItemFromObjectCaseSensitive(old, U_SETTING_PER_SCREEN_JSON_KEY) : NULL;
	cJSON_Delete(old);
	if (screens != NULL) {
		cJSON_AddItemToObject(root, U_SETTING_PER_SCREEN_JSON_KEY, screens);
	}
	bool ok = user_save_json(root);
	cJSON_Delete(root);
	if (ok) {
		u_setting_reload();
	}
	return ok;
}

const char *
u_setting_user_get(const char *name, char *buf, size_t cap)
{
	int idx = managed_index(name);
	if (idx < 0 || buf == NULL || cap == 0) {
		return NULL;
	}
	const char *ret = NULL;
	lock();
	load_locked();
	if (s_entries[idx].user_set) {
		snprintf(buf, cap, "%s", s_entries[idx].user);
		ret = buf;
	}
	unlock();
	return ret;
}

bool
u_setting_user_path(char *buf, size_t cap)
{
	return user_path(buf, cap);
}

const char *
u_setting_user_written(char *buf, size_t cap)
{
	if (buf == NULL || cap == 0) {
		return NULL;
	}
	const char *ret = NULL;
	lock();
	load_locked();
	if (s_written[0] != '\0') {
		snprintf(buf, cap, "%s", s_written);
		ret = buf;
	}
	unlock();
	return ret;
}

void
u_setting_reload(void)
{
	lock();
	s_loaded = false;
	s_screen_loaded = false;
	unlock();
}


/*
 *
 * Per-screen display-processor preference (display dashboard phase 7).
 *
 */

#define SCREEN_PREF_MAX 16
#define SCREEN_ENV_MAX 1024

#ifdef XRT_OS_WINDOWS
#define SCREEN_MACHINE_KEY_PATH L"Software\\DisplayXR\\DisplayProcessors\\PreferredPlugin"
#endif

struct screen_pref
{
	char key[U_SETTING_SCREEN_KEY_MAX];
	char id[SETTING_VALUE_MAX];
};

static struct screen_pref s_screen_user[SCREEN_PREF_MAX];
static uint32_t s_screen_user_count = 0;
static struct screen_pref s_screen_machine[SCREEN_PREF_MAX];
static uint32_t s_screen_machine_count = 0;

static bool
str_ieq(const char *a, const char *b)
{
	if (a == NULL || b == NULL) {
		return false;
	}
	for (; *a != '\0' && *b != '\0'; a++, b++) {
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') {
			ca = (char)(ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb = (char)(cb - 'A' + 'a');
		}
		if (ca != cb) {
			return false;
		}
	}
	return *a == '\0' && *b == '\0';
}

static bool
is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void
copy_trim(char *dst, size_t cap, const char *b, const char *e)
{
	while (b < e && is_space(*b)) {
		b++;
	}
	while (e > b && is_space(e[-1])) {
		e--;
	}
	size_t n = (size_t)(e - b);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, b, n);
	dst[n] = '\0';
}

static bool
screen_key_valid(const char *key)
{
	if (key == NULL || key[0] == '\0' || strlen(key) >= U_SETTING_SCREEN_KEY_MAX) {
		return false;
	}
	return strchr(key, '=') == NULL && strchr(key, ';') == NULL;
}

bool
u_setting_per_screen_env_lookup(const char *spec, const char *key, char *buf, size_t cap)
{
	if (spec == NULL || key == NULL || key[0] == '\0' || buf == NULL || cap == 0) {
		return false;
	}
	const char *p = spec;
	while (*p != '\0') {
		const char *end = strchr(p, ';');
		if (end == NULL) {
			end = p + strlen(p);
		}
		// The first '=' splits: a key never contains one (refused on write).
		const char *eq = NULL;
		for (const char *q = p; q < end; q++) {
			if (*q == '=') {
				eq = q;
				break;
			}
		}
		if (eq != NULL) {
			char k[U_SETTING_SCREEN_KEY_MAX];
			char v[SETTING_VALUE_MAX];
			copy_trim(k, sizeof(k), p, eq);
			copy_trim(v, sizeof(v), eq + 1, end);
			if (k[0] != '\0' && v[0] != '\0' && str_ieq(k, key)) {
				snprintf(buf, cap, "%s", v);
				return true;
			}
		}
		p = (*end == ';') ? end + 1 : end;
	}
	return false;
}

//! The `preferred_plugin_per_screen` object of a parsed settings root, or NULL.
static const cJSON *
screen_object(const cJSON *root)
{
	if (root == NULL || !cJSON_IsObject(root)) {
		return NULL;
	}
	const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, U_SETTING_PER_SCREEN_JSON_KEY);
	return cJSON_IsObject(o) ? o : NULL;
}

static bool
screen_object_lookup(const cJSON *obj, const char *key, char *buf, size_t cap)
{
	if (obj == NULL || key == NULL) {
		return false;
	}
	const cJSON *it = NULL;
	cJSON_ArrayForEach(it, obj)
	{
		if (it->string != NULL && str_ieq(it->string, key) && cJSON_IsString(it) && it->valuestring != NULL &&
		    it->valuestring[0] != '\0') {
			snprintf(buf, cap, "%s", it->valuestring);
			return true;
		}
	}
	return false;
}

bool
u_setting_per_screen_json_lookup(const char *json_text, const char *key, char *buf, size_t cap)
{
	if (json_text == NULL || key == NULL || key[0] == '\0' || buf == NULL || cap == 0) {
		return false;
	}
	cJSON *root = cJSON_Parse(json_text);
	const bool ok = screen_object_lookup(screen_object(root), key, buf, cap);
	cJSON_Delete(root);
	return ok;
}

const char *
u_setting_per_screen_resolve(const char *key,
                             const char *env_spec,
                             const char *user_json,
                             const char *machine_value,
                             char *buf,
                             size_t cap,
                             enum u_setting_source *out_source)
{
	enum u_setting_source src = U_SETTING_SOURCE_DEFAULT;
	const char *ret = NULL;
	if (key != NULL && key[0] != '\0' && buf != NULL && cap > 0) {
		if (u_setting_per_screen_env_lookup(env_spec, key, buf, cap)) {
			src = U_SETTING_SOURCE_ENV;
			ret = buf;
		} else if (u_setting_per_screen_json_lookup(user_json, key, buf, cap)) {
			src = U_SETTING_SOURCE_USER;
			ret = buf;
		} else if (machine_value != NULL && machine_value[0] != '\0') {
			snprintf(buf, cap, "%s", machine_value);
			src = U_SETTING_SOURCE_MACHINE;
			ret = buf;
		}
	}
	if (out_source != NULL) {
		*out_source = src;
	}
	return ret;
}

#ifdef XRT_OS_WINDOWS
//! Every value under the machine key, into @ref s_screen_machine. Caller holds the lock.
static void
screen_machine_load_locked(void)
{
	HKEY hk = NULL;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SCREEN_MACHINE_KEY_PATH, 0, KEY_READ | KEY_WOW64_64KEY, &hk) !=
	    ERROR_SUCCESS) {
		return;
	}
	for (DWORD i = 0; s_screen_machine_count < SCREEN_PREF_MAX; i++) {
		wchar_t wname[U_SETTING_SCREEN_KEY_MAX];
		wchar_t wval[SETTING_VALUE_MAX];
		DWORD name_len = (DWORD)(sizeof(wname) / sizeof(wname[0]));
		DWORD val_bytes = sizeof(wval) - sizeof(wchar_t);
		DWORD type = 0;
		const LSTATUS rc = RegEnumValueW(hk, i, wname, &name_len, NULL, &type, (BYTE *)wval, &val_bytes);
		if (rc == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (rc != ERROR_SUCCESS || type != REG_SZ) {
			continue; // too long, or not a string: not set
		}
		wval[val_bytes / sizeof(wchar_t)] = L'\0';
		struct screen_pref *e = &s_screen_machine[s_screen_machine_count];
		if (WideCharToMultiByte(CP_UTF8, 0, wname, -1, e->key, (int)sizeof(e->key), NULL, NULL) <= 0 ||
		    WideCharToMultiByte(CP_UTF8, 0, wval, -1, e->id, (int)sizeof(e->id), NULL, NULL) <= 0 ||
		    e->key[0] == '\0' || e->id[0] == '\0') {
			continue;
		}
		s_screen_machine_count++;
	}
	RegCloseKey(hk);
}
#endif

//! Caller holds the lock.
static void
screen_load_locked(void)
{
	if (s_screen_loaded) {
		return;
	}
	s_screen_loaded = true;
	memset(s_screen_user, 0, sizeof(s_screen_user));
	memset(s_screen_machine, 0, sizeof(s_screen_machine));
	s_screen_user_count = 0;
	s_screen_machine_count = 0;

	cJSON *root = user_load_json();
	const cJSON *obj = screen_object(root);
	const cJSON *it = NULL;
	if (obj != NULL) {
		cJSON_ArrayForEach(it, obj)
		{
			if (s_screen_user_count >= SCREEN_PREF_MAX) {
				break;
			}
			if (it->string == NULL || !screen_key_valid(it->string) || !cJSON_IsString(it) ||
			    it->valuestring == NULL || it->valuestring[0] == '\0') {
				continue;
			}
			struct screen_pref *e = &s_screen_user[s_screen_user_count++];
			snprintf(e->key, sizeof(e->key), "%s", it->string);
			snprintf(e->id, sizeof(e->id), "%s", it->valuestring);
		}
	}
	cJSON_Delete(root);

#ifdef XRT_OS_WINDOWS
	screen_machine_load_locked();
#endif
}

static const char *
screen_find(const struct screen_pref *arr, uint32_t n, const char *key)
{
	for (uint32_t i = 0; i < n; i++) {
		if (str_ieq(arr[i].key, key)) {
			return arr[i].id;
		}
	}
	return NULL;
}

const char *
u_setting_get_preferred_plugin_for_screen(const char *key, char *buf, size_t cap, enum u_setting_source *out_source)
{
	if (out_source != NULL) {
		*out_source = U_SETTING_SOURCE_DEFAULT;
	}
	if (key == NULL || key[0] == '\0' || buf == NULL || cap == 0) {
		return NULL;
	}

	// 1. The environment always wins, as for every other option.
	char env[SCREEN_ENV_MAX];
	if (env_get_raw(env, sizeof(env), U_SETTING_PER_SCREEN_ENV) != NULL &&
	    u_setting_per_screen_env_lookup(env, key, buf, cap)) {
		if (out_source != NULL) {
			*out_source = U_SETTING_SOURCE_ENV;
		}
		return buf;
	}

	// 2/3. The cached stores.
	const char *ret = NULL;
	enum u_setting_source src = U_SETTING_SOURCE_DEFAULT;
	lock();
	screen_load_locked();
	const char *user = screen_find(s_screen_user, s_screen_user_count, key);
	const char *machine = screen_find(s_screen_machine, s_screen_machine_count, key);
	if (user != NULL) {
		snprintf(buf, cap, "%s", user);
		src = U_SETTING_SOURCE_USER;
		ret = buf;
	} else if (machine != NULL) {
		snprintf(buf, cap, "%s", machine);
		src = U_SETTING_SOURCE_MACHINE;
		ret = buf;
	}
	unlock();
	if (out_source != NULL) {
		*out_source = src;
	}
	return ret;
}

void
u_setting_per_screen_reload(void)
{
	lock();
	s_screen_loaded = false;
	unlock();
}

bool
u_setting_user_set_preferred_plugin_for_screen(const char *key, const char *plugin_id)
{
	if (!screen_key_valid(key) || (plugin_id != NULL && strlen(plugin_id) >= SETTING_VALUE_MAX)) {
		return false;
	}
	cJSON *root = user_load_json();
	if (root == NULL) {
		return false;
	}
	cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, U_SETTING_PER_SCREEN_JSON_KEY);
	if (obj != NULL && !cJSON_IsObject(obj)) {
		cJSON_DeleteItemFromObjectCaseSensitive(root, U_SETTING_PER_SCREEN_JSON_KEY);
		obj = NULL;
	}
	const bool set = plugin_id != NULL && plugin_id[0] != '\0';
	if (obj == NULL && set) {
		obj = cJSON_AddObjectToObject(root, U_SETTING_PER_SCREEN_JSON_KEY);
	}
	if (obj != NULL) {
		// Drop every spelling of the key, then add the new value.
		cJSON *it = obj->child;
		while (it != NULL) {
			cJSON *next = it->next;
			if (it->string != NULL && str_ieq(it->string, key)) {
				cJSON_Delete(cJSON_DetachItemViaPointer(obj, it));
			}
			it = next;
		}
		if (set) {
			cJSON_AddStringToObject(obj, key, plugin_id);
		} else if (obj->child == NULL) {
			cJSON_DeleteItemFromObjectCaseSensitive(root, U_SETTING_PER_SCREEN_JSON_KEY);
		}
	}
	const bool ok = user_save_json(root);
	cJSON_Delete(root);
	if (ok) {
		u_setting_per_screen_reload();
	}
	return ok;
}

bool
u_setting_user_clear_preferred_plugin_per_screen(void)
{
	cJSON *root = user_load_json();
	if (root == NULL) {
		return false;
	}
	if (cJSON_GetObjectItemCaseSensitive(root, U_SETTING_PER_SCREEN_JSON_KEY) == NULL) {
		cJSON_Delete(root);
		return true; // nothing to do: do not touch the file
	}
	cJSON_DeleteItemFromObjectCaseSensitive(root, U_SETTING_PER_SCREEN_JSON_KEY);
	const bool ok = user_save_json(root);
	cJSON_Delete(root);
	if (ok) {
		u_setting_per_screen_reload();
	}
	return ok;
}

enum u_setting_write_result
u_setting_machine_set_preferred_plugin_for_screen(const char *key, const char *plugin_id)
{
#ifdef XRT_OS_WINDOWS
	if (key == NULL) {
		// Every machine-tier preference: the whole subkey.
		const LSTATUS rc = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, SCREEN_MACHINE_KEY_PATH, KEY_WOW64_64KEY, 0);
		if (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) {
			u_setting_per_screen_reload();
			return U_SETTING_WRITE_OK;
		}
		return rc == ERROR_ACCESS_DENIED ? U_SETTING_WRITE_DENIED : U_SETTING_WRITE_FAILED;
	}
	if (!screen_key_valid(key) || (plugin_id != NULL && strlen(plugin_id) >= SETTING_VALUE_MAX)) {
		return U_SETTING_WRITE_FAILED;
	}
	wchar_t wkey[U_SETTING_SCREEN_KEY_MAX];
	if (MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, (int)(sizeof(wkey) / sizeof(wkey[0]))) <= 0) {
		return U_SETTING_WRITE_FAILED;
	}
	const bool set = plugin_id != NULL && plugin_id[0] != '\0';
	HKEY hk = NULL;
	LSTATUS rc;
	if (set) {
		// 64-bit view, matching every HKLM\Software\DisplayXR reader.
		rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, SCREEN_MACHINE_KEY_PATH, 0, NULL, 0,
		                     KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hk, NULL);
	} else {
		rc =
		    RegOpenKeyExW(HKEY_LOCAL_MACHINE, SCREEN_MACHINE_KEY_PATH, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &hk);
		if (rc == ERROR_FILE_NOT_FOUND) {
			return U_SETTING_WRITE_OK; // nothing to clear
		}
	}
	if (rc != ERROR_SUCCESS) {
		return rc == ERROR_ACCESS_DENIED ? U_SETTING_WRITE_DENIED : U_SETTING_WRITE_FAILED;
	}
	if (set) {
		wchar_t wid[SETTING_VALUE_MAX];
		if (MultiByteToWideChar(CP_UTF8, 0, plugin_id, -1, wid, (int)(sizeof(wid) / sizeof(wid[0]))) <= 0) {
			RegCloseKey(hk);
			return U_SETTING_WRITE_FAILED;
		}
		rc = RegSetValueExW(hk, wkey, 0, REG_SZ, (const BYTE *)wid,
		                    (DWORD)((wcslen(wid) + 1) * sizeof(wchar_t)));
	} else {
		rc = RegDeleteValueW(hk, wkey);
		if (rc == ERROR_FILE_NOT_FOUND) {
			rc = ERROR_SUCCESS;
		}
	}
	RegCloseKey(hk);
	if (rc != ERROR_SUCCESS) {
		return rc == ERROR_ACCESS_DENIED ? U_SETTING_WRITE_DENIED : U_SETTING_WRITE_FAILED;
	}
	u_setting_per_screen_reload();
	return U_SETTING_WRITE_OK;
#else
	(void)key;
	(void)plugin_id;
	return U_SETTING_WRITE_UNSUPPORTED;
#endif
}
