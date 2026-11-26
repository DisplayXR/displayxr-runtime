// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-kwin-geometry-bridge: what it accepts, with no I/O.
 * @ingroup targets
 */

#include "kwgb_payload.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char *
kwgb_sanitize(const char *json)
{
	if (json == NULL) {
		return NULL;
	}
	const size_t len = strlen(json);
	if (len == 0 || len > KWGB_MAX_PAYLOAD) {
		return NULL;
	}

	cJSON *root = cJSON_ParseWithLength(json, len);
	if (root == NULL || !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return NULL;
	}

	// Schema version: absent is 1, anything other than 1 is refused — a
	// consumer would refuse a newer one anyway, and the bridge must not
	// relabel one.
	const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
	if (version != NULL && (!cJSON_IsNumber(version) || version->valuedouble != 1.0)) {
		cJSON_Delete(root);
		return NULL;
	}
	if (version == NULL) {
		cJSON_AddNumberToObject(root, "version", 1);
	}

	cJSON *windows = cJSON_GetObjectItemCaseSensitive(root, "windows");
	if (!cJSON_IsArray(windows)) {
		cJSON_Delete(root);
		return NULL;
	}
	cJSON *win = NULL;
	cJSON_ArrayForEach(win, windows)
	{
		if (!cJSON_IsObject(win)) {
			cJSON_Delete(root);
			return NULL;
		}
		cJSON_DeleteItemFromObjectCaseSensitive(win, "title");
	}

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

bool
kwgb_comm_is_kwin(const char *comm)
{
	if (comm == NULL) {
		return false;
	}
	size_t len = strlen(comm);
	while (len > 0 && (comm[len - 1] == '\n' || comm[len - 1] == '\r')) {
		len--;
	}
	return (len == 12 && strncmp(comm, "kwin_wayland", 12) == 0) || (len == 8 && strncmp(comm, "kwin_x11", 8) == 0);
}

bool
kwgb_desktop_is_kde(const char *xdg_current_desktop)
{
	const char *p = xdg_current_desktop;
	while (p != NULL && *p != '\0') {
		const char *end = strchr(p, ':');
		const size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
		if (len == 3 && toupper((unsigned char)p[0]) == 'K' && toupper((unsigned char)p[1]) == 'D' &&
		    toupper((unsigned char)p[2]) == 'E') {
			return true;
		}
		p = end != NULL ? end + 1 : NULL;
	}
	return false;
}

void
kwgb_free(char *s)
{
	if (s != NULL) {
		cJSON_free(s);
	}
}
