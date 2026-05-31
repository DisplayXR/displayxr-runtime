// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the shared CLI query core + serializers.
 * @author David Fattal
 */

#include "cli_query.h"

#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_config_os.h"

#include "util/u_git_tag.h"

#include "target_plugin_loader.h"

#include <cjson/cJSON.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


/*
 *
 * Query.
 *
 */

#ifdef XRT_OS_WINDOWS
static void
read_active_runtime(struct cli_query_result *r)
{
	r->active_runtime_queried = true;
	wchar_t wbuf[1024];
	DWORD wbuf_bytes = sizeof(wbuf);
	LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Khronos\\OpenXR\\1", L"ActiveRuntime",
	                          RRF_RT_REG_SZ, NULL, wbuf, &wbuf_bytes);
	if (rc == ERROR_SUCCESS) {
		WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, r->active_runtime, (int)sizeof(r->active_runtime), NULL,
		                    NULL);
		r->active_runtime_set = r->active_runtime[0] != '\0';
	}
}
#endif

void
cli_query_run(struct cli_query_result *r)
{
	memset(r, 0, sizeof(*r));
	snprintf(r->runtime_description, sizeof(r->runtime_description), "%s", u_runtime_description);
	snprintf(r->git_tag, sizeof(r->git_tag), "%s", u_git_tag);
	r->plugin_abi_version = (uint32_t)XRT_PLUGIN_API_VERSION_CURRENT;
	r->result_code = CLI_SELFTEST_INIT_FAIL;

#ifdef XRT_OS_WINDOWS
	read_active_runtime(r);
#endif

	struct xrt_instance *xi = NULL;
	struct xrt_system *xsys = NULL;
	struct xrt_system_devices *xsysd = NULL;
	struct xrt_space_overseer *xso = NULL;

	if (xrt_instance_create(NULL, &xi) != 0) {
		return; // instance_ok stays false, result_code = INIT_FAIL
	}
	r->instance_ok = true;

	xrt_result_t xret = xrt_instance_create_system(xi, &xsys, &xsysd, &xso, NULL);
	if (xret != XRT_SUCCESS || xsysd == NULL) {
		r->result_code = CLI_SELFTEST_INIT_FAIL;
		goto out;
	}
	r->system_ok = true;

	// The head role is the display-processor-backed device the active
	// plug-in created through the builder. No head = no display.
	struct xrt_device *head = xsysd->static_roles.head;
	if (head == NULL) {
		r->result_code = CLI_SELFTEST_NO_DP;
		goto out;
	}
	r->head_ok = true;
	snprintf(r->head_str, sizeof(r->head_str), "%s", head->str);

	const struct xrt_plugin_iface *iface = target_plugin_get_active();
	if (iface == NULL) {
		r->result_code = CLI_SELFTEST_NO_DP;
		goto out;
	}
	r->plugin_ok = true;
	snprintf(r->plugin_id, sizeof(r->plugin_id), "%s", iface->id ? iface->id : "");
	snprintf(r->plugin_name, sizeof(r->plugin_name), "%s", iface->display_name ? iface->display_name : "");
	snprintf(r->plugin_vendor, sizeof(r->plugin_vendor), "%s", iface->vendor ? iface->vendor : "");
	snprintf(r->plugin_version, sizeof(r->plugin_version), "%s", iface->version ? iface->version : "");

	if (iface->get_display_info == NULL) {
		r->result_code = CLI_SELFTEST_BAD_INFO;
		goto out;
	}

	struct xrt_plugin_display_info info = {0};
	info.struct_size = (uint32_t)sizeof(info);
	if (!iface->get_display_info(target_plugin_get_active_instance(), head, &info)) {
		r->result_code = CLI_SELFTEST_BAD_INFO;
		goto out;
	}
	r->display_info = info;
	r->display_info_ok = true;

	if (!(info.display_width_m > 0.0f) || !(info.display_height_m > 0.0f) || info.display_pixel_width == 0 ||
	    info.display_pixel_height == 0) {
		r->result_code = CLI_SELFTEST_BAD_INFO;
		goto out;
	}
	r->dims_ok = true;
	r->result_code = CLI_SELFTEST_PASS;

out:
	xrt_space_overseer_destroy(&xso);
	xrt_system_devices_destroy(&xsysd);
	xrt_system_destroy(&xsys);
	xrt_instance_destroy(&xi);
}


/*
 *
 * Info serializers.
 *
 */

#define P(...) printf(__VA_ARGS__)
#define PT(...) printf("\t" __VA_ARGS__)

static const char *
or_q(const char *s)
{
	return (s != NULL && s[0] != '\0') ? s : "?";
}

/*!
 * Decode the eye-tracking-mode bitmask into a human label. Bits per
 * `xrt_plugin_display_info`: 0x1 = MANAGED, 0x2 = MANUAL.
 */
static const char *
eye_modes_label(uint32_t mask, char *buf, size_t cap)
{
	buf[0] = '\0';
	if (mask & 0x1u) {
		snprintf(buf, cap, "MANAGED");
	}
	if (mask & 0x2u) {
		size_t n = strlen(buf);
		snprintf(buf + n, cap - n, "%sMANUAL", n ? "|" : "");
	}
	if (buf[0] == '\0') {
		snprintf(buf, cap, "none");
	}
	return buf;
}

/*!
 * Decode the default-eye-tracking-mode value (0 = MANAGED, 1 = MANUAL).
 */
static const char *
eye_default_label(uint32_t def)
{
	switch (def) {
	case 0: return "MANAGED";
	case 1: return "MANUAL";
	default: return "?";
	}
}

void
cli_query_print_info_text(const struct cli_query_result *r)
{
	P(" :: Runtime\n");
	PT("description: '%s'\n", r->runtime_description);
	PT("git-tag:     '%s'\n", r->git_tag);
	PT("plug-in ABI: v%u (runtime speaks XRT_PLUGIN_API_VERSION_CURRENT)\n", (unsigned)r->plugin_abi_version);

	if (r->active_runtime_queried) {
		P(" :: Active OpenXR runtime (HKLM\\Software\\Khronos\\OpenXR\\1\\ActiveRuntime)\n");
		PT("%s\n", r->active_runtime_set ? r->active_runtime : "<unset>");
	}

	P(" :: Display processor\n");
	if (!r->head_ok) {
		PT("No display processor discovered.\n");
		return;
	}
	if (!r->plugin_ok) {
		PT("device: '%s' (no active vendor plug-in iface)\n", r->head_str);
		return;
	}

	PT("plug-in: id='%s' name='%s' vendor='%s' version='%s'\n", or_q(r->plugin_id), or_q(r->plugin_name),
	   or_q(r->plugin_vendor), or_q(r->plugin_version));
	PT("ABI:     v%u (loader-verified match)\n", (unsigned)r->plugin_abi_version);
	PT("device:  '%s'\n", r->head_str);

	if (!r->display_info_ok) {
		PT("get_display_info unavailable or returned false.\n");
		return;
	}
	const struct xrt_plugin_display_info *i = &r->display_info;
	PT("physical:     %.4fm x %.4fm\n", (double)i->display_width_m, (double)i->display_height_m);
	PT("pixels:       %ux%u\n", i->display_pixel_width, i->display_pixel_height);
	PT("viewer:       (%.4f, %.4f, %.4f) m\n", (double)i->nominal_viewer_x_m, (double)i->nominal_viewer_y_m,
	   (double)i->nominal_viewer_z_m);
	PT("view scale:   (%.3f, %.3f)\n", (double)i->recommended_view_scale_x, (double)i->recommended_view_scale_y);
	PT("screen pos:   (%d, %d)\n", i->display_screen_left, i->display_screen_top);
	char et_buf[64];
	PT("eye-tracking: supported=%s (0x%x) default=%s\n",
	   eye_modes_label(i->supported_eye_tracking_modes, et_buf, sizeof(et_buf)),
	   i->supported_eye_tracking_modes, eye_default_label(i->default_eye_tracking_mode));
}

void
cli_query_print_info_json(const struct cli_query_result *r)
{
	cJSON *root = cJSON_CreateObject();

	cJSON *rt = cJSON_AddObjectToObject(root, "runtime");
	cJSON_AddStringToObject(rt, "description", r->runtime_description);
	cJSON_AddStringToObject(rt, "git_tag", r->git_tag);
	cJSON_AddNumberToObject(rt, "plugin_abi_version", (double)r->plugin_abi_version);

	if (r->active_runtime_queried) {
		cJSON *ar = cJSON_AddObjectToObject(root, "active_openxr_runtime");
		cJSON_AddBoolToObject(ar, "set", r->active_runtime_set);
		if (r->active_runtime_set) {
			cJSON_AddStringToObject(ar, "value", r->active_runtime);
		} else {
			cJSON_AddNullToObject(ar, "value");
		}
	}

	if (r->plugin_ok) {
		cJSON *pl = cJSON_AddObjectToObject(root, "plugin");
		cJSON_AddStringToObject(pl, "id", r->plugin_id);
		cJSON_AddStringToObject(pl, "display_name", r->plugin_name);
		cJSON_AddStringToObject(pl, "vendor", r->plugin_vendor);
		cJSON_AddStringToObject(pl, "version", r->plugin_version);
		cJSON_AddNumberToObject(pl, "abi_version", (double)r->plugin_abi_version);
	} else {
		cJSON_AddNullToObject(root, "plugin");
	}

	if (r->head_ok) {
		cJSON_AddStringToObject(root, "device", r->head_str);
	} else {
		cJSON_AddNullToObject(root, "device");
	}

	if (r->display_info_ok) {
		const struct xrt_plugin_display_info *i = &r->display_info;
		cJSON *d = cJSON_AddObjectToObject(root, "display");
		cJSON_AddNumberToObject(d, "physical_width_m", (double)i->display_width_m);
		cJSON_AddNumberToObject(d, "physical_height_m", (double)i->display_height_m);
		cJSON_AddNumberToObject(d, "pixel_width", (double)i->display_pixel_width);
		cJSON_AddNumberToObject(d, "pixel_height", (double)i->display_pixel_height);
		cJSON *v = cJSON_AddObjectToObject(d, "viewer_m");
		cJSON_AddNumberToObject(v, "x", (double)i->nominal_viewer_x_m);
		cJSON_AddNumberToObject(v, "y", (double)i->nominal_viewer_y_m);
		cJSON_AddNumberToObject(v, "z", (double)i->nominal_viewer_z_m);
		cJSON *s = cJSON_AddObjectToObject(d, "recommended_view_scale");
		cJSON_AddNumberToObject(s, "x", (double)i->recommended_view_scale_x);
		cJSON_AddNumberToObject(s, "y", (double)i->recommended_view_scale_y);
		cJSON *sp = cJSON_AddObjectToObject(d, "screen_pos");
		cJSON_AddNumberToObject(sp, "left", (double)i->display_screen_left);
		cJSON_AddNumberToObject(sp, "top", (double)i->display_screen_top);
		cJSON *et = cJSON_AddObjectToObject(d, "eye_tracking");
		cJSON_AddNumberToObject(et, "supported_modes", (double)i->supported_eye_tracking_modes);
		cJSON_AddNumberToObject(et, "default_mode", (double)i->default_eye_tracking_mode);
		char et_buf[64];
		cJSON_AddStringToObject(et, "supported_label",
		                        eye_modes_label(i->supported_eye_tracking_modes, et_buf, sizeof(et_buf)));
		cJSON_AddStringToObject(et, "default_label", eye_default_label(i->default_eye_tracking_mode));
	} else {
		cJSON_AddNullToObject(root, "display");
	}

	char *out = cJSON_Print(root);
	if (out != NULL) {
		printf("%s\n", out);
		cJSON_free(out);
	}
	cJSON_Delete(root);
}


/*
 *
 * Self-test serializers.
 *
 */

struct check
{
	const char *name;
	bool ok;
	char detail[256];
};

/*!
 * Build the ordered list of checks the self-test reports. Returns the count
 * and whether the overall verdict is PASS. A check is only meaningful up to
 * the first failure; later checks are reported as not-run (ok=false) only
 * when the run got far enough to evaluate them.
 */
static int
build_checks(const struct cli_query_result *r, struct check *out)
{
	int n = 0;
	struct check *c;

	c = &out[n++];
	c->name = "instance";
	c->ok = r->instance_ok;
	snprintf(c->detail, sizeof(c->detail), "%s", r->instance_ok ? "xrt_instance_create ok" : "creation failed");

	c = &out[n++];
	c->name = "system";
	c->ok = r->system_ok;
	snprintf(c->detail, sizeof(c->detail), "%s",
	         r->system_ok ? "system devices created" : "xrt_instance_create_system failed");

	c = &out[n++];
	c->name = "head_device";
	c->ok = r->head_ok;
	snprintf(c->detail, sizeof(c->detail), "%s", r->head_ok ? r->head_str : "no head/display device");

	c = &out[n++];
	c->name = "active_plugin";
	c->ok = r->plugin_ok;
	if (r->plugin_ok) {
		snprintf(c->detail, sizeof(c->detail), "id='%s' name='%s' (ABI v%u)", or_q(r->plugin_id),
		         or_q(r->plugin_name), (unsigned)r->plugin_abi_version);
	} else {
		snprintf(c->detail, sizeof(c->detail), "%s", "no active vendor plug-in");
	}

	c = &out[n++];
	c->name = "display_info";
	c->ok = r->display_info_ok;
	snprintf(c->detail, sizeof(c->detail), "%s",
	         r->display_info_ok ? "get_display_info returned valid struct" : "get_display_info missing/false");

	c = &out[n++];
	c->name = "display_dims";
	c->ok = r->dims_ok;
	if (r->display_info_ok) {
		const struct xrt_plugin_display_info *i = &r->display_info;
		snprintf(c->detail, sizeof(c->detail), "%.4fm x %.4fm, %ux%u px", (double)i->display_width_m,
		         (double)i->display_height_m, i->display_pixel_width, i->display_pixel_height);
	} else {
		snprintf(c->detail, sizeof(c->detail), "%s", "not evaluated");
	}

	return n;
}

void
cli_query_print_selftest_text(const struct cli_query_result *r)
{
	P(" :: DisplayXR CLI self-test (headless, no compositor)\n");

	struct check checks[8];
	int n = build_checks(r, checks);
	for (int i = 0; i < n; i++) {
		P("%s: %s — %s\n", checks[i].ok ? "PASS" : "FAIL", checks[i].name, checks[i].detail);
	}

	if (r->result_code == CLI_SELFTEST_PASS) {
		P(" :: SELF-TEST PASSED\n");
	} else {
		P(" :: SELF-TEST FAILED (rc=%d)\n", (int)r->result_code);
	}
}

void
cli_query_print_selftest_json(const struct cli_query_result *r)
{
	cJSON *root = cJSON_CreateObject();

	struct check checks[8];
	int n = build_checks(r, checks);
	cJSON *arr = cJSON_AddArrayToObject(root, "checks");
	for (int i = 0; i < n; i++) {
		cJSON *c = cJSON_CreateObject();
		cJSON_AddStringToObject(c, "name", checks[i].name);
		cJSON_AddBoolToObject(c, "ok", checks[i].ok);
		cJSON_AddStringToObject(c, "detail", checks[i].detail);
		cJSON_AddItemToArray(arr, c);
	}

	cJSON_AddStringToObject(root, "verdict", r->result_code == CLI_SELFTEST_PASS ? "PASS" : "FAIL");
	cJSON_AddNumberToObject(root, "result_code", (double)r->result_code);

	char *out = cJSON_Print(root);
	if (out != NULL) {
		printf("%s\n", out);
		cJSON_free(out);
	}
	cJSON_Delete(root);
}
