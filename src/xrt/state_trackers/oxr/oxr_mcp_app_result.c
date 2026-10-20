// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_mcp_tools result shaping — see oxr_mcp_app_result.h.
 * @ingroup oxr_main
 */

#include "oxr_mcp_app_result.h"

#include <string.h>

static cJSON *
error_result(cJSON *error_value)
{
	cJSON *out = cJSON_CreateObject();
	cJSON_AddTrueToObject(out, "isError");
	cJSON_AddItemToObject(out, "error", error_value);
	return out;
}

cJSON *
oxr_mcp_app_tool_result(bool success, const char *result_json)
{
	const char *text = result_json != NULL ? result_json : "";
	cJSON *parsed = text[0] != '\0' ? cJSON_Parse(text) : NULL;

	if (success) {
		if (parsed != NULL) {
			return parsed;
		}
		cJSON *out = cJSON_CreateObject();
		if (text[0] != '\0') {
			// App submitted unparseable JSON; surface raw.
			cJSON_AddStringToObject(out, "raw", text);
		}
		return out;
	}

	if (parsed == NULL) {
		return error_result(cJSON_CreateString(text[0] != '\0' ? text : "tool failed"));
	}
	if (!cJSON_IsObject(parsed)) {
		return error_result(parsed);
	}

	// Rebuild with the flag first; the app's members follow in order (its
	// own "isError", whatever it said, is superseded — this IS an error).
	cJSON *out = cJSON_CreateObject();
	cJSON_AddTrueToObject(out, "isError");
	bool any = false;
	while (parsed->child != NULL) {
		cJSON *item = cJSON_DetachItemViaPointer(parsed, parsed->child);
		if (item->string != NULL && strcmp(item->string, "isError") == 0) {
			cJSON_Delete(item);
			continue;
		}
		cJSON_AddItemToObject(out, item->string, item);
		any = true;
	}
	cJSON_Delete(parsed);
	if (!any) {
		cJSON_AddStringToObject(out, "error", "tool failed");
	}
	return out;
}
