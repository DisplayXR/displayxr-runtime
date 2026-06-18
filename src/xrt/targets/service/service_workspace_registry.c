// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Workspace controller registry enumerator implementation.
 * @ingroup ipc
 */

#include "service_workspace_registry.h"

#include "xrt/xrt_config_os.h"
#include "util/u_logging.h"

#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define WORKSPACE_REGISTRY_KEY "Software\\DisplayXR\\WorkspaceControllers"

static bool
file_exists(const char *path)
{
	DWORD attrs = GetFileAttributesA(path);
	return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

//! Read a REG_DWORD value. Returns 0 on miss or wrong type.
static uint32_t
read_reg_dword(HKEY key, const char *value_name)
{
	DWORD type = 0;
	DWORD value = 0;
	DWORD size = sizeof(value);
	LSTATUS rc = RegQueryValueExA(key, value_name, NULL, &type,
	                              (LPBYTE)&value, &size);
	if (rc != ERROR_SUCCESS || type != REG_DWORD) {
		return 0;
	}
	return (uint32_t)value;
}

//! Read a REG_SZ value into a fixed-size buffer. Returns false on miss
//! or oversize. On false, the buffer is zeroed.
static bool
read_reg_string(HKEY key, const char *value_name, char *buf, size_t buf_size)
{
	if (buf_size == 0) {
		return false;
	}
	buf[0] = '\0';

	DWORD type = 0;
	DWORD size = (DWORD)buf_size;
	LSTATUS rc = RegQueryValueExA(key, value_name, NULL, &type,
	                              (LPBYTE)buf, &size);
	if (rc != ERROR_SUCCESS || type != REG_SZ) {
		buf[0] = '\0';
		return false;
	}
	// RegQueryValueEx may or may not include the trailing NUL.
	if (size > 0 && buf[size - 1] != '\0' && size < buf_size) {
		buf[size] = '\0';
	} else if (size >= buf_size) {
		buf[buf_size - 1] = '\0';
	}
	return buf[0] != '\0';
}

//! Walk the optional `<id>\Actions\*` sub-subkeys (alphabetical order
//! via RegEnumKeyEx) and populate entry->actions[]. Skips entries
//! where the Label value is missing/empty. Caps at
//! WORKSPACE_REGISTRY_MAX_ACTIONS.
static void
populate_actions(HKEY parent_subkey, struct workspace_controller_entry *entry)
{
	HKEY actions_key = NULL;
	if (RegOpenKeyExA(parent_subkey, "Actions", 0,
	                  KEY_READ | KEY_WOW64_64KEY, &actions_key) != ERROR_SUCCESS) {
		return; // Optional subkey absent; tray falls back to hardcoded defaults.
	}

	for (DWORD index = 0;; index++) {
		if (entry->n_actions >= WORKSPACE_REGISTRY_MAX_ACTIONS) {
			U_LOG_W("workspace registry: '%s' has more than %d actions; truncating",
			        entry->id, WORKSPACE_REGISTRY_MAX_ACTIONS);
			break;
		}

		char ordering[64];
		DWORD ordering_size = sizeof(ordering);
		LSTATUS rc = RegEnumKeyExA(actions_key, index, ordering, &ordering_size,
		                           NULL, NULL, NULL, NULL);
		if (rc == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (rc != ERROR_SUCCESS) {
			break;
		}

		HKEY action_key = NULL;
		if (RegOpenKeyExA(actions_key, ordering, 0,
		                  KEY_READ | KEY_WOW64_64KEY,
		                  &action_key) != ERROR_SUCCESS) {
			continue;
		}

		struct workspace_controller_action *action =
		    &entry->actions[entry->n_actions];
		memset(action, 0, sizeof(*action));

		read_reg_string(action_key, "Label", action->label, sizeof(action->label));
		read_reg_string(action_key, "Type", action->type, sizeof(action->type));

		// "separator" type is allowed without a label (it's a divider);
		// every other type needs a label or it's silently skipped.
		bool keep = false;
		if (strcmp(action->type, "separator") == 0) {
			keep = true;
		} else if (action->label[0] != '\0' && action->type[0] != '\0') {
			keep = true;
		}

		if (keep) {
			entry->n_actions++;
		}

		RegCloseKey(action_key);
	}

	RegCloseKey(actions_key);
}

//! Populate @p entry from one subkey. Returns true if Binary exists
//! on disk.
static bool
populate_from_subkey(const char *id, HKEY subkey,
                     struct workspace_controller_entry *entry)
{
	memset(entry, 0, sizeof(*entry));
	snprintf(entry->id, sizeof(entry->id), "%s", id);

	if (!read_reg_string(subkey, "Binary", entry->binary, sizeof(entry->binary))) {
		return false;
	}

	if (!file_exists(entry->binary)) {
		U_LOG_W("workspace registry: '%s' Binary='%s' does not exist; skipping",
		        id, entry->binary);
		return false;
	}

	read_reg_string(subkey, "DisplayName", entry->display_name,
	                sizeof(entry->display_name));
	if (entry->display_name[0] == '\0') {
		snprintf(entry->display_name, sizeof(entry->display_name),
		         "Workspace Controller");
	}
	read_reg_string(subkey, "Vendor", entry->vendor, sizeof(entry->vendor));
	read_reg_string(subkey, "Version", entry->version, sizeof(entry->version));
	read_reg_string(subkey, "UninstallString", entry->uninstall_string,
	                sizeof(entry->uninstall_string));

	if (read_reg_dword(subkey, "SupportsFileDialog") != 0) {
		entry->capabilities |= WORKSPACE_CAPABILITY_FILE_DIALOG;
	}

	populate_actions(subkey, entry);

	return true;
}

int
service_workspace_registry_enumerate(struct workspace_controller_entry *out,
                                     int max_entries)
{
	if (out == NULL || max_entries <= 0) {
		return 0;
	}

	HKEY parent = NULL;
	LSTATUS rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE, WORKSPACE_REGISTRY_KEY, 0,
	                           KEY_READ | KEY_WOW64_64KEY, &parent);
	if (rc != ERROR_SUCCESS) {
		return 0;
	}

	int count = 0;
	for (DWORD index = 0;; index++) {
		char name[64];
		DWORD name_size = sizeof(name);
		rc = RegEnumKeyExA(parent, index, name, &name_size, NULL, NULL, NULL,
		                   NULL);
		if (rc == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (rc != ERROR_SUCCESS) {
			break;
		}

		HKEY subkey = NULL;
		if (RegOpenKeyExA(parent, name, 0, KEY_READ | KEY_WOW64_64KEY,
		                  &subkey) != ERROR_SUCCESS) {
			continue;
		}

		if (count < max_entries) {
			if (populate_from_subkey(name, subkey, &out[count])) {
				count++;
			}
		}

		RegCloseKey(subkey);
	}

	RegCloseKey(parent);
	return count;
}

bool
service_workspace_registry_lookup(const char *id,
                                  struct workspace_controller_entry *out)
{
	if (id == NULL || id[0] == '\0' || out == NULL) {
		return false;
	}

	char path[256];
	snprintf(path, sizeof(path), WORKSPACE_REGISTRY_KEY "\\%s", id);

	HKEY subkey = NULL;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY,
	                  &subkey) != ERROR_SUCCESS) {
		return false;
	}

	bool ok = populate_from_subkey(id, subkey, out);
	RegCloseKey(subkey);
	return ok;
}

#else // !XRT_OS_WINDOWS

// POSIX discovery is JSON-manifest based, mirroring the plug-in loader
// (`target_plugin_loader.c`): there is no registry, so each controller drops a
// `<id>.json` manifest into a well-known directory and the runtime scans it.
// Schema + roots documented in
// `docs/specs/runtime/workspace-controller-registration.md` (POSIX section).

#include "util/u_json.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

//! Dev override: colon-separated list of dirs, like XRT_PLUGIN_SEARCH_PATH.
#define WORKSPACE_CONTROLLER_PATH_ENV "XRT_WORKSPACE_CONTROLLER_PATH"

//! Subpath under each Application Support / share root.
#define WORKSPACE_CONTROLLER_SUBDIR "DisplayXR/WorkspaceControllers"

static bool
regular_file_exists(const char *path)
{
	struct stat st;
	return path[0] != '\0' && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

//! Walk the optional `controller.actions` array and populate entry->actions[].
//! Mirrors the Windows populate_actions() keep/skip rules: "separator" needs no
//! label, every other type needs both a label and a type.
static void
populate_actions(const cJSON *controller, struct workspace_controller_entry *entry)
{
	const cJSON *actions = u_json_get(controller, "actions");
	if (actions == NULL || !cJSON_IsArray(actions)) {
		return; // Optional; tray falls back to hardcoded defaults.
	}

	const cJSON *node = NULL;
	cJSON_ArrayForEach(node, actions)
	{
		if (entry->n_actions >= WORKSPACE_REGISTRY_MAX_ACTIONS) {
			U_LOG_W("workspace registry: '%s' has more than %d actions; truncating", entry->id,
			        WORKSPACE_REGISTRY_MAX_ACTIONS);
			break;
		}

		struct workspace_controller_action *action = &entry->actions[entry->n_actions];
		memset(action, 0, sizeof(*action));

		const cJSON *label = u_json_get(node, "label");
		if (label != NULL) {
			(void)u_json_get_string_into_array(label, action->label, sizeof(action->label));
		}
		const cJSON *type = u_json_get(node, "type");
		if (type != NULL) {
			(void)u_json_get_string_into_array(type, action->type, sizeof(action->type));
		}

		bool keep = false;
		if (strcmp(action->type, "separator") == 0) {
			keep = true;
		} else if (action->label[0] != '\0' && action->type[0] != '\0') {
			keep = true;
		}

		if (keep) {
			entry->n_actions++;
		}
	}
}

//! Parse one manifest file into @p entry. Returns true if the manifest is valid
//! and its binary exists on disk. @p fallback_id is the basename minus ".json",
//! used when the manifest omits an explicit `controller.id`.
static bool
parse_manifest(const char *path, const char *fallback_id, struct workspace_controller_entry *entry)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long len = ftell(f);
	if (len <= 0 || len > 64 * 1024) {
		U_LOG_W("workspace registry: %s: implausible manifest size %ld.", path, len);
		fclose(f);
		return false;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}
	char *buf = (char *)malloc((size_t)len + 1);
	if (buf == NULL) {
		fclose(f);
		return false;
	}
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	if (got != (size_t)len) {
		free(buf);
		return false;
	}
	buf[len] = '\0';

	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (root == NULL) {
		U_LOG_W("workspace registry: %s: JSON parse failed.", path);
		return false;
	}

	char fmt[16] = {0};
	const cJSON *fmt_node = u_json_get(root, "file_format_version");
	if (fmt_node != NULL) {
		(void)u_json_get_string_into_array(fmt_node, fmt, sizeof(fmt));
	}
	if (strcmp(fmt, "1.0") != 0) {
		U_LOG_W("workspace registry: %s: unsupported file_format_version='%s'.", path, fmt);
		cJSON_Delete(root);
		return false;
	}

	const cJSON *controller = u_json_get(root, "controller");
	if (controller == NULL) {
		U_LOG_W("workspace registry: %s: missing 'controller' object.", path);
		cJSON_Delete(root);
		return false;
	}

	memset(entry, 0, sizeof(*entry));

	const cJSON *id_node = u_json_get(controller, "id");
	if (id_node != NULL) {
		(void)u_json_get_string_into_array(id_node, entry->id, sizeof(entry->id));
	}
	if (entry->id[0] == '\0') {
		snprintf(entry->id, sizeof(entry->id), "%s", fallback_id);
	}

	const cJSON *binary_node = u_json_get(controller, "binary");
	if (binary_node != NULL) {
		(void)u_json_get_string_into_array(binary_node, entry->binary, sizeof(entry->binary));
	}
	if (entry->binary[0] == '\0') {
		U_LOG_W("workspace registry: %s: required field 'controller.binary' missing.", path);
		cJSON_Delete(root);
		return false;
	}
	if (!regular_file_exists(entry->binary)) {
		U_LOG_W("workspace registry: '%s' binary='%s' does not exist; skipping", entry->id, entry->binary);
		cJSON_Delete(root);
		return false;
	}

	const cJSON *dn = u_json_get(controller, "display_name");
	if (dn != NULL) {
		(void)u_json_get_string_into_array(dn, entry->display_name, sizeof(entry->display_name));
	}
	if (entry->display_name[0] == '\0') {
		snprintf(entry->display_name, sizeof(entry->display_name), "Workspace Controller");
	}

	const cJSON *vendor = u_json_get(controller, "vendor");
	if (vendor != NULL) {
		(void)u_json_get_string_into_array(vendor, entry->vendor, sizeof(entry->vendor));
	}
	const cJSON *version = u_json_get(controller, "version");
	if (version != NULL) {
		(void)u_json_get_string_into_array(version, entry->version, sizeof(entry->version));
	}
	const cJSON *uninstall = u_json_get(controller, "uninstall_string");
	if (uninstall != NULL) {
		(void)u_json_get_string_into_array(uninstall, entry->uninstall_string,
		                                   sizeof(entry->uninstall_string));
	}

	bool supports_file_dialog = false;
	const cJSON *sfd = u_json_get(controller, "supports_file_dialog");
	if (sfd != NULL && u_json_get_bool(sfd, &supports_file_dialog) && supports_file_dialog) {
		entry->capabilities |= WORKSPACE_CAPABILITY_FILE_DIALOG;
	}

	populate_actions(controller, entry);

	cJSON_Delete(root);
	return true;
}

static bool
already_have_id(const struct workspace_controller_entry *entries, int n, const char *id)
{
	for (int i = 0; i < n; i++) {
		if (strcmp(entries[i].id, id) == 0) {
			return true;
		}
	}
	return false;
}

static void
append_root(char roots[][PATH_MAX], int max_roots, int *n_roots, const char *path)
{
	if (path == NULL || *path == '\0' || *n_roots >= max_roots) {
		return;
	}
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return;
	}
	snprintf(roots[(*n_roots)++], PATH_MAX, "%s", path);
}

//! Assemble discovery roots in priority order (per-user shadows system).
static int
build_discovery_roots(char roots[][PATH_MAX], int max_roots)
{
	int n_roots = 0;

	const char *override = getenv(WORKSPACE_CONTROLLER_PATH_ENV);
	if (override != NULL && *override != '\0') {
		const char *p = override;
		while (*p && n_roots < max_roots) {
			const char *colon = strchr(p, ':');
			size_t len = colon ? (size_t)(colon - p) : strlen(p);
			if (len > 0 && len < PATH_MAX) {
				char buf[PATH_MAX];
				memcpy(buf, p, len);
				buf[len] = '\0';
				append_root(roots, max_roots, &n_roots, buf);
			}
			if (!colon) {
				break;
			}
			p = colon + 1;
		}
	}

	const char *home = getenv("HOME");
	char user_root[PATH_MAX];
#ifdef __APPLE__
	if (home != NULL && *home != '\0') {
		snprintf(user_root, sizeof(user_root), "%s/Library/Application Support/" WORKSPACE_CONTROLLER_SUBDIR,
		         home);
		append_root(roots, max_roots, &n_roots, user_root);
	}
	append_root(roots, max_roots, &n_roots, "/Library/Application Support/" WORKSPACE_CONTROLLER_SUBDIR);
#else
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg != NULL && *xdg != '\0') {
		snprintf(user_root, sizeof(user_root), "%s/" WORKSPACE_CONTROLLER_SUBDIR, xdg);
		append_root(roots, max_roots, &n_roots, user_root);
	} else if (home != NULL && *home != '\0') {
		snprintf(user_root, sizeof(user_root), "%s/.local/share/" WORKSPACE_CONTROLLER_SUBDIR, home);
		append_root(roots, max_roots, &n_roots, user_root);
	}
	append_root(roots, max_roots, &n_roots, "/usr/local/share/displayxr/WorkspaceControllers");
	append_root(roots, max_roots, &n_roots, "/usr/share/displayxr/WorkspaceControllers");
#endif

	return n_roots;
}

//! Scan one directory for `*.json` manifests, appending valid+existing
//! controllers to @p out[start..max). Earlier roots shadow later ones by id.
static int
enumerate_dir(const char *root, struct workspace_controller_entry *out, int start, int max)
{
	DIR *d = opendir(root);
	if (d == NULL) {
		return start;
	}

	int count = start;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (count >= max) {
			U_LOG_W("workspace registry: more than %d entries — truncating.", max);
			break;
		}
		const char *name = de->d_name;
		size_t n = strlen(name);
		if (n < 6 || strcmp(name + n - 5, ".json") != 0) {
			continue;
		}

		char manifest_path[PATH_MAX];
		snprintf(manifest_path, sizeof(manifest_path), "%s/%s", root, name);

		// Fallback id is the basename minus the ".json" suffix.
		char fallback_id[64];
		size_t stem = n - 5;
		if (stem >= sizeof(fallback_id)) {
			stem = sizeof(fallback_id) - 1;
		}
		memcpy(fallback_id, name, stem);
		fallback_id[stem] = '\0';

		struct workspace_controller_entry e;
		if (!parse_manifest(manifest_path, fallback_id, &e)) {
			continue;
		}
		if (already_have_id(out, count, e.id)) {
			continue; // per-user shadows system-wide
		}
		out[count++] = e;
	}
	closedir(d);
	return count;
}

int
service_workspace_registry_enumerate(struct workspace_controller_entry *out, int max_entries)
{
	if (out == NULL || max_entries <= 0) {
		return 0;
	}

	char roots[8][PATH_MAX];
	int n_roots = build_discovery_roots(roots, 8);

	int count = 0;
	for (int i = 0; i < n_roots && count < max_entries; i++) {
		count = enumerate_dir(roots[i], out, count, max_entries);
	}
	return count;
}

bool
service_workspace_registry_lookup(const char *id, struct workspace_controller_entry *out)
{
	if (id == NULL || id[0] == '\0' || out == NULL) {
		return false;
	}

	struct workspace_controller_entry entries[WORKSPACE_REGISTRY_MAX_ENTRIES];
	int count = service_workspace_registry_enumerate(entries, WORKSPACE_REGISTRY_MAX_ENTRIES);
	for (int i = 0; i < count; i++) {
		if (strcmp(entries[i].id, id) == 0) {
			*out = entries[i];
			return true;
		}
	}
	return false;
}

#endif // XRT_OS_WINDOWS
