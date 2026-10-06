// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The real stereo camera consent store (ADR-043 R3, spec §7.1).
 *
 * Windows — registry, per user:
 *   HKCU\Software\DisplayXR\CameraConsent
 *     Sharing      REG_DWORD   1 (default) / 0 — the user's sharing toggle
 *     Secret       REG_BINARY  32 bytes, generated once — keys persistentId
 *     Apps\        REG_DWORD   value NAME = full executable path, data 1 = Allow, 2 = Deny
 *     Delegating\  REG_SZ      value name = any id, data = executable path (user-level list,
 *                              `displayxr-cli camera trust [--signer CN]`)
 *                  REG_SZ      value name = "<id>.signer", data = the signer CN the entry
 *                              requires (optional; spec §7.1.1 — only consulted when the
 *                              executable's path is user-writable, i.e. not under
 *                              %ProgramFiles%, %ProgramFiles(x86)% or %SystemRoot%).
 *                              Never itself an entry, whatever its data.
 *   HKLM\Software\DisplayXR\CameraConsent\Delegating   same shape, written by installers
 *   The OS camera privacy switch is read from CapabilityAccessManager\ConsentStore\webcam
 *   (HKLM global policy, HKCU global, NonPackaged = desktop apps, and the per-app entry
 *   whose key name is the path with '\' replaced by '#').
 *
 * POSIX — JSON, mode 0600, in the user config dir ($XDG_CONFIG_HOME/displayxr or
 * ~/.config/monado, like every other runtime file): camera_consent.json
 *   {"sharing": true, "secret": "<64 hex>", "apps": {"<exe>": "allow"|"deny"},
 *    "delegating": ["<exe>", ...]}
 * plus the system delegating list an installer may write:
 *   Linux  /etc/displayxr/camera-delegating.json
 *   macOS  /Library/Application Support/DisplayXR/camera-delegating.json
 *   {"delegating": ["<exe>", ...]}
 * POSIX delegation is PATH-ONLY for now: no signer is recorded and
 * u_camera_consent_path_user_writable() answers false (no code-signature
 * check exists here yet — a follow-up for macOS codesign / Linux).
 *
 * Paths compare with u_camera_consent_path_equal(): Windows ASCII-case-
 * insensitively with either separator, POSIX byte-exact. On Windows every
 * registry access uses the wide API (paths are UTF-8 in the runtime, UTF-16 in
 * the registry), and an oversized or malformed value is skipped, never ends a
 * list scan (u_camera_consent_list_find()).
 *
 * @ingroup aux_util
 */

#include "util/u_camera_consent.h"
#include "util/u_logging.h"

#include "xrt/xrt_config_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#else
#include "util/u_file.h"
#include "util/u_json.h"
#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


/*
 *
 * Shared helpers.
 *
 */

static void
fill_random(uint8_t *out, size_t n)
{
#ifdef XRT_OS_WINDOWS
	if (BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
		return;
	}
#else
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		size_t got = 0;
		while (got < n) {
			ssize_t r = read(fd, out + got, n - got);
			if (r <= 0) {
				break;
			}
			got += (size_t)r;
		}
		close(fd);
		if (got == n) {
			return;
		}
	}
#endif
	// Last resort: still unpredictable enough to key an id for one install.
	uint64_t x = (uint64_t)(uintptr_t)out ^ 0x9e3779b97f4a7c15ull;
	for (size_t i = 0; i < n; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		out[i] = (uint8_t)(x ^ (uint64_t)rand());
	}
}


#ifdef XRT_OS_WINDOWS

/*
 *
 * Windows: registry. Every access goes through the WIDE API: the executable
 * paths the service compares are UTF-8 (QueryFullProcessImageNameW ->
 * CP_UTF8), and an installer writes UTF-16. The ANSI API would convert through
 * the process code page and a non-ASCII path could never match.
 *
 */

#define CONSENT_KEY L"Software\\DisplayXR\\CameraConsent"
#define CAM_CONSENT_STORE_WEBCAM                                                                                       \
	L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam"

//! UTF-8 -> freshly allocated UTF-16 (free()); NULL on failure.
static wchar_t *
u8_to_w(const char *s)
{
	if (s == NULL) {
		return NULL;
	}
	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
	if (n <= 0) {
		return NULL;
	}
	wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
	if (w != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
		free(w);
		w = NULL;
	}
	return w;
}

//! UTF-16 (@p wlen chars, need not be terminated) -> UTF-8 in @p out; false if it does not fit.
static bool
w_to_u8(const wchar_t *w, int wlen, char *out, size_t cap)
{
	if (cap == 0) {
		return false;
	}
	out[0] = '\0';
	if (wlen == 0) {
		return true;
	}
	int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, wlen, out, (int)cap - 1, NULL, NULL);
	if (n <= 0) {
		out[0] = '\0';
		return false;
	}
	out[n] = '\0';
	return true;
}

static bool
reg_read_dword(HKEY root, const wchar_t *sub, const wchar_t *name, DWORD *out)
{
	HKEY k;
	if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) {
		return false;
	}
	DWORD type = 0, data = 0, size = sizeof(data);
	LSTATUS rc = RegQueryValueExW(k, name, NULL, &type, (LPBYTE)&data, &size);
	RegCloseKey(k);
	if (rc != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(data)) {
		return false;
	}
	*out = data;
	return true;
}

//! A short REG_SZ value (ConsentStore "Value"); false if absent, not a string, or too long.
static bool
reg_read_string_w(HKEY root, const wchar_t *sub, const wchar_t *name, wchar_t *out, DWORD cap_chars)
{
	HKEY k;
	if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) {
		return false;
	}
	DWORD type = 0, size = (cap_chars - 1) * (DWORD)sizeof(wchar_t);
	LSTATUS rc = RegQueryValueExW(k, name, NULL, &type, (LPBYTE)out, &size);
	RegCloseKey(k);
	if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
		return false;
	}
	out[size / sizeof(wchar_t)] = L'\0';
	return true;
}

static bool
reg_write_dword(HKEY root, const wchar_t *sub, const wchar_t *name, DWORD v)
{
	HKEY k;
	if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) {
		return false;
	}
	LSTATUS rc = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
	RegCloseKey(k);
	return rc == ERROR_SUCCESS;
}

static bool
reg_delete_value(HKEY root, const wchar_t *sub, const wchar_t *name)
{
	HKEY k;
	if (RegOpenKeyExW(root, sub, 0, KEY_WRITE, &k) != ERROR_SUCCESS) {
		return true; // nothing to delete
	}
	LSTATUS rc = RegDeleteValueW(k, name);
	RegCloseKey(k);
	return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

/*!
 * An open key's values, enumerated with buffers sized from RegQueryInfoKeyW so
 * a legitimately long value fits; whatever still does not (the key grew under
 * us) is reported as SKIP for that index, never as the end of the list.
 */
struct reg_enum
{
	HKEY key;
	wchar_t *name; //!< name_cap chars
	DWORD name_cap;
	BYTE *data; //!< data_cap bytes (even), +4 so any REG_SZ can be terminated
	DWORD data_cap;
};

static bool
reg_enum_open(struct reg_enum *e, HKEY root, const wchar_t *sub)
{
	memset(e, 0, sizeof(*e));
	if (RegOpenKeyExW(root, sub, 0, KEY_READ, &e->key) != ERROR_SUCCESS) {
		return false;
	}
	DWORD max_name = 0, max_data = 0;
	if (RegQueryInfoKeyW(e->key, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &max_name, &max_data, NULL, NULL) !=
	    ERROR_SUCCESS) {
		max_name = 1024;
		max_data = 4096;
	}
	e->name_cap = max_name + 1;
	e->data_cap = (max_data + 1u) & ~1u;
	e->name = (wchar_t *)malloc((size_t)e->name_cap * sizeof(wchar_t));
	e->data = (BYTE *)malloc((size_t)e->data_cap + 4);
	if (e->name == NULL || e->data == NULL) {
		free(e->name);
		free(e->data);
		RegCloseKey(e->key);
		memset(e, 0, sizeof(*e));
		return false;
	}
	return true;
}

static void
reg_enum_close(struct reg_enum *e)
{
	if (e->key != NULL) {
		RegCloseKey(e->key);
	}
	free(e->name);
	free(e->data);
	memset(e, 0, sizeof(*e));
}

/*!
 * Value @p i: on ENTRY, e->name is terminated, *type and *data_len are set and
 * e->data has zero bytes after data_len (a REG_SZ is always terminated, even an
 * odd-length or unterminated one an installer wrote).
 */
static enum u_camera_consent_enum_result
reg_enum_at(struct reg_enum *e, DWORD i, DWORD *type, DWORD *data_len)
{
	DWORD name_len = e->name_cap, dlen = e->data_cap;
	LSTATUS rc = RegEnumValueW(e->key, i, e->name, &name_len, NULL, type, e->data, &dlen);
	if (rc == ERROR_MORE_DATA) {
		return U_CAMERA_CONSENT_ENUM_SKIP; // oversized: ignore this one, not the rest
	}
	if (rc != ERROR_SUCCESS) {
		return U_CAMERA_CONSENT_ENUM_END; // ERROR_NO_MORE_ITEMS, or the key is gone
	}
	e->name[name_len < e->name_cap ? name_len : e->name_cap - 1] = L'\0';
	memset(e->data + dlen, 0, 4);
	*data_len = dlen;
	return U_CAMERA_CONSENT_ENUM_ENTRY;
}

#define SIGNER_SUFFIX L".signer"

//! Is @p name an entry's "<id>.signer" companion value (case-insensitive suffix)?
static bool
is_signer_value_name(const wchar_t *name)
{
	size_t n = wcslen(name), s = wcslen(SIGNER_SUFFIX);
	return n > s && _wcsicmp(name + (n - s), SIGNER_SUFFIX) == 0;
}

/*!
 * Read the "<id>.signer" REG_SZ next to delegating entry @p id_u8 under
 * @p root's Delegating key into @p out (UTF-8); "" when absent / unusable.
 */
static void
read_entry_signer(HKEY root, const char *id_u8, char *out, size_t cap)
{
	out[0] = '\0';
	wchar_t *wid = u8_to_w(id_u8);
	if (wid == NULL) {
		return;
	}
	size_t n = wcslen(wid) + wcslen(SIGNER_SUFFIX) + 1;
	wchar_t *vname = (wchar_t *)malloc(n * sizeof(wchar_t));
	if (vname != NULL) {
		_snwprintf_s(vname, n, _TRUNCATE, L"%ls%ls", wid, SIGNER_SUFFIX);
		wchar_t w[U_CAMERA_CONSENT_SIGNER_MAX];
		if (reg_read_string_w(root, CONSENT_KEY L"\\Delegating", vname, w, (DWORD)(sizeof(w) / sizeof(w[0])))) {
			if (!w_to_u8(w, (int)wcslen(w), out, cap)) {
				out[0] = '\0';
			}
		}
		free(vname);
	}
	free(wid);
}

//! u_camera_consent_enum_fn over a Delegating key: value DATA = executable path.
static enum u_camera_consent_enum_result
reg_delegating_enum_fn(void *ctx, uint32_t index, char *name, size_t name_cap, char *path, size_t path_cap)
{
	struct reg_enum *e = (struct reg_enum *)ctx;
	DWORD type = 0, dlen = 0;
	enum u_camera_consent_enum_result r = reg_enum_at(e, (DWORD)index, &type, &dlen);
	if (r != U_CAMERA_CONSENT_ENUM_ENTRY) {
		return r;
	}
	if (type != REG_SZ && type != REG_EXPAND_SZ) {
		return U_CAMERA_CONSENT_ENUM_SKIP;
	}
	if (is_signer_value_name(e->name)) {
		return U_CAMERA_CONSENT_ENUM_SKIP; // an entry's signer, not an entry
	}
	const wchar_t *wd = (const wchar_t *)e->data;
	if (!w_to_u8(wd, (int)wcslen(wd), path, path_cap) || !w_to_u8(e->name, (int)wcslen(e->name), name, name_cap)) {
		return U_CAMERA_CONSENT_ENUM_SKIP; // does not fit / not valid UTF-16
	}
	return U_CAMERA_CONSENT_ENUM_ENTRY;
}

//! Walk a Delegating key: each value's DATA is an executable path.
static bool
win_delegating_has(HKEY root, const char *exe, char *match_name, size_t match_cap)
{
	struct reg_enum e;
	if (!reg_enum_open(&e, root, CONSENT_KEY L"\\Delegating")) {
		return false;
	}
	bool found = u_camera_consent_list_find(reg_delegating_enum_fn, &e, exe, true, match_name, match_cap);
	reg_enum_close(&e);
	return found;
}

static bool
win_get(void *ctx, const char *exe, enum u_camera_consent_stored *out)
{
	(void)ctx;
	*out = U_CAMERA_CONSENT_STORED_NONE;
	wchar_t *wexe = u8_to_w(exe);
	if (wexe == NULL) {
		return false;
	}
	DWORD v = 0;
	bool ok = reg_read_dword(HKEY_CURRENT_USER, CONSENT_KEY L"\\Apps", wexe, &v);
	free(wexe);
	if (!ok) {
		return false;
	}
	*out = v == 1 ? U_CAMERA_CONSENT_STORED_ALLOW : v == 2 ? U_CAMERA_CONSENT_STORED_DENY : U_CAMERA_CONSENT_STORED_NONE;
	return true;
}

static bool
win_set(void *ctx, const char *exe, enum u_camera_consent_stored value)
{
	(void)ctx;
	wchar_t *wexe = u8_to_w(exe);
	if (wexe == NULL) {
		return false;
	}
	bool ok;
	if (value == U_CAMERA_CONSENT_STORED_NONE) {
		ok = reg_delete_value(HKEY_CURRENT_USER, CONSENT_KEY L"\\Apps", wexe);
	} else {
		ok = reg_write_dword(HKEY_CURRENT_USER, CONSENT_KEY L"\\Apps", wexe,
		                     value == U_CAMERA_CONSENT_STORED_ALLOW ? 1u : 2u);
	}
	free(wexe);
	return ok;
}

static bool
win_get_delegation(void *ctx, const char *exe, struct u_camera_consent_delegation *out)
{
	(void)ctx;
	memset(out, 0, sizeof(*out));
	static const struct
	{
		HKEY root;
		enum u_camera_consent_delegation_scope scope;
	} lists[2] = {
	    {HKEY_LOCAL_MACHINE, U_CAMERA_CONSENT_DELEGATION_SYSTEM}, // the installer's list wins
	    {HKEY_CURRENT_USER, U_CAMERA_CONSENT_DELEGATION_USER},
	};
	for (int i = 0; i < 2; i++) {
		char id[1024];
		if (win_delegating_has(lists[i].root, exe, id, sizeof(id))) {
			out->scope = lists[i].scope;
			read_entry_signer(lists[i].root, id, out->signer, sizeof(out->signer));
			return true;
		}
	}
	return false;
}

static bool
win_sharing_enabled(void *ctx)
{
	(void)ctx;
	DWORD v = 1;
	if (reg_read_dword(HKEY_CURRENT_USER, CONSENT_KEY, L"Sharing", &v)) {
		return v != 0;
	}
	return true;
}

static bool
win_set_sharing_enabled(void *ctx, bool enabled)
{
	(void)ctx;
	return reg_write_dword(HKEY_CURRENT_USER, CONSENT_KEY, L"Sharing", enabled ? 1u : 0u);
}

static bool
win_get_secret(void *ctx, uint8_t out[U_CAMERA_CONSENT_SECRET_SIZE])
{
	(void)ctx;
	HKEY k;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, CONSENT_KEY, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, NULL) !=
	    ERROR_SUCCESS) {
		return false;
	}
	DWORD type = 0, size = U_CAMERA_CONSENT_SECRET_SIZE;
	LSTATUS rc = RegQueryValueExW(k, L"Secret", NULL, &type, out, &size);
	if (rc == ERROR_SUCCESS && type == REG_BINARY && size == U_CAMERA_CONSENT_SECRET_SIZE) {
		RegCloseKey(k);
		return true;
	}
	fill_random(out, U_CAMERA_CONSENT_SECRET_SIZE);
	rc = RegSetValueExW(k, L"Secret", 0, REG_BINARY, out, U_CAMERA_CONSENT_SECRET_SIZE);
	RegCloseKey(k);
	return rc == ERROR_SUCCESS;
}

static const struct u_camera_consent_store_ops win_ops = {
    .get = win_get,
    .set = win_set,
    .get_delegation = win_get_delegation,
    .sharing_enabled = win_sharing_enabled,
    .set_sharing_enabled = win_set_sharing_enabled,
    .get_secret = win_get_secret,
};

const struct u_camera_consent_store_ops *
u_camera_consent_store_default(void)
{
	return &win_ops;
}

//! Set (non-empty @p signer_u8) or delete the "<id>.signer" companion of entry @p id (open key @p k).
static bool
write_entry_signer(HKEY k, const wchar_t *id, const char *signer_u8)
{
	size_t n = wcslen(id) + wcslen(SIGNER_SUFFIX) + 1;
	wchar_t *vname = (wchar_t *)malloc(n * sizeof(wchar_t));
	if (vname == NULL) {
		return false;
	}
	_snwprintf_s(vname, n, _TRUNCATE, L"%ls%ls", id, SIGNER_SUFFIX);
	bool ok;
	if (signer_u8 == NULL || signer_u8[0] == '\0') {
		LSTATUS rc = RegDeleteValueW(k, vname);
		ok = rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
	} else {
		wchar_t *ws = u8_to_w(signer_u8);
		ok = ws != NULL && RegSetValueExW(k, vname, 0, REG_SZ, (const BYTE *)ws,
		                                  (DWORD)((wcslen(ws) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
		free(ws);
	}
	free(vname);
	return ok;
}

bool
u_camera_consent_store_set_delegating(const char *exe, bool delegating, const char *signer)
{
	char name[1024];
	bool present = win_delegating_has(HKEY_CURRENT_USER, exe, name, sizeof(name));
	wchar_t *wid = NULL;
	if (present) {
		wid = u8_to_w(name);
		if (wid == NULL) {
			return false;
		}
	} else if (!delegating) {
		return true; // nothing to remove
	} else {
		// New entry. Value name: the executable's base name (unique enough; data is what matters).
		wchar_t *wexe = u8_to_w(exe);
		if (wexe == NULL) {
			return false;
		}
		const wchar_t *base = wexe;
		for (const wchar_t *p = wexe; *p != L'\0'; p++) {
			if (*p == L'\\' || *p == L'/') {
				base = p + 1;
			}
		}
		wid = _wcsdup(base);
		free(wexe);
		if (wid == NULL) {
			return false;
		}
	}
	HKEY k;
	bool ok = false;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, CONSENT_KEY L"\\Delegating", 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k,
	                    NULL) == ERROR_SUCCESS) {
		if (!delegating) {
			LSTATUS rc = RegDeleteValueW(k, wid);
			ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) && write_entry_signer(k, wid, NULL);
		} else {
			ok = true;
			if (!present) {
				wchar_t *wexe = u8_to_w(exe);
				ok = wexe != NULL &&
				     RegSetValueExW(k, wid, 0, REG_SZ, (const BYTE *)wexe,
				                    (DWORD)((wcslen(wexe) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
				free(wexe);
			}
			ok = ok && write_entry_signer(k, wid, signer);
		}
		RegCloseKey(k);
	}
	free(wid);
	return ok;
}

static void
win_list_delegating(HKEY root, const char *origin, void (*cb)(void *, char, const char *, const char *), void *ctx)
{
	struct reg_enum e;
	if (!reg_enum_open(&e, root, CONSENT_KEY L"\\Delegating")) {
		return;
	}
	char name[1024];
	char path[4096];
	for (uint32_t i = 0; i < U_CAMERA_CONSENT_LIST_MAX; i++) {
		enum u_camera_consent_enum_result r =
		    reg_delegating_enum_fn(&e, i, name, sizeof(name), path, sizeof(path));
		if (r == U_CAMERA_CONSENT_ENUM_END) {
			break;
		}
		if (r == U_CAMERA_CONSENT_ENUM_ENTRY) {
			char signer[U_CAMERA_CONSENT_SIGNER_MAX];
			read_entry_signer(root, name, signer, sizeof(signer));
			char value[U_CAMERA_CONSENT_SIGNER_MAX + 64];
			if (signer[0] != '\0') {
				snprintf(value, sizeof(value), "%s, signer \"%s\"", origin, signer);
			} else {
				snprintf(value, sizeof(value), "%s, no signer", origin);
			}
			cb(ctx, 'd', path, value);
		}
	}
	reg_enum_close(&e);
}

void
u_camera_consent_store_list(void (*cb)(void *ctx, char kind, const char *exe, const char *value), void *ctx)
{
	cb(ctx, 's', "", win_sharing_enabled(NULL) ? "on" : "off");
	struct reg_enum e;
	if (reg_enum_open(&e, HKEY_CURRENT_USER, CONSENT_KEY L"\\Apps")) {
		char exe[4096];
		for (DWORD i = 0; i < U_CAMERA_CONSENT_LIST_MAX; i++) {
			DWORD type = 0, dlen = 0;
			enum u_camera_consent_enum_result r = reg_enum_at(&e, i, &type, &dlen);
			if (r == U_CAMERA_CONSENT_ENUM_END) {
				break;
			}
			if (r != U_CAMERA_CONSENT_ENUM_ENTRY || type != REG_DWORD || dlen != sizeof(DWORD) ||
			    !w_to_u8(e.name, (int)wcslen(e.name), exe, sizeof(exe))) {
				continue;
			}
			DWORD v;
			memcpy(&v, e.data, sizeof(v));
			cb(ctx, 'a', exe, v == 1 ? "allow" : v == 2 ? "deny" : "?");
		}
		reg_enum_close(&e);
	}
	win_list_delegating(HKEY_LOCAL_MACHINE, "system", cb, ctx);
	win_list_delegating(HKEY_CURRENT_USER, "user", cb, ctx);
}

bool
u_camera_consent_store_path(char *out, size_t cap)
{
	snprintf(out, cap, "HKCU\\Software\\DisplayXR\\CameraConsent");
	return true;
}

//! "Deny" in a ConsentStore Value means off; anything else (Allow, absent) is on.
static bool
consent_store_value_denies(HKEY root, const wchar_t *sub)
{
	wchar_t v[32];
	return reg_read_string_w(root, sub, L"Value", v, (DWORD)(sizeof(v) / sizeof(v[0]))) &&
	       _wcsicmp(v, L"Deny") == 0;
}

bool
u_camera_consent_os_camera_allowed(const char *exe)
{
	// Global (policy, then user), then the desktop-app class, then this app.
	if (consent_store_value_denies(HKEY_LOCAL_MACHINE, CAM_CONSENT_STORE_WEBCAM) ||
	    consent_store_value_denies(HKEY_CURRENT_USER, CAM_CONSENT_STORE_WEBCAM) ||
	    consent_store_value_denies(HKEY_CURRENT_USER, CAM_CONSENT_STORE_WEBCAM L"\\NonPackaged")) {
		return false;
	}
	if (exe == NULL || exe[0] == '\0') {
		return true;
	}
	wchar_t *wexe = u8_to_w(exe);
	if (wexe == NULL) {
		return true; // not a path the OS could have stored an entry under
	}
	static const wchar_t prefix[] = CAM_CONSENT_STORE_WEBCAM L"\\NonPackaged\\";
	size_t plen = wcslen(prefix), elen = wcslen(wexe);
	wchar_t *sub = (wchar_t *)malloc((plen + elen + 1) * sizeof(wchar_t));
	bool denied = false;
	if (sub != NULL) {
		memcpy(sub, prefix, plen * sizeof(wchar_t));
		for (size_t i = 0; i < elen; i++) {
			sub[plen + i] = (wexe[i] == L'\\' || wexe[i] == L'/') ? L'#' : wexe[i];
		}
		sub[plen + elen] = L'\0';
		denied = consent_store_value_denies(HKEY_CURRENT_USER, sub);
		free(sub);
	}
	free(wexe);
	return !denied;
}

//! A known folder as UTF-8 into @p out; false if unavailable.
static bool
known_folder_u8(REFKNOWNFOLDERID id, char *out, size_t cap)
{
	PWSTR w = NULL;
	bool ok = false;
	if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, NULL, &w)) && w != NULL) {
		ok = w_to_u8(w, (int)wcslen(w), out, cap) && out[0] != '\0';
	}
	CoTaskMemFree(w);
	return ok;
}

bool
u_camera_consent_path_user_writable(const char *exe)
{
	if (exe == NULL || exe[0] == '\0') {
		return true;
	}
	// Admin-protected roots (spec §7.1.1). Known caveat, documented there: a
	// few subfolders of %SystemRoot% (Temp, Tasks, ...) and any install dir
	// whose ACL an installer loosened are writable all the same; the path
	// rule is the cheap 99% gate, the signer check covers everything else.
	static const KNOWNFOLDERID *const roots[] = {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86,
	                                             &FOLDERID_Windows};
	for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		char dir[1024];
		if (known_folder_u8(roots[i], dir, sizeof(dir)) && u_camera_consent_path_is_under(exe, dir, true)) {
			return false;
		}
	}
	return true;
}

bool
u_camera_consent_exe_signer(const char *exe, char *out, size_t cap)
{
	if (cap > 0) {
		out[0] = '\0';
	}
	if (exe == NULL || exe[0] == '\0' || cap == 0) {
		return false;
	}
	wchar_t *wexe = u8_to_w(exe);
	if (wexe == NULL) {
		return false;
	}
	WINTRUST_FILE_INFO fi;
	memset(&fi, 0, sizeof(fi));
	fi.cbStruct = sizeof(fi);
	fi.pcwszFilePath = wexe;

	WINTRUST_DATA wd;
	memset(&wd, 0, sizeof(wd));
	wd.cbStruct = sizeof(wd);
	wd.dwUIChoice = WTD_UI_NONE;
	wd.fdwRevocationChecks = WTD_REVOKE_NONE;
	wd.dwUnionChoice = WTD_CHOICE_FILE;
	wd.pFile = &fi;
	wd.dwStateAction = WTD_STATEACTION_VERIFY;
	// Never touch the network: the service must not stall a stream start on a
	// CRL / AIA fetch. Revocation is not checked (WTD_REVOKE_NONE) and any URL
	// retrieval is cache-only.
	wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;

	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	LONG st = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
	bool ok = false;
	if (st == ERROR_SUCCESS) {
		CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
		CRYPT_PROVIDER_SGNR *sg = pd != NULL ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : NULL;
		CRYPT_PROVIDER_CERT *pc = sg != NULL ? WTHelperGetProvCertFromChain(sg, 0) : NULL;
		if (pc != NULL && pc->pCert != NULL) {
			wchar_t cn[U_CAMERA_CONSENT_SIGNER_MAX];
			DWORD n = CertGetNameStringW(pc->pCert, CERT_NAME_ATTR_TYPE, 0, (void *)szOID_COMMON_NAME, cn,
			                             (DWORD)(sizeof(cn) / sizeof(cn[0])));
			ok = n > 1 && w_to_u8(cn, (int)wcslen(cn), out, cap) && out[0] != '\0';
		}
	}
	wd.dwStateAction = WTD_STATEACTION_CLOSE;
	WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
	free(wexe);
	return ok;
}

#else // POSIX

/*
 *
 * POSIX: JSON file.
 *
 */

#define CONSENT_FILE "camera_consent.json"

//! POSIX paths compare byte-exact.
static bool
path_equal(const char *a, const char *b)
{
	return u_camera_consent_path_equal(a, b, false);
}

#ifdef XRT_OS_MACOS
#define SYSTEM_DELEGATING_FILE "/Library/Application Support/DisplayXR/camera-delegating.json"
#else
#define SYSTEM_DELEGATING_FILE "/etc/displayxr/camera-delegating.json"
#endif

static bool
user_path(char *out, size_t cap)
{
	int n = u_file_get_path_in_config_dir(CONSENT_FILE, out, cap);
	return n > 0 && (size_t)n < cap;
}

//! Parse the user file; NULL = absent / unreadable. An empty object otherwise.
static cJSON *
load_user(bool create)
{
	char path[1024];
	if (!user_path(path, sizeof(path))) {
		return NULL;
	}
	size_t size = 0;
	char *content = u_file_read_content_from_path(path, &size);
	cJSON *root = NULL;
	if (content != NULL) {
		root = cJSON_Parse(content);
		free(content);
	}
	if (root == NULL && create) {
		root = cJSON_CreateObject();
	}
	return root;
}

static bool
save_user(cJSON *root)
{
	// u_file_open_file_in_config_dir creates the directory; we then clamp the
	// mode — the file holds the id-keying secret.
	FILE *f = u_file_open_file_in_config_dir(CONSENT_FILE, "w");
	if (f == NULL) {
		return false;
	}
	char *text = cJSON_Print(root);
	bool ok = text != NULL && fputs(text, f) >= 0;
	free(text);
	fclose(f);
	char path[1024];
	if (user_path(path, sizeof(path))) {
		(void)chmod(path, S_IRUSR | S_IWUSR);
	}
	return ok;
}

static bool
posix_get(void *ctx, const char *exe, enum u_camera_consent_stored *out)
{
	(void)ctx;
	*out = U_CAMERA_CONSENT_STORED_NONE;
	cJSON *root = load_user(false);
	if (root == NULL) {
		return false;
	}
	bool found = false;
	cJSON *apps = cJSON_GetObjectItem(root, "apps");
	cJSON *it = NULL;
	cJSON_ArrayForEach(it, apps)
	{
		if (it->string != NULL && cJSON_IsString(it) && path_equal(it->string, exe)) {
			found = true;
			*out = strcmp(it->valuestring, "allow") == 0  ? U_CAMERA_CONSENT_STORED_ALLOW
			       : strcmp(it->valuestring, "deny") == 0 ? U_CAMERA_CONSENT_STORED_DENY
			                                              : U_CAMERA_CONSENT_STORED_NONE;
			break;
		}
	}
	cJSON_Delete(root);
	return found;
}

static bool
posix_set(void *ctx, const char *exe, enum u_camera_consent_stored value)
{
	(void)ctx;
	cJSON *root = load_user(true);
	if (root == NULL) {
		return false;
	}
	cJSON *apps = cJSON_GetObjectItem(root, "apps");
	if (apps == NULL || !cJSON_IsObject(apps)) {
		cJSON_DeleteItemFromObject(root, "apps");
		apps = cJSON_AddObjectToObject(root, "apps");
	}
	cJSON_DeleteItemFromObject(apps, exe);
	if (value != U_CAMERA_CONSENT_STORED_NONE) {
		cJSON_AddStringToObject(apps, exe, value == U_CAMERA_CONSENT_STORED_ALLOW ? "allow" : "deny");
	}
	bool ok = save_user(root);
	cJSON_Delete(root);
	return ok;
}

static bool
delegating_array_has(const cJSON *arr, const char *exe)
{
	const cJSON *it = NULL;
	cJSON_ArrayForEach(it, arr)
	{
		if (cJSON_IsString(it) && path_equal(it->valuestring, exe)) {
			return true;
		}
	}
	return false;
}

static cJSON *
load_system(void)
{
	size_t size = 0;
	char *content = u_file_read_content_from_path(SYSTEM_DELEGATING_FILE, &size);
	if (content == NULL) {
		return NULL;
	}
	cJSON *root = cJSON_Parse(content);
	free(content);
	return root;
}

//! Path-only (no signer is recorded on POSIX yet): the system list wins over the user's.
static bool
posix_get_delegation(void *ctx, const char *exe, struct u_camera_consent_delegation *out)
{
	(void)ctx;
	memset(out, 0, sizeof(*out));
	cJSON *sys = load_system();
	if (sys != NULL) {
		if (delegating_array_has(cJSON_GetObjectItem(sys, "delegating"), exe)) {
			out->scope = U_CAMERA_CONSENT_DELEGATION_SYSTEM;
		}
		cJSON_Delete(sys);
	}
	if (out->scope == U_CAMERA_CONSENT_DELEGATION_NONE) {
		cJSON *user = load_user(false);
		if (user != NULL) {
			if (delegating_array_has(cJSON_GetObjectItem(user, "delegating"), exe)) {
				out->scope = U_CAMERA_CONSENT_DELEGATION_USER;
			}
			cJSON_Delete(user);
		}
	}
	return out->scope != U_CAMERA_CONSENT_DELEGATION_NONE;
}

static bool
posix_sharing_enabled(void *ctx)
{
	(void)ctx;
	bool on = true;
	cJSON *root = load_user(false);
	if (root != NULL) {
		cJSON *s = cJSON_GetObjectItem(root, "sharing");
		if (cJSON_IsBool(s)) {
			on = cJSON_IsTrue(s);
		}
		cJSON_Delete(root);
	}
	return on;
}

static bool
posix_set_sharing_enabled(void *ctx, bool enabled)
{
	(void)ctx;
	cJSON *root = load_user(true);
	if (root == NULL) {
		return false;
	}
	cJSON_DeleteItemFromObject(root, "sharing");
	cJSON_AddBoolToObject(root, "sharing", enabled);
	bool ok = save_user(root);
	cJSON_Delete(root);
	return ok;
}

static int
hexval(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static bool
posix_get_secret(void *ctx, uint8_t out[U_CAMERA_CONSENT_SECRET_SIZE])
{
	(void)ctx;
	cJSON *root = load_user(true);
	if (root == NULL) {
		return false;
	}
	cJSON *s = cJSON_GetObjectItem(root, "secret");
	if (cJSON_IsString(s) && strlen(s->valuestring) == 2 * U_CAMERA_CONSENT_SECRET_SIZE) {
		bool ok = true;
		for (int i = 0; i < U_CAMERA_CONSENT_SECRET_SIZE && ok; i++) {
			int hi = hexval(s->valuestring[2 * i]), lo = hexval(s->valuestring[2 * i + 1]);
			ok = hi >= 0 && lo >= 0;
			out[i] = (uint8_t)((hi << 4) | lo);
		}
		if (ok) {
			cJSON_Delete(root);
			return true;
		}
	}
	fill_random(out, U_CAMERA_CONSENT_SECRET_SIZE);
	char hex[2 * U_CAMERA_CONSENT_SECRET_SIZE + 1];
	for (int i = 0; i < U_CAMERA_CONSENT_SECRET_SIZE; i++) {
		snprintf(hex + 2 * i, 3, "%02x", out[i]);
	}
	cJSON_DeleteItemFromObject(root, "secret");
	cJSON_AddStringToObject(root, "secret", hex);
	bool ok = save_user(root);
	cJSON_Delete(root);
	return ok;
}

static const struct u_camera_consent_store_ops posix_ops = {
    .get = posix_get,
    .set = posix_set,
    .get_delegation = posix_get_delegation,
    .sharing_enabled = posix_sharing_enabled,
    .set_sharing_enabled = posix_set_sharing_enabled,
    .get_secret = posix_get_secret,
};

const struct u_camera_consent_store_ops *
u_camera_consent_store_default(void)
{
	return &posix_ops;
}

bool
u_camera_consent_store_set_delegating(const char *exe, bool delegating, const char *signer)
{
	(void)signer; // POSIX delegation is path-only for now (see the file comment)
	cJSON *root = load_user(true);
	if (root == NULL) {
		return false;
	}
	cJSON *arr = cJSON_GetObjectItem(root, "delegating");
	if (arr == NULL || !cJSON_IsArray(arr)) {
		cJSON_DeleteItemFromObject(root, "delegating");
		arr = cJSON_AddArrayToObject(root, "delegating");
	}
	// Remove any existing entry, then re-add when requested.
	for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
		cJSON *it = cJSON_GetArrayItem(arr, i);
		if (cJSON_IsString(it) && path_equal(it->valuestring, exe)) {
			cJSON_DeleteItemFromArray(arr, i);
		}
	}
	if (delegating) {
		cJSON_AddItemToArray(arr, cJSON_CreateString(exe));
	}
	bool ok = save_user(root);
	cJSON_Delete(root);
	return ok;
}

void
u_camera_consent_store_list(void (*cb)(void *ctx, char kind, const char *exe, const char *value), void *ctx)
{
	cb(ctx, 's', "", posix_sharing_enabled(NULL) ? "on" : "off");
	cJSON *user = load_user(false);
	if (user != NULL) {
		cJSON *it = NULL;
		cJSON_ArrayForEach(it, cJSON_GetObjectItem(user, "apps"))
		{
			if (it->string != NULL && cJSON_IsString(it)) {
				cb(ctx, 'a', it->string, it->valuestring);
			}
		}
		cJSON_ArrayForEach(it, cJSON_GetObjectItem(user, "delegating"))
		{
			if (cJSON_IsString(it)) {
				cb(ctx, 'd', it->valuestring, "user");
			}
		}
		cJSON_Delete(user);
	}
	cJSON *sys = load_system();
	if (sys != NULL) {
		cJSON *it = NULL;
		cJSON_ArrayForEach(it, cJSON_GetObjectItem(sys, "delegating"))
		{
			if (cJSON_IsString(it)) {
				cb(ctx, 'd', it->valuestring, "system");
			}
		}
		cJSON_Delete(sys);
	}
}

bool
u_camera_consent_store_path(char *out, size_t cap)
{
	return user_path(out, cap);
}

bool
u_camera_consent_os_camera_allowed(const char *exe)
{
	(void)exe;
	return true; // the OS switch (macOS TCC) does not see this consumer: nothing to read
}

bool
u_camera_consent_path_user_writable(const char *exe)
{
	// Path-only delegation on POSIX for now: no code-signature check exists
	// here yet (macOS SecStaticCode / Linux: follow-up), so a "user-writable"
	// answer could only ever refuse. Documented in spec §7.1.1.
	(void)exe;
	return false;
}

bool
u_camera_consent_exe_signer(const char *exe, char *out, size_t cap)
{
	(void)exe;
	if (cap > 0) {
		out[0] = '\0';
	}
	return false;
}

#endif
