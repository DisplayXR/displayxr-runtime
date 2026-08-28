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
	unlock();
}
