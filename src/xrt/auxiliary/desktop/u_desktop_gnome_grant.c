// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: the saved portal grant. See u_desktop_gnome_grant.h.
 */

#include "u_desktop_gnome_grant.h"
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
	if (g->nmonitors > U_DESKTOP_GNOME_GRANT_MONITORS) {
		return false;
	}
	return g->screencast ? (g->screens && !g->devices) : (g->devices || g->screens);
}

char *
u_desktop_gnome_grant_encode(const char *app_id, const struct u_desktop_gnome_grant *g)
{
	if (g == NULL || !u_desktop_grant_key_valid(app_id) || !token_ok(g->token, sizeof(g->token)) || !shape_ok(g)) {
		return NULL;
	}
	cJSON *o = cJSON_CreateObject();
	if (o == NULL) {
		return NULL;
	}
	char now[32];
	u_desktop_audit_utc_now(now, sizeof(now));
	cJSON_AddNumberToObject(o, "version", U_DESKTOP_GNOME_GRANT_VERSION);
	cJSON_AddStringToObject(o, "app_id", app_id);
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
	cJSON_AddStringToObject(o, "saved", now);
	char *txt = cJSON_Print(o);
	// cJSON keeps its own copy of the token in the tree: wipe it before freeing.
	cJSON *tok = cJSON_GetObjectItemCaseSensitive(o, "restore_token");
	if (cJSON_IsString(tok) && tok->valuestring != NULL) {
		memset(tok->valuestring, 0, strlen(tok->valuestring));
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
	memset(txt, 0, len);
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
u_desktop_gnome_grant_decode(const char *json, const char *app_id, struct u_desktop_gnome_grant *out)
{
	memset(out, 0, sizeof(*out));
	if (json == NULL || app_id == NULL) {
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
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "app_id");
	const cJSON *portal = cJSON_GetObjectItemCaseSensitive(o, "portal");
	const cJSON *tok = cJSON_GetObjectItemCaseSensitive(o, "restore_token");
	const cJSON *mons = cJSON_GetObjectItemCaseSensitive(o, "monitors");
	if (!cJSON_IsNumber(ver) || ver->valuedouble < 1 || ver->valuedouble != (double)(int)ver->valuedouble) {
		goto done;
	}
	if ((int)ver->valuedouble > U_DESKTOP_GNOME_GRANT_VERSION) {
		st = U_DESKTOP_GNOME_GRANT_NEWER;
		goto done;
	}
	if (!cJSON_IsString(id) || strcmp(id->valuestring, app_id) != 0) {
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
	if (!g.screens) {
		g.nmonitors = 0;
	}
	if (!shape_ok(&g)) {
		goto done;
	}
	snprintf(g.token, sizeof(g.token), "%s", tok->valuestring);
	*out = g;
	st = U_DESKTOP_GNOME_GRANT_OK;
done:
	if (cJSON_IsString(tok) && tok->valuestring != NULL) {
		memset(tok->valuestring, 0, strlen(tok->valuestring));
	}
	cJSON_Delete(o);
	memset(&g, 0, sizeof(g));
	return st;
}

//! Wipe and free a text that may hold a token.
static void
free_secret(char *s)
{
	if (s != NULL) {
		memset(s, 0, strlen(s));
		free(s);
	}
}

enum u_desktop_gnome_grant_status
u_desktop_gnome_grant_load(const char *app_id, struct u_desktop_gnome_grant *out)
{
	memset(out, 0, sizeof(*out));
	char *txt = NULL;
	enum u_desktop_grant_read_result rr = u_desktop_grant_read(app_id, &txt);
	if (rr == U_DESKTOP_GRANT_READ_NONE) {
		return U_DESKTOP_GNOME_GRANT_NONE;
	}
	if (rr == U_DESKTOP_GRANT_READ_UNSAFE) {
		return U_DESKTOP_GNOME_GRANT_UNSAFE;
	}
	enum u_desktop_gnome_grant_status st = u_desktop_gnome_grant_decode(txt, app_id, out);
	free_secret(txt);
	if (st == U_DESKTOP_GNOME_GRANT_CORRUPT) {
		u_desktop_grant_remove(app_id);
	}
	return st;
}

bool
u_desktop_gnome_grant_save(const char *app_id, const struct u_desktop_gnome_grant *g)
{
	char *txt = u_desktop_gnome_grant_encode(app_id, g);
	if (txt == NULL) {
		return false;
	}
	bool ok = u_desktop_grant_write(app_id, txt);
	free_secret(txt);
	return ok;
}

bool
u_desktop_gnome_grant_forget(const char *app_id, const char *token)
{
	if (!u_desktop_grant_key_valid(app_id)) {
		return false;
	}
	if (token == NULL) {
		return u_desktop_grant_remove(app_id);
	}
	char *txt = NULL;
	if (u_desktop_grant_read(app_id, &txt) != U_DESKTOP_GRANT_READ_OK) {
		return false;
	}
	struct u_desktop_gnome_grant g;
	bool mine =
	    u_desktop_gnome_grant_decode(txt, app_id, &g) == U_DESKTOP_GNOME_GRANT_OK && strcmp(g.token, token) == 0;
	free_secret(txt);
	memset(&g, 0, sizeof(g));
	return mine && u_desktop_grant_remove(app_id);
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
