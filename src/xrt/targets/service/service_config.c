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
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#include <shlobj.h> // SHGetFolderPathA
#else
#include <sys/stat.h> // mkdir
#include <stdlib.h>   // getenv
#endif

#include "service_workspace_registry.h"


/*
 *
 * Helpers
 *
 */

#define CONFIG_FILENAME "service.json"

void
service_config_set_defaults(struct service_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->workspace = SERVICE_CHILD_AUTO;
	cfg->start_on_login = true;
	// Empty = orchestrator picks the first registered workspace controller
	// from HKLM\Software\DisplayXR\WorkspaceControllers. The runtime owns no
	// specific workspace app; controllers self-register from their own
	// installer. See docs/specs/runtime/workspace-controller-registration.md.
	cfg->workspace_binary[0] = '\0';
	// No per-controller entries = every controller on Ctrl+Space, auto.
	cfg->controller_count = 0;
}

bool
service_config_path(char *buf, size_t buf_size)
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

static bool
id_eq(const char *a, const char *b)
{
#ifdef XRT_OS_WINDOWS
	return _stricmp(a, b) == 0; // registry subkey names are case-insensitive
#else
	return strcmp(a, b) == 0;
#endif
}

static bool
looks_like_path(const char *s)
{
	return s != NULL && (strchr(s, '\\') != NULL || strchr(s, '/') != NULL);
}

static struct service_controller_launch *
find_entry(const struct service_config *cfg, const char *id)
{
	if (id == NULL || id[0] == '\0') {
		return NULL;
	}
	for (uint32_t i = 0; i < cfg->controller_count && i < SERVICE_CONTROLLERS_MAX; i++) {
		if (id_eq(cfg->controllers[i].id, id)) {
			return (struct service_controller_launch *)&cfg->controllers[i];
		}
	}
	return NULL;
}

//! Find or append the entry for @p id. NULL when @p id is unusable or the table is full.
static struct service_controller_launch *
get_entry(struct service_config *cfg, const char *id)
{
	struct service_controller_launch *e = find_entry(cfg, id);
	if (e != NULL) {
		return e;
	}
	if (id == NULL || id[0] == '\0' || strlen(id) >= SERVICE_CONTROLLER_ID_MAX ||
	    cfg->controller_count >= SERVICE_CONTROLLERS_MAX) {
		return NULL;
	}
	e = &cfg->controllers[cfg->controller_count++];
	memset(e, 0, sizeof(*e));
	snprintf(e->id, sizeof(e->id), "%s", id);
	e->mode = SERVICE_CHILD_AUTO;
	e->hotkey_setting = SERVICE_HOTKEY_SETTING_DEFAULT;
	return e;
}

//! Drop entries that went back to the defaults, so "entry exists" == customised.
static void
prune_entries(struct service_config *cfg)
{
	uint32_t w = 0;
	for (uint32_t r = 0; r < cfg->controller_count && r < SERVICE_CONTROLLERS_MAX; r++) {
		struct service_controller_launch *e = &cfg->controllers[r];
		if (e->has_mode && e->mode != SERVICE_CHILD_DISABLE) {
			e->has_mode = false; // "auto" is the default
		}
		if (e->hotkey_setting == SERVICE_HOTKEY_SETTING_COMBO &&
		    strcmp(e->hotkey, SERVICE_HOTKEY_DEFAULT) == 0) {
			e->hotkey_setting = SERVICE_HOTKEY_SETTING_DEFAULT;
			e->hotkey[0] = '\0';
		}
		if (!e->has_mode && e->hotkey_setting == SERVICE_HOTKEY_SETTING_DEFAULT) {
			continue;
		}
		if (w != r) {
			cfg->controllers[w] = *e;
		}
		w++;
	}
	cfg->controller_count = w;
}

static void
parse_controllers(struct service_config *cfg, const cJSON *controllers)
{
	if (!cJSON_IsObject(controllers)) {
		return;
	}
	const cJSON *node = NULL;
	cJSON_ArrayForEach(node, controllers)
	{
		if (!cJSON_IsObject(node) || node->string == NULL) {
			continue;
		}
		struct service_controller_launch *e = get_entry(cfg, node->string);
		if (e == NULL) {
			continue; // unusable id or table full
		}

		const cJSON *hotkey = cJSON_GetObjectItemCaseSensitive(node, "hotkey");
		if (cJSON_IsNull(hotkey)) {
			e->hotkey_setting = SERVICE_HOTKEY_SETTING_NONE;
			e->hotkey[0] = '\0';
		} else if (cJSON_IsString(hotkey) &&
		           service_hotkey_canonicalize(hotkey->valuestring, e->hotkey, sizeof(e->hotkey))) {
			e->hotkey_setting = SERVICE_HOTKEY_SETTING_COMBO;
		}
		// Anything else (absent, malformed combo) → the default.

		const cJSON *mode = cJSON_GetObjectItemCaseSensitive(node, "mode");
		if (cJSON_IsString(mode)) {
			if (strcmp(mode->valuestring, "disabled") == 0 || strcmp(mode->valuestring, "disable") == 0) {
				e->has_mode = true;
				e->mode = SERVICE_CHILD_DISABLE;
			} else if (strcmp(mode->valuestring, "auto") == 0) {
				e->has_mode = true;
				e->mode = SERVICE_CHILD_AUTO;
			}
		}
	}
	prune_entries(cfg);
}


/*
 *
 * Public API
 *
 */

void
service_config_parse_json(struct service_config *cfg, const char *json_text)
{
	if (json_text == NULL) {
		return;
	}
	cJSON *root = cJSON_Parse(json_text);
	if (!root) {
		return;
	}

	cJSON *workspace = cJSON_GetObjectItemCaseSensitive(root, "workspace");
	if (cJSON_IsString(workspace)) {
		cfg->workspace = str_to_mode(workspace->valuestring);
	}

	cJSON *workspace_binary = cJSON_GetObjectItemCaseSensitive(root, "workspace_binary");
	if (cJSON_IsString(workspace_binary) && workspace_binary->valuestring[0] != '\0') {
		snprintf(cfg->workspace_binary, sizeof(cfg->workspace_binary), "%s", workspace_binary->valuestring);
	}

	cJSON *sol = cJSON_GetObjectItemCaseSensitive(root, "start_on_login");
	if (cJSON_IsBool(sol)) {
		cfg->start_on_login = cJSON_IsTrue(sol);
	}

	parse_controllers(cfg, cJSON_GetObjectItemCaseSensitive(root, "controllers"));

	cJSON_Delete(root);
}

void
service_config_load(struct service_config *cfg)
{
	service_config_set_defaults(cfg);

	char path[512];
	if (!service_config_path(path, sizeof(path))) {
		return;
	}

	char *json_str = read_file(path);
	if (!json_str) {
		return; // File absent or unreadable — use defaults
	}
	service_config_parse_json(cfg, json_str);
	free(json_str);
}

char *
service_config_to_json(const struct service_config *cfg)
{
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		return NULL;
	}

	cJSON_AddStringToObject(root, "workspace", mode_to_str(cfg->workspace));
	cJSON_AddStringToObject(root, "workspace_binary", cfg->workspace_binary);
	cJSON_AddBoolToObject(root, "start_on_login", cfg->start_on_login);

	cJSON *controllers = cJSON_AddObjectToObject(root, "controllers");
	for (uint32_t i = 0; controllers != NULL && i < cfg->controller_count && i < SERVICE_CONTROLLERS_MAX; i++) {
		const struct service_controller_launch *e = &cfg->controllers[i];
		cJSON *o = cJSON_AddObjectToObject(controllers, e->id);
		if (o == NULL) {
			continue;
		}
		if (e->hotkey_setting == SERVICE_HOTKEY_SETTING_NONE) {
			cJSON_AddNullToObject(o, "hotkey");
		} else if (e->hotkey_setting == SERVICE_HOTKEY_SETTING_COMBO) {
			cJSON_AddStringToObject(o, "hotkey", e->hotkey);
		}
		if (e->has_mode) {
			cJSON_AddStringToObject(o, "mode", service_config_launch_mode_str(e->mode));
		}
	}

	char *json = cJSON_Print(root);
	cJSON_Delete(root);
	if (json == NULL) {
		return NULL;
	}
	// cJSON_Print allocates with cJSON's allocator; hand back a plain malloc copy
	// so callers free() it regardless of how cjson was built (it is a DLL here).
	size_t len = strlen(json);
	char *out = (char *)malloc(len + 1);
	if (out != NULL) {
		memcpy(out, json, len + 1);
	}
	cJSON_free(json);
	return out;
}

bool
service_config_save(const struct service_config *cfg)
{
	char path[512];
	if (!service_config_path(path, sizeof(path))) {
		return false;
	}

	ensure_parent_dir(path);

	char *json_str = service_config_to_json(cfg);
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

int
service_config_pick_controller(const struct service_config *cfg,
                               const struct workspace_controller_entry *entries,
                               int n)
{
	if (looks_like_path(cfg->workspace_binary)) {
		return SERVICE_PICK_DEV_OVERRIDE;
	}
	if (entries == NULL || n <= 0) {
		return -1;
	}
	if (cfg->workspace_binary[0] != '\0') {
		for (int i = 0; i < n; i++) {
			if (id_eq(entries[i].id, cfg->workspace_binary)) {
				return i;
			}
		}
	}
	return 0;
}

void
service_config_resolve_launch(const struct service_config *cfg,
                              const char *id,
                              bool is_active,
                              struct service_launch_resolved *out)
{
	memset(out, 0, sizeof(*out));
	const struct service_controller_launch *e = find_entry(cfg, id);

	// Mode. The per-controller entry wins over the top-level spelling; the
	// top-level one only speaks for the active controller, and only it can
	// carry the legacy always-on ENABLE.
	if (e != NULL && e->has_mode) {
		if (e->mode == SERVICE_CHILD_DISABLE) {
			out->mode = SERVICE_CHILD_DISABLE;
		} else {
			out->mode = (is_active && cfg->workspace == SERVICE_CHILD_ENABLE) ? SERVICE_CHILD_ENABLE
			                                                                  : SERVICE_CHILD_AUTO;
		}
	} else {
		out->mode = is_active ? cfg->workspace : SERVICE_CHILD_AUTO;
	}

	// Hotkey.
	const char *text = SERVICE_HOTKEY_DEFAULT;
	if (e != NULL && e->hotkey_setting == SERVICE_HOTKEY_SETTING_NONE) {
		text = NULL;
	} else if (e != NULL && e->hotkey_setting == SERVICE_HOTKEY_SETTING_COMBO) {
		text = e->hotkey;
	}
	if (text != NULL && service_hotkey_parse(text, &out->hotkey) &&
	    service_hotkey_format(&out->hotkey, out->hotkey_text, sizeof(out->hotkey_text))) {
		out->has_hotkey = true;
	} else {
		out->has_hotkey = false;
		out->hotkey.mods = 0;
		out->hotkey.vk = 0;
		out->hotkey_text[0] = '\0';
	}

	out->user = (e != NULL) || (is_active && cfg->workspace != SERVICE_CHILD_AUTO);
}

bool
service_config_set_controller_hotkey(struct service_config *cfg, const char *id, const char *combo)
{
	char canon[SERVICE_HOTKEY_MAX] = "";
	if (combo != NULL && !service_hotkey_canonicalize(combo, canon, sizeof(canon))) {
		return false;
	}
	struct service_controller_launch *e = get_entry(cfg, id);
	if (e == NULL) {
		return false;
	}
	if (combo == NULL) {
		e->hotkey_setting = SERVICE_HOTKEY_SETTING_NONE;
		e->hotkey[0] = '\0';
	} else {
		e->hotkey_setting = SERVICE_HOTKEY_SETTING_COMBO;
		snprintf(e->hotkey, sizeof(e->hotkey), "%s", canon);
	}
	prune_entries(cfg);
	return true;
}

bool
service_config_set_controller_mode(struct service_config *cfg,
                                   const char *id,
                                   enum service_child_mode mode,
                                   bool is_active)
{
	if (mode != SERVICE_CHILD_AUTO && mode != SERVICE_CHILD_DISABLE) {
		return false;
	}
	struct service_controller_launch *e = get_entry(cfg, id);
	if (e == NULL) {
		return false;
	}
	e->has_mode = true;
	e->mode = mode;
	if (is_active) {
		cfg->workspace = mode;
	}
	prune_entries(cfg);
	return true;
}

void
service_config_reset_controller(struct service_config *cfg, const char *id, bool is_active)
{
	struct service_controller_launch *e = find_entry(cfg, id);
	if (e != NULL) {
		e->has_mode = false;
		e->hotkey_setting = SERVICE_HOTKEY_SETTING_DEFAULT;
		e->hotkey[0] = '\0';
		prune_entries(cfg);
	}
	if (is_active) {
		cfg->workspace = SERVICE_CHILD_AUTO;
	}
}

void
service_config_sync_active_mode(struct service_config *cfg, const char *active_id)
{
	if (active_id == NULL || active_id[0] == '\0') {
		return;
	}
	if (cfg->workspace == SERVICE_CHILD_DISABLE) {
		struct service_controller_launch *e = get_entry(cfg, active_id);
		if (e != NULL) {
			e->has_mode = true;
			e->mode = SERVICE_CHILD_DISABLE;
		}
	} else {
		struct service_controller_launch *e = find_entry(cfg, active_id);
		if (e != NULL) {
			e->has_mode = false;
		}
	}
	prune_entries(cfg);
}

const char *
service_config_launch_mode_str(enum service_child_mode mode)
{
	return mode == SERVICE_CHILD_DISABLE ? "disabled" : "auto";
}

void *
service_workspace_controllers_to_cjson(const struct service_config *cfg,
                                       const struct workspace_controller_entry *entries,
                                       int n,
                                       const struct service_controller_live *live)
{
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return NULL;
	}

	int active = service_config_pick_controller(cfg, entries, n);
	if (active >= 0) {
		cJSON_AddStringToObject(root, "active_id", entries[active].id);
	} else {
		cJSON_AddNullToObject(root, "active_id");
	}
	// A dev-path override launches a binary no registration describes; say so
	// rather than pretend a registered controller is the active one.
	if (active == SERVICE_PICK_DEV_OVERRIDE) {
		cJSON_AddStringToObject(root, "dev_override", cfg->workspace_binary);
	} else {
		cJSON_AddNullToObject(root, "dev_override");
	}

	cJSON *arr = cJSON_AddArrayToObject(root, "controllers");
	for (int i = 0; arr != NULL && entries != NULL && i < n; i++) {
		const struct workspace_controller_entry *ce = &entries[i];
		cJSON *o = cJSON_CreateObject();
		if (o == NULL) {
			break;
		}
		cJSON_AddStringToObject(o, "id", ce->id);
		cJSON_AddStringToObject(o, "display_name", ce->display_name);
		cJSON_AddStringToObject(o, "vendor", ce->vendor);
		cJSON_AddStringToObject(o, "version", ce->version);
		cJSON_AddStringToObject(o, "binary", ce->binary);
		cJSON_AddBoolToObject(o, "registered", true);
		if (live != NULL && live[i].known) {
			cJSON_AddBoolToObject(o, "connected", live[i].connected);
			if (live[i].connected) {
				cJSON_AddNumberToObject(o, "pid", (double)live[i].pid);
			} else {
				cJSON_AddNullToObject(o, "pid");
			}
		} else {
			cJSON_AddNullToObject(o, "connected");
			cJSON_AddNullToObject(o, "pid");
		}

		struct service_launch_resolved r;
		service_config_resolve_launch(cfg, ce->id, i == active, &r);
		cJSON *launch = cJSON_AddObjectToObject(o, "launch");
		if (launch != NULL) {
			cJSON_AddStringToObject(launch, "mode", service_config_launch_mode_str(r.mode));
			if (r.has_hotkey) {
				cJSON_AddStringToObject(launch, "hotkey", r.hotkey_text);
			} else {
				cJSON_AddNullToObject(launch, "hotkey");
			}
			cJSON_AddStringToObject(launch, "source", r.user ? "user" : "default");
		}
		cJSON_AddItemToArray(arr, o);
	}
	return root;
}
