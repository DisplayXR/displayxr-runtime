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

int
service_workspace_registry_enumerate(struct workspace_controller_entry *out,
                                     int max_entries)
{
	(void)out;
	(void)max_entries;
	return 0;
}

bool
service_workspace_registry_lookup(const char *id,
                                  struct workspace_controller_entry *out)
{
	(void)id;
	(void)out;
	return false;
}

#endif // XRT_OS_WINDOWS
