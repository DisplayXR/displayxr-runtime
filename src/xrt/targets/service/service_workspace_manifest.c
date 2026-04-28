// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Workspace controller sidecar manifest loader.
 * @ingroup ipc
 */

#include "service_workspace_manifest.h"

#include "xrt/xrt_config_os.h"
#include "util/u_logging.h"
#include "cjson/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef _WIN32
#define strncasecmp _strnicmp
#endif


/*
 *
 * Helpers
 *
 */

//! Build the path to the sidecar manifest from the workspace binary path.
//! Strips a trailing ".exe" (case-insensitive) if present, then appends
//! ".controller.json". Returns false on buffer overflow.
static bool
manifest_path_for(const char *workspace_binary_path, char *buf, size_t buf_size)
{
	size_t len = strlen(workspace_binary_path);
	size_t base_len = len;
	if (len >= 4 && strncasecmp(workspace_binary_path + len - 4, ".exe", 4) == 0) {
		base_len = len - 4;
	}

	int written = snprintf(buf, buf_size, "%.*s.controller.json",
	                       (int)base_len, workspace_binary_path);
	if (written < 0 || (size_t)written >= buf_size) {
		return false;
	}
	return true;
}

//! Quick existence check; avoids opening the file just to fail in fopen.
static bool
file_exists(const char *path)
{
#ifdef XRT_OS_WINDOWS
	DWORD attrs = GetFileAttributesA(path);
	return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	fclose(f);
	return true;
#endif
}

//! Read entire file into malloc'd buffer. Returns NULL on failure.
//! Mirrors service_config.c::read_file.
static char *
read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
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
	if (!buf) {
		fclose(f);
		return NULL;
	}

	size_t read = fread(buf, 1, (size_t)len, f);
	fclose(f);

	buf[read] = '\0';
	return buf;
}

//! Copy a JSON string field into a fixed-size buffer, length-capped.
//! Missing or non-string fields land as empty strings.
static void
copy_string_field(const cJSON *root, const char *key, char *out, size_t out_size)
{
	const cJSON *node = cJSON_GetObjectItemCaseSensitive(root, key);
	if (cJSON_IsString(node) && node->valuestring != NULL) {
		snprintf(out, out_size, "%s", node->valuestring);
	} else {
		out[0] = '\0';
	}
}


/*
 *
 * Public API
 *
 */

bool
service_workspace_manifest_load(const char *workspace_binary_path,
                                struct workspace_manifest *out)
{
	memset(out, 0, sizeof(*out));

	if (workspace_binary_path == NULL || workspace_binary_path[0] == '\0') {
		return false;
	}

	char path[512];
	if (!manifest_path_for(workspace_binary_path, path, sizeof(path))) {
		return false;
	}

	if (!file_exists(path)) {
		return false;
	}

	char *json_str = read_file(path);
	if (!json_str) {
		return false;
	}

	cJSON *root = cJSON_Parse(json_str);
	free(json_str);
	if (!root) {
		U_LOG_W("workspace manifest: parse failed for %s", path);
		return false;
	}

	const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
	if (!cJSON_IsNumber(schema) || schema->valueint != 1) {
		U_LOG_W("workspace manifest: unsupported schema_version in %s (expected 1)", path);
		cJSON_Delete(root);
		memset(out, 0, sizeof(*out));
		return false;
	}

	copy_string_field(root, "display_name", out->display_name, sizeof(out->display_name));
	copy_string_field(root, "vendor", out->vendor, sizeof(out->vendor));
	copy_string_field(root, "version", out->version, sizeof(out->version));
	copy_string_field(root, "icon_path", out->icon_path, sizeof(out->icon_path));

	cJSON_Delete(root);
	return true;
}
