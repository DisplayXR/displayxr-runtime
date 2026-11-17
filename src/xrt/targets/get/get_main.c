// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  dxr-get: plan / fetch / install DisplayXR components from the
 *         signed component catalog. Epic #2702; P1 #2704, P2 #2705.
 *
 * Rules that matter more than the code (docs/specs/runtime/dxr-get.md):
 * - Fail closed: a payload installs only if its size, SHA-256 and
 *   Authenticode signer all match the signed catalog, and the signer is in
 *   the compiled-in list too.
 * - A stale or unreachable catalog is "offline", never an authority: it can
 *   only ever cause dxr-get to do nothing.
 * - Never downgrade, never touch ActiveRuntime, never write Trust\Allowlist.
 * - The display plug-in installs only on positive evidence (a high-confidence
 *   catalog detect rule matching this machine).
 */

#include "get.h"

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

#include "cjson/cJSON.h"

#define DEFAULT_CATALOG "https://updates.displayxr.org/catalog/v1/stable.json"
#define SERIAL_KEY "SOFTWARE\\DisplayXR\\Update"

static const char *const k_trusted_signers[] = {"Leia, Inc."};

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

/*
 * Versions: dotted numeric, up to 4 parts; anything after '-' or '+' ignored.
 */
static int
vercmp(const char *a, const char *b)
{
	for (int i = 0; i < 4; i++) {
		unsigned long x = 0, y = 0;
		if (a && *a >= '0' && *a <= '9') {
			x = strtoul(a, (char **)&a, 10);
		}
		if (b && *b >= '0' && *b <= '9') {
			y = strtoul(b, (char **)&b, 10);
		}
		if (x != y) {
			return x < y ? -1 : 1;
		}
		if (a && *a == '.') {
			a++;
		}
		if (b && *b == '.') {
			b++;
		}
	}
	return 0;
}

//! ">=2.62.0" -> "2.62.0"; other operators are not in schema 1.
static const char *
floor_of(const char *req)
{
	if (req == NULL) {
		return NULL;
	}
	if (strncmp(req, ">=", 2) == 0) {
		req += 2;
	}
	while (*req == ' ') {
		req++;
	}
	return *req ? req : NULL;
}

static const char *
jstr(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

/*
 * Catalog.
 */

struct catalog
{
	cJSON *root;
	cJSON *components;
	long long serial;
	char key_id[9];
};

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

static time_t
parse_utc(const char *s)
{
	struct tm t;
	memset(&t, 0, sizeof(t));
	if (s == NULL ||
	    sscanf(s, "%d-%d-%dT%d:%d:%dZ", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) {
		return 0;
	}
	t.tm_year -= 1900;
	t.tm_mon -= 1;
	return _mkgmtime(&t);
}

static int
load_catalog(const char *where, bool verify, struct catalog *c)
{
	memset(c, 0, sizeof(*c));
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
	if (verify) {
		bool ok = dxr_catalog_signature_ok(body, blen, sig, c->key_id);
		free(sig);
		if (!ok) {
			free(body);
			dxr_log("catalog signature INVALID -- refusing it\n");
			return DXR_GET_BAD_SIGNATURE;
		}
	} else {
		(void)snprintf(c->key_id, sizeof(c->key_id), "UNVERIFIED");
		dxr_log("WARNING: catalog signature NOT verified (DXR_GET_DEV)\n");
	}
	c->root = cJSON_ParseWithLength(body, blen);
	free(body);
	const cJSON *schema = c->root ? cJSON_GetObjectItemCaseSensitive(c->root, "schema") : NULL;
	if (!cJSON_IsNumber(schema) || schema->valueint != 1) {
		dxr_log("catalog: unsupported schema -- treating as offline\n");
		cJSON_Delete(c->root);
		c->root = NULL;
		return DXR_GET_OFFLINE;
	}
	const cJSON *serial = cJSON_GetObjectItemCaseSensitive(c->root, "serial");
	c->serial = cJSON_IsNumber(serial) ? (long long)serial->valuedouble : 0;
	c->components = cJSON_GetObjectItemCaseSensitive(c->root, "components");

	time_t exp = parse_utc(jstr(c->root, "expires"));
	if (exp == 0 || time(NULL) > exp) {
		dxr_log("catalog expired (%s) -- treating as offline\n", jstr(c->root, "expires"));
		cJSON_Delete(c->root);
		c->root = NULL;
		return DXR_GET_OFFLINE;
	}
	// Rollback guard: never accept an older serial than one this machine
	// already verified (a replayed old-but-validly-signed catalog).
	if (verify) {
		char buf[32] = {0};
		DWORD sz = sizeof(buf);
		long long last = 0;
		if (RegGetValueA(HKEY_LOCAL_MACHINE, SERIAL_KEY, "CatalogSerial", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
		                 NULL, buf, &sz) == ERROR_SUCCESS) {
			last = _atoi64(buf);
		}
		if (c->serial < last) {
			dxr_log("catalog serial %lld is older than one already seen (%lld) -- refusing it\n", c->serial,
			        last);
			cJSON_Delete(c->root);
			c->root = NULL;
			return DXR_GET_BAD_SIGNATURE;
		}
		if (c->serial > last) {
			HKEY k;
			if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, SERIAL_KEY, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
			                    NULL, &k, NULL) == ERROR_SUCCESS) {
				(void)snprintf(buf, sizeof(buf), "%lld", c->serial);
				(void)RegSetValueExA(k, "CatalogSerial", 0, REG_SZ, (const BYTE *)buf,
				                     (DWORD)strlen(buf) + 1);
				RegCloseKey(k);
			}
		}
	}
	dxr_log("catalog ok: serial %lld, key %s, expires %s\n", c->serial, c->key_id, jstr(c->root, "expires"));
	return DXR_GET_OK;
}

static const cJSON *
component(const struct catalog *c, const char *name)
{
	return c->components ? cJSON_GetObjectItemCaseSensitive(c->components, name) : NULL;
}

static const cJSON *
win_block(const struct catalog *c, const char *name)
{
	const cJSON *comp = component(c, name);
	const cJSON *w = comp ? cJSON_GetObjectItemCaseSensitive(comp, "windows") : NULL;
	return cJSON_IsObject(w) ? w : NULL;
}

/*
 * Installed state. The registry path comes from the catalog when it names one,
 * else from the compiled-in defaults (they are the contract with today's
 * installers; the catalog's copy lets a future installer move the key).
 */
static const char *
default_reg_version(const char *name)
{
	if (strcmp(name, "runtime") == 0) {
		return "HKLM\\SOFTWARE\\DisplayXR\\Runtime\\Version";
	}
	if (strcmp(name, "leia_plugin") == 0) {
		return "HKLM\\SOFTWARE\\DisplayXR\\Plugins\\LeiaSR\\Version";
	}
	if (strcmp(name, "browser") == 0) {
		return "HKLM\\SOFTWARE\\DisplayXR\\Browser\\Version";
	}
	return NULL;
}

static bool
installed_version(const struct catalog *c, const char *name, char *out, size_t out_size)
{
	const cJSON *w = win_block(c, name);
	const char *reg = w ? jstr(w, "registry_version") : NULL;
	if (reg == NULL) {
		reg = default_reg_version(name);
	}
	return reg != NULL && dxr_reg_read_path_sz(reg, out, out_size) && out[0] != '\0';
}

/*
 * Detection (P2): catalog rules against this machine's evidence.
 */
static bool
detect_match(const cJSON *comp, bool high_only, char *why, size_t why_size)
{
	const cJSON *rules = cJSON_GetObjectItemCaseSensitive(comp, "detect");
	if (!cJSON_IsArray(rules)) {
		return false;
	}
	struct dxr_edid_id ids[32];
	int n = dxr_detect_edids(ids, 32);
	const cJSON *r;
	cJSON_ArrayForEach(r, rules)
	{
		const char *kind = jstr(r, "kind");
		const char *conf = jstr(r, "confidence");
		const bool high = conf != NULL && strcmp(conf, "high") == 0;
		if (kind == NULL || (high_only && !high)) {
			continue;
		}
		if (strcmp(kind, "edid") == 0) {
			const cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "man");
			const cJSON *p = cJSON_GetObjectItemCaseSensitive(r, "prod");
			if (!cJSON_IsNumber(m) || !cJSON_IsNumber(p)) {
				continue;
			}
			for (int i = 0; i < n; i++) {
				if (ids[i].man == (uint16_t)m->valueint && ids[i].prod == (uint16_t)p->valueint) {
					const char *note = jstr(r, "note");
					(void)snprintf(why, why_size, "display EDID %u/%u%s%s (%s confidence)",
					               ids[i].man, ids[i].prod, note ? " " : "", note ? note : "",
					               conf ? conf : "?");
					return true;
				}
			}
		} else if (strcmp(kind, "registry_dword_min") == 0) {
			const char *key = jstr(r, "key"), *val = jstr(r, "value");
			const cJSON *min = cJSON_GetObjectItemCaseSensitive(r, "min");
			uint32_t v = 0;
			if (key && val && cJSON_IsNumber(min) && dxr_reg_read_dword(key, val, &v) &&
			    v >= (uint32_t)min->valueint) {
				const char *note = jstr(r, "note");
				(void)snprintf(why, why_size, "%s (%s confidence)", note ? note : val,
				               conf ? conf : "?");
				return true;
			}
		}
	}
	return false;
}

/*
 * Plan.
 */

enum state
{
	ST_OK,
	ST_MISSING,
	ST_TOO_OLD,
	ST_NOT_FETCHABLE,
};
static const char *const k_state_names[] = {"ok", "missing", "too_old", "not_fetchable"};

struct action
{
	const char *component;
	const char *label;
	char installed[64]; //!< "" when absent
	enum state state;
	char required[64]; //!< floor, "" when none
	const char *target;
	double size;
	char reason[256];
};

struct plan
{
	const char *for_component;
	struct action a[4];
	int n;
};

static const char *
label_of(const char *name)
{
	if (strcmp(name, "runtime") == 0) {
		return "DisplayXR runtime";
	}
	if (strcmp(name, "leia_plugin") == 0) {
		return "Leia display plug-in";
	}
	if (strcmp(name, "browser") == 0) {
		return "DisplayXR Browser";
	}
	return name;
}

//! Fills @p a for @p name given a floor (may be NULL: then only presence matters). @p want_latest: also act
//! when installed is older than the catalog's version (used for the display plug-in, whose pins are an
//! ABI-matched set with the runtime).
static void
plan_one(const struct catalog *c, const char *name, const char *floor, bool want_latest, struct action *a)
{
	memset(a, 0, sizeof(*a));
	a->component = name;
	a->label = label_of(name);
	const cJSON *w = win_block(c, name);
	const char *target = w ? jstr(w, "version") : NULL;
	a->target = target;
	if (w) {
		const cJSON *sz = cJSON_GetObjectItemCaseSensitive(w, "size");
		a->size = cJSON_IsNumber(sz) ? sz->valuedouble : 0;
	}
	if (floor) {
		(void)snprintf(a->required, sizeof(a->required), ">=%s", floor);
	}
	bool have = installed_version(c, name, a->installed, sizeof(a->installed));
	if (!have) {
		a->state = ST_MISSING;
	} else if (floor && vercmp(a->installed, floor) < 0) {
		a->state = ST_TOO_OLD;
	} else if (want_latest && target && vercmp(a->installed, target) < 0) {
		a->state = ST_TOO_OLD;
	} else {
		a->state = ST_OK;
		return;
	}
	// Fetchable only if the catalog offers a version that satisfies the floor
	// and is not a downgrade.
	if (target == NULL || (floor && vercmp(target, floor) < 0) || (have && vercmp(target, a->installed) <= 0)) {
		(void)snprintf(a->reason, sizeof(a->reason), "%s",
		               target == NULL ? "no signed Windows build in the catalog"
		                              : "the catalog's version does not satisfy the requirement");
		a->state = ST_NOT_FETCHABLE;
	}
}

static void
make_plan(const struct catalog *c, const char *for_component, const char *min_runtime, struct plan *p)
{
	memset(p, 0, sizeof(*p));
	p->for_component = for_component;
	const cJSON *comp = component(c, for_component);
	const cJSON *req = comp ? cJSON_GetObjectItemCaseSensitive(comp, "requires") : NULL;
	const char *floor = min_runtime ? min_runtime : (req ? floor_of(jstr(req, "runtime")) : NULL);

	// Display plug-ins: positive evidence only.
	const cJSON *leia = component(c, "leia_plugin");
	char why[256] = {0};
	const bool leia_wanted =
	    leia != NULL && strcmp(for_component, "leia_plugin") != 0 && detect_match(leia, true, why, sizeof(why));
	if (leia_wanted) {
		// The plug-in's own runtime floor joins the runtime requirement.
		const cJSON *lreq = cJSON_GetObjectItemCaseSensitive(leia, "requires");
		const char *lf = lreq ? floor_of(jstr(lreq, "runtime")) : NULL;
		if (lf && (floor == NULL || vercmp(lf, floor) > 0)) {
			floor = lf;
		}
	}

	if (strcmp(for_component, "runtime") != 0) {
		plan_one(c, "runtime", floor, false, &p->a[p->n++]);
	}
	if (leia_wanted) {
		struct action *runtime = &p->a[0];
		// Upgrading the runtime makes an older plug-in ABI-stale: bring it to the
		// catalog's ABI-matched pin in the same pass. Otherwise only install it
		// when it is absent -- this tool ensures, it does not update.
		const bool runtime_moves = p->n > 0 && runtime->state != ST_OK && runtime->state != ST_NOT_FETCHABLE;
		struct action *a = &p->a[p->n++];
		plan_one(c, "leia_plugin", NULL, runtime_moves, a);
		(void)snprintf(a->reason + strlen(a->reason), sizeof(a->reason) - strlen(a->reason),
		               "%sLeia display: %s", a->reason[0] ? "; " : "", why);
	}
}

static void
print_plan_json(const struct plan *p)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "for", p->for_component);
	cJSON *arr = cJSON_AddArrayToObject(o, "actions");
	for (int i = 0; i < p->n; i++) {
		const struct action *a = &p->a[i];
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "component", a->component);
		if (a->installed[0]) {
			cJSON_AddStringToObject(x, "installed", a->installed);
		} else {
			cJSON_AddNullToObject(x, "installed");
		}
		cJSON_AddStringToObject(x, "state", k_state_names[a->state]);
		if (a->required[0]) {
			cJSON_AddStringToObject(x, "required", a->required);
		}
		if (a->target) {
			cJSON_AddStringToObject(x, "target", a->target);
		}
		cJSON_AddNumberToObject(x, "size", a->size);
		if (a->reason[0]) {
			cJSON_AddStringToObject(x, "reason", a->reason);
		}
		cJSON_AddItemToArray(arr, x);
	}
	char *s = cJSON_PrintUnformatted(o);
	printf("%s\n", s);
	cJSON_free(s);
	cJSON_Delete(o);
}

static bool
needs_work(const struct action *a)
{
	return a->state == ST_MISSING || a->state == ST_TOO_OLD;
}

//! Human-readable plan: what the browser installer shows in its one consent box.
static void
print_plan_text(const struct plan *p)
{
	double total = 0;
	int todo = 0;
	for (int i = 0; i < p->n; i++) {
		const struct action *a = &p->a[i];
		if (needs_work(a)) {
			todo++;
			total += a->size;
			if (a->state == ST_MISSING) {
				printf("%s: not installed -> install %s (%.1f MB)\n", a->label, a->target,
				       a->size / 1048576.0);
			} else {
				printf("%s: %s is too old -> update to %s (%.1f MB)\n", a->label, a->installed,
				       a->target, a->size / 1048576.0);
			}
		} else if (a->state == ST_NOT_FETCHABLE) {
			printf("%s: %s, but %s\n", a->label, a->installed[0] ? "too old" : "not installed", a->reason);
		}
	}
	if (todo == 0) {
		printf("Nothing to install.\n");
	} else {
		printf("Total download: %.1f MB\n", total / 1048576.0);
	}
	(void)fflush(stdout);
}

/*
 * Fetch + verify + install.
 */

static bool
is_admin(void)
{
	return IsUserAnAdmin() != FALSE;
}

static bool
downloads_dir(wchar_t *out, size_t cap)
{
	wchar_t *pd = NULL;
	if (FAILED(SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL, &pd))) {
		return false;
	}
	(void)_snwprintf(out, cap, L"%ls\\DisplayXR\\Downloads", pd);
	CoTaskMemFree(pd);
	(void)SHCreateDirectoryExW(NULL, out, NULL);
	return true;
}

static int
fetch_component(const struct catalog *c, const char *name, wchar_t *out_path, size_t cap, const wchar_t *dir_override)
{
	const cJSON *w = win_block(c, name);
	if (w == NULL) {
		dxr_log("%s: nothing fetchable in the catalog\n", name);
		return DXR_GET_NOT_FETCHABLE;
	}
	const char *sha = jstr(w, "sha256"), *signer = jstr(w, "signer"), *file = jstr(w, "file");
	const cJSON *size = cJSON_GetObjectItemCaseSensitive(w, "size");
	const cJSON *urls = cJSON_GetObjectItemCaseSensitive(w, "urls");
	if (sha == NULL || strlen(sha) != 64 || signer == NULL || file == NULL || !cJSON_IsNumber(size) ||
	    !cJSON_IsArray(urls)) {
		dxr_log("%s: malformed catalog entry\n", name);
		return DXR_GET_NOT_FETCHABLE;
	}
	bool signer_known = false;
	for (size_t i = 0; i < sizeof(k_trusted_signers) / sizeof(k_trusted_signers[0]); i++) {
		signer_known = signer_known || strcmp(signer, k_trusted_signers[i]) == 0;
	}
	if (!signer_known) {
		dxr_log("%s: catalog names signer \"%s\", which this dxr-get does not trust\n", name, signer);
		return DXR_GET_VERIFY;
	}

	wchar_t dir[MAX_PATH];
	if (dir_override) {
		(void)_snwprintf(dir, MAX_PATH, L"%ls", dir_override);
		(void)SHCreateDirectoryExW(NULL, dir, NULL);
	} else if (!downloads_dir(dir, MAX_PATH)) {
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
		dxr_log("%s: downloading %s %s (%.1f MB)\n", name, file, jstr(w, "version"), want / 1048576.0);
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
		if (!dxr_authenticode_signer(out_path, cn, sizeof(cn)) || strcmp(cn, signer) != 0) {
			dxr_log("%s: Authenticode signer \"%s\" does not match \"%s\" -- refusing\n", name, cn, signer);
			return DXR_GET_VERIFY;
		}
		dxr_log("%s: verified (sha256 ok, signed by %s)\n", name, cn);
	}
	return DXR_GET_OK;
}

static int
run_installer(const char *name, const wchar_t *path, const cJSON *args)
{
	wchar_t cmd[4096];
	int n = _snwprintf(cmd, 4096, L"\"%ls\"", path);
	const cJSON *a;
	cJSON_ArrayForEach(a, args)
	{
		if (cJSON_IsString(a)) {
			n += _snwprintf(cmd + n, 4096 - n, L" %hs", a->valuestring);
		}
	}
	dxr_log("%s: running installer %ls\n", name, cmd);
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		dxr_log("%s: cannot start installer (error %lu)\n", name, GetLastError());
		return DXR_GET_CHILD;
	}
	// NSIS /S installers re-exec themselves from %TEMP% (the uninstaller copy
	// trick) for some paths; WaitForSingleObject on the parent is what NSIS
	// documents as sufficient for a plain installer.
	(void)WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	(void)GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (code != 0) {
		dxr_log("%s: installer exited with %lu\n", name, code);
		return DXR_GET_CHILD;
	}
	dxr_log("%s: installed\n", name);
	return DXR_GET_OK;
}

static void
stop_service(void)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return;
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
		HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
		if (h != NULL) {
			dxr_log("stopping %ls (pid %lu) for the install\n", pe.szExeFile, pe.th32ProcessID);
			(void)TerminateProcess(h, 0);
			(void)WaitForSingleObject(h, 5000);
			CloseHandle(h);
		}
	}
	CloseHandle(snap);
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

static int
install_component(const struct catalog *c, const char *name)
{
	wchar_t path[MAX_PATH];
	int r = fetch_component(c, name, path, MAX_PATH, NULL);
	if (r != DXR_GET_OK) {
		return r;
	}
	const cJSON *w = win_block(c, name);
	return run_installer(name, path, cJSON_GetObjectItemCaseSensitive(w, "args"));
}

/*
 * CLI.
 */

static void
usage(void)
{
	dxr_log(
	    "dxr-get -- fetch, verify and install DisplayXR components from the signed catalog\n"
	    "\n"
	    "  dxr-get [--catalog <url|file>] plan --for <component> [--min-runtime X.Y.Z] [--json]\n"
	    "  dxr-get [--catalog <url|file>] ensure-for <component> [--min-runtime X.Y.Z] [--yes] "
	    "[--no-start-service]\n"
	    "  dxr-get [--catalog <url|file>] fetch <component> [--out <dir>]\n"
	    "  dxr-get [--catalog <url|file>] install <component> [--yes]\n"
	    "  dxr-get [--catalog <url|file>] detect [--json]\n"
	    "  dxr-get [--catalog <url|file>] catalog\n"
	    "\n"
	    "components: runtime, leia_plugin, browser\n"
	    "exit: 0 ok, 2 usage, 10 offline/expired catalog, 11 bad signature, 20 download,\n"
	    "      21 verification, 30 installer failed, 40 not fetchable, 50 needs elevation\n");
}

int
main(int argc, char **argv)
{
	// DXR_GET_CATALOG redirects to another catalog (a test or staging copy). It
	// grants nothing: that catalog must still carry a valid signature from a
	// compiled-in key, and the serial guard still applies.
	const char *catalog_where = getenv("DXR_GET_CATALOG") ? getenv("DXR_GET_CATALOG") : DEFAULT_CATALOG;
	const char *cmd = NULL, *arg = NULL, *for_c = NULL, *min_runtime = NULL, *out_dir = NULL;
	bool json = false, yes = false, no_verify = false, no_start = false;
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
	if (cmd == NULL) {
		usage();
		return DXR_GET_USAGE;
	}
	if (no_verify && !env_on("DXR_GET_DEV")) {
		dxr_log("--no-verify-catalog needs DXR_GET_DEV=1\n");
		return DXR_GET_USAGE;
	}
	if (strcmp(cmd, "ensure-for") == 0) {
		for_c = arg;
	}

	struct catalog c;
	int r = load_catalog(catalog_where, !no_verify, &c);
	if (r != DXR_GET_OK) {
		return r;
	}

	if (strcmp(cmd, "catalog") == 0) {
		char *s = cJSON_Print(c.root);
		printf("%s\n", s);
		cJSON_free(s);
		r = DXR_GET_OK;
	} else if (strcmp(cmd, "detect") == 0) {
		struct dxr_edid_id ids[32];
		int n = dxr_detect_edids(ids, 32);
		const cJSON *leia = component(&c, "leia_plugin");
		char why_hi[256] = {0}, why_any[256] = {0};
		bool hi = leia && detect_match(leia, true, why_hi, sizeof(why_hi));
		bool any = leia && detect_match(leia, false, why_any, sizeof(why_any));
		if (json) {
			cJSON *o = cJSON_CreateObject();
			cJSON *e = cJSON_AddArrayToObject(o, "edids");
			for (int i = 0; i < n; i++) {
				cJSON *x = cJSON_CreateObject();
				cJSON_AddNumberToObject(x, "man", ids[i].man);
				cJSON_AddNumberToObject(x, "prod", ids[i].prod);
				cJSON_AddItemToArray(e, x);
			}
			cJSON *l = cJSON_AddObjectToObject(o, "leia_plugin");
			cJSON_AddBoolToObject(l, "match", any);
			cJSON_AddBoolToObject(l, "high_confidence", hi);
			cJSON_AddStringToObject(l, "evidence", hi ? why_hi : why_any);
			char *s = cJSON_PrintUnformatted(o);
			printf("%s\n", s);
			cJSON_free(s);
			cJSON_Delete(o);
		} else {
			for (int i = 0; i < n; i++) {
				printf("monitor EDID man=%u (0x%04X) prod=%u (0x%04X)\n", ids[i].man, ids[i].man,
				       ids[i].prod, ids[i].prod);
			}
			printf("leia_plugin: %s%s%s\n",
			       any ? (hi ? "MATCH (high confidence)" : "match (low confidence)") : "no match",
			       any ? " -- " : "", hi ? why_hi : why_any);
		}
		r = DXR_GET_OK;
	} else if (strcmp(cmd, "plan") == 0 || strcmp(cmd, "ensure-for") == 0) {
		if (for_c == NULL) {
			usage();
			r = DXR_GET_USAGE;
			goto done;
		}
		struct plan p;
		make_plan(&c, for_c, min_runtime, &p);
		if (strcmp(cmd, "plan") == 0) {
			if (json) {
				print_plan_json(&p);
			} else {
				print_plan_text(&p);
			}
			r = DXR_GET_OK;
			goto done;
		}
		// ensure-for
		(void)yes; // installers pass --yes; the consent prompt is theirs, not ours
		int todo = 0;
		for (int i = 0; i < p.n; i++) {
			todo += needs_work(&p.a[i]);
		}
		print_plan_text(&p);
		if (todo > 0 && !is_admin()) {
			dxr_log("installing needs an elevated process\n");
			r = DXR_GET_NEEDS_ADMIN;
			goto done;
		}
		r = DXR_GET_OK;
		for (int i = 0; i < p.n; i++) {
			if (p.a[i].state == ST_NOT_FETCHABLE && strcmp(p.a[i].component, "runtime") == 0) {
				// The runtime the caller needs can't be had: say so, and don't
				// install a plug-in for a runtime that isn't there.
				r = DXR_GET_NOT_FETCHABLE;
				goto done;
			}
		}
		if (todo == 0) {
			goto done;
		}
		// Download and verify EVERYTHING first: a network failure halfway must
		// not leave a new runtime next to an ABI-stale plug-in.
		wchar_t paths[4][MAX_PATH];
		for (int i = 0; i < p.n; i++) {
			if (needs_work(&p.a[i])) {
				int rr = fetch_component(&c, p.a[i].component, paths[i], MAX_PATH, NULL);
				if (rr != DXR_GET_OK) {
					r = rr;
					goto done;
				}
			}
		}
		// Same order as the bundle (#461/#342): nothing may hold the runtime's
		// or a plug-in's DLLs while their installers replace them -- an NSIS /S
		// install skips a locked file silently -- and the service starts once,
		// at the end, at the user's integrity level.
		stop_service();
		for (int i = 0; i < p.n && r == DXR_GET_OK; i++) {
			if (needs_work(&p.a[i])) {
				r = run_installer(
				    p.a[i].component, paths[i],
				    cJSON_GetObjectItemCaseSensitive(win_block(&c, p.a[i].component), "args"));
			}
		}
		if (!no_start) {
			start_service();
		}
	} else if (strcmp(cmd, "fetch") == 0 || strcmp(cmd, "install") == 0) {
		if (arg == NULL) {
			usage();
			r = DXR_GET_USAGE;
			goto done;
		}
		if (strcmp(cmd, "install") == 0) {
			char inst[64];
			const cJSON *w = win_block(&c, arg);
			const char *target = w ? jstr(w, "version") : NULL;
			if (target && installed_version(&c, arg, inst, sizeof(inst)) && vercmp(inst, target) >= 0) {
				dxr_log("%s: %s installed, catalog has %s -- nothing to do (never downgrades)\n", arg,
				        inst, target);
				r = DXR_GET_OK;
				goto done;
			}
			if (!is_admin()) {
				dxr_log("installing needs an elevated process\n");
				r = DXR_GET_NEEDS_ADMIN;
				goto done;
			}
			r = install_component(&c, arg);
		} else {
			wchar_t wout[MAX_PATH], path[MAX_PATH];
			if (out_dir) {
				(void)MultiByteToWideChar(CP_UTF8, 0, out_dir, -1, wout, MAX_PATH);
			}
			r = fetch_component(&c, arg, path, MAX_PATH, out_dir ? wout : NULL);
			if (r == DXR_GET_OK) {
				printf("%ls\n", path);
			}
		}
	} else {
		usage();
		r = DXR_GET_USAGE;
	}
done:
	cJSON_Delete(c.root);
	return r;
}
