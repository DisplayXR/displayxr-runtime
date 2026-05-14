// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service orchestrator config persistence (service.json).
 * @ingroup ipc
 */

#include "service_config.h"

#include "xrt/xrt_config_os.h"
#include "cjson/cJSON.h"

#include <stdio.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#include <shlobj.h> // SHGetFolderPathA
#else
#include <sys/stat.h> // mkdir
#include <stdlib.h>   // getenv
#endif


/*
 *
 * Helpers
 *
 */

#define CONFIG_FILENAME "service.json"

static void
config_defaults(struct service_config *cfg)
{
	cfg->workspace = SERVICE_CHILD_AUTO;
	cfg->bridge = SERVICE_CHILD_AUTO;
	cfg->start_on_login = true;
	// Empty = orchestrator picks the first registered workspace controller
	// from HKLM\Software\DisplayXR\WorkspaceControllers. The runtime owns no
	// specific workspace app; controllers self-register from their own
	// installer. See docs/specs/runtime/workspace-controller-registration.md.
	cfg->workspace_binary[0] = '\0';
}

//! Build the full path to service.json into @p buf.
static bool
config_path(char *buf, size_t buf_size)
{
#ifdef XRT_OS_WINDOWS
	char appdata[MAX_PATH];
	if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata))) {
		return false;
	}
	snprintf(buf, buf_size, "%s\\DisplayXR\\" CONFIG_FILENAME, appdata);
#else
	const char *config_home = getenv("XDG_CONFIG_HOME");
	if (config_home && config_home[0]) {
		snprintf(buf, buf_size, "%s/displayxr/" CONFIG_FILENAME, config_home);
	} else {
		const char *home = getenv("HOME");
		if (!home || !home[0]) {
			return false;
		}
		snprintf(buf, buf_size, "%s/.config/displayxr/" CONFIG_FILENAME, home);
	}
#endif
	return true;
}

//! Ensure the directory containing @p filepath exists.
static void
ensure_parent_dir(const char *filepath)
{
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", filepath);

	// Strip filename to get directory
	char *last_sep = strrchr(dir, '/');
#ifdef XRT_OS_WINDOWS
	char *last_bsep = strrchr(dir, '\\');
	if (last_bsep && (!last_sep || last_bsep > last_sep)) {
		last_sep = last_bsep;
	}
#endif
	if (!last_sep) {
		return;
	}
	*last_sep = '\0';

#ifdef XRT_OS_WINDOWS
	CreateDirectoryA(dir, NULL); // Ignore error if exists
#else
	mkdir(dir, 0755); // Ignore error if exists
#endif
}

static const char *
mode_to_str(enum service_child_mode m)
{
	switch (m) {
	case SERVICE_CHILD_ENABLE: return "enable";
	case SERVICE_CHILD_DISABLE: return "disable";
	case SERVICE_CHILD_AUTO: return "auto";
	default: return "auto";
	}
}

static enum service_child_mode
str_to_mode(const char *s)
{
	if (!s) {
		return SERVICE_CHILD_AUTO;
	}
	if (strcmp(s, "enable") == 0) {
		return SERVICE_CHILD_ENABLE;
	}
	if (strcmp(s, "disable") == 0) {
		return SERVICE_CHILD_DISABLE;
	}
	return SERVICE_CHILD_AUTO;
}

//! Read entire file into malloc'd buffer. Returns NULL on failure.
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


/*
 *
 * Public API
 *
 */

void
service_config_load(struct service_config *cfg)
{
	config_defaults(cfg);

	char path[512];
	if (!config_path(path, sizeof(path))) {
		return;
	}

	char *json_str = read_file(path);
	if (!json_str) {
		return; // File absent or unreadable — use defaults
	}

	cJSON *root = cJSON_Parse(json_str);
	free(json_str);
	if (!root) {
		return;
	}

	cJSON *workspace = cJSON_GetObjectItemCaseSensitive(root, "workspace");
	if (cJSON_IsString(workspace)) {
		cfg->workspace = str_to_mode(workspace->valuestring);
	}

	cJSON *workspace_binary = cJSON_GetObjectItemCaseSensitive(root, "workspace_binary");
	if (cJSON_IsString(workspace_binary) && workspace_binary->valuestring[0] != '\0') {
		snprintf(cfg->workspace_binary, sizeof(cfg->workspace_binary),
		         "%s", workspace_binary->valuestring);
	}

	cJSON *bridge = cJSON_GetObjectItemCaseSensitive(root, "bridge");
	if (cJSON_IsString(bridge)) {
		cfg->bridge = str_to_mode(bridge->valuestring);
	}

	cJSON *sol = cJSON_GetObjectItemCaseSensitive(root, "start_on_login");
	if (cJSON_IsBool(sol)) {
		cfg->start_on_login = cJSON_IsTrue(sol);
	}

	cJSON_Delete(root);
}

bool
service_config_save(const struct service_config *cfg)
{
	char path[512];
	if (!config_path(path, sizeof(path))) {
		return false;
	}

	ensure_parent_dir(path);

	cJSON *root = cJSON_CreateObject();
	if (!root) {
		return false;
	}

	cJSON_AddStringToObject(root, "workspace", mode_to_str(cfg->workspace));
	cJSON_AddStringToObject(root, "workspace_binary", cfg->workspace_binary);
	cJSON_AddStringToObject(root, "bridge", mode_to_str(cfg->bridge));
	cJSON_AddBoolToObject(root, "start_on_login", cfg->start_on_login);

	char *json_str = cJSON_Print(root);
	cJSON_Delete(root);
	if (!json_str) {
		return false;
	}

	FILE *f = fopen(path, "w");
	if (!f) {
		free(json_str);
		return false;
	}

	fputs(json_str, f);
	fclose(f);
	free(json_str);

	return true;
}
