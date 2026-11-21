// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  dxr-get: plan / fetch / install / uninstall DisplayXR components
 *         from the signed component catalog. Epic #2702 (P1 #2704, P2 #2705);
 *         ADR-064 steps 2 and 3 (schema 2, receipts, uninstall, inventory).
 *
 * This file is the Windows half: the network, the registry, Authenticode and
 * the installers. What to do is decided in get_core.c, which has no OS calls
 * and is unit-tested (tests/tests_get_catalog.cpp).
 *
 * Rules that matter more than the code (docs/specs/runtime/dxr-get.md):
 * - Fail closed: a payload installs only if its size, SHA-256 and
 *   Authenticode signer all match the signed catalog, and the signer is one
 *   the catalog delegates to the item's own publisher.
 * - Every installable binary is signed. There is no hash-pinned-only form.
 * - A stale or unreachable catalog is "offline", never an authority: it can
 *   only ever cause dxr-get to do nothing.
 * - Never downgrade, never touch ActiveRuntime, never write Trust\Allowlist.
 * - A hardware item installs only on positive evidence (a high-confidence
 *   catalog detect rule matching this machine).
 * - No unbounded wait: every network call has a timeout and a retry limit,
 *   and a child installer is waited for a bounded time.
 */

#include "get.h"
#include "get_core.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CATALOG_V2 "https://updates.displayxr.org/catalog/v2/stable.json"
#define CATALOG_V1 "https://updates.displayxr.org/catalog/v1/stable.json"
#define SERIAL_KEY "SOFTWARE\\DisplayXR\\Update"
#define RECEIPT_KEY "SOFTWARE\\DisplayXR\\Catalog"
#define MAX_RECEIPTS 128

//! How long a child installer or uninstaller is waited for. One that is still
//! running after this is reported as failed; it is not killed.
#define CHILD_WAIT_MS (60u * 60u * 1000u)
//! How long an uninstaller that returned at once (NSIS re-executes itself from
//! %TEMP%) is given to actually remove the component.
#define UNINSTALL_SETTLE_MS (120u * 1000u)

void
dxr_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fflush(stderr);
}

static bool
env_on(const char *name)
{
	const char *v = getenv(name);
	return v != NULL && strcmp(v, "1") == 0;
}

static const char *
jstr(const cJSON *o, const char *k)
{
	const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void
print_json(cJSON *o)
{
	char *s = cJSON_PrintUnformatted(o);
	printf("%s\n", s);
	(void)fflush(stdout);
	cJSON_free(s);
	cJSON_Delete(o);
}

static bool
is_admin(void)
{
	return IsUserAnAdmin() != FALSE;
}

/*
 * Download cache.
 */

static bool
downloads_dir(wchar_t *out, size_t cap, bool create)
{
	wchar_t *pd = NULL;
	if (FAILED(SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL, &pd))) {
		return false;
	}
	(void)_snwprintf(out, cap, L"%ls\\DisplayXR\\Downloads", pd);
	CoTaskMemFree(pd);
	if (create) {
		(void)SHCreateDirectoryExW(NULL, out, NULL);
	}
	return true;
}

static uint64_t
size_of_file(const wchar_t *path)
{
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
		return 0;
	}
	return ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

/*
 * Receipts: HKLM\Software\DisplayXR\Catalog\<id>, 64-bit view.
 */

static bool
receipt_id_ok(const char *id)
{
	return id != NULL && id[0] != '\0' && strlen(id) < 96 && strpbrk(id, "\\/") == NULL;
}

static void
reg_get(HKEY k, const char *name, char *out, size_t out_size)
{
	DWORD sz = (DWORD)out_size;
	out[0] = '\0';
	if (RegGetValueA(k, NULL, name, RRF_RT_REG_SZ, NULL, out, &sz) != ERROR_SUCCESS) {
		out[0] = '\0';
	}
}

static bool
receipt_read(const char *id, struct dxr_receipt *r)
{
	memset(r, 0, sizeof(*r));
	if (!receipt_id_ok(id)) {
		return false;
	}
	char key[256];
	(void)snprintf(key, sizeof(key), RECEIPT_KEY "\\%s", id);
	HKEY k;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) {
		return false;
	}
	(void)snprintf(r->id, sizeof(r->id), "%s", id);
	reg_get(k, "Name", r->name, sizeof(r->name));
	reg_get(k, "Class", r->item_class, sizeof(r->item_class));
	reg_get(k, "Publisher", r->publisher, sizeof(r->publisher));
	reg_get(k, "Version", r->version, sizeof(r->version));
	reg_get(k, "SourceUrl", r->source_url, sizeof(r->source_url));
	reg_get(k, "Sha256", r->sha256, sizeof(r->sha256));
	reg_get(k, "File", r->file, sizeof(r->file));
	reg_get(k, "InstallTime", r->install_time, sizeof(r->install_time));
	reg_get(k, "UninstallCommand", r->uninstall, sizeof(r->uninstall));
	reg_get(k, "VersionPath", r->version_path, sizeof(r->version_path));
	reg_get(k, "Requires", r->requires_ids, sizeof(r->requires_ids));
	RegCloseKey(k);
	return r->version[0] != '\0';
}

static bool
receipt_write(const struct dxr_receipt *r)
{
	if (!receipt_id_ok(r->id)) {
		return false;
	}
	char key[256];
	(void)snprintf(key, sizeof(key), RECEIPT_KEY "\\%s", r->id);
	HKEY k;
	if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &k, NULL) !=
	    ERROR_SUCCESS) {
		return false;
	}
	const struct
	{
		const char *name, *value;
	} v[] = {
	    {"Name", r->name},
	    {"Class", r->item_class},
	    {"Publisher", r->publisher},
	    {"Version", r->version},
	    {"SourceUrl", r->source_url},
	    {"Sha256", r->sha256},
	    {"File", r->file},
	    {"InstallTime", r->install_time},
	    {"UninstallCommand", r->uninstall},
	    {"VersionPath", r->version_path},
	    {"Requires", r->requires_ids},
	};
	bool ok = true;
	for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		ok = RegSetValueExA(k, v[i].name, 0, REG_SZ, (const BYTE *)v[i].value, (DWORD)strlen(v[i].value) + 1) ==
		         ERROR_SUCCESS &&
		     ok;
	}
	RegCloseKey(k);
	return ok;
}

static void
receipt_delete(const char *id)
{
	char key[256];
	if (!receipt_id_ok(id)) {
		return;
	}
	(void)snprintf(key, sizeof(key), RECEIPT_KEY "\\%s", id);
	(void)RegDeleteKeyExA(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);
}

static int
receipts_all(struct dxr_receipt *out, int max)
{
	HKEY k;
	int n = 0;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, RECEIPT_KEY, 0, KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &k) !=
	    ERROR_SUCCESS) {
		return 0;
	}
	for (DWORD i = 0; n < max; i++) {
		char name[128];
		DWORD sz = sizeof(name);
		if (RegEnumKeyExA(k, i, name, &sz, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
			break;
		}
		if (receipt_read(name, &out[n])) {
			n++;
		}
	}
	RegCloseKey(k);
	return n;
}

/*
 * The machine, as get_core.c sees it.
 */

static bool
env_reg_sz(void *u, const char *full, char *out, size_t n)
{
	(void)u;
	return dxr_reg_read_path_sz(full, out, n);
}

static bool
env_reg_dword(void *u, const char *key, const char *value, uint32_t *out)
{
	(void)u;
	return dxr_reg_read_dword(key, value, out);
}

static bool
env_reg_key_exists(void *u, const char *key)
{
	(void)u;
	return dxr_reg_key_exists(key);
}

static bool
env_file_exists(void *u, const char *path)
{
	(void)u;
	return dxr_file_exists(path);
}

static bool
env_service_exists(void *u, const char *name)
{
	(void)u;
	return dxr_service_exists(name);
}

/*
 * Detect evidence, gathered once per run (#2737): `list` evaluates every item's
 * rules, and a SetupAPI walk per rule set would be wasted work.
 */
#define MAX_EDIDS 32
#define MAX_USB 128

static struct
{
	struct dxr_edid_id edid[MAX_EDIDS];
	int n_edid; //!< -1 until gathered
	struct dxr_usb_id usb[MAX_USB];
	int n_usb; //!< -1 until gathered
} g_evidence = {.n_edid = -1, .n_usb = -1};

static int
env_edids(void *u, struct dxr_edid_id *out, int max)
{
	(void)u;
	if (g_evidence.n_edid < 0) {
		g_evidence.n_edid = dxr_detect_edids(g_evidence.edid, MAX_EDIDS);
	}
	int n = g_evidence.n_edid < max ? g_evidence.n_edid : max;
	memcpy(out, g_evidence.edid, (size_t)n * sizeof(*out));
	return n;
}

static int
env_usb(void *u, struct dxr_usb_id *out, int max)
{
	(void)u;
	if (g_evidence.n_usb < 0) {
		g_evidence.n_usb = dxr_detect_usb(g_evidence.usb, MAX_USB);
	}
	int n = g_evidence.n_usb < max ? g_evidence.n_usb : max;
	memcpy(out, g_evidence.usb, (size_t)n * sizeof(*out));
	return n;
}

static bool
env_receipt(void *u, const char *id, struct dxr_receipt *out)
{
	(void)u;
	return receipt_read(id, out);
}

static uint64_t
env_cached_bytes(void *u, const char *sha, const char *file, uint64_t size)
{
	(void)u;
	wchar_t dir[MAX_PATH], path[MAX_PATH + 8];
	if (!downloads_dir(dir, MAX_PATH, false)) {
		return 0;
	}
	(void)_snwprintf(path, MAX_PATH, L"%ls\\%hs-%hs", dir, sha, file);
	if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
		return size; // a finished file is named by its hash; it is re-hashed before use
	}
	(void)_snwprintf(path, MAX_PATH + 8, L"%ls\\%hs-%hs.part", dir, sha, file);
	uint64_t got = size_of_file(path);
	return got > size ? 0 : got;
}

//! Not const: --assume-runtime / --assume-abi fill in the tail of it.
static struct dxr_env k_env = {
    .reg_sz = env_reg_sz,
    .reg_dword = env_reg_dword,
    .reg_key_exists = env_reg_key_exists,
    .file_exists = env_file_exists,
    .service_exists = env_service_exists,
    .edids = env_edids,
    .usb = env_usb,
    .receipt = env_receipt,
    .cached_bytes = env_cached_bytes,
};

/*
 * Catalog.
 */

static bool
read_whole_file(const char *path, char **out, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	(void)fseek(f, 0, SEEK_END);
	long n = ftell(f);
	(void)fseek(f, 0, SEEK_SET);
	if (n < 0 || n > 8 * 1024 * 1024) {
		fclose(f);
		return false;
	}
	*out = (char *)malloc((size_t)n + 1);
	*len = fread(*out, 1, (size_t)n, f);
	(*out)[*len] = '\0';
	fclose(f);
	return *len == (size_t)n;
}

/*!
 * Fetches, verifies and parses one catalog document (schema 1 or 2).
 * @p no_network is set when the failure was a transport error: the host could
 * not be reached at all, so asking it for another document is pointless.
 */
static int
load_catalog(const char *where, bool verify, struct dxr_catalog *c, bool *no_network)
{
	memset(c, 0, sizeof(*c));
	*no_network = false;
	char *body = NULL, *sig = NULL;
	size_t blen = 0, slen = 0;
	char sig_where[2100];
	(void)snprintf(sig_where, sizeof(sig_where), "%s.sig", where);
	const bool remote = _strnicmp(where, "https://", 8) == 0 || _strnicmp(where, "http://", 7) == 0;
	dxr_log("catalog: %s\n", where);
	if (remote) {
		int st = dxr_http_get_mem(where, &body, &blen, 4 * 1024 * 1024);
		if (st != 200) {
			dxr_log("catalog unavailable (%s %d) -- treating as offline\n",
			        st < 0 ? "network error" : "HTTP", st);
			*no_network = st < 0;
			return DXR_GET_OFFLINE;
		}
		if (verify && dxr_http_get_mem(sig_where, &sig, &slen, 4096) != 200) {
			free(body);
			dxr_log("catalog signature unavailable -- treating as offline\n");
			return DXR_GET_OFFLINE;
		}
	} else {
		if (!read_whole_file(where, &body, &blen)) {
			dxr_log("cannot read catalog file %s\n", where);
			return DXR_GET_OFFLINE;
		}
		if (verify && !read_whole_file(sig_where, &sig, &slen)) {
			free(body);
			dxr_log("cannot read catalog signature %s\n", sig_where);
			return DXR_GET_OFFLINE;
		}
	}
	char key_id[16];
	if (verify) {
		// The root of trust for both schemas: the catalog keys compiled into
		// this build. Everything else a schema-2 catalog says (publishers,
		// their signers) is trusted because this signature covers it.
		bool ok = dxr_catalog_signature_ok(body, blen, sig, key_id);
		free(sig);
		if (!ok) {
			free(body);
			dxr_log("catalog signature INVALID -- refusing it\n");
			return DXR_GET_BAD_SIGNATURE;
		}
	} else {
		(void)snprintf(key_id, sizeof(key_id), "UNVERIFIED");
		dxr_log("WARNING: catalog signature NOT verified (DXR_GET_DEV)\n");
	}
	char err[128];
	int r = dxr_catalog_parse(body, blen, time(NULL), c, err, sizeof(err));
	free(body);
	if (r != DXR_GET_OK) {
		dxr_log("catalog: %s -- treating as offline\n", err);
		return r;
	}
	// Rollback guard: never accept an older serial than one this machine
	// already verified (a replayed old-but-validly-signed catalog). Each schema
	// keeps its own value, so a schema-1 catalog accepted by an older dxr-get
	// on this machine can never make this one refuse a schema-2 catalog.
	if (verify) {
		const char *value = c->schema == 2 ? "CatalogSerialV2" : "CatalogSerial";
		char buf[32] = {0};
		DWORD sz = sizeof(buf);
		long long last = 0;
		if (RegGetValueA(HKEY_LOCAL_MACHINE, SERIAL_KEY, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL,
		                 buf, &sz) == ERROR_SUCCESS) {
			last = _atoi64(buf);
		}
		if (c->serial < last) {
			dxr_log("catalog serial %lld is older than one already seen (%lld) -- refusing it\n", c->serial,
			        last);
			dxr_catalog_free(c);
			return DXR_GET_BAD_SIGNATURE;
		}
		if (c->serial > last) {
			HKEY k;
			if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, SERIAL_KEY, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
			                    NULL, &k, NULL) == ERROR_SUCCESS) {
				(void)snprintf(buf, sizeof(buf), "%lld", c->serial);
				(void)RegSetValueExA(k, value, 0, REG_SZ, (const BYTE *)buf, (DWORD)strlen(buf) + 1);
				RegCloseKey(k);
			}
		}
	}
	dxr_log("catalog ok: schema %d, serial %lld, key %s, expires %s\n", c->schema, c->serial, key_id,
	        jstr(c->doc, "expires"));
	return DXR_GET_OK;
}

/*!
 * The default catalog: schema 2, falling back to schema 1 when schema 2 is
 * not published, unreadable or expired. A schema-2 document whose SIGNATURE
 * or serial is refused does not fall back: that is a refusal, not an absence.
 */
static int
load_default_catalog(bool verify, struct dxr_catalog *c)
{
	bool no_network = false;
	int r = load_catalog(CATALOG_V2, verify, c, &no_network);
	if (r != DXR_GET_OFFLINE || no_network) {
		return r;
	}
	dxr_log("schema-2 catalog not usable -- falling back to schema 1\n");
	return load_catalog(CATALOG_V1, verify, c, &no_network);
}

/*
 * Fetch + verify + install.
 */

/*!
 * Downloads (resuming a partial file), then checks size, SHA-256 and the
 * Authenticode signer of one asset. @p item is the catalog item whose
 * publisher's signers apply.
 */
static int
fetch_asset(const struct dxr_catalog *c,
            const cJSON *item,
            const cJSON *asset,
            const char *name,
            wchar_t *out_path,
            size_t cap,
            const wchar_t *dir_override)
{
	char why[256];
	int r = dxr_asset_check(c, item, asset, NULL, why, sizeof(why));
	if (r != DXR_GET_OK) {
		dxr_log("%s: %s\n", name, why);
		return r;
	}
	const char *sha = jstr(asset, "sha256"), *file = jstr(asset, "file");
	const cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
	const cJSON *urls = cJSON_GetObjectItemCaseSensitive(asset, "urls");

	wchar_t dir[MAX_PATH];
	if (dir_override) {
		(void)_snwprintf(dir, MAX_PATH, L"%ls", dir_override);
		(void)SHCreateDirectoryExW(NULL, dir, NULL);
	} else if (!downloads_dir(dir, MAX_PATH, true)) {
		return DXR_GET_DOWNLOAD;
	}
	// Named by hash, so a resumed or cached file can only ever be these bytes.
	wchar_t part[MAX_PATH];
	(void)_snwprintf(out_path, cap, L"%ls\\%hs-%hs", dir, sha, file);
	(void)_snwprintf(part, MAX_PATH, L"%ls.part", out_path);

	const uint64_t want = (uint64_t)size->valuedouble;
	char got_sha[65] = {0};
	bool have = GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES && dxr_sha256_file(out_path, got_sha) &&
	            _stricmp(got_sha, sha) == 0;
	if (have) {
		dxr_log("%s: using cached %ls\n", name, out_path);
	} else {
		(void)DeleteFileW(out_path);
		const uint64_t resume = size_of_file(part);
		dxr_log("%s: downloading %s %s (%.1f MB)\n", name, file, jstr(asset, "version"), want / 1048576.0);
		if (resume > 0 && resume <= want) {
			dxr_log("%s: resuming at %.1f MB\n", name, resume / 1048576.0);
		}
		bool ok = false;
		const cJSON *u;
		int idx = 0;
		cJSON_ArrayForEach(u, urls)
		{
			if (!cJSON_IsString(u)) {
				continue;
			}
			// The API asset URL (second form) needs the octet-stream Accept to
			// return the bytes instead of JSON. A different URL means a fresh file.
			const bool api = strstr(u->valuestring, "api.github.com") != NULL;
			if (idx++ > 0) {
				dxr_log("  trying mirror %s\n", u->valuestring);
			}
			if (dxr_http_get_file(u->valuestring, part, want, api)) {
				ok = true;
				break;
			}
		}
		if (!ok) {
			dxr_log("%s: download failed\n", name);
			return DXR_GET_DOWNLOAD;
		}
		if (!dxr_sha256_file(part, got_sha) || _stricmp(got_sha, sha) != 0) {
			(void)DeleteFileW(part);
			dxr_log("%s: SHA-256 mismatch (got %s, catalog %s) -- discarded\n", name, got_sha, sha);
			return DXR_GET_VERIFY;
		}
		if (!MoveFileExW(part, out_path, MOVEFILE_REPLACE_EXISTING)) {
			return DXR_GET_DOWNLOAD;
		}
	}

	if (env_on("DXR_GET_ALLOW_UNSIGNED")) {
		dxr_log("%s: WARNING Authenticode check skipped (DXR_GET_ALLOW_UNSIGNED)\n", name);
	} else {
		char cn[256];
		if (!dxr_authenticode_signer(out_path, cn, sizeof(cn))) {
			dxr_log("%s: no valid Authenticode signature -- refusing\n", name);
			return DXR_GET_VERIFY;
		}
		if (dxr_asset_check(c, item, asset, cn, why, sizeof(why)) != DXR_GET_OK) {
			dxr_log("%s: %s -- refusing\n", name, why);
			return DXR_GET_VERIFY;
		}
		dxr_log("%s: verified (sha256 ok, signed by %s)\n", name, cn);
	}
	return DXR_GET_OK;
}

//! Runs @p cmd and waits a bounded time. @p what: "installer" / "uninstaller".
static int
run_child(const char *name, wchar_t *cmd, const char *what)
{
	dxr_log("%s: running %s %ls\n", name, what, cmd);
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		dxr_log("%s: cannot start the %s (error %lu)\n", name, what, GetLastError());
		return DXR_GET_CHILD;
	}
	// NSIS /S installers re-exec themselves from %TEMP% (the uninstaller copy
	// trick) for some paths; waiting on the parent is what NSIS documents as
	// sufficient for a plain installer. The wait is bounded: a child that is
	// still running after an hour is hung, and this tool is run by installers
	// that are themselves being waited for.
	DWORD code = 1;
	if (WaitForSingleObject(pi.hProcess, CHILD_WAIT_MS) != WAIT_OBJECT_0) {
		dxr_log("%s: the %s is still running after %u minutes -- not waiting any longer\n", name, what,
		        CHILD_WAIT_MS / 60000u);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return DXR_GET_CHILD;
	}
	(void)GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (code != 0) {
		dxr_log("%s: %s exited with %lu\n", name, what, code);
		return DXR_GET_CHILD;
	}
	return DXR_GET_OK;
}

static int
run_installer(const char *name, const wchar_t *path, const cJSON *args)
{
	static wchar_t cmd[4096];
	int n = _snwprintf(cmd, 4096, L"\"%ls\"", path);
	const cJSON *a;
	cJSON_ArrayForEach(a, args)
	{
		if (cJSON_IsString(a) && n > 0 && n < 4000) {
			n += _snwprintf(cmd + n, 4096 - n, L" %hs", a->valuestring);
		}
	}
	int r = run_child(name, cmd, "installer");
	if (r == DXR_GET_OK) {
		dxr_log("%s: installed\n", name);
	}
	return r;
}

//! Stops the service and its satellites. Returns true when a running
//! displayxr-service.exe was found, so a failed install can put it back (#2713).
static bool
stop_service(void)
{
	bool was_running = false;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return false;
	}
	PROCESSENTRY32W pe;
	pe.dwSize = sizeof(pe);
	for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
		// The satellites map plug-in DLLs too (the conversion host loads the
		// vendor's conversion DLL), and they are not job-bound to the service
		// (#969), so they can outlive it.
		if (_wcsicmp(pe.szExeFile, L"displayxr-service.exe") != 0 &&
		    _wcsicmp(pe.szExeFile, L"displayxr-conversion-host.exe") != 0 &&
		    _wcsicmp(pe.szExeFile, L"displayxr-tracking-host.exe") != 0) {
			continue;
		}
		if (_wcsicmp(pe.szExeFile, L"displayxr-service.exe") == 0) {
			was_running = true;
		}
		HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
		if (h != NULL) {
			dxr_log("stopping %ls (pid %lu)\n", pe.szExeFile, pe.th32ProcessID);
			(void)TerminateProcess(h, 0);
			(void)WaitForSingleObject(h, 5000);
			CloseHandle(h);
		}
	}
	CloseHandle(snap);
	return was_running;
}

//! Starts the service the way the runtime installer does (#1478): through the
//! user's explorer.exe, so it runs at Medium integrity even though dxr-get is
//! elevated. A High-integrity service would lock every Medium app out of IPC.
static void
start_service(void)
{
	char dir[MAX_PATH];
	if (!dxr_reg_read_path_sz("HKLM\\SOFTWARE\\DisplayXR\\Runtime\\InstallPath", dir, sizeof(dir))) {
		return;
	}
	wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32], win[MAX_PATH];
	(void)_snwprintf(exe, MAX_PATH, L"%hs\\displayxr-service.exe", dir);
	if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
		return;
	}
	(void)GetWindowsDirectoryW(win, MAX_PATH);
	(void)_snwprintf(cmd, MAX_PATH + 32, L"\"%ls\\explorer.exe\" \"%ls\"", win, exe);
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		dxr_log("started DisplayXR service via explorer.exe\n");
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
}

static void
now_utc(char *out, size_t n)
{
	time_t t = time(NULL);
	struct tm *g = gmtime(&t);
	if (g == NULL || strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", g) == 0) {
		out[0] = '\0';
	}
}

//! Records what was just installed, so `list --installed`, `outdated` and `uninstall` have something to read.
static void
write_receipt(const struct dxr_step *s)
{
	struct dxr_receipt r;
	memset(&r, 0, sizeof(r));
	(void)snprintf(r.id, sizeof(r.id), "%s", s->id);
	(void)snprintf(r.name, sizeof(r.name), "%s", s->label);
	(void)snprintf(r.item_class, sizeof(r.item_class), "%s",
	               dxr_item_class(s->item) ? dxr_item_class(s->item) : "");
	(void)snprintf(r.publisher, sizeof(r.publisher), "%s",
	               jstr(s->item, "publisher") ? jstr(s->item, "publisher") : "");
	(void)snprintf(r.version, sizeof(r.version), "%s", s->target ? s->target : "");
	const cJSON *u0 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(s->asset, "urls"), 0);
	(void)snprintf(r.source_url, sizeof(r.source_url), "%s", cJSON_IsString(u0) ? u0->valuestring : "");
	(void)snprintf(r.sha256, sizeof(r.sha256), "%s", jstr(s->asset, "sha256") ? jstr(s->asset, "sha256") : "");
	(void)snprintf(r.file, sizeof(r.file), "%s", jstr(s->asset, "file") ? jstr(s->asset, "file") : "");
	now_utc(r.install_time, sizeof(r.install_time));
	(void)dxr_installed_path(s->item, r.version_path, sizeof(r.version_path));

	// The uninstall command is read NOW, from where the component's installer
	// just wrote it, so `uninstall` needs no catalog (and works offline).
	const cJSON *un = cJSON_GetObjectItemCaseSensitive(s->asset, "uninstall");
	const char *kind = jstr(un, "kind"), *path = jstr(un, "path");
	char cmd[768];
	if (kind != NULL && strcmp(kind, "registry_command") == 0 && path != NULL &&
	    (dxr_reg_read_path_sz(path, cmd, sizeof(cmd)) || dxr_reg_read_path_sz_32(path, cmd, sizeof(cmd))) &&
	    cmd[0] != '\0') {
		size_t l = (size_t)snprintf(r.uninstall, sizeof(r.uninstall), "%s", cmd);
		const cJSON *a;
		cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(un, "args"))
		{
			if (cJSON_IsString(a) && l < sizeof(r.uninstall)) {
				l += (size_t)snprintf(r.uninstall + l, sizeof(r.uninstall) - l, " %s", a->valuestring);
			}
		}
	}
	const cJSON *q;
	cJSON_ArrayForEach(q, cJSON_GetObjectItemCaseSensitive(s->item, "requires"))
	{
		const char *qk = jstr(q, "kind");
		const char *dep = NULL;
		if (qk != NULL && strcmp(qk, "item") == 0) {
			dep = jstr(q, "id");
		} else if (qk != NULL && strcmp(qk, "runtime") == 0) {
			dep = "runtime";
		}
		size_t l = strlen(r.requires_ids);
		if (dep != NULL && l + strlen(dep) + 2 < sizeof(r.requires_ids)) {
			(void)snprintf(r.requires_ids + l, sizeof(r.requires_ids) - l, "%s%s", l ? "," : "", dep);
		}
	}
	if (!receipt_write(&r)) {
		dxr_log("%s: installed, but the install receipt could not be written\n", s->label);
	} else if (r.uninstall[0] == '\0') {
		dxr_log("%s: receipt written (the catalog gives no uninstall command for it)\n", s->label);
	}
}

static bool
step_stops_service(const struct dxr_step *s)
{
	return s->kind == DXR_STEP_EXTERNAL || dxr_class_is_loaded(dxr_item_class(s->item));
}

/*!
 * Runs a plan: download and verify EVERYTHING first, then stop the service
 * once, run the installers in plan order (prerequisites first), write a
 * receipt per item, start the service once.
 */
static int
execute_plan(const struct dxr_catalog *c, struct dxr_plan *p, bool no_start, const char *retry_hint)
{
	static wchar_t paths[DXR_MAX_STEPS][MAX_PATH];
	// A network failure halfway must not leave a new runtime next to an
	// ABI-stale plug-in, or an item without the prerequisite it needs.
	for (int i = 0; i < p->n; i++) {
		struct dxr_step *s = &p->steps[i];
		if (!dxr_step_needs_work(s)) {
			continue;
		}
		int r = fetch_asset(c, s->item, s->asset, s->label, paths[i], MAX_PATH, NULL);
		if (r != DXR_GET_OK) {
			dxr_log("Nothing was installed. Not installed:");
			for (int j = 0; j < p->n; j++) {
				if (dxr_step_needs_work(&p->steps[j])) {
					dxr_log(" %s%s", p->steps[j].label, j + 1 < p->n ? ";" : "");
				}
			}
			dxr_log("\n");
			if (r == DXR_GET_DOWNLOAD) {
				dxr_log(
				    "The download could not be completed (no connection, or the server is "
				    "unreachable). "
				    "What was fetched is kept and resumes next time: %s\n",
				    retry_hint);
			}
			return r;
		}
	}
	// Same order as the bundle (#461/#342): nothing may hold the runtime's or a
	// plug-in's DLLs while their installers replace them -- an NSIS /S install
	// skips a locked file silently -- and the service starts once, at the end,
	// at the user's integrity level. An application-only plan leaves it alone.
	bool touches_service = false;
	for (int i = 0; i < p->n; i++) {
		touches_service =
		    touches_service || (dxr_step_needs_work(&p->steps[i]) && step_stops_service(&p->steps[i]));
	}
	bool was_running = touches_service ? stop_service() : false;
	int r = DXR_GET_OK;
	for (int i = 0; i < p->n && r == DXR_GET_OK; i++) {
		struct dxr_step *s = &p->steps[i];
		if (!dxr_step_needs_work(s)) {
			continue;
		}
		const cJSON *ins = cJSON_GetObjectItemCaseSensitive(s->asset, "installer");
		r = run_installer(s->label, paths[i], cJSON_GetObjectItemCaseSensitive(ins, "args"));
		if (r == DXR_GET_OK && s->kind == DXR_STEP_ITEM) {
			write_receipt(s);
		}
	}
	// #2713: a failed child install (exit 6: a program holds the client DLL)
	// must leave the machine as it was found -- restart the service only if it
	// was running before. A successful install starts it as always. The service
	// is single-instance, so racing the installer's own restore-on-abort is
	// harmless.
	if (touches_service && !no_start && (r == DXR_GET_OK || was_running)) {
		start_service();
	}
	return r;
}

/*
 * Offline: say what was not done and how to get it later. Never a hang: the
 * catalog fetch that led here is bounded (timeouts, four attempts).
 */
static void
report_offline(const char *cmd, const char *target, bool json)
{
	char hint[256];
	if (target != NULL && strcmp(target, "--default") == 0) {
		(void)snprintf(hint, sizeof(hint), "dxr-get install --default");
	} else if (target != NULL) {
		(void)snprintf(hint, sizeof(hint), "dxr-get %s %s", strcmp(cmd, "plan") == 0 ? "ensure-for" : cmd,
		               target);
	} else {
		(void)snprintf(hint, sizeof(hint), "dxr-get %s", cmd);
	}
	dxr_log("OFFLINE: the DisplayXR catalog could not be reached (or it has expired).\n");
	if (target != NULL) {
		dxr_log("Skipped: %s%s. Nothing was downloaded, installed or changed.\n", target,
		        strcmp(cmd, "install") == 0 ? "" : " and whatever it needs");
	} else {
		dxr_log("Skipped: %s. Nothing was changed.\n", cmd);
	}
	dxr_log("What is already installed keeps working. To get it later, connect and run: %s\n", hint);
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "offline", 1);
		cJSON_AddStringToObject(o, "reason", "catalog unreachable or expired");
		cJSON *sk = cJSON_AddArrayToObject(o, "skipped");
		cJSON_AddItemToArray(sk, cJSON_CreateString(target ? target : cmd));
		cJSON_AddStringToObject(o, "retry", hint);
		print_json(o);
	}
}

/*
 * Verbs.
 */

static void
usage(void)
{
	dxr_log(
	    "dxr-get -- fetch, verify, install and remove DisplayXR components from the signed catalog\n"
	    "\n"
	    "  dxr-get [--catalog <url|file>] plan --for <item> | --default [--min-runtime X.Y.Z] [--json]\n"
	    "  dxr-get [--catalog <url|file>] ensure-for <item> [--min-runtime X.Y.Z] [--yes] [--no-start-service]\n"
	    "  dxr-get [--catalog <url|file>] fetch <item> [--out <dir>]\n"
	    "  dxr-get [--catalog <url|file>] install <item> | --default [--yes] [--json] [--no-start-service]\n"
	    "  dxr-get [--catalog <url|file>] detect [--json]\n"
	    "  dxr-get [--catalog <url|file>] catalog\n"
	    "  dxr-get [--catalog <url|file>] list [--json]\n"
	    "  dxr-get [--catalog <url|file>] outdated [--json]\n"
	    "  dxr-get list --installed [--json]\n"
	    "  dxr-get uninstall <id> [--json] [--no-start-service]\n"
	    "\n"
	    "  --default            the catalog's recommended items plus high-confidence hardware matches\n"
	    "  --no-runtime         (plan, ensure-for, install, list) never install or upgrade the runtime\n"
	    "                       itself; an item that would need it is refused with exit 41\n"
	    "  --assume-runtime V   (list, plan) answer as if runtime V were installed\n"
	    "  --assume-abi C=N     with --assume-runtime: that runtime implements major N of class C\n"
	    "\n"
	    "items: the ids `dxr-get list` shows (the schema-1 names runtime, leia_plugin, browser still work)\n"
	    "exit: 0 ok, 2 usage, 10 offline/expired catalog, 11 bad signature, 20 download,\n"
	    "      21 verification, 30 installer failed, 40 not fetchable, 41 not compatible with the\n"
	    "      installed runtime, 42 not installed, 43 cannot uninstall, 50 needs elevation\n");
}

static int
cmd_detect(const struct dxr_catalog *c, bool json)
{
	struct dxr_edid_id ids[MAX_EDIDS];
	int n = env_edids(NULL, ids, MAX_EDIDS);
	cJSON *o = cJSON_CreateObject();
	cJSON *e = cJSON_AddArrayToObject(o, "edids");
	for (int i = 0; i < n; i++) {
		cJSON *x = cJSON_CreateObject();
		cJSON_AddNumberToObject(x, "man", ids[i].man);
		cJSON_AddNumberToObject(x, "prod", ids[i].prod);
		cJSON_AddItemToArray(e, x);
		if (!json) {
			printf("monitor EDID man=%u (0x%04X) prod=%u (0x%04X)\n", ids[i].man, ids[i].man, ids[i].prod,
			       ids[i].prod);
		}
	}
	// Present USB devices: the evidence the `usb` rule kind reads (#2737).
	struct dxr_usb_id usb[MAX_USB];
	int n_usb = env_usb(NULL, usb, MAX_USB);
	cJSON *u = cJSON_AddArrayToObject(o, "usb");
	for (int i = 0; i < n_usb; i++) {
		cJSON *x = cJSON_CreateObject();
		cJSON_AddNumberToObject(x, "vid", usb[i].vid);
		cJSON_AddNumberToObject(x, "pid", usb[i].pid);
		cJSON_AddItemToArray(u, x);
		if (!json) {
			printf("USB device %04X:%04X\n", usb[i].vid, usb[i].pid);
		}
	}
	// One entry per item that has detect rules: in the `display_plugins` list
	// (#2737), and -- for one release -- also as an object keyed by the name a
	// schema-1 caller knows it by (so `leia_plugin` keeps its key).
	cJSON *dps = cJSON_AddArrayToObject(o, "display_plugins");
	const cJSON *it;
	cJSON_ArrayForEach(it, c->items)
	{
		const cJSON *rules = cJSON_GetObjectItemCaseSensitive(it, "detect");
		if (!cJSON_IsArray(rules) || dxr_item_find(c, dxr_item_id(it)) != it) {
			continue;
		}
		char why_hi[256], why_any[256];
		bool hi = dxr_detect_rules(rules, &k_env, true, why_hi, sizeof(why_hi));
		bool any = dxr_detect_rules(rules, &k_env, false, why_any, sizeof(why_any));
		const char *name = dxr_item_legacy_name(it);
		cJSON *l = cJSON_AddObjectToObject(o, name);
		cJSON_AddBoolToObject(l, "match", any);
		cJSON_AddBoolToObject(l, "high_confidence", hi);
		cJSON_AddStringToObject(l, "evidence", hi ? why_hi : why_any);
		cJSON_AddStringToObject(l, "id", dxr_item_id(it));
		cJSON *d = cJSON_CreateObject();
		cJSON_AddStringToObject(d, "id", dxr_item_id(it));
		cJSON_AddStringToObject(d, "component", name);
		cJSON_AddStringToObject(d, "label", dxr_item_label(it));
		cJSON_AddBoolToObject(d, "match", any);
		cJSON_AddBoolToObject(d, "high_confidence", hi);
		cJSON_AddStringToObject(d, "evidence", hi ? why_hi : why_any);
		cJSON_AddItemToArray(dps, d);
		if (!json) {
			printf("%s: %s%s%s\n", name,
			       any ? (hi ? "MATCH (high confidence)" : "match (low confidence)") : "no match",
			       any ? " -- " : "", hi ? why_hi : why_any);
		}
	}
	if (json) {
		print_json(o);
	} else {
		cJSON_Delete(o);
		(void)fflush(stdout);
	}
	return DXR_GET_OK;
}

static void
print_table(const cJSON *arr, const char *ver_key, const char *avail_key, const char *empty)
{
	const cJSON *x;
	int rows = 0;
	cJSON_ArrayForEach(x, arr)
	{
		const char *avail = avail_key ? jstr(x, avail_key) : NULL;
		const char *state = jstr(x, "state");
		if (rows++ == 0) {
			printf("%-28s %-22s %-12s %-12s %s\n", "ID", "CLASS", "INSTALLED", avail_key ? "AVAILABLE" : "",
			       avail_key ? "STATE" : "INSTALLED AT");
		}
		printf("%-28s %-22s %-12s %-12s %s\n", jstr(x, "id"), jstr(x, "class") ? jstr(x, "class") : "",
		       jstr(x, ver_key) ? jstr(x, ver_key) : "-", avail ? avail : (avail_key ? "-" : ""),
		       avail_key ? (state ? state : "") : (jstr(x, "install_time") ? jstr(x, "install_time") : ""));
	}
	if (rows == 0) {
		printf("%s\n", empty);
	}
	(void)fflush(stdout);
}

static int
cmd_list_installed(bool json)
{
	static struct dxr_receipt all[MAX_RECEIPTS];
	int n = receipts_all(all, MAX_RECEIPTS);
	cJSON *arr = dxr_list_receipts(all, n);
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddItemToObject(o, "installed", arr);
		print_json(o);
	} else {
		print_table(arr, "version", NULL,
		            "Nothing was installed through dxr-get on this machine. (Components installed by their own "
		            "installers have no receipt; `dxr-get list` shows them.)");
		cJSON_Delete(arr);
	}
	return DXR_GET_OK;
}

static int
cmd_list(const struct dxr_catalog *c, bool outdated, bool json, bool fixed_runtime)
{
	cJSON *arr = dxr_list_catalog(c, &k_env, outdated, fixed_runtime);
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "schema", c->schema);
		cJSON_AddItemToObject(o, outdated ? "outdated" : "items", arr);
		print_json(o);
	} else {
		print_table(arr, "installed", "available",
		            outdated ? "Everything installed is at the catalog's version."
		                     : "The catalog lists no items.");
		cJSON_Delete(arr);
	}
	return DXR_GET_OK;
}

static int
uninstall_result(const char *id, int code, const char *result, const char *detail, bool json)
{
	dxr_log("%s: %s%s%s\n", id, result, detail[0] ? " -- " : "", detail);
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "id", id);
		cJSON_AddStringToObject(o, "result", result);
		cJSON_AddNumberToObject(o, "exit", code);
		if (detail[0]) {
			cJSON_AddStringToObject(o, "detail", detail);
		}
		print_json(o);
	}
	return code;
}

//! `uninstall <id>`: needs no catalog and no network. Everything comes from the receipt.
static int
cmd_uninstall(const char *id, bool json, bool no_start)
{
	static struct dxr_receipt all[MAX_RECEIPTS];
	struct dxr_receipt r;
	char detail[700];
	const char *result = "";
	const bool have = receipt_read(id, &r);
	int n = receipts_all(all, MAX_RECEIPTS);
	int check = dxr_uninstall_check(have ? &r : NULL, all, n, is_admin(), &result, detail, sizeof(detail));
	if (check != DXR_GET_OK) {
		return uninstall_result(id, check, result, detail, json);
	}
	const bool loaded = dxr_class_is_loaded(r.item_class);
	const bool is_runtime = strcmp(r.item_class, "runtime") == 0;
	bool was_running = loaded ? stop_service() : false;
	static wchar_t cmd[2048];
	(void)_snwprintf(cmd, 2048, L"%hs", r.uninstall);
	int rc = run_child(r.name[0] ? r.name : id, cmd, "uninstaller");
	bool gone = rc == DXR_GET_OK;
	if (rc == DXR_GET_OK && r.version_path[0] != '\0') {
		// An NSIS uninstaller copies itself to %TEMP% and returns at once. The
		// component is gone when its version value is; wait a bounded time.
		char v[64];
		for (DWORD waited = 0;; waited += 500) {
			gone = !dxr_reg_read_path_sz(r.version_path, v, sizeof(v)) || v[0] == '\0';
			if (gone || waited >= UNINSTALL_SETTLE_MS) {
				break;
			}
			Sleep(500);
		}
	}
	if (loaded && !is_runtime && !no_start && (was_running || gone)) {
		start_service();
	}
	if (rc != DXR_GET_OK) {
		return uninstall_result(id, DXR_GET_CHILD, "failed", "the uninstaller did not finish successfully",
		                        json);
	}
	if (!gone) {
		return uninstall_result(
		    id, DXR_GET_CHILD, "failed",
		    "the uninstaller returned, but the component still reports an installed version; "
		    "the receipt is kept",
		    json);
	}
	receipt_delete(id);
	(void)snprintf(detail, sizeof(detail), "%s %s removed", r.name[0] ? r.name : id, r.version);
	return uninstall_result(id, DXR_GET_OK, "uninstalled", detail, json);
}

int
main(int argc, char **argv)
{
	// DXR_GET_CATALOG redirects to another catalog (a test or staging copy). It
	// grants nothing: that catalog must still carry a valid signature from a
	// compiled-in key, and the serial guard still applies.
	const char *catalog_where = getenv("DXR_GET_CATALOG");
	const char *cmd = NULL, *arg = NULL, *for_c = NULL, *min_runtime = NULL, *out_dir = NULL;
	bool json = false, yes = false, no_verify = false, no_start = false, installed_only = false;
	bool use_default = false;
	struct dxr_plan_opts opts = {false};
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--catalog") == 0 && i + 1 < argc) {
			catalog_where = argv[++i];
		} else if (strcmp(a, "--no-verify-catalog") == 0) {
			no_verify = true;
		} else if (strcmp(a, "--for") == 0 && i + 1 < argc) {
			for_c = argv[++i];
		} else if (strcmp(a, "--min-runtime") == 0 && i + 1 < argc) {
			min_runtime = argv[++i];
		} else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
			out_dir = argv[++i];
		} else if (strcmp(a, "--json") == 0) {
			json = true;
		} else if (strcmp(a, "--yes") == 0) {
			yes = true;
		} else if (strcmp(a, "--installed") == 0) {
			installed_only = true;
		} else if (strcmp(a, "--default") == 0) {
			use_default = true;
		} else if (strcmp(a, "--no-runtime") == 0) {
			opts.no_runtime = true;
		} else if (strcmp(a, "--assume-runtime") == 0 && i + 1 < argc) {
			k_env.assume_runtime = argv[++i];
		} else if (strcmp(a, "--assume-abi") == 0 && i + 1 < argc) {
			// <class>=<major>, e.g. display_processor=5. argv outlives the run,
			// so the class name is cut in place.
			char *eq = strchr(argv[++i], '=');
			if (eq == NULL || eq == argv[i] || k_env.n_assume_abi >= 8) {
				usage();
				return DXR_GET_USAGE;
			}
			*eq = '\0';
			k_env.assume_abi[k_env.n_assume_abi].item_class = argv[i];
			k_env.assume_abi[k_env.n_assume_abi].major = atoi(eq + 1);
			k_env.n_assume_abi++;
		} else if (strcmp(a, "--no-start-service") == 0) {
			no_start = true;
		} else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
			usage();
			return DXR_GET_OK;
		} else if (a[0] == '-') {
			usage();
			return DXR_GET_USAGE;
		} else if (cmd == NULL) {
			cmd = a;
		} else if (arg == NULL) {
			arg = a;
		} else {
			usage();
			return DXR_GET_USAGE;
		}
	}
	// A build whose catalog root key was replaced for testing says so on every
	// run, before anything else, and reads a local catalog file only.
	const char *test_banner = dxr_test_root_key_banner();
	if (test_banner != NULL) {
		dxr_log(
		    "\n*** %s ***\n*** This dxr-get trusts a throwaway key instead of the DisplayXR catalog keys. "
		    "***\n\n",
		    test_banner);
	}
	if (cmd == NULL) {
		usage();
		return DXR_GET_USAGE;
	}
	if (no_verify && !env_on("DXR_GET_DEV")) {
		dxr_log("--no-verify-catalog needs DXR_GET_DEV=1\n");
		return DXR_GET_USAGE;
	}
	(void)yes; // installers pass --yes; the consent prompt is theirs, not ours
	if (strcmp(cmd, "ensure-for") == 0) {
		for_c = arg;
	}

	// The two verbs that work from receipts alone: no catalog, no network.
	if (strcmp(cmd, "uninstall") == 0) {
		if (arg == NULL) {
			usage();
			return DXR_GET_USAGE;
		}
		return cmd_uninstall(arg, json, no_start);
	}
	if (strcmp(cmd, "list") == 0 && installed_only) {
		return cmd_list_installed(json);
	}
	static const char *const known[] = {"plan",   "ensure-for", "fetch", "install",
	                                    "detect", "catalog",    "list",  "outdated"};
	bool is_known = false;
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		is_known = is_known || strcmp(cmd, known[i]) == 0;
	}
	const bool plan_verb = strcmp(cmd, "plan") == 0 || strcmp(cmd, "ensure-for") == 0;
	const bool item_verb = strcmp(cmd, "fetch") == 0 || strcmp(cmd, "install") == 0;
	// --default stands in for the item, on `plan` and `install` only.
	const bool default_ok = strcmp(cmd, "plan") == 0 || strcmp(cmd, "install") == 0;
	if (!is_known || (use_default && (!default_ok || for_c != NULL || arg != NULL)) ||
	    (!use_default && ((plan_verb && for_c == NULL) || (item_verb && arg == NULL)))) {
		usage();
		return DXR_GET_USAGE;
	}
	if (k_env.n_assume_abi > 0 && k_env.assume_runtime == NULL) {
		dxr_log("--assume-abi needs --assume-runtime\n");
		return DXR_GET_USAGE;
	}
	const char *target = use_default ? "--default" : (plan_verb ? for_c : arg);

	if (test_banner != NULL && (catalog_where == NULL || _strnicmp(catalog_where, "http", 4) == 0)) {
		// It cannot verify the real catalog (it does not hold its keys), and it
		// must never be pointed at one by accident.
		dxr_log("this build reads a local test catalog only: pass --catalog <file>\n");
		return DXR_GET_USAGE;
	}
	struct dxr_catalog c;
	bool no_network = false;
	int r = catalog_where != NULL ? load_catalog(catalog_where, !no_verify, &c, &no_network)
	                              : load_default_catalog(!no_verify, &c);
	if (r != DXR_GET_OK) {
		if (r == DXR_GET_OFFLINE) {
			report_offline(cmd, target, json);
		}
		return r;
	}

	if (strcmp(cmd, "catalog") == 0) {
		char *s = cJSON_Print(c.doc);
		printf("%s\n", s);
		cJSON_free(s);
		r = DXR_GET_OK;
	} else if (strcmp(cmd, "detect") == 0) {
		r = cmd_detect(&c, json);
	} else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "outdated") == 0) {
		// A runtime that is assumed, or must not be touched, is a fixed one.
		r = cmd_list(&c, strcmp(cmd, "outdated") == 0, json, opts.no_runtime || k_env.assume_runtime != NULL);
	} else if (plan_verb) {
		static struct dxr_plan p;
		char hint[200];
		if (use_default) {
			dxr_make_default_plan(&c, &k_env, &opts, &p);
		} else {
			dxr_make_plan(&c, &k_env, for_c, min_runtime, DXR_PLAN_ENSURE, &opts, &p);
		}
		if (strcmp(cmd, "plan") == 0) {
			if (json) {
				char *s = dxr_plan_json(&p);
				printf("%s\n", s);
				cJSON_free(s);
			} else {
				dxr_plan_text(&p, stdout);
			}
			// `plan` reports; only a range/ABI refusal changes its exit code,
			// so a caller can tell the user before it asks for consent.
			r = p.gate == DXR_GET_INCOMPATIBLE ? DXR_GET_INCOMPATIBLE : DXR_GET_OK;
			goto done;
		}
		// ensure-for
		dxr_plan_text(&p, stdout);
		if (p.gate == DXR_GET_INCOMPATIBLE) {
			dxr_log("not compatible: %s\n", p.gate_reason);
			r = DXR_GET_INCOMPATIBLE;
			goto done;
		}
		if (p.todo > 0 && !is_admin()) {
			dxr_log("installing needs an elevated process\n");
			r = DXR_GET_NEEDS_ADMIN;
			goto done;
		}
		if (p.gate == DXR_GET_NOT_FETCHABLE) {
			// What the caller needs can't be had: say so, and don't install a
			// plug-in for a runtime that isn't there.
			dxr_log("not fetchable: %s\n", p.gate_reason);
			r = DXR_GET_NOT_FETCHABLE;
			goto done;
		}
		if (p.todo == 0) {
			r = DXR_GET_OK;
			goto done;
		}
		(void)snprintf(hint, sizeof(hint), "dxr-get ensure-for %s", for_c);
		r = execute_plan(&c, &p, no_start, hint);
	} else if (strcmp(cmd, "install") == 0) {
		static struct dxr_plan p;
		char hint[200];
		const char *what = use_default ? "the default set" : arg;
		if (use_default) {
			// The recommended set plus high-confidence hardware matches. An
			// item of the set that cannot be installed here is listed as
			// skipped in the plan and does not fail the rest.
			dxr_make_default_plan(&c, &k_env, &opts, &p);
		} else {
			dxr_make_plan(&c, &k_env, arg, min_runtime, DXR_PLAN_INSTALL, &opts, &p);
		}
		if (json) {
			char *js = dxr_plan_json(&p);
			printf("%s\n", js);
			(void)fflush(stdout);
			cJSON_free(js);
		} else {
			dxr_plan_text(&p, stdout);
		}
		if (p.gate == DXR_GET_INCOMPATIBLE) {
			dxr_log("%s: not compatible -- %s\n", what, p.gate_reason);
			r = DXR_GET_INCOMPATIBLE;
			goto done;
		}
		if (p.todo == 0 && p.gate == DXR_GET_OK) {
			for (int i = 0; i < p.n; i++) {
				if (p.steps[i].is_target && p.steps[i].state == DXR_ST_OK) {
					dxr_log(
					    "%s: %s installed, catalog has %s -- nothing to do (never downgrades)\n",
					    p.steps[i].component, p.steps[i].installed,
					    p.steps[i].target ? p.steps[i].target : "no build");
				}
			}
			r = DXR_GET_OK;
			goto done;
		}
		if (!is_admin()) {
			dxr_log("installing needs an elevated process\n");
			r = DXR_GET_NEEDS_ADMIN;
			goto done;
		}
		if (p.gate == DXR_GET_NOT_FETCHABLE) {
			dxr_log("%s: nothing fetchable in the catalog -- %s\n", what, p.gate_reason);
			r = DXR_GET_NOT_FETCHABLE;
			goto done;
		}
		(void)snprintf(hint, sizeof(hint), "dxr-get install %s", use_default ? "--default" : arg);
		r = execute_plan(&c, &p, no_start, hint);
	} else { // fetch
		const cJSON *item = dxr_item_find(&c, arg);
		if (item == NULL) {
			dxr_log("%s: nothing fetchable in the catalog\n", arg);
			r = DXR_GET_NOT_FETCHABLE;
			goto done;
		}
		wchar_t wout[MAX_PATH], path[MAX_PATH];
		if (out_dir) {
			(void)MultiByteToWideChar(CP_UTF8, 0, out_dir, -1, wout, MAX_PATH);
		}
		r = fetch_asset(&c, item, dxr_item_asset(item), arg, path, MAX_PATH, out_dir ? wout : NULL);
		if (r == DXR_GET_OK) {
			printf("%ls\n", path);
		}
	}
done:
	dxr_catalog_free(&c);
	return r;
}
