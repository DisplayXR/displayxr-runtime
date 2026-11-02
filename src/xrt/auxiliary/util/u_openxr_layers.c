// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Find a registered OpenXR implicit API layer by name. See the header.
 * @ingroup aux_util
 */

#include "util/u_openxr_layers.h"

#include "util/u_json.h"

#include "xrt/xrt_config_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include "xrt/xrt_windows.h"
#endif


static void
copy_str(char *dst, size_t size, const char *src)
{
	if (dst == NULL || size == 0) {
		return;
	}
	snprintf(dst, size, "%s", src != NULL ? src : "");
}

bool
u_openxr_layer_manifest_declares(const char *json_text,
                                 const char *layer_name,
                                 char *out_disable_env,
                                 size_t disable_env_size,
                                 char *out_enable_env,
                                 size_t enable_env_size)
{
	copy_str(out_disable_env, disable_env_size, "");
	copy_str(out_enable_env, enable_env_size, "");
	if (json_text == NULL || layer_name == NULL || layer_name[0] == '\0') {
		return false;
	}
	cJSON *root = cJSON_Parse(json_text);
	if (root == NULL) {
		return false;
	}
	bool match = false;
	const cJSON *layer = cJSON_GetObjectItemCaseSensitive(root, "api_layer");
	const cJSON *name = cJSON_IsObject(layer) ? cJSON_GetObjectItemCaseSensitive(layer, "name") : NULL;
	if (cJSON_IsString(name) && name->valuestring != NULL && strcmp(name->valuestring, layer_name) == 0) {
		match = true;
		const cJSON *dis = cJSON_GetObjectItemCaseSensitive(layer, "disable_environment");
		if (cJSON_IsString(dis)) {
			copy_str(out_disable_env, disable_env_size, dis->valuestring);
		}
		const cJSON *en = cJSON_GetObjectItemCaseSensitive(layer, "enable_environment");
		if (cJSON_IsString(en)) {
			copy_str(out_enable_env, enable_env_size, en->valuestring);
		}
	}
	cJSON_Delete(root);
	return match;
}


void
u_openxr_layer_ultraleap_advice(const struct u_openxr_implicit_layer *layer, char *buf, size_t size)
{
	if (buf == NULL || size == 0) {
		return;
	}
	const char *env =
	    layer != NULL && layer->disable_env[0] != '\0' ? layer->disable_env : U_OPENXR_LAYER_ULTRALEAP_DISABLE_ENV;
	snprintf(buf, size,
	         "Ultraleap's implicit OpenXR API layer %s is registered and enabled (%s %s). The OpenXR loader puts "
	         "it in every OpenXR app, and it resets the Leap tracking service to Head-mounted mode: with a Leap on "
	         "the desk the hands come back left/right-swapped and sideways motion turns into depth. Workaround: "
	         "set %s=1 in the user environment, restart OpenXR apps, then set Desktop mode in the Ultraleap "
	         "Control Panel.",
	         U_OPENXR_LAYER_ULTRALEAP_HAND_TRACKING, layer != NULL ? layer->hive : "?",
	         layer != NULL ? layer->manifest_path : "?", env);
}


#ifdef XRT_OS_WINDOWS

//! A manifest is a few hundred bytes; anything past this is not one.
#define U_OXR_LAYER_MANIFEST_MAX (64 * 1024)

static char *
read_small_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	char *buf = (char *)malloc(U_OXR_LAYER_MANIFEST_MAX + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	size_t n = fread(buf, 1, U_OXR_LAYER_MANIFEST_MAX, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

/*!
 * Set to a non-empty value, as the loader reads it. Through the Win32
 * environment block, not the CRT's: a DLL with a static CRT has its own copy.
 */
static bool
env_is_set(const char *name)
{
	if (name == NULL || name[0] == '\0') {
		return false;
	}
	char tmp[8];
	// Returns the length (or the size needed when tmp is too small); 0 = unset or empty.
	return GetEnvironmentVariableA(name, tmp, (DWORD)sizeof(tmp)) != 0;
}

static void
scan_hive(HKEY hive, const char *hive_name, REGSAM view, const char *layer_name, struct u_openxr_implicit_layer *out)
{
	HKEY key = NULL;
	if (RegOpenKeyExA(hive, "Software\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ | view, &key) !=
	    ERROR_SUCCESS) {
		return;
	}
	for (DWORD i = 0;; i++) {
		char path[512];
		DWORD path_len = (DWORD)sizeof(path);
		DWORD type = 0;
		DWORD value = 1;
		DWORD value_len = (DWORD)sizeof(value);
		LONG rc = RegEnumValueA(key, i, path, &path_len, NULL, &type, (LPBYTE)&value, &value_len);
		if (rc == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (rc != ERROR_SUCCESS) {
			continue; // a value too long for the buffers: not a manifest we can read
		}
		char *text = read_small_file(path);
		if (text == NULL) {
			continue;
		}
		struct u_openxr_implicit_layer cand = {0};
		const bool match =
		    u_openxr_layer_manifest_declares(text, layer_name, cand.disable_env, sizeof(cand.disable_env),
		                                     cand.enable_env, sizeof(cand.enable_env));
		free(text);
		if (!match) {
			continue;
		}
		cand.found = true;
		cand.enabled_in_registry = type == REG_DWORD && value == 0;
		cand.disabled_by_env = env_is_set(cand.disable_env);
		cand.not_enabled_by_env = cand.enable_env[0] != '\0' && !env_is_set(cand.enable_env);
		cand.active = cand.enabled_in_registry && !cand.disabled_by_env && !cand.not_enabled_by_env;
		copy_str(cand.hive, sizeof(cand.hive), hive_name);
		copy_str(cand.manifest_path, sizeof(cand.manifest_path), path);
		// First match wins, unless a later one is active and the kept one is not.
		if (!out->found || (cand.active && !out->active)) {
			*out = cand;
		}
	}
	RegCloseKey(key);
}

bool
u_openxr_implicit_layer_find(const char *layer_name, struct u_openxr_implicit_layer *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (layer_name == NULL || layer_name[0] == '\0') {
		return false;
	}
	scan_hive(HKEY_LOCAL_MACHINE, "HKLM", KEY_WOW64_64KEY, layer_name, out);
	scan_hive(HKEY_CURRENT_USER, "HKCU", 0, layer_name, out);
	return out->found;
}

#else

bool
u_openxr_implicit_layer_find(const char *layer_name, struct u_openxr_implicit_layer *out)
{
	(void)layer_name;
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	return false;
}

#endif
