// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  dxr-get core: catalog model, plan and trust decisions (ADR-064
 *         steps 2 and 3). No operating-system calls: see get_core.h.
 */

// Also built into tests/tests_get_catalog, which does not set this on the command line.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "get_core.h"

#include <stdlib.h>
#include <string.h>

#define RUNTIME_VERSION_PATH "HKLM\\SOFTWARE\\DisplayXR\\Runtime\\Version"
#define RUNTIME_ABI_KEY "HKLM\\SOFTWARE\\DisplayXR\\Runtime\\Abi"
//! The runtime version the Abi values were written by. They count only while it is the installed one.
#define RUNTIME_ABI_STAMP RUNTIME_ABI_KEY "\\RuntimeVersion"
#define V1_PUBLISHER "displayxr"

/*
 * Schema 1 only: the Authenticode signers a schema-1 payload may carry. A
 * schema-2 catalog names its publishers' signers itself, in the signed index,
 * and this list plays no part there (ADR-064 §4).
 */
static const char *const k_v1_trusted_signers[] = {"Leia, Inc."};

/*
 * Schema 1 only: what that schema says by component NAME and schema 2 says as
 * data. This table is the single place a component name appears; it is the
 * same mapping scripts/gen_catalog.py uses to derive the schema-2 document.
 */
static const struct
{
	const char *name; //!< schema-1 component
	const char *id;   //!< schema-2 item id
	const char *item_class;
	const char *registered_id;
	const char *label;
	const char *version_path; //!< where today's installers write the installed version
} k_v1_items[] = {
    {"runtime", "runtime", "runtime", NULL, "DisplayXR runtime", RUNTIME_VERSION_PATH},
    {"leia_plugin", "leia-sr", "display_processor", "leia-sr", "Leia display plug-in",
     "HKLM\\SOFTWARE\\DisplayXR\\Plugins\\LeiaSR\\Version"},
    {"browser", "browser", "app", NULL, "DisplayXR Browser", "HKLM\\SOFTWARE\\DisplayXR\\Browser\\Version"},
};

static const struct
{
	const char *item_class;
	const char *root; //!< registration root under HKLM\Software\DisplayXR, NULL when the class has none
	bool loaded;      //!< the runtime or the service maps or spawns it
} k_classes[] = {
    {"runtime", NULL, true},
    {"display_processor", "DisplayProcessors", true},
    {"input_provider", "InputProviders", true},
    {"conversion_provider", "ConversionProviders", true},
    {"tracking_provider", "TrackingProviders", true},
    {"workspace_controller", "WorkspaceControllers", true},
    {"companion", "PoseConsumers", true},
    {"app", NULL, false},
    {"sdk", NULL, false},
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/*
 * Helpers.
 */

int
dxr_vercmp(const char *a, const char *b)
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

const char *
dxr_floor_of(const char *req)
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

time_t
dxr_parse_utc(const char *s)
{
	int y, mo, d, h, mi, se;
	if (s == NULL || sscanf(s, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &se) != 6 || y < 1970 || mo < 1 ||
	    mo > 12 || d < 1 || d > 31) {
		return 0;
	}
	// Days from the civil date (no timegm / _mkgmtime: neither is portable).
	long long yy = mo <= 2 ? y - 1 : y;
	long long era = yy / 400;
	long long yoe = yy - era * 400;
	long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	long long days = era * 146097 + doe - 719468;
	return (time_t)(days * 86400 + h * 3600 + mi * 60 + se);
}

static const char *
jstr(const cJSON *o, const char *k)
{
	const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static const cJSON *
jobj(const cJSON *o, const char *k)
{
	const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
	return cJSON_IsObject(v) ? v : NULL;
}

static const cJSON *
jarr(const cJSON *o, const char *k)
{
	const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
	return cJSON_IsArray(v) ? v : NULL;
}

static void
copy(char *out, size_t n, const char *s)
{
	(void)snprintf(out, n, "%s", s ? s : "");
}

static const char *
class_root(const char *item_class)
{
	for (size_t i = 0; item_class && i < ARRAY_SIZE(k_classes); i++) {
		if (strcmp(k_classes[i].item_class, item_class) == 0) {
			return k_classes[i].root;
		}
	}
	return NULL;
}

static bool
class_known(const char *item_class)
{
	for (size_t i = 0; item_class && i < ARRAY_SIZE(k_classes); i++) {
		if (strcmp(k_classes[i].item_class, item_class) == 0) {
			return true;
		}
	}
	return false;
}

bool
dxr_class_is_loaded(const char *item_class)
{
	for (size_t i = 0; item_class && i < ARRAY_SIZE(k_classes); i++) {
		if (strcmp(k_classes[i].item_class, item_class) == 0) {
			return k_classes[i].loaded;
		}
	}
	return true; // unknown: assume the careful answer
}

/*
 * Catalog.
 */

//! Schema-1 `windows` block -> schema-2 asset. Same bytes, same signer, regrouped.
static cJSON *
v1_asset(const cJSON *win, const char *default_version_path)
{
	cJSON *a = cJSON_CreateObject();
	static const char *const same[] = {"version", "file", "size", "sha256", "urls"};
	for (size_t i = 0; i < ARRAY_SIZE(same); i++) {
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(win, same[i]);
		if (v) {
			cJSON_AddItemToObject(a, same[i], cJSON_Duplicate(v, 1));
		}
	}
	if (jstr(win, "signer")) {
		cJSON *sig = cJSON_AddObjectToObject(a, "signature");
		cJSON_AddStringToObject(sig, "kind", "authenticode");
		cJSON_AddStringToObject(sig, "signer", jstr(win, "signer"));
	}
	cJSON *ins = cJSON_AddObjectToObject(a, "installer");
	cJSON_AddStringToObject(ins, "kind", "exe");
	const cJSON *args = cJSON_GetObjectItemCaseSensitive(win, "args");
	cJSON_AddItemToObject(ins, "args", args ? cJSON_Duplicate(args, 1) : cJSON_CreateArray());
	cJSON_AddStringToObject(ins, "scope", "machine");
	cJSON_AddStringToObject(ins, "elevation", "required");
	const char *reg = jstr(win, "registry_version");
	cJSON *inst = cJSON_AddObjectToObject(a, "installed");
	cJSON_AddStringToObject(inst, "kind", "registry_sz");
	cJSON_AddStringToObject(inst, "path", reg ? reg : default_version_path);
	return a;
}

static cJSON *
v1_to_model(const cJSON *v1)
{
	cJSON *m = cJSON_CreateObject();
	cJSON_AddNumberToObject(m, "schema", 2);
	cJSON *pubs = cJSON_AddObjectToObject(m, "publishers");
	cJSON *pub = cJSON_AddObjectToObject(pubs, V1_PUBLISHER);
	cJSON_AddStringToObject(pub, "name", "DisplayXR");
	cJSON *signers = cJSON_AddArrayToObject(cJSON_AddObjectToObject(pub, "signers"), DXR_PLATFORM);
	for (size_t i = 0; i < ARRAY_SIZE(k_v1_trusted_signers); i++) {
		cJSON *s = cJSON_CreateObject();
		cJSON_AddStringToObject(s, "kind", "authenticode");
		cJSON_AddStringToObject(s, "subject_cn", k_v1_trusted_signers[i]);
		cJSON_AddItemToArray(signers, s);
	}
	cJSON *items = cJSON_AddArrayToObject(m, "items");
	const cJSON *comps = jobj(v1, "components");
	for (size_t i = 0; i < ARRAY_SIZE(k_v1_items); i++) {
		const cJSON *comp = jobj(comps, k_v1_items[i].name);
		if (comp == NULL) {
			continue;
		}
		cJSON *it = cJSON_CreateObject();
		cJSON_AddStringToObject(it, "id", k_v1_items[i].id);
		if (strcmp(k_v1_items[i].id, k_v1_items[i].name) != 0) {
			cJSON *al = cJSON_AddArrayToObject(it, "aliases");
			cJSON_AddItemToArray(al, cJSON_CreateString(k_v1_items[i].name));
		}
		cJSON_AddStringToObject(it, "class", k_v1_items[i].item_class);
		if (k_v1_items[i].registered_id) {
			cJSON_AddStringToObject(it, "registered_id", k_v1_items[i].registered_id);
		}
		cJSON_AddStringToObject(it, "name", k_v1_items[i].label);
		cJSON_AddStringToObject(it, "publisher", V1_PUBLISHER);
		if (jstr(comp, "recommended")) {
			cJSON_AddStringToObject(it, "version", jstr(comp, "recommended"));
		}
		const char *floor = jstr(jobj(comp, "requires"), "runtime");
		if (floor) {
			cJSON *r = cJSON_CreateObject();
			cJSON_AddStringToObject(r, "kind", "runtime");
			cJSON_AddStringToObject(r, "version", floor);
			cJSON_AddItemToArray(cJSON_AddArrayToObject(it, "requires"), r);
		}
		const cJSON *win = jobj(comp, "windows");
		if (win) {
			cJSON_AddItemToObject(cJSON_AddObjectToObject(it, "assets"), DXR_PLATFORM,
			                      v1_asset(win, k_v1_items[i].version_path));
		}
		// Today's installers write the version here whether or not the catalog
		// has a fetchable build of the component.
		cJSON_AddStringToObject(it, "x-installed-path", k_v1_items[i].version_path);
		const cJSON *detect = jarr(comp, "detect");
		if (detect) {
			cJSON_AddItemToObject(it, "detect", cJSON_Duplicate(detect, 1));
		}
		cJSON_AddItemToArray(items, it);
	}
	return m;
}

int
dxr_catalog_parse(const char *body, size_t len, time_t now, struct dxr_catalog *c, char *err, size_t err_size)
{
	memset(c, 0, sizeof(*c));
	c->doc = cJSON_ParseWithLength(body, len);
	const cJSON *schema = c->doc ? cJSON_GetObjectItemCaseSensitive(c->doc, "schema") : NULL;
	if (!cJSON_IsNumber(schema) || (schema->valueint != 1 && schema->valueint != 2)) {
		copy(err, err_size, c->doc ? "unsupported schema" : "not JSON");
		dxr_catalog_free(c);
		return DXR_GET_OFFLINE;
	}
	c->schema = schema->valueint;
	const cJSON *serial = cJSON_GetObjectItemCaseSensitive(c->doc, "serial");
	c->serial = cJSON_IsNumber(serial) ? (long long)serial->valuedouble : 0;

	time_t exp = dxr_parse_utc(jstr(c->doc, "expires"));
	if (exp == 0 || now > exp) {
		(void)snprintf(err, err_size, "expired (%s)", jstr(c->doc, "expires") ? jstr(c->doc, "expires") : "?");
		dxr_catalog_free(c);
		return DXR_GET_OFFLINE;
	}
	c->model = c->schema == 1 ? v1_to_model(c->doc) : c->doc;
	c->items = jarr(c->model, "items");
	c->publishers = jobj(c->model, "publishers");
	c->revoked = jarr(c->model, "revoked");
	if (c->items == NULL || c->publishers == NULL) {
		copy(err, err_size, "no items or no publishers");
		dxr_catalog_free(c);
		return DXR_GET_OFFLINE;
	}
	return DXR_GET_OK;
}

void
dxr_catalog_free(struct dxr_catalog *c)
{
	if (c->model != NULL && c->model != c->doc) {
		cJSON_Delete(c->model);
	}
	cJSON_Delete(c->doc);
	memset(c, 0, sizeof(*c));
}

const char *
dxr_item_id(const cJSON *item)
{
	return jstr(item, "id");
}

const char *
dxr_item_class(const cJSON *item)
{
	return jstr(item, "class");
}

const char *
dxr_item_legacy_name(const cJSON *item)
{
	const cJSON *al = jarr(item, "aliases");
	const cJSON *first = al ? cJSON_GetArrayItem(al, 0) : NULL;
	return cJSON_IsString(first) ? first->valuestring : dxr_item_id(item);
}

const char *
dxr_item_label(const cJSON *item)
{
	const char *n = jstr(item, "name");
	return n ? n : dxr_item_id(item);
}

const cJSON *
dxr_item_find(const struct dxr_catalog *c, const char *name)
{
	const cJSON *it;
	if (name == NULL) {
		return NULL;
	}
	cJSON_ArrayForEach(it, c->items)
	{
		const char *id = dxr_item_id(it);
		if (id == NULL || !class_known(dxr_item_class(it))) {
			continue; // a class this build does not know is skipped, not guessed at
		}
		if (strcmp(id, name) == 0) {
			return it;
		}
		const cJSON *al;
		cJSON_ArrayForEach(al, jarr(it, "aliases"))
		{
			if (cJSON_IsString(al) && strcmp(al->valuestring, name) == 0) {
				return it;
			}
		}
	}
	return NULL;
}

const cJSON *
dxr_runtime_item(const struct dxr_catalog *c)
{
	const cJSON *it;
	cJSON_ArrayForEach(it, c->items)
	{
		const char *cls = dxr_item_class(it);
		if (cls != NULL && strcmp(cls, "runtime") == 0 && dxr_item_id(it) != NULL) {
			return it;
		}
	}
	return NULL;
}

static const cJSON *
platform_asset(const cJSON *item)
{
	return jobj(jobj(item, "assets"), DXR_PLATFORM);
}

const cJSON *
dxr_item_asset(const cJSON *item)
{
	const cJSON *a = platform_asset(item);
	return (a != NULL && jobj(a, "listing") == NULL) ? a : NULL;
}

bool
dxr_installed_path(const cJSON *item, char *out, size_t out_size)
{
	out[0] = '\0';
	const cJSON *inst = jobj(dxr_item_asset(item), "installed");
	const char *kind = jstr(inst, "kind");
	const char *cls = dxr_item_class(item);
	if (kind != NULL && strcmp(kind, "registry_sz") == 0 && jstr(inst, "path") != NULL) {
		copy(out, out_size, jstr(inst, "path"));
	} else if (jstr(item, "x-installed-path") != NULL) {
		copy(out, out_size, jstr(item, "x-installed-path"));
	} else if (cls != NULL && strcmp(cls, "runtime") == 0) {
		copy(out, out_size, RUNTIME_VERSION_PATH);
	} else if (class_root(cls) != NULL && jstr(item, "registered_id") != NULL) {
		// `registration_row`, and the fallback when the item has no asset here:
		// the Version value of the item's own class row.
		(void)snprintf(out, out_size, "HKLM\\Software\\DisplayXR\\%s\\%s\\Version", class_root(cls),
		               jstr(item, "registered_id"));
	}
	return out[0] != '\0';
}

//! The installed runtime's version when the catalog has no runtime item to say where it is.
static bool
runtime_installed(const struct dxr_env *env, char *out, size_t out_size)
{
	out[0] = '\0';
	if (env->assume_runtime != NULL) {
		copy(out, out_size, env->assume_runtime);
		return out[0] != '\0';
	}
	return env->reg_sz != NULL && env->reg_sz(env->user, RUNTIME_VERSION_PATH, out, out_size) && out[0] != '\0';
}

bool
dxr_installed_version(const struct dxr_env *env, const cJSON *item, char *out, size_t out_size)
{
	char path[512];
	out[0] = '\0';
	const char *cls = dxr_item_class(item);
	if (env->assume_runtime != NULL && cls != NULL && strcmp(cls, "runtime") == 0) {
		copy(out, out_size, env->assume_runtime);
		return out[0] != '\0';
	}
	if (dxr_installed_path(item, path, sizeof(path))) {
		return env->reg_sz != NULL && env->reg_sz(env->user, path, out, out_size) && out[0] != '\0';
	}
	// No rule to read: the receipt is all there is to go on.
	struct dxr_receipt r;
	if (env->receipt != NULL && dxr_item_id(item) != NULL && env->receipt(env->user, dxr_item_id(item), &r) &&
	    r.version[0] != '\0') {
		copy(out, out_size, r.version);
		return true;
	}
	return false;
}

static bool
safe_file_name(const char *f)
{
	return f != NULL && f[0] != '\0' && strpbrk(f, "/\\:") == NULL && strcmp(f, "..") != 0 && strcmp(f, ".") != 0;
}

int
dxr_asset_check(const struct dxr_catalog *c,
                const cJSON *item,
                const cJSON *asset,
                const char *actual_cn,
                char *reason,
                size_t reason_size)
{
	reason[0] = '\0';
	if (asset == NULL || jobj(asset, "listing") != NULL) {
		copy(reason, reason_size, "no signed Windows build in the catalog");
		return DXR_GET_NOT_FETCHABLE;
	}
	const char *sha = jstr(asset, "sha256");
	const cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
	const cJSON *urls = jarr(asset, "urls");
	bool urls_ok = urls != NULL && cJSON_GetArraySize(urls) > 0;
	const cJSON *u;
	cJSON_ArrayForEach(u, urls)
	{
		urls_ok = urls_ok && cJSON_IsString(u) && strncmp(u->valuestring, "https://", 8) == 0;
	}
	if (sha == NULL || strlen(sha) != 64 || !safe_file_name(jstr(asset, "file")) || !cJSON_IsNumber(size) ||
	    size->valuedouble <= 0 || !urls_ok) {
		copy(reason, reason_size, "malformed catalog entry");
		return DXR_GET_NOT_FETCHABLE;
	}
	const cJSON *rv;
	cJSON_ArrayForEach(rv, c->revoked)
	{
		const char *rsha = jstr(rv, "sha256");
		if (rsha != NULL && strcmp(rsha, sha) == 0) {
			(void)snprintf(reason, reason_size, "this build was revoked%s%s",
			               jstr(rv, "reason") ? ": " : "", jstr(rv, "reason") ? jstr(rv, "reason") : "");
			return DXR_GET_VERIFY;
		}
	}
	const char *ikind = jstr(jobj(asset, "installer"), "kind");
	if (ikind == NULL || strcmp(ikind, "exe") != 0) {
		(void)snprintf(reason, reason_size, "installer kind \"%s\" is not supported by this dxr-get",
		               ikind ? ikind : "(none)");
		return DXR_GET_NOT_FETCHABLE;
	}
	// Every installable binary is signed: there is no hash-pinned-only form,
	// and dxr-get never writes a Trust\Allowlist row to stand in for one.
	const cJSON *sig = jobj(asset, "signature");
	if (sig == NULL) {
		copy(reason, reason_size, "the asset is not signed; only signed assets are installed");
		return DXR_GET_NOT_FETCHABLE;
	}
	const char *skind = jstr(sig, "kind"), *signer = jstr(sig, "signer");
	if (skind == NULL || strcmp(skind, "authenticode") != 0 || signer == NULL || signer[0] == '\0') {
		(void)snprintf(reason, reason_size, "unknown signature kind \"%s\"", skind ? skind : "(none)");
		return DXR_GET_VERIFY;
	}
	// The signer must be one the signed index delegates to THIS item's
	// publisher: another publisher's signer cannot vouch for it.
	const char *pub_id = jstr(item, "publisher");
	const cJSON *delegated = jarr(jobj(jobj(c->publishers, pub_id ? pub_id : ""), "signers"), DXR_PLATFORM);
	bool found = false;
	const cJSON *d;
	cJSON_ArrayForEach(d, delegated)
	{
		const char *dk = jstr(d, "kind"), *cn = jstr(d, "subject_cn");
		found =
		    found || (dk != NULL && strcmp(dk, "authenticode") == 0 && cn != NULL && strcmp(cn, signer) == 0);
	}
	if (!found) {
		(void)snprintf(reason, reason_size,
		               "catalog names signer \"%s\", which is not delegated to publisher \"%s\"", signer,
		               pub_id ? pub_id : "(none)");
		return DXR_GET_VERIFY;
	}
	if (actual_cn != NULL && strcmp(actual_cn, signer) != 0) {
		(void)snprintf(reason, reason_size, "Authenticode signer \"%s\" does not match \"%s\"", actual_cn,
		               signer);
		return DXR_GET_VERIFY;
	}
	return DXR_GET_OK;
}

/*
 * Detection.
 */

static bool
absolute_windows_path(const char *p)
{
	return p != NULL && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
	       (p[2] == '\\' || p[2] == '/') && strchr(p, '%') == NULL;
}

bool
dxr_detect_rules(const cJSON *rules, const struct dxr_env *env, bool high_only, char *why, size_t why_size)
{
	why[0] = '\0';
	if (!cJSON_IsArray(rules)) {
		return false;
	}
	struct dxr_edid_id ids[32];
	int n = -1; // gathered on first use: most rule sets have no EDID rule
	const cJSON *r;
	cJSON_ArrayForEach(r, rules)
	{
		const char *kind = jstr(r, "kind");
		const char *conf = jstr(r, "confidence");
		const char *note = jstr(r, "note");
		const char *key = jstr(r, "key"), *val = jstr(r, "value");
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
			if (n < 0) {
				n = env->edids != NULL ? env->edids(env->user, ids, 32) : 0;
			}
			for (int i = 0; i < n; i++) {
				if (ids[i].man == (uint16_t)m->valueint && ids[i].prod == (uint16_t)p->valueint) {
					(void)snprintf(why, why_size, "display EDID %u/%u%s%s (%s confidence)",
					               ids[i].man, ids[i].prod, note ? " " : "", note ? note : "",
					               conf ? conf : "?");
					return true;
				}
			}
		} else if (strcmp(kind, "registry_dword_min") == 0) {
			const cJSON *min = cJSON_GetObjectItemCaseSensitive(r, "min");
			uint32_t v = 0;
			if (key && val && cJSON_IsNumber(min) && env->reg_dword != NULL &&
			    env->reg_dword(env->user, key, val, &v) && v >= (uint32_t)min->valueint) {
				(void)snprintf(why, why_size, "%s (%s confidence)", note ? note : val,
				               conf ? conf : "?");
				return true;
			}
		} else if (strcmp(kind, "registry_sz_min") == 0) {
			const char *min = jstr(r, "min");
			char path[768], v[64];
			if (key && val && min && env->reg_sz != NULL) {
				(void)snprintf(path, sizeof(path), "%s\\%s", key, val);
				if (env->reg_sz(env->user, path, v, sizeof(v)) && v[0] != '\0' &&
				    dxr_vercmp(v, min) >= 0) {
					(void)snprintf(why, why_size, "%s\\%s = %s (needs >= %s)", key, val, v, min);
					return true;
				}
			}
		} else if (strcmp(kind, "registry_key_exists") == 0) {
			if (key && env->reg_key_exists != NULL && env->reg_key_exists(env->user, key)) {
				(void)snprintf(why, why_size, "registry key %s", key);
				return true;
			}
		} else if (strcmp(kind, "file_exists") == 0) {
			const char *path = jstr(r, "path");
			// An absolute path, no environment expansion: the rule is data
			// from the catalog and must mean the same file for every user.
			if (absolute_windows_path(path) && env->file_exists != NULL &&
			    env->file_exists(env->user, path)) {
				(void)snprintf(why, why_size, "file %s", path);
				return true;
			}
		} else if (strcmp(kind, "service_exists") == 0) {
			const char *name = jstr(r, "name");
			if (name && env->service_exists != NULL && env->service_exists(env->user, name)) {
				(void)snprintf(why, why_size, "service %s", name);
				return true;
			}
		}
		// A rule of an unknown kind is not matched.
	}
	return false;
}

/*
 * Plan.
 */

static const char *const k_state_names[] = {"ok", "missing", "too_old", "not_fetchable", "incompatible"};

struct node
{
	const cJSON *item;
	const char *floor;
	bool needed;
	bool want_latest;
	bool is_target;
	char why[256];
};

struct build
{
	const struct dxr_catalog *c;
	const struct dxr_env *env;
	struct dxr_plan *p;
	bool no_runtime;
	const cJSON *runtime;
	struct node nodes[DXR_MAX_STEPS];
	int n;
	const cJSON *visiting[16];
	int depth;
	const char *rt_floor;
	bool rt_floor_from_range; //!< the floor comes from an item's `runtime.min`
	const cJSON *rt_floor_owner;
};

static void
set_gate(struct dxr_plan *p, int code, const char *reason)
{
	// The first refusal is the one reported; an incompatibility outranks "not fetchable".
	if (p->gate == DXR_GET_OK || (code == DXR_GET_INCOMPATIBLE && p->gate != DXR_GET_INCOMPATIBLE)) {
		p->gate = code;
		copy(p->gate_reason, sizeof(p->gate_reason), reason);
	}
}

static void
raise_floor(struct build *b, const char *floor, bool from_range, const cJSON *owner)
{
	if (floor != NULL && (b->rt_floor == NULL || dxr_vercmp(floor, b->rt_floor) > 0)) {
		b->rt_floor = floor;
		b->rt_floor_from_range = from_range;
		b->rt_floor_owner = owner;
	}
}

static void
collect(struct build *b,
        const cJSON *item,
        const char *floor,
        bool needed,
        bool want_latest,
        bool is_target,
        const char *why);

//! The item's own requirements: a runtime floor, and other items (installed first).
static void
collect_requires(struct build *b, const cJSON *item, bool needed)
{
	char buf[320];
	if (b->depth >= (int)ARRAY_SIZE(b->visiting)) {
		return;
	}
	for (int i = 0; i < b->depth; i++) {
		if (b->visiting[i] == item) {
			(void)snprintf(buf, sizeof(buf), "%s: its requirements form a cycle", dxr_item_label(item));
			if (needed) {
				set_gate(b->p, DXR_GET_NOT_FETCHABLE, buf);
			}
			return;
		}
	}
	b->visiting[b->depth++] = item;
	const cJSON *r;
	cJSON_ArrayForEach(r, jarr(item, "requires"))
	{
		const char *kind = jstr(r, "kind");
		if (kind == NULL) {
			continue;
		}
		if (strcmp(kind, "runtime") == 0) {
			raise_floor(b, dxr_floor_of(jstr(r, "version")), false, item);
		} else if (strcmp(kind, "item") == 0) {
			const cJSON *dep = dxr_item_find(b->c, jstr(r, "id"));
			if (dep == NULL) {
				(void)snprintf(buf, sizeof(buf), "%s requires \"%s\", which is not in the catalog",
				               dxr_item_label(item), jstr(r, "id") ? jstr(r, "id") : "?");
				if (needed) {
					set_gate(b->p, DXR_GET_NOT_FETCHABLE, buf);
				}
				continue;
			}
			(void)snprintf(buf, sizeof(buf), "required by %s", dxr_item_label(item));
			collect(b, dep, dxr_floor_of(jstr(r, "version")), needed, false, false, buf);
		}
	}
	// `runtime.min` is the same statement as a runtime requirement.
	raise_floor(b, jstr(jobj(item, "runtime"), "min"), true, item);
	b->depth--;
}

static void
collect(struct build *b,
        const cJSON *item,
        const char *floor,
        bool needed,
        bool want_latest,
        bool is_target,
        const char *why)
{
	if (item == b->runtime && !is_target) {
		raise_floor(b, floor, false, item); // the runtime as a prerequisite item is a floor
		return;
	}
	for (int i = 0; i < b->n; i++) {
		if (b->nodes[i].item == item) {
			if (floor != NULL && (b->nodes[i].floor == NULL || dxr_vercmp(floor, b->nodes[i].floor) > 0)) {
				b->nodes[i].floor = floor;
			}
			b->nodes[i].needed = b->nodes[i].needed || needed;
			return;
		}
	}
	collect_requires(b, item, needed); // requirements first: they install before the item
	if (b->n >= DXR_MAX_STEPS) {
		return;
	}
	struct node *nd = &b->nodes[b->n++];
	memset(nd, 0, sizeof(*nd));
	nd->item = item;
	nd->floor = floor;
	nd->needed = needed;
	nd->want_latest = want_latest;
	nd->is_target = is_target;
	copy(nd->why, sizeof(nd->why), why);
}

static void
append_reason(struct dxr_step *s, const char *text)
{
	size_t l = strlen(s->reason);
	(void)snprintf(s->reason + l, sizeof(s->reason) - l, "%s%s", l ? "; " : "", text);
}

//! State of one item against a floor. @p want_latest: also act when the catalog has a newer version.
static void
plan_step(struct build *b, const cJSON *item, const char *floor, bool want_latest, struct dxr_step *s)
{
	memset(s, 0, sizeof(*s));
	s->kind = DXR_STEP_ITEM;
	s->item = item;
	s->hash_match = -1;
	copy(s->component, sizeof(s->component), dxr_item_legacy_name(item));
	copy(s->id, sizeof(s->id), dxr_item_id(item));
	copy(s->label, sizeof(s->label), dxr_item_label(item));
	const cJSON *asset = dxr_item_asset(item);
	const char *target = jstr(asset, "version");
	s->asset = asset;
	s->target = target;
	const cJSON *sz = asset ? cJSON_GetObjectItemCaseSensitive(asset, "size") : NULL;
	s->size = cJSON_IsNumber(sz) ? sz->valuedouble : 0;
	if (floor != NULL) {
		(void)snprintf(s->required, sizeof(s->required), ">=%s", floor);
	}
	const bool have = dxr_installed_version(b->env, item, s->installed, sizeof(s->installed));

	// Same hash as the one installed? Only a receipt knows, and only while it
	// still describes the version that is on the machine.
	struct dxr_receipt rc;
	const char *sha = jstr(asset, "sha256");
	if (have && sha != NULL && b->env->receipt != NULL && b->env->receipt(b->env->user, s->id, &rc) &&
	    rc.sha256[0] != '\0' && dxr_vercmp(rc.version, s->installed) == 0) {
		s->hash_match = strcmp(rc.sha256, sha) == 0 ? 1 : 0;
	}
	s->current = have && target != NULL && dxr_vercmp(s->installed, target) == 0;
	s->rebuild = want_latest && s->current && s->hash_match == 0;

	if (!have) {
		s->state = DXR_ST_MISSING;
	} else if (floor != NULL && dxr_vercmp(s->installed, floor) < 0) {
		s->state = DXR_ST_TOO_OLD;
	} else if (want_latest && target != NULL && (dxr_vercmp(s->installed, target) < 0 || s->rebuild)) {
		s->state = DXR_ST_TOO_OLD;
		if (s->rebuild) {
			append_reason(s, "same version, different build than the one installed");
		}
	} else {
		s->state = DXR_ST_OK;
		return;
	}
	// Fetchable only if the catalog offers a version that satisfies the floor
	// and is not a downgrade.
	if (target == NULL || (floor != NULL && dxr_vercmp(target, floor) < 0) ||
	    (have && !s->rebuild && dxr_vercmp(target, s->installed) <= 0)) {
		const cJSON *listing = jobj(platform_asset(item), "listing");
		if (listing != NULL && jstr(listing, "url") != NULL) {
			char buf[300];
			(void)snprintf(buf, sizeof(buf), "listed only, get it from %s", jstr(listing, "url"));
			append_reason(s, buf);
		} else {
			append_reason(s, target == NULL ? "no signed Windows build in the catalog"
			                                : "the catalog's version does not satisfy the requirement");
		}
		s->state = DXR_ST_NOT_FETCHABLE;
		return;
	}
	char why[256];
	if (dxr_asset_check(b->c, item, asset, NULL, why, sizeof(why)) != DXR_GET_OK) {
		append_reason(s, why);
		s->state = DXR_ST_NOT_FETCHABLE;
	}
}

//! Class ABI majors the runtime at @p rt_eff implements. false when nothing says.
static bool
runtime_abi(struct build *b, const char *item_class, const char *rt_eff, bool rt_moves, int *lo, int *hi)
{
	uint32_t v = 0;
	if (!rt_moves && b->env->assume_runtime != NULL) {
		// An assumed runtime: only the caller knows what it implements. What
		// the registry says describes the runtime that is installed NOW.
		for (int i = 0; i < b->env->n_assume_abi; i++) {
			if (strcmp(b->env->assume_abi[i].item_class, item_class) == 0) {
				*lo = *hi = b->env->assume_abi[i].major;
				return true;
			}
		}
	} else if (!rt_moves && b->env->reg_dword != NULL && b->env->reg_sz != NULL) {
		// The installed runtime publishes its own majors; that is the truth
		// while the runtime is not being replaced in this pass -- and only
		// while the values are that runtime's. An older runtime installed over
		// a newer one does not know the Abi key and leaves it behind, so the
		// values carry the version that wrote them, and a stamp that is absent
		// or names another version means "nothing says": not checked.
		char stamp[64];
		if (rt_eff != NULL && b->env->reg_sz(b->env->user, RUNTIME_ABI_STAMP, stamp, sizeof(stamp)) &&
		    strcmp(stamp, rt_eff) == 0 && b->env->reg_dword(b->env->user, RUNTIME_ABI_KEY, item_class, &v)) {
			*lo = *hi = (int)v;
			return true;
		}
	}
	// Otherwise the catalog's runtime item describes exactly one version.
	const cJSON *rt = b->runtime;
	const char *ver = jstr(dxr_item_asset(rt), "version");
	const cJSON *range = jobj(jobj(rt, "abi"), item_class);
	const cJSON *mn = range ? cJSON_GetObjectItemCaseSensitive(range, "min") : NULL;
	const cJSON *mx = range ? cJSON_GetObjectItemCaseSensitive(range, "max") : NULL;
	if (ver != NULL && rt_eff != NULL && rt_eff[0] != '\0' && dxr_vercmp(ver, rt_eff) == 0 && cJSON_IsNumber(mn) &&
	    cJSON_IsNumber(mx)) {
		*lo = mn->valueint;
		*hi = mx->valueint;
		return true;
	}
	return false;
}

/*!
 * Does the item's `runtime` range or class ABI range exclude the runtime this
 * machine has (or will have after this plan)? true = excluded, with the
 * reason. @p note receives what could not be checked.
 */
static bool
incompatible(struct build *b,
             const cJSON *item,
             const char *rt_eff,
             bool rt_moves,
             char *reason,
             size_t reason_size,
             char *note,
             size_t note_size)
{
	reason[0] = '\0';
	note[0] = '\0';
	const cJSON *range = jobj(item, "runtime");
	const char *mn = jstr(range, "min"), *mx = jstr(range, "max");
	const bool have_rt = rt_eff != NULL && rt_eff[0] != '\0';
	if (have_rt && mx != NULL && dxr_vercmp(rt_eff, mx) > 0) {
		(void)snprintf(
		    reason, reason_size,
		    "%s works with DisplayXR runtime %s%s%s up to %s; this machine %s %s (the runtime is never "
		    "downgraded to fit)",
		    dxr_item_label(item), mn ? "from " : "", mn ? mn : "", mn ? "" : "versions", mx,
		    rt_moves ? "would get" : "has", rt_eff);
		return true;
	}
	if (have_rt && mn != NULL && dxr_vercmp(rt_eff, mn) < 0) {
		(void)snprintf(reason, reason_size, "%s needs DisplayXR runtime %s or newer; this machine %s %s",
		               dxr_item_label(item), mn, rt_moves ? "would get" : "has", rt_eff);
		return true;
	}
	const cJSON *abi = jobj(item, "abi");
	const cJSON *e;
	cJSON_ArrayForEach(e, abi)
	{
		const cJSON *lo = cJSON_GetObjectItemCaseSensitive(e, "min");
		const cJSON *hi = cJSON_GetObjectItemCaseSensitive(e, "max");
		if (e->string == NULL || !cJSON_IsNumber(lo) || !cJSON_IsNumber(hi)) {
			continue;
		}
		int rlo = 0, rhi = 0;
		if (!have_rt || !runtime_abi(b, e->string, rt_eff, rt_moves, &rlo, &rhi)) {
			(void)snprintf(note, note_size, "%s ABI not checked: the runtime does not publish it",
			               e->string);
			continue;
		}
		if (hi->valueint < rlo || lo->valueint > rhi) {
			char want[32], got[32];
			if (lo->valueint == hi->valueint) {
				(void)snprintf(want, sizeof(want), "%d", lo->valueint);
			} else {
				(void)snprintf(want, sizeof(want), "%d to %d", lo->valueint, hi->valueint);
			}
			if (rlo == rhi) {
				(void)snprintf(got, sizeof(got), "%d", rlo);
			} else {
				(void)snprintf(got, sizeof(got), "%d to %d", rlo, rhi);
			}
			(void)snprintf(
			    reason, reason_size,
			    "%s is built for %s ABI %s; DisplayXR runtime %s implements %s, and would refuse to "
			    "load it",
			    dxr_item_label(item), e->string, want, rt_eff, got);
			return true;
		}
	}
	return false;
}

//! What is still to fetch for each step, and the plan's totals. Run once, when the steps are final.
static void
cache_and_totals(const struct dxr_env *env, struct dxr_plan *p)
{
	p->total_size = 0;
	p->total_download = 0;
	p->todo = 0;
	for (int i = 0; i < p->n; i++) {
		struct dxr_step *s = &p->steps[i];
		s->download = s->size;
		if (!dxr_step_needs_work(s)) {
			continue;
		}
		const char *sha = jstr(s->asset, "sha256"), *file = jstr(s->asset, "file");
		if (env->cached_bytes != NULL && sha != NULL && file != NULL) {
			uint64_t got = env->cached_bytes(env->user, sha, file, (uint64_t)s->size);
			s->download = (double)got >= s->size ? 0 : s->size - (double)got;
		}
		p->total_size += s->size;
		p->total_download += s->download;
		p->todo++;
	}
}

static void
push_step(struct build *b, const struct dxr_step *s)
{
	if (b->p->n < DXR_MAX_STEPS) {
		b->p->steps[b->p->n++] = *s;
	}
}

//! `--no-runtime`: the runtime step that would need an installer run is refused instead.
static void
refuse_runtime_step(struct build *b, struct dxr_step *s, const char *who)
{
	char buf[640];
	if (b->rt_floor != NULL && s->installed[0] != '\0') {
		(void)snprintf(buf, sizeof(buf),
		               "%s needs DisplayXR runtime %s or newer; this machine has %s, and the runtime is not "
		               "installed or upgraded in this run (--no-runtime)",
		               who, b->rt_floor, s->installed);
	} else if (s->installed[0] != '\0') {
		(void)snprintf(buf, sizeof(buf),
		               "DisplayXR runtime %s is installed, and the runtime is not upgraded in this run "
		               "(--no-runtime)",
		               s->installed);
	} else {
		(void)snprintf(buf, sizeof(buf),
		               "%s needs the DisplayXR runtime, which is not installed, and the runtime is not "
		               "installed in this run (--no-runtime)",
		               who);
	}
	s->state = DXR_ST_NOT_FETCHABLE;
	copy(s->reason, sizeof(s->reason), "not installed or upgraded in this run (--no-runtime)");
	set_gate(b->p, DXR_GET_INCOMPATIBLE, buf);
}

/*!
 * The item's external prerequisites: detected and reported, and, for one that
 * carries its own installer asset and is absent, planned as a step that runs
 * before the item. @p install_assets: the item is going to be installed (by
 * this plan or by the caller), so an absent prerequisite's asset is wanted.
 */
static void
plan_externals(struct build *b, const cJSON *item, bool install_assets)
{
	const cJSON *r;
	cJSON_ArrayForEach(r, jarr(item, "requires"))
	{
		const char *kind = jstr(r, "kind");
		if (kind == NULL || strcmp(kind, "external") != 0) {
			continue;
		}
		const char *name = jstr(r, "name") ? jstr(r, "name") : "(unnamed prerequisite)";
		const cJSON *asset = jobj(jobj(r, "asset"), DXR_PLATFORM);
		char why[256];
		const bool met = dxr_detect_rules(jarr(r, "detect"), b->env, false, why, sizeof(why));
		if (b->p->n_prereqs < DXR_MAX_PREREQS) {
			struct dxr_prereq *q = &b->p->prereqs[b->p->n_prereqs++];
			memset(q, 0, sizeof(*q));
			copy(q->name, sizeof(q->name), name);
			copy(q->for_id, sizeof(q->for_id), dxr_item_id(item));
			copy(q->info_url, sizeof(q->info_url), jstr(r, "info_url"));
			copy(q->evidence, sizeof(q->evidence), why);
			q->met = met;
			q->installable = asset != NULL;
		}
		// Installed only when absent, never to upgrade one that is present.
		if (met || asset == NULL || !install_assets) {
			continue;
		}
		struct dxr_step s;
		memset(&s, 0, sizeof(s));
		s.kind = DXR_STEP_EXTERNAL;
		s.item = item;
		s.asset = asset;
		s.hash_match = -1;
		s.needed = true;
		copy(s.component, sizeof(s.component), name);
		copy(s.id, sizeof(s.id), dxr_item_id(item));
		copy(s.label, sizeof(s.label), name);
		s.target = jstr(asset, "version");
		const cJSON *sz = cJSON_GetObjectItemCaseSensitive(asset, "size");
		s.size = cJSON_IsNumber(sz) ? sz->valuedouble : 0;
		s.state = DXR_ST_MISSING;
		(void)snprintf(s.reason, sizeof(s.reason), "prerequisite of %s", dxr_item_label(item));
		// Signed by a signer delegated to the same publisher, like any asset.
		if (dxr_asset_check(b->c, item, asset, NULL, why, sizeof(why)) != DXR_GET_OK) {
			append_reason(&s, why);
			s.state = DXR_ST_NOT_FETCHABLE;
		}
		push_step(b, &s);
	}
}

/*!
 * Builds a plan. DXR_PLAN_ENSURE: for the one item @p targets[0] (may be NULL:
 * an id the catalog does not know still gets its runtime looked at).
 * DXR_PLAN_INSTALL: for every item of @p targets, each with a reason in
 * @p whys (may be NULL).
 */
static void
build_plan(const struct dxr_catalog *c,
           const struct dxr_env *env,
           const char *for_name,
           const cJSON *const *targets,
           const char *const *whys,
           int n_targets,
           const char *min_runtime,
           enum dxr_plan_mode mode,
           const struct dxr_plan_opts *opts,
           struct dxr_plan *p)
{
	// Too large for the stack of a small tool.
	static struct build bs;
	struct build *b = &bs;
	memset(b, 0, sizeof(*b));
	memset(p, 0, sizeof(*p));
	p->for_name = for_name;
	p->schema = c->schema;
	b->c = c;
	b->env = env;
	b->p = p;
	b->no_runtime = opts != NULL && opts->no_runtime;
	b->runtime = dxr_runtime_item(c);
	const cJSON *x = n_targets == 1 ? targets[0] : NULL;
	char buf[640], note[160];

	if (mode == DXR_PLAN_ENSURE) {
		if (x != NULL) {
			collect_requires(b, x, true);
		}
		// The caller's own floor replaces the catalog's for X (schema-1 contract).
		if (min_runtime != NULL) {
			b->rt_floor = min_runtime;
			b->rt_floor_from_range = false;
			b->rt_floor_owner = x;
		}
		// Hardware items: positive evidence only (a high-confidence rule).
		const cJSON *it;
		cJSON_ArrayForEach(it, c->items)
		{
			char why[256];
			if (it == x || it == b->runtime || dxr_item_find(c, dxr_item_id(it)) != it ||
			    !dxr_detect_rules(jarr(it, "detect"), env, true, why, sizeof(why))) {
				continue;
			}
			(void)snprintf(buf, sizeof(buf), "detected: %s", why);
			collect(b, it, NULL, false, false, false, buf);
		}
	} else {
		for (int i = 0; i < n_targets; i++) {
			collect(b, targets[i], NULL, true, true, true, whys != NULL && whys[i] != NULL ? whys[i] : "");
		}
		raise_floor(b, min_runtime, false, x);
	}

	// The runtime first. `ensure-for` always looks at it (schema-1 contract);
	// `install` only when something asks for a floor.
	const bool x_is_runtime = x != NULL && x == b->runtime;
	char rt_eff[64] = {0};
	bool rt_moves = false;
	if (!x_is_runtime && (mode == DXR_PLAN_ENSURE || b->rt_floor != NULL)) {
		struct dxr_step s;
		if (b->runtime != NULL) {
			plan_step(b, b->runtime, b->rt_floor, false, &s);
		} else {
			// No runtime item in the catalog: it can still be present.
			memset(&s, 0, sizeof(s));
			s.hash_match = -1;
			copy(s.component, sizeof(s.component), "runtime");
			copy(s.id, sizeof(s.id), "runtime");
			copy(s.label, sizeof(s.label), "DisplayXR runtime");
			if (b->rt_floor != NULL) {
				(void)snprintf(s.required, sizeof(s.required), ">=%s", b->rt_floor);
			}
			const bool have = runtime_installed(env, s.installed, sizeof(s.installed));
			if (!have || (b->rt_floor != NULL && dxr_vercmp(s.installed, b->rt_floor) < 0)) {
				s.state = DXR_ST_NOT_FETCHABLE;
				copy(s.reason, sizeof(s.reason), "no signed Windows build in the catalog");
			}
		}
		s.needed = true;
		if (b->no_runtime && dxr_step_needs_work(&s)) {
			refuse_runtime_step(b, &s,
			                    b->rt_floor_owner != NULL ? dxr_item_label(b->rt_floor_owner)
			                                              : (for_name ? for_name : "this item"));
		}
		rt_moves = dxr_step_needs_work(&s);
		copy(rt_eff, sizeof(rt_eff), rt_moves ? s.target : s.installed);
		if (s.state == DXR_ST_NOT_FETCHABLE) {
			if (b->rt_floor_from_range && b->rt_floor_owner != NULL) {
				(void)snprintf(
				    buf, sizeof(buf),
				    "%s needs DisplayXR runtime %s or newer; this machine has %s and the catalog "
				    "offers %s",
				    dxr_item_label(b->rt_floor_owner), b->rt_floor,
				    s.installed[0] ? s.installed : "none", s.target ? s.target : "none");
				set_gate(p, DXR_GET_INCOMPATIBLE, buf);
			} else {
				(void)snprintf(buf, sizeof(buf), "%s: %s", s.label, s.reason);
				set_gate(p, DXR_GET_NOT_FETCHABLE, buf);
			}
		}
		push_step(b, &s);
	} else if (b->runtime != NULL) {
		(void)dxr_installed_version(env, b->runtime, rt_eff, sizeof(rt_eff));
	} else {
		(void)runtime_installed(env, rt_eff, sizeof(rt_eff));
	}

	// `ensure-for X`: X itself is the caller's to install, but whether it can
	// work here, and what it needs from outside, is this plan's to say.
	if (mode == DXR_PLAN_ENSURE && x != NULL && !x_is_runtime) {
		if (incompatible(b, x, rt_eff, rt_moves, buf, sizeof(buf), note, sizeof(note))) {
			set_gate(p, DXR_GET_INCOMPATIBLE, buf);
		}
		plan_externals(b, x, true);
	}

	for (int i = 0; i < b->n; i++) {
		const struct node *nd = &b->nodes[i];
		struct dxr_step s;
		// A detected item follows the runtime: a new runtime makes an older
		// plug-in ABI-stale, and the catalog's pins are a matched set.
		// Otherwise it is installed only when absent.
		const bool is_runtime = nd->item == b->runtime;
		plan_step(b, nd->item, nd->floor, nd->want_latest || (!nd->needed && rt_moves), &s);
		s.needed = nd->needed;
		s.is_target = nd->is_target;
		if (nd->why[0] != '\0') {
			append_reason(&s, nd->why);
		}
		if (is_runtime) {
			if (b->no_runtime && dxr_step_needs_work(&s)) {
				refuse_runtime_step(b, &s, s.label);
			}
			rt_moves = dxr_step_needs_work(&s);
			copy(rt_eff, sizeof(rt_eff), rt_moves ? s.target : s.installed);
		} else if (incompatible(b, nd->item, rt_eff, rt_moves, buf, sizeof(buf), note, sizeof(note))) {
			s.state = DXR_ST_INCOMPATIBLE;
			copy(s.reason, sizeof(s.reason), buf);
			if (nd->needed) {
				set_gate(p, DXR_GET_INCOMPATIBLE, buf);
			}
		} else if (note[0] != '\0') {
			append_reason(&s, note);
		}
		if (s.state == DXR_ST_NOT_FETCHABLE && nd->needed) {
			(void)snprintf(buf, sizeof(buf), "%s: %s", s.label, s.reason);
			set_gate(p, DXR_GET_NOT_FETCHABLE, buf);
		}
		plan_externals(b, nd->item, dxr_step_needs_work(&s));
		push_step(b, &s);
	}
	cache_and_totals(env, p);
}

void
dxr_make_plan(const struct dxr_catalog *c,
              const struct dxr_env *env,
              const char *for_name,
              const char *min_runtime,
              enum dxr_plan_mode mode,
              const struct dxr_plan_opts *opts,
              struct dxr_plan *p)
{
	const cJSON *x = dxr_item_find(c, for_name);
	if (mode == DXR_PLAN_INSTALL && x == NULL) {
		memset(p, 0, sizeof(*p));
		p->for_name = for_name;
		p->schema = c->schema;
		(void)snprintf(p->gate_reason, sizeof(p->gate_reason), "\"%s\" is not in the catalog",
		               for_name ? for_name : "");
		p->gate = DXR_GET_NOT_FETCHABLE;
		return;
	}
	build_plan(c, env, for_name, &x, NULL, 1, min_runtime, mode, opts, p);
}

void
dxr_make_default_plan(const struct dxr_catalog *c,
                      const struct dxr_env *env,
                      const struct dxr_plan_opts *opts,
                      struct dxr_plan *p)
{
	static struct dxr_plan one;
	static struct dxr_step skipped[DXR_MAX_STEPS];
	static char whys[DXR_MAX_STEPS][256];
	const cJSON *targets[DXR_MAX_STEPS];
	const char *why_ptrs[DXR_MAX_STEPS];
	int n = 0, n_skipped = 0;
	const cJSON *it;
	cJSON_ArrayForEach(it, c->items)
	{
		const char *id = dxr_item_id(it);
		const cJSON *rec = cJSON_GetObjectItemCaseSensitive(it, "recommended");
		char why[200];
		if (id == NULL || dxr_item_find(c, id) != it || it == dxr_runtime_item(c) || n >= DXR_MAX_STEPS) {
			continue;
		}
		// Hardware evidence outranks the recommendation as the stated reason.
		if (dxr_detect_rules(jarr(it, "detect"), env, true, why, sizeof(why))) {
			(void)snprintf(whys[n], sizeof(whys[n]), "detected: %s", why);
		} else if (cJSON_IsTrue(rec)) {
			copy(whys[n], sizeof(whys[n]), "recommended");
		} else {
			continue;
		}
		// Judged alone first, so one item that cannot be installed here does
		// not raise a floor, or set a refusal, for the rest of the set.
		dxr_make_plan(c, env, id, NULL, DXR_PLAN_INSTALL, opts, &one);
		if (one.gate != DXR_GET_OK) {
			struct dxr_step *s = &skipped[n_skipped++];
			memset(s, 0, sizeof(*s));
			s->hash_match = -1;
			for (int i = 0; i < one.n; i++) {
				if (one.steps[i].is_target) {
					*s = one.steps[i];
				}
			}
			s->item = it;
			copy(s->component, sizeof(s->component), dxr_item_legacy_name(it));
			copy(s->id, sizeof(s->id), id);
			copy(s->label, sizeof(s->label), dxr_item_label(it));
			s->state = one.gate == DXR_GET_INCOMPATIBLE ? DXR_ST_INCOMPATIBLE : DXR_ST_NOT_FETCHABLE;
			s->needed = false;
			s->is_target = true;
			copy(s->reason, sizeof(s->reason), one.gate_reason);
			append_reason(s, whys[n]);
			continue;
		}
		targets[n] = it;
		why_ptrs[n] = whys[n];
		n++;
	}
	build_plan(c, env, "(default)", targets, why_ptrs, n, NULL, DXR_PLAN_INSTALL, opts, p);
	for (int i = 0; i < n_skipped && p->n < DXR_MAX_STEPS; i++) {
		p->steps[p->n++] = skipped[i];
	}
	cache_and_totals(env, p);
}

static const char *
action_of(const struct dxr_step *s)
{
	if (s->state == DXR_ST_MISSING) {
		return "install";
	}
	if (s->state == DXR_ST_TOO_OLD) {
		return s->rebuild ? "reinstall" : "upgrade";
	}
	return s->state == DXR_ST_OK ? "none" : "skip";
}

char *
dxr_plan_json(const struct dxr_plan *p)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "for", p->for_name ? p->for_name : "");
	cJSON_AddNumberToObject(o, "schema", p->schema);
	cJSON_AddNumberToObject(o, "gate", p->gate);
	if (p->gate_reason[0]) {
		cJSON_AddStringToObject(o, "gate_reason", p->gate_reason);
	}
	cJSON_AddNumberToObject(o, "total_size", p->total_size);
	cJSON_AddNumberToObject(o, "total_download", p->total_download);
	/*
	 * One FLAT object per action, `component` first: deployed installers scan
	 * this with a string search from `"component":"` to the first `}` and
	 * read `state`, `target` and `size` from that span (the DisplayXR Browser
	 * installer's PlanSummary). Never nest an object or an array in here.
	 */
	cJSON *arr = cJSON_AddArrayToObject(o, "actions");
	for (int i = 0; i < p->n; i++) {
		const struct dxr_step *s = &p->steps[i];
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "component", s->component);
		if (s->installed[0]) {
			cJSON_AddStringToObject(x, "installed", s->installed);
		} else {
			cJSON_AddNullToObject(x, "installed");
		}
		cJSON_AddStringToObject(x, "state", k_state_names[s->state]);
		if (s->required[0]) {
			cJSON_AddStringToObject(x, "required", s->required);
		}
		if (s->target) {
			cJSON_AddStringToObject(x, "target", s->target);
		}
		cJSON_AddNumberToObject(x, "size", s->size);
		if (s->reason[0]) {
			cJSON_AddStringToObject(x, "reason", s->reason);
		}
		// Schema-2 additions, after the schema-1 fields.
		cJSON_AddStringToObject(x, "id", s->id);
		cJSON_AddStringToObject(x, "kind", s->kind == DXR_STEP_EXTERNAL ? "external" : "item");
		cJSON_AddStringToObject(x, "action", action_of(s));
		cJSON_AddBoolToObject(x, "current", s->current);
		if (s->hash_match >= 0) {
			cJSON_AddBoolToObject(x, "hash_match", s->hash_match == 1);
		}
		cJSON_AddNumberToObject(x, "download", dxr_step_needs_work(s) ? s->download : 0);
		cJSON_AddItemToArray(arr, x);
	}
	cJSON *pre = cJSON_AddArrayToObject(o, "prerequisites");
	for (int i = 0; i < p->n_prereqs; i++) {
		const struct dxr_prereq *q = &p->prereqs[i];
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "name", q->name);
		cJSON_AddStringToObject(x, "for", q->for_id);
		cJSON_AddBoolToObject(x, "met", q->met);
		cJSON_AddBoolToObject(x, "installable", q->installable);
		if (q->info_url[0]) {
			cJSON_AddStringToObject(x, "info_url", q->info_url);
		}
		cJSON_AddItemToArray(pre, x);
	}
	char *s = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	return s;
}

void
dxr_plan_text(const struct dxr_plan *p, FILE *out)
{
	double cached = p->total_size - p->total_download;
	for (int i = 0; i < p->n; i++) {
		const struct dxr_step *s = &p->steps[i];
		if (s->state == DXR_ST_MISSING) {
			(void)fprintf(out, "%s: not installed -> install %s (%.1f MB)\n", s->label, s->target,
			              s->size / 1048576.0);
		} else if (s->state == DXR_ST_TOO_OLD && s->rebuild) {
			(void)fprintf(out, "%s: %s is a different build -> reinstall %s (%.1f MB)\n", s->label,
			              s->installed, s->target, s->size / 1048576.0);
		} else if (s->state == DXR_ST_TOO_OLD) {
			(void)fprintf(out, "%s: %s is too old -> update to %s (%.1f MB)\n", s->label, s->installed,
			              s->target, s->size / 1048576.0);
		} else if (s->state == DXR_ST_NOT_FETCHABLE) {
			(void)fprintf(out, "%s: %s, but %s\n", s->label, s->installed[0] ? "too old" : "not installed",
			              s->reason);
		} else if (s->state == DXR_ST_INCOMPATIBLE) {
			(void)fprintf(out, "%s: not compatible -> skipped. %s\n", s->label, s->reason);
		} else if (s->current) {
			(void)fprintf(out, "%s: %s is installed and is the catalog's version -> not downloaded\n",
			              s->label, s->installed);
		} else {
			(void)fprintf(out, "%s: %s is installed and satisfies the requirement -> not downloaded\n",
			              s->label, s->installed);
		}
	}
	for (int i = 0; i < p->n_prereqs; i++) {
		const struct dxr_prereq *q = &p->prereqs[i];
		if (q->met) {
			(void)fprintf(out, "Prerequisite present: %s\n", q->name);
		} else if (!q->installable) {
			(void)fprintf(out,
			              "Prerequisite MISSING: %s (needed by %s). DisplayXR does not install it%s%s\n",
			              q->name, q->for_id, q->info_url[0] ? "; get it from " : "", q->info_url);
		}
	}
	if (p->gate == DXR_GET_INCOMPATIBLE) {
		(void)fprintf(out, "Not compatible: %s\n", p->gate_reason);
	}
	if (p->todo == 0) {
		(void)fprintf(out, "Nothing to install.\n");
	} else {
		(void)fprintf(out, "Total download: %.1f MB\n", p->total_size / 1048576.0);
		if (cached > 0) {
			(void)fprintf(out, "Already downloaded: %.1f MB, still to fetch: %.1f MB\n", cached / 1048576.0,
			              p->total_download / 1048576.0);
		}
	}
	(void)fflush(out);
}

/*
 * Inventory.
 */

cJSON *
dxr_list_catalog(const struct dxr_catalog *c, const struct dxr_env *env, bool outdated_only, bool fixed_runtime)
{
	static struct dxr_plan plan;
	const struct dxr_plan_opts opts = {fixed_runtime};
	cJSON *arr = cJSON_CreateArray();
	const cJSON *it;
	cJSON_ArrayForEach(it, c->items)
	{
		const char *id = dxr_item_id(it);
		if (id == NULL || dxr_item_find(c, id) != it) {
			continue;
		}
		const cJSON *asset = dxr_item_asset(it);
		const char *target = jstr(asset, "version");
		char installed[64];
		const bool have = dxr_installed_version(env, it, installed, sizeof(installed));
		struct dxr_receipt rc;
		const bool receipt = env->receipt != NULL && env->receipt(env->user, id, &rc);
		const char *sha = jstr(asset, "sha256");
		const char *state = "not_installed";
		if (have && target != NULL) {
			int cmp = dxr_vercmp(installed, target);
			state = cmp < 0 ? "outdated" : (cmp > 0 ? "newer" : "current");
			if (cmp == 0 && receipt && sha != NULL && rc.sha256[0] != '\0' &&
			    dxr_vercmp(rc.version, installed) == 0 && strcmp(rc.sha256, sha) != 0) {
				state = "rebuilt"; // same version, another build than the one installed
			}
		} else if (have) {
			state = "installed";
		}
		if (outdated_only && strcmp(state, "outdated") != 0 && strcmp(state, "rebuilt") != 0) {
			continue;
		}
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "id", id);
		cJSON_AddStringToObject(x, "name", dxr_item_label(it));
		cJSON_AddStringToObject(x, "class", dxr_item_class(it));
		if (jstr(it, "registered_id")) {
			cJSON_AddStringToObject(x, "registered_id", jstr(it, "registered_id"));
		}
		cJSON_AddStringToObject(x, "publisher", jstr(it, "publisher") ? jstr(it, "publisher") : "");
		cJSON_AddStringToObject(x, "tier", jstr(it, "tier") ? jstr(it, "tier") : "listed");
		if (have) {
			cJSON_AddStringToObject(x, "installed", installed);
		} else {
			cJSON_AddNullToObject(x, "installed");
		}
		if (target != NULL) {
			cJSON_AddStringToObject(x, "available", target);
		} else {
			cJSON_AddNullToObject(x, "available");
		}
		cJSON_AddStringToObject(x, "state", state);
		cJSON_AddBoolToObject(x, "installable", asset != NULL);
		cJSON_AddBoolToObject(x, "receipt", receipt);
		const char *listing = jstr(jobj(platform_asset(it), "listing"), "url");
		if (listing == NULL) {
			listing = jstr(jobj(it, "links"), "listing");
		}
		if (listing != NULL) {
			cJSON_AddStringToObject(x, "listing", listing);
		}

		// What a setup page needs beyond the state, so one call answers it.
		if (jstr(it, "summary")) {
			cJSON_AddStringToObject(x, "summary", jstr(it, "summary"));
		}
		const cJSON *aliases = jarr(it, "aliases");
		cJSON_AddItemToObject(x, "aliases", aliases ? cJSON_Duplicate(aliases, 1) : cJSON_CreateArray());
		cJSON_AddBoolToObject(x, "recommended",
		                      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "recommended")));
		char why[256];
		const cJSON *rules = jarr(it, "detect");
		const bool high = dxr_detect_rules(rules, env, true, why, sizeof(why));
		const bool any = high || dxr_detect_rules(rules, env, false, why, sizeof(why));
		cJSON_AddStringToObject(x, "detected", high ? "high" : (any ? "low" : "none"));
		if (any) {
			cJSON_AddStringToObject(x, "evidence", why);
		}
		const cJSON *sz = asset ? cJSON_GetObjectItemCaseSensitive(asset, "size") : NULL;
		cJSON_AddNumberToObject(x, "size", cJSON_IsNumber(sz) ? sz->valuedouble : 0);

		// `install <id>` is the question: what would it fetch for this item, is
		// the item compatible with the runtime, what does it need from outside.
		// The runtime item is the yardstick, not a thing to judge against itself.
		double download = 0;
		bool compatible = true;
		cJSON *pre = cJSON_CreateArray();
		const char *cls = dxr_item_class(it);
		if (cls != NULL && strcmp(cls, "runtime") == 0) {
			if (asset != NULL && (strcmp(state, "not_installed") == 0 || strcmp(state, "outdated") == 0 ||
			                      strcmp(state, "rebuilt") == 0)) {
				download = cJSON_IsNumber(sz) ? sz->valuedouble : 0;
				if (env->cached_bytes != NULL && sha != NULL && jstr(asset, "file") != NULL) {
					double got = (double)env->cached_bytes(env->user, sha, jstr(asset, "file"),
					                                       (uint64_t)download);
					download = got >= download ? 0 : download - got;
				}
			}
		} else {
			dxr_make_plan(c, env, id, NULL, DXR_PLAN_INSTALL, &opts, &plan);
			compatible = plan.gate != DXR_GET_INCOMPATIBLE;
			for (int i = 0; i < plan.n; i++) {
				// The item's own asset and its external prerequisites' assets;
				// not the runtime or other items, which have rows of their own.
				if (plan.steps[i].item == it && dxr_step_needs_work(&plan.steps[i])) {
					download += plan.steps[i].download;
				}
			}
			for (int i = 0; i < plan.n_prereqs; i++) {
				const struct dxr_prereq *q = &plan.prereqs[i];
				if (strcmp(q->for_id, id) != 0) {
					continue;
				}
				cJSON *e = cJSON_CreateObject();
				cJSON_AddStringToObject(e, "name", q->name);
				cJSON_AddBoolToObject(e, "met", q->met);
				cJSON_AddBoolToObject(e, "installable", q->installable);
				if (q->info_url[0]) {
					cJSON_AddStringToObject(e, "info_url", q->info_url);
				}
				cJSON_AddItemToArray(pre, e);
			}
		}
		cJSON_AddNumberToObject(x, "download", compatible ? download : 0);
		cJSON_AddBoolToObject(x, "compatible", compatible);
		if (!compatible) {
			cJSON_AddStringToObject(x, "incompatible_reason", plan.gate_reason);
		}
		cJSON_AddItemToObject(x, "prerequisites", pre);
		cJSON_AddItemToArray(arr, x);
	}
	return arr;
}

cJSON *
dxr_list_receipts(const struct dxr_receipt *r, int n)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < n; i++) {
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "id", r[i].id);
		cJSON_AddStringToObject(x, "name", r[i].name[0] ? r[i].name : r[i].id);
		cJSON_AddStringToObject(x, "class", r[i].item_class);
		cJSON_AddStringToObject(x, "publisher", r[i].publisher);
		cJSON_AddStringToObject(x, "version", r[i].version);
		cJSON_AddStringToObject(x, "source_url", r[i].source_url);
		cJSON_AddStringToObject(x, "sha256", r[i].sha256);
		cJSON_AddStringToObject(x, "install_time", r[i].install_time);
		cJSON_AddBoolToObject(x, "can_uninstall", r[i].uninstall[0] != '\0');
		cJSON_AddStringToObject(x, "uninstall_command", r[i].uninstall);
		cJSON_AddItemToArray(arr, x);
	}
	return arr;
}

int
dxr_uninstall_check(const struct dxr_receipt *r,
                    const struct dxr_receipt *all,
                    int n,
                    bool elevated,
                    const char **result,
                    char *detail,
                    size_t detail_size)
{
	char deps[400];
	detail[0] = '\0';
	// What elevation cannot fix comes first: a caller told "needs elevation"
	// would elevate, run again, and only then learn it could never work.
	if (r == NULL) {
		*result = "not_installed";
		copy(
		    detail, detail_size,
		    "dxr-get has no install receipt for this id (see `dxr-get list --installed`). A component that was "
		    "installed by its own installer or by the bundle is removed from Windows Settings > Apps > "
		    "Installed "
		    "apps");
		return DXR_GET_NOT_INSTALLED;
	}
	if (r->uninstall[0] == '\0') {
		*result = "no_uninstall_command";
		(void)snprintf(
		    detail, detail_size,
		    "%s was installed from a catalog entry that names no uninstall command, so dxr-get cannot "
		    "remove it. That is the case for every item of the schema-1 catalog, and for a schema-2 "
		    "item without an `uninstall` rule. Remove it from Windows Settings > Apps > Installed apps "
		    "(or run its own uninstaller). Nothing was changed",
		    r->name[0] ? r->name : r->id);
		return DXR_GET_CANNOT_UNINSTALL;
	}
	if (dxr_receipt_dependents(all, n, r->id, deps, sizeof(deps)) > 0) {
		*result = "required_by_others";
		(void)snprintf(detail, detail_size, "still required by: %s. Uninstall those first", deps);
		return DXR_GET_CANNOT_UNINSTALL;
	}
	if (!elevated) {
		*result = "needs_elevation";
		copy(detail, detail_size, "uninstalling needs an elevated process");
		return DXR_GET_NEEDS_ADMIN;
	}
	*result = "ok";
	return DXR_GET_OK;
}

int
dxr_receipt_dependents(const struct dxr_receipt *all, int n, const char *id, char *out, size_t out_size)
{
	int count = 0;
	out[0] = '\0';
	const size_t idl = strlen(id);
	for (int i = 0; i < n; i++) {
		if (strcmp(all[i].id, id) == 0) {
			continue;
		}
		for (const char *p = all[i].requires_ids; *p != '\0';) {
			const char *e = strchr(p, ',');
			size_t l = e ? (size_t)(e - p) : strlen(p);
			if (l == idl && strncmp(p, id, l) == 0) {
				size_t ol = strlen(out);
				(void)snprintf(out + ol, out_size - ol, "%s%s", ol ? ", " : "", all[i].id);
				count++;
				break;
			}
			p += l + (e ? 1 : 0);
		}
	}
	return count;
}
