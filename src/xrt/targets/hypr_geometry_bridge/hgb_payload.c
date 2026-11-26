// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-hypr-geometry-bridge: snapshot building, no I/O.
 * @ingroup targets
 */

#include "hgb_payload.h"

#include <cjson/cJSON.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HGB_MAX_MONITORS 32

struct hgb_monitor
{
	int id;
	int x, y, w, h; //!< logical
	double scale;
	int active_ws;  //!< workspace shown
	int special_ws; //!< special workspace shown over it, 0 = none
	char name[64];
};

static bool
num(const cJSON *obj, const char *key, double *out)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!cJSON_IsNumber(v)) {
		return false;
	}
	*out = v->valuedouble;
	return true;
}

//! Booleans in Hyprland's JSON; some (fullscreen) became numbers in 0.42.
static bool
truthy(const cJSON *obj, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsBool(v)) {
		return cJSON_IsTrue(v);
	}
	if (cJSON_IsNumber(v)) {
		return v->valuedouble != 0.0;
	}
	return false;
}

static int
sub_id(const cJSON *obj, const char *key)
{
	const cJSON *o = cJSON_GetObjectItemCaseSensitive(obj, key);
	double id = 0.0;
	if (cJSON_IsObject(o) && num(o, "id", &id)) {
		return (int)id;
	}
	return 0;
}

static bool
pair(const cJSON *obj, const char *key, double *a, double *b)
{
	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != 2) {
		return false;
	}
	const cJSON *x = cJSON_GetArrayItem(arr, 0);
	const cJSON *y = cJSON_GetArrayItem(arr, 1);
	if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y)) {
		return false;
	}
	*a = x->valuedouble;
	*b = y->valuedouble;
	return true;
}

static cJSON *
rect(double x, double y, double w, double h)
{
	const double v[4] = {round(x), round(y), round(w), round(h)};
	return cJSON_CreateDoubleArray(v, 4);
}

static int
parse_monitors(const cJSON *arr, struct hgb_monitor *out)
{
	int count = 0;
	const cJSON *m = NULL;
	cJSON_ArrayForEach(m, arr)
	{
		if (count >= HGB_MAX_MONITORS || !cJSON_IsObject(m)) {
			continue;
		}
		double id = 0, x = 0, y = 0, w = 0, h = 0, scale = 1.0, transform = 0;
		if (!num(m, "id", &id) || !num(m, "x", &x) || !num(m, "y", &y) || !num(m, "width", &w) ||
		    !num(m, "height", &h) || w <= 0 || h <= 0) {
			continue;
		}
		if (truthy(m, "disabled")) {
			continue;
		}
		if (!num(m, "scale", &scale) || scale <= 0.0) {
			scale = 1.0;
		}
		num(m, "transform", &transform);
		const bool quarter = (((int)transform) & 1) != 0; // 1,3,5,7: 90/270, flipped or not
		struct hgb_monitor *o = &out[count++];
		memset(o, 0, sizeof(*o));
		o->id = (int)id;
		o->x = (int)round(x);
		o->y = (int)round(y);
		o->w = (int)round((quarter ? h : w) / scale);
		o->h = (int)round((quarter ? w : h) / scale);
		o->scale = scale;
		o->active_ws = sub_id(m, "activeWorkspace");
		o->special_ws = sub_id(m, "specialWorkspace");
		const cJSON *name = cJSON_GetObjectItemCaseSensitive(m, "name");
		if (cJSON_IsString(name) && name->valuestring != NULL) {
			strncpy(o->name, name->valuestring, sizeof(o->name) - 1);
		}
	}
	return count;
}

char *
hgb_build_snapshot(const char *clients_json, const char *monitors_json, struct hgb_snapshot_info *info)
{
	if (info != NULL) {
		memset(info, 0, sizeof(*info));
	}
	if (clients_json == NULL || monitors_json == NULL) {
		return NULL;
	}
	cJSON *clients = cJSON_Parse(clients_json);
	cJSON *monitors = cJSON_Parse(monitors_json);
	if (!cJSON_IsArray(clients) || !cJSON_IsArray(monitors)) {
		cJSON_Delete(clients);
		cJSON_Delete(monitors);
		return NULL;
	}

	struct hgb_monitor mons[HGB_MAX_MONITORS];
	const int mon_count = parse_monitors(monitors, mons);

	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "version", 1);
	cJSON_AddStringToObject(root, "layout_mode", "logical");
	cJSON *windows = cJSON_AddArrayToObject(root, "windows");

	const cJSON *c = NULL;
	cJSON_ArrayForEach(c, clients)
	{
		if (!cJSON_IsObject(c)) {
			continue;
		}
		double pid = 0, mon_id = -1, x = 0, y = 0, w = 0, h = 0, fh = -1;
		if (!num(c, "pid", &pid) || pid <= 0 || pid > INT32_MAX || !num(c, "monitor", &mon_id) ||
		    !pair(c, "at", &x, &y) || !pair(c, "size", &w, &h) || w <= 0 || h <= 0) {
			continue;
		}
		const struct hgb_monitor *mon = NULL;
		for (int i = 0; i < mon_count; i++) {
			if (mons[i].id == (int)mon_id) {
				mon = &mons[i];
				break;
			}
		}
		if (mon == NULL) {
			continue;
		}

		const cJSON *mapped_v = cJSON_GetObjectItemCaseSensitive(c, "mapped");
		const bool mapped = mapped_v == NULL || cJSON_IsTrue(mapped_v);
		const int ws = sub_id(c, "workspace");
		const bool on_shown_ws = ws == mon->active_ws || (mon->special_ws != 0 && ws == mon->special_ws);
		const bool hidden = !mapped || truthy(c, "hidden") || !on_shown_ws;
		const bool floating = truthy(c, "floating");
		num(c, "focusHistoryID", &fh);

		cJSON *win = cJSON_CreateObject();
		cJSON_AddNumberToObject(win, "pid", pid);
		const cJSON *cls = cJSON_GetObjectItemCaseSensitive(c, "class");
		if (cJSON_IsString(cls) && cls->valuestring != NULL) {
			cJSON_AddStringToObject(win, "app_id", cls->valuestring);
		}
		cJSON_AddItemToObject(win, "frame", rect(x, y, w, h));
		cJSON_AddItemToObject(win, "buffer", rect(x, y, w, h));
		cJSON_AddItemToObject(win, "buffer_device",
		                      rect((x - mon->x) * mon->scale, (y - mon->y) * mon->scale, w * mon->scale,
		                           h * mon->scale));
		cJSON_AddBoolToObject(win, "focus", fh == 0);
		cJSON_AddBoolToObject(win, "hidden", hidden);
		cJSON_AddBoolToObject(win, "xwayland", truthy(c, "xwayland"));
		cJSON_AddBoolToObject(win, "fullscreen", truthy(c, "fullscreen"));
		cJSON_AddBoolToObject(win, "floating", floating);

		cJSON *m = cJSON_AddObjectToObject(win, "monitor");
		cJSON_AddNumberToObject(m, "x", mon->x);
		cJSON_AddNumberToObject(m, "y", mon->y);
		cJSON_AddNumberToObject(m, "w", mon->w);
		cJSON_AddNumberToObject(m, "h", mon->h);
		cJSON_AddNumberToObject(m, "scale", mon->scale);
		cJSON_AddNumberToObject(m, "device_scale", mon->scale);
		if (mon->name[0] != '\0') {
			cJSON_AddStringToObject(m, "connector", mon->name);
		}

		cJSON_AddItemToArray(windows, win);
		if (info != NULL) {
			info->window_count++;
			if (floating && !hidden) {
				info->any_floating_visible = true;
			}
		}
	}

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	cJSON_Delete(clients);
	cJSON_Delete(monitors);
	return out;
}

void
hgb_free(char *s)
{
	if (s != NULL) {
		cJSON_free(s);
	}
}
