// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Code to manage the settings file.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup st_prober
 */

#include <xrt/xrt_device.h>
#include "xrt/xrt_settings.h"
#include "xrt/xrt_config.h"

#include "util/u_file.h"
#include "util/u_json.h"
#include "util/u_debug.h"

#include "u_config_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bindings/b_generated_bindings_helpers.h"

DEBUG_GET_ONCE_OPTION(active_config, "P_OVERRIDE_ACTIVE_CONFIG", NULL)

#define CONFIG_FILE_NAME "config_v0.json"

void
u_config_json_close(struct u_config_json *json)
{
	if (json->root != NULL) {
		cJSON_Delete(json->root);
		json->root = NULL;
	}
	json->file_loaded = false;
}

static void
u_config_json_open_or_create_file(struct u_config_json *json, const char *filename)
{
	json->file_loaded = false;
#if (defined(XRT_OS_LINUX) || defined(XRT_OS_WINDOWS)) && !defined(XRT_OS_ANDROID)
	char tmp[1024];
	ssize_t ret = u_file_get_path_in_config_dir(filename, tmp, sizeof(tmp));
	if (ret <= 0) {
		U_LOG_E(
		    "Could not load or create config file no $HOME "
		    "or $XDG_CONFIG_HOME env variables defined");
		return;
	}

	FILE *file = u_file_open_file_in_config_dir(filename, "rb");
	if (file == NULL) {
		return;
	}

	json->file_loaded = true;

	char *str = u_file_read_content(file, NULL);
	fclose(file);
	if (str == NULL) {
		U_LOG_E("Could not read the contents of '%s'!", tmp);
		return;
	}

	// No config created, ignore.
	if (strlen(str) == 0) {
		free(str);
		return;
	}

	json->root = cJSON_Parse(str);
	if (json->root == NULL) {
		U_LOG_E("Failed to parse JSON in '%s':\n%s\n#######", tmp, str);
		U_LOG_E("'%s'", cJSON_GetErrorPtr());
	}

	free(str);
#else
	//! @todo implement the underlying u_file_get_path_in_config_dir
	return;
#endif
}

void
u_config_json_open_or_create_main_file(struct u_config_json *json)
{
	u_config_json_open_or_create_file(json, CONFIG_FILE_NAME);
}

static cJSON *
get_obj(cJSON *json, const char *name)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
	if (item == NULL) {
		U_LOG_I("JSON does not contain node '%s'!", name);
	}
	return item;
}

static bool
get_obj_float(cJSON *json, const char *name, float *out_float)
{
	if (json == NULL) {
		return false;
	}
	cJSON *item = get_obj(json, name);
	if (item == NULL) {
		return false;
	}

	if (!u_json_get_float(item, out_float)) {
		U_LOG_E("Failed to parse '%s'!", name);
		return false;
	}

	return true;
}

static bool
get_obj_str(cJSON *json, const char *name, char *array, size_t array_size)
{
	if (json == NULL) {
		return false;
	}
	cJSON *item = get_obj(json, name);
	if (item == NULL) {
		return false;
	}

	if (!u_json_get_string_into_array(item, array, array_size)) {
		U_LOG_E("Failed to parse '%s'!", name);
		return false;
	}

	return true;
}

static bool
is_json_ok(struct u_config_json *json)
{
	if (json->root == NULL) {
		if (json->file_loaded) {
			U_LOG_E("Config file was loaded but JSON is not parsed!");
		} else {
			U_LOG_I("No config file was loaded!");
		}
		return false;
	}

	return true;
}

static void
u_config_json_assign_schema(struct u_config_json *json)
{
	cJSON_DeleteItemFromObject(json->root, "$schema");
	cJSON_AddStringToObject(json->root, "$schema",
	                        "https://monado.pages.freedesktop.org/monado/config_v0.schema.json");
}

static bool
parse_active(const char *str, const char *from, enum u_config_json_active_config *out_active)
{
	if (strcmp(str, "none") == 0) {
		*out_active = U_ACTIVE_CONFIG_NONE;
	} else if (strcmp(str, "tracking") == 0) {
		*out_active = U_ACTIVE_CONFIG_TRACKING;
	} else if (strcmp(str, "remote") == 0) {
		*out_active = U_ACTIVE_CONFIG_REMOTE;
	} else {
		U_LOG_E("Unknown active config '%s' from %s.", str, from);
		*out_active = U_ACTIVE_CONFIG_NONE;
		return false;
	}

	return true;
}

void
u_config_json_get_active(struct u_config_json *json, enum u_config_json_active_config *out_active)
{
	const char *str = debug_get_option_active_config();
	if (str != NULL && parse_active(str, "environment", out_active)) {
		return;
	}

	char tmp[256];
	if (!is_json_ok(json) || !get_obj_str(json->root, "active", tmp, sizeof(tmp))) {
		*out_active = U_ACTIVE_CONFIG_NONE;
		return;
	}

	parse_active(tmp, "json", out_active);
}

static cJSON *
open_tracking_settings(struct u_config_json *json)
{
	if (!is_json_ok(json)) {
		return NULL;
	}

	cJSON *t = cJSON_GetObjectItemCaseSensitive(json->root, "tracking");
	if (t == NULL) {
		U_LOG_I("Config file does not contain tracking config");
		return NULL;
	}

	return t;
}

bool
u_config_json_get_tracking_overrides(struct u_config_json *json,
                                     struct xrt_tracking_override *out_overrides,
                                     size_t *out_override_count)
{
	cJSON *t = open_tracking_settings(json);
	if (t == NULL) {
		return false;
	}


	cJSON *overrides = cJSON_GetObjectItemCaseSensitive(t, "tracking_overrides");

	*out_override_count = 0;

	cJSON *override = NULL;
	cJSON_ArrayForEach(override, overrides)
	{
		bool bad = false;

		struct xrt_tracking_override *o = &out_overrides[(*out_override_count)++];
		bad |= !get_obj_str(override, "target_device_serial", o->target_device_serial, XRT_DEVICE_NAME_LEN);
		bad |= !get_obj_str(override, "tracker_device_serial", o->tracker_device_serial, XRT_DEVICE_NAME_LEN);

		char override_type[256];
		bad |= !get_obj_str(override, "type", override_type, sizeof(override_type));
		if (strncmp(override_type, "direct", sizeof(override_type) - 1) == 0) {
			o->override_type = XRT_TRACKING_OVERRIDE_DIRECT;
		} else if (strncmp(override_type, "attached", sizeof(override_type) - 1) == 0) {
			o->override_type = XRT_TRACKING_OVERRIDE_ATTACHED;
		}

		cJSON *offset = cJSON_GetObjectItemCaseSensitive(override, "offset");
		if (offset) {
			cJSON *orientation = cJSON_GetObjectItemCaseSensitive(offset, "orientation");
			bad |= !get_obj_float(orientation, "x", &o->offset.orientation.x);
			bad |= !get_obj_float(orientation, "y", &o->offset.orientation.y);
			bad |= !get_obj_float(orientation, "z", &o->offset.orientation.z);
			bad |= !get_obj_float(orientation, "w", &o->offset.orientation.w);

			cJSON *position = cJSON_GetObjectItemCaseSensitive(offset, "position");
			bad |= !get_obj_float(position, "x", &o->offset.position.x);
			bad |= !get_obj_float(position, "y", &o->offset.position.y);
			bad |= !get_obj_float(position, "z", &o->offset.position.z);
		} else {
			o->offset.orientation.w = 1;
		}

		char input_name[512] = {'\0'};
		get_obj_str(override, "xrt_input_name", input_name, 512);
		o->input_name = xrt_input_name_enum(input_name);

		if (bad) {
			*out_override_count = 0;
			return false;
		}
	}
	return true;
}

static void
u_config_json_make_default_root(struct u_config_json *json)
{
	json->root = cJSON_CreateObject();
}

static void
u_config_write(struct u_config_json *json, const char *filename)
{
	char *str = cJSON_Print(json->root);
	U_LOG_D("%s", str);

	FILE *config_file = u_file_open_file_in_config_dir(filename, "w");
	fprintf(config_file, "%s\n", str);
	fflush(config_file);
	fclose(config_file);
	config_file = NULL;
	free(str);
}

static cJSON *
make_pose(struct xrt_pose *pose)
{
	cJSON *json = cJSON_CreateObject();

	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "x", pose->orientation.x);
	cJSON_AddNumberToObject(o, "y", pose->orientation.y);
	cJSON_AddNumberToObject(o, "z", pose->orientation.z);
	cJSON_AddNumberToObject(o, "w", pose->orientation.w);
	cJSON_AddItemToObject(json, "orientation", o);

	cJSON *p = cJSON_CreateObject();
	cJSON_AddNumberToObject(p, "x", pose->position.x);
	cJSON_AddNumberToObject(p, "y", pose->position.y);
	cJSON_AddNumberToObject(p, "z", pose->position.z);
	cJSON_AddItemToObject(json, "position", p);

	return json;
}

void
u_config_json_save_overrides(struct u_config_json *json, struct xrt_tracking_override *overrides, size_t override_count)
{
	if (!json->file_loaded) {
		u_config_json_make_default_root(json);
	}
	u_config_json_assign_schema(json);
	cJSON *root = json->root;

	cJSON *t = cJSON_GetObjectItem(root, "tracking");
	if (!t) {
		t = cJSON_AddObjectToObject(root, "tracking");
	}

	cJSON_DeleteItemFromObject(t, "tracking_overrides");
	cJSON *o = cJSON_AddArrayToObject(t, "tracking_overrides");

	for (size_t i = 0; i < override_count; i++) {
		cJSON *entry = cJSON_CreateObject();

		cJSON_AddStringToObject(entry, "target_device_serial", overrides[i].target_device_serial);
		cJSON_AddStringToObject(entry, "tracker_device_serial", overrides[i].tracker_device_serial);

		char buffer[256];
		switch (overrides[i].override_type) {
		case XRT_TRACKING_OVERRIDE_DIRECT: snprintf(buffer, ARRAY_SIZE(buffer), "direct"); break;
		case XRT_TRACKING_OVERRIDE_ATTACHED: snprintf(buffer, ARRAY_SIZE(buffer), "attached"); break;
		}
		cJSON_AddStringToObject(entry, "type", buffer);

		cJSON_AddItemToObject(entry, "offset", make_pose(&overrides[i].offset));

		const char *input_name_string = xrt_input_name_string(overrides[i].input_name);
		cJSON_AddStringToObject(entry, "xrt_input_name", input_name_string);

		cJSON_AddItemToArray(o, entry);
	}

	u_config_write(json, CONFIG_FILE_NAME);
}
