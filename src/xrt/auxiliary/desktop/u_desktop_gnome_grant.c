// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: the saved portal grant. See u_desktop_gnome_grant.h.
 */

#include "u_desktop_gnome_grant.h"
#include "u_desktop.h"
#include "u_desktop_gnome_logic.h"
#include "u_desktop_policy.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORTAL_RD "remote_desktop"
#define PORTAL_SC "screencast"

const char *
u_desktop_gnome_grant_status_str(enum u_desktop_gnome_grant_status s)
{
	switch (s) {
	case U_DESKTOP_GNOME_GRANT_NONE: return "none";
	case U_DESKTOP_GNOME_GRANT_OK: return "ok";
	case U_DESKTOP_GNOME_GRANT_CORRUPT: return "corrupt";
	case U_DESKTOP_GNOME_GRANT_NEWER: return "newer";
	case U_DESKTOP_GNOME_GRANT_UNSAFE: return "unsafe";
	default: return "?";
	}
}

const char *
u_desktop_gnome_key_source_str(enum u_desktop_gnome_key_source s)
{
	switch (s) {
	case U_DESKTOP_GNOME_KEY_REGISTERED: return "registered";
	case U_DESKTOP_GNOME_KEY_APP_ID: return "app_id";
	case U_DESKTOP_GNOME_KEY_EXECUTABLE: return "executable";
	default: return "none";
	}
}

enum u_desktop_gnome_key_source
u_desktop_gnome_grant_key(const char *registered_id, const char *app_id, const char *exe_path, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return U_DESKTOP_GNOME_KEY_NONE;
	}
	out[0] = '\0';
	if (u_desktop_grant_key_valid(registered_id) && strlen(registered_id) < cap) {
		snprintf(out, cap, "%s", registered_id);
		return U_DESKTOP_GNOME_KEY_REGISTERED;
	}
	if (u_desktop_grant_key_valid(app_id) && strlen(app_id) < cap) {
		snprintf(out, cap, "%s", app_id);
		return U_DESKTOP_GNOME_KEY_APP_ID;
	}
	if (exe_path == NULL || exe_path[0] != '/') {
		return U_DESKTOP_GNOME_KEY_NONE;
	}
	const char *base = strrchr(exe_path, '/') + 1;
	char name[49];
	size_t n = 0;
	for (; base[n] != '\0' && n + 1 < sizeof(name); n++) {
		char c = base[n];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
		          c == '_' || c == '-';
		name[n] = ok ? c : '_';
	}
	name[n] = '\0';
	if (n == 0) {
		return U_DESKTOP_GNOME_KEY_NONE; // a path ending in '/'
	}
	int w =
	    snprintf(out, cap, "exe-%s-%016llx", name, (unsigned long long)u_desktop_fnv1a(exe_path, strlen(exe_path)));
	if (w <= 0 || (size_t)w >= cap || !u_desktop_grant_key_valid(out)) {
		out[0] = '\0';
		return U_DESKTOP_GNOME_KEY_NONE;
	}
	return U_DESKTOP_GNOME_KEY_EXECUTABLE;
}

bool
u_desktop_gnome_launch_scope(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
#ifdef __linux__
	FILE *f = fopen("/proc/self/cgroup", "r");
	if (f == NULL) {
		return false;
	}
	char line[1024];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "0::", 3) != 0) {
			continue; // cgroup v2 only
		}
		line[strcspn(line, "\n")] = '\0';
		const char *leaf = strrchr(line, '/');
		leaf = leaf != NULL ? leaf + 1 : line + 3;
		if (leaf[0] != '\0') {
			snprintf(out, cap, "%s", leaf);
			found = true;
		}
	}
	fclose(f);
	return found;
#else
	return false;
#endif
}

//! 1..cap-1 bytes, every one printable ASCII without the space (portal tokens are UUIDs).
static bool
token_ok(const char *t, size_t cap)
{
	if (t == NULL || t[0] == '\0') {
		return false;
	}
	size_t n = 0;
	for (; t[n] != '\0'; n++) {
		if (n + 1 >= cap || (unsigned char)t[n] <= 0x20 || (unsigned char)t[n] >= 0x7f) {
			return false;
		}
	}
	return true;
}

//! A connector name ("DP-1", "eDP-1", "Meta-0"): printable ASCII, fits.
static bool
connector_ok(const char *c)
{
	return token_ok(c, U_DESKTOP_GNOME_CONNECTOR_SIZE);
}

//! What a saved grant may claim: a ScreenCast token covers the screens only; a
//! RemoteDesktop one input and / or the screens. Never nothing.
static bool
shape_ok(const struct u_desktop_gnome_grant *g)
{
	if (g->nmonitors > U_DESKTOP_GNOME_GRANT_MONITORS || g->nvirtual > U_DESKTOP_GNOME_GRANT_MONITORS) {
		return false;
	}
	return g->screencast ? (g->screens && !g->devices) : (g->devices || g->screens);
}

//! Zero memory that held a token. A memset() right before free() or before a buffer
//! goes out of scope is a dead store the compiler may drop; volatile stores are kept.
static void
wipe(void *p, size_t n)
{
	volatile unsigned char *v = (volatile unsigned char *)p;
	while (n-- > 0) {
		*v++ = 0;
	}
}

char *
u_desktop_gnome_grant_encode(const char *key,
                             const struct u_desktop_gnome_grant *g,
                             const struct u_desktop_gnome_grant_origin *origin)
{
	if (g == NULL || !u_desktop_grant_key_valid(key) || !token_ok(g->token, sizeof(g->token)) || !shape_ok(g)) {
		return NULL;
	}
	cJSON *o = cJSON_CreateObject();
	if (o == NULL) {
		return NULL;
	}
	char now[32];
	u_desktop_audit_utc_now(now, sizeof(now));
	cJSON_AddNumberToObject(o, "version", U_DESKTOP_GNOME_GRANT_VERSION);
	cJSON_AddStringToObject(o, "key", key);
	if (origin != NULL && origin->source != U_DESKTOP_GNOME_KEY_NONE) {
		cJSON_AddStringToObject(o, "key_source", u_desktop_gnome_key_source_str(origin->source));
	}
	cJSON_AddStringToObject(o, "portal", g->screencast ? PORTAL_SC : PORTAL_RD);
	cJSON_AddStringToObject(o, "restore_token", g->token);
	cJSON_AddBoolToObject(o, "devices", g->devices);
	cJSON_AddBoolToObject(o, "screens", g->screens);
	cJSON *mons = cJSON_AddArrayToObject(o, "monitors");
	for (uint32_t i = 0; mons != NULL && g->screens && i < g->nmonitors; i++) {
		if (connector_ok(g->monitors[i])) {
			cJSON_AddItemToArray(mons, cJSON_CreateString(g->monitors[i]));
		}
	}
	// Optional (an older runtime ignores it): the covered screen-cast virtual monitors.
	cJSON *vmons = g->screens && g->nvirtual > 0 ? cJSON_AddArrayToObject(o, "virtual_monitors") : NULL;
	for (uint32_t i = 0; vmons != NULL && i < g->nvirtual; i++) {
		if (!connector_ok(g->virtual_monitors[i]) || !connector_ok(g->virtual_serials[i])) {
			continue;
		}
		cJSON *vm = cJSON_CreateObject();
		if (vm != NULL) {
			cJSON_AddStringToObject(vm, "connector", g->virtual_monitors[i]);
			cJSON_AddStringToObject(vm, "serial", g->virtual_serials[i]);
			cJSON_AddItemToArray(vmons, vm);
		}
	}
	cJSON_AddStringToObject(o, "saved", now);
	// Diagnostics: which id the portal bound the token to (when known), what ran.
	if (origin != NULL && origin->portal_app_id != NULL && origin->portal_app_id[0] != '\0') {
		cJSON_AddStringToObject(o, "portal_app_id", origin->portal_app_id);
	}
	if (origin != NULL && origin->exe != NULL && origin->exe[0] != '\0') {
		cJSON_AddStringToObject(o, "exe", origin->exe);
	}
	if (origin != NULL && origin->launch_scope != NULL && origin->launch_scope[0] != '\0') {
		cJSON_AddStringToObject(o, "launch_scope", origin->launch_scope);
	}
	char *txt = cJSON_Print(o);
	// cJSON keeps its own copy of the token in the tree: wipe it before freeing.
	cJSON *tok = cJSON_GetObjectItemCaseSensitive(o, "restore_token");
	if (cJSON_IsString(tok) && tok->valuestring != NULL) {
		wipe(tok->valuestring, strlen(tok->valuestring));
	}
	cJSON_Delete(o);
	if (txt == NULL) {
		return NULL;
	}
	// cJSON_Print allocates with cJSON's hooks (malloc by default): hand out a
	// plain malloc'd copy so the caller can free() it, and wipe cJSON's.
	size_t len = strlen(txt);
	char *out = (char *)malloc(len + 2);
	if (out != NULL) {
		memcpy(out, txt, len);
		out[len] = '\n';
		out[len + 1] = '\0';
	}
	wipe(txt, len);
	cJSON_free(txt);
	return out;
}

static bool
get_bool(const cJSON *o, const char *key, bool *out)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	if (!cJSON_IsBool(v)) {
		return false;
	}
	*out = cJSON_IsTrue(v);
	return true;
}

enum u_desktop_gnome_grant_status
u_desktop_gnome_grant_decode(const char *json, const char *key, struct u_desktop_gnome_grant *out)
{
	memset(out, 0, sizeof(*out));
	if (json == NULL || key == NULL) {
		return U_DESKTOP_GNOME_GRANT_CORRUPT;
	}
	cJSON *o = cJSON_Parse(json);
	if (!cJSON_IsObject(o)) {
		cJSON_Delete(o);
		return U_DESKTOP_GNOME_GRANT_CORRUPT;
	}
	enum u_desktop_gnome_grant_status st = U_DESKTOP_GNOME_GRANT_CORRUPT;
	struct u_desktop_gnome_grant g;
	memset(&g, 0, sizeof(g));
	const cJSON *ver = cJSON_GetObjectItemCaseSensitive(o, "version");
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "key");
	const cJSON *portal = cJSON_GetObjectItemCaseSensitive(o, "portal");
	const cJSON *tok = cJSON_GetObjectItemCaseSensitive(o, "restore_token");
	const cJSON *mons = cJSON_GetObjectItemCaseSensitive(o, "monitors");
	const cJSON *vmons = cJSON_GetObjectItemCaseSensitive(o, "virtual_monitors");
	if (!cJSON_IsNumber(ver) || ver->valuedouble < 1 || ver->valuedouble != (double)(int)ver->valuedouble) {
		goto done;
	}
	if ((int)ver->valuedouble > U_DESKTOP_GNOME_GRANT_VERSION) {
		st = U_DESKTOP_GNOME_GRANT_NEWER;
		goto done;
	}
	if (!cJSON_IsString(id) || strcmp(id->valuestring, key) != 0) {
		goto done; // a file under our name that is not ours
	}
	if (!cJSON_IsString(portal) ||
	    (strcmp(portal->valuestring, PORTAL_RD) != 0 && strcmp(portal->valuestring, PORTAL_SC) != 0)) {
		goto done;
	}
	g.screencast = strcmp(portal->valuestring, PORTAL_SC) == 0;
	if (!cJSON_IsString(tok) || !token_ok(tok->valuestring, sizeof(g.token))) {
		goto done;
	}
	if (!get_bool(o, "devices", &g.devices) || !get_bool(o, "screens", &g.screens)) {
		goto done;
	}
	if (mons != NULL && !cJSON_IsArray(mons)) {
		goto done;
	}
	if (mons != NULL) {
		int n = cJSON_GetArraySize(mons);
		if (n > U_DESKTOP_GNOME_GRANT_MONITORS) {
			goto done;
		}
		for (int i = 0; i < n; i++) {
			const cJSON *m = cJSON_GetArrayItem(mons, i);
			if (!cJSON_IsString(m) || !connector_ok(m->valuestring)) {
				goto done;
			}
			snprintf(g.monitors[g.nmonitors++], sizeof(g.monitors[0]), "%s", m->valuestring);
		}
	}
	if (vmons != NULL && !cJSON_IsArray(vmons)) {
		goto done;
	}
	if (vmons != NULL) {
		int n = cJSON_GetArraySize(vmons);
		if (n > U_DESKTOP_GNOME_GRANT_MONITORS) {
			goto done;
		}
		for (int i = 0; i < n; i++) {
			const cJSON *m = cJSON_GetArrayItem(vmons, i);
			const cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "connector");
			const cJSON *s = cJSON_GetObjectItemCaseSensitive(m, "serial");
			if (!cJSON_IsObject(m) || !cJSON_IsString(c) || !connector_ok(c->valuestring) ||
			    !cJSON_IsString(s) || !connector_ok(s->valuestring)) {
				goto done;
			}
			snprintf(g.virtual_monitors[g.nvirtual], sizeof(g.virtual_monitors[0]), "%s", c->valuestring);
			snprintf(g.virtual_serials[g.nvirtual], sizeof(g.virtual_serials[0]), "%s", s->valuestring);
			g.nvirtual++;
		}
	}
	if (!g.screens) {
		g.nmonitors = 0;
		g.nvirtual = 0;
	}
	if (!shape_ok(&g)) {
		goto done;
	}
	snprintf(g.token, sizeof(g.token), "%s", tok->valuestring);
	*out = g;
	st = U_DESKTOP_GNOME_GRANT_OK;
done:
	if (cJSON_IsString(tok) && tok->valuestring != NULL) {
		wipe(tok->valuestring, strlen(tok->valuestring));
	}
	cJSON_Delete(o);
	wipe(&g, sizeof(g));
	return st;
}

//! Wipe and free a text that may hold a token.
static void
free_secret(char *s)
{
	if (s != NULL) {
		wipe(s, strlen(s));
		free(s);
	}
}

enum u_desktop_gnome_grant_status
u_desktop_gnome_grant_load(const char *key, struct u_desktop_gnome_grant *out)
{
	memset(out, 0, sizeof(*out));
	char *txt = NULL;
	enum u_desktop_grant_read_result rr = u_desktop_grant_read(key, &txt);
	if (rr == U_DESKTOP_GRANT_READ_NONE) {
		return U_DESKTOP_GNOME_GRANT_NONE;
	}
	if (rr == U_DESKTOP_GRANT_READ_UNSAFE) {
		return U_DESKTOP_GNOME_GRANT_UNSAFE;
	}
	enum u_desktop_gnome_grant_status st = u_desktop_gnome_grant_decode(txt, key, out);
	free_secret(txt);
	if (st == U_DESKTOP_GNOME_GRANT_CORRUPT) {
		u_desktop_grant_remove(key);
	}
	return st;
}

bool
u_desktop_gnome_grant_save(const char *key,
                           const struct u_desktop_gnome_grant *g,
                           const struct u_desktop_gnome_grant_origin *origin)
{
	char *txt = u_desktop_gnome_grant_encode(key, g, origin);
	if (txt == NULL) {
		return false;
	}
	bool ok = u_desktop_grant_write(key, txt);
	free_secret(txt);
	return ok;
}

bool
u_desktop_gnome_grant_forget(const char *key, const char *token)
{
	if (!u_desktop_grant_key_valid(key)) {
		return false;
	}
	if (token == NULL) {
		return u_desktop_grant_remove(key);
	}
	char *txt = NULL;
	if (u_desktop_grant_read(key, &txt) != U_DESKTOP_GRANT_READ_OK) {
		return false;
	}
	struct u_desktop_gnome_grant g;
	bool mine =
	    u_desktop_gnome_grant_decode(txt, key, &g) == U_DESKTOP_GNOME_GRANT_OK && strcmp(g.token, token) == 0;
	free_secret(txt);
	wipe(&g, sizeof(g));
	return mine && u_desktop_grant_remove(key);
}

static void
copy_str(const cJSON *o, const char *k, char *out, size_t cap)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	snprintf(out, cap, "%s", cJSON_IsString(v) && v->valuestring != NULL ? v->valuestring : "");
}

//! @p info for @p key, without the token and without removing anything.
static void
read_info(const char *key, struct u_desktop_gnome_grant_info *info)
{
	memset(info, 0, sizeof(*info));
	info->key = key;
	char *txt = NULL;
	enum u_desktop_grant_read_result rr = u_desktop_grant_read(key, &txt);
	if (rr != U_DESKTOP_GRANT_READ_OK) {
		info->status =
		    rr == U_DESKTOP_GRANT_READ_UNSAFE ? U_DESKTOP_GNOME_GRANT_UNSAFE : U_DESKTOP_GNOME_GRANT_NONE;
		return;
	}
	struct u_desktop_gnome_grant g;
	info->status = u_desktop_gnome_grant_decode(txt, key, &g);
	info->screencast = g.screencast;
	info->devices = g.devices;
	info->screens = g.screens;
	wipe(&g, sizeof(g));
	cJSON *o = cJSON_Parse(txt);
	free_secret(txt);
	if (cJSON_IsObject(o)) {
		copy_str(o, "key_source", info->key_source, sizeof(info->key_source));
		copy_str(o, "portal_app_id", info->portal_app_id, sizeof(info->portal_app_id));
		copy_str(o, "exe", info->exe, sizeof(info->exe));
		copy_str(o, "launch_scope", info->launch_scope, sizeof(info->launch_scope));
		copy_str(o, "saved", info->saved, sizeof(info->saved));
		cJSON *tok = cJSON_GetObjectItemCaseSensitive(o, "restore_token");
		if (cJSON_IsString(tok) && tok->valuestring != NULL) {
			wipe(tok->valuestring, strlen(tok->valuestring));
		}
	}
	cJSON_Delete(o);
}

struct list_ctx
{
	u_desktop_gnome_grant_info_fn fn;
	void *ctx;
	const char *exe;
	uint32_t scopes;
	uint32_t removed;
};

static void
list_one(void *ctx, const char *key)
{
	struct list_ctx *l = (struct list_ctx *)ctx;
	struct u_desktop_gnome_grant_info info;
	read_info(key, &info);
	if (l->fn != NULL) {
		l->fn(l->ctx, &info);
	}
}

uint32_t
u_desktop_gnome_grant_list(u_desktop_gnome_grant_info_fn fn, void *ctx)
{
	struct list_ctx l = {fn, ctx, NULL, 0, 0};
	return u_desktop_grant_for_each_key(list_one, &l);
}

static void
forget_one(void *ctx, const char *key)
{
	struct list_ctx *l = (struct list_ctx *)ctx;
	struct u_desktop_gnome_grant_info info;
	read_info(key, &info);
	bool covers = ((l->scopes & U_DESKTOP_SCOPE_CONTROL) != 0 && info.devices) ||
	              ((l->scopes & U_DESKTOP_SCOPE_CAPTURE) != 0 && info.screens);
	if (info.status == U_DESKTOP_GNOME_GRANT_OK && covers && info.exe[0] != '\0' &&
	    u_desktop_policy_exe_equal(info.exe, l->exe) && u_desktop_grant_remove(key)) {
		l->removed++;
	}
}

uint32_t
u_desktop_gnome_grant_forget_exe(const char *exe, uint32_t scopes)
{
	if (exe == NULL || exe[0] == '\0') {
		return 0;
	}
	struct list_ctx l = {NULL, NULL, exe, scopes, 0};
	u_desktop_grant_for_each_key(forget_one, &l);
	return l.removed;
}

enum u_desktop_gnome_grant_action
u_desktop_gnome_grant_after_request(bool granted, bool got_token, bool declined, bool ask_again, bool had_grant)
{
	if (granted) {
		return got_token ? U_DESKTOP_GNOME_GRANT_SAVE : U_DESKTOP_GNOME_GRANT_FORGET;
	}
	if (declined && (ask_again || !had_grant)) {
		return U_DESKTOP_GNOME_GRANT_FORGET;
	}
	return U_DESKTOP_GNOME_GRANT_KEEP;
}
