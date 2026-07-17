// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the shared CLI query core + serializers.
 * @author David Fattal
 */

// COBJMACROS must be defined before ANY include: xrt_plugin.h / xrt_compositor.h
// transitively pull in <d3d11.h>, so defining it later (at the WIN32 block below)
// is too late — the guard has already skipped the C COM-macro definitions, and
// ID3D11Device_Release/ID3D11DeviceContext_Release link as unresolved externals.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#endif

#include "cli_query.h"

#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_compositor.h" // xrt_dp_factory_registry + XRT_DP_REGISTRY_MAX_ENTRIES
#include "xrt/xrt_config_os.h"

#include "os/os_display_edid.h"
#include "util/u_git_tag.h"

#include "target_plugin_loader.h"

#include <cjson/cJSON.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h> // WIN32_LEAN_AND_MEAN + COBJMACROS are set at the top of the file
#include <d3d11.h>   // WARP device for the headless zone-caps probe (#224 / ADR-027 P4)
#include "xrt/xrt_display_processor_d3d11.h"
#endif


/*
 *
 * Query.
 *
 */

#ifdef XRT_OS_WINDOWS
/*!
 * #224 / ADR-027 P4 — headless zone-caps probe. Creates a D3D11 WARP device
 * (no GPU / display required), asks the active plug-in's D3D11 DP factory
 * for a DP with a NULL window handle, queries get_local_zone_caps with a
 * caller-zeroed struct (struct_size pre-set per the append contract), then
 * tears everything down.
 *
 * Outcome contract: ABSENCE NEVER FAILS — no D3D11 factory, factory
 * failure, WARP failure, or a DP without the zone slots all leave
 * zone_caps_probed false with an informational note (an old Leia plug-in on
 * a user box must pass). Only a present-but-MALFORMED answer (supported > 1,
 * supported with a zero grid, wish_fractional > 1, switch_granularity out of
 * range) sets zone_caps_malformed.
 */
static void
probe_zone_caps_d3d11(struct cli_query_result *r, const struct xrt_plugin_iface *iface)
{
	// create_dp_d3d11 is a core v2 iface field (the loader rejects
	// mismatched ABI majors before we get here), so a NULL check suffices.
	if (iface->create_dp_d3d11 == NULL) {
		snprintf(r->zone_probe_note, sizeof(r->zone_probe_note),
		         "not probed: plug-in has no D3D11 DP factory (OK)");
		return;
	}

	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	D3D_FEATURE_LEVEL fl;
	HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, &fl,
	                               &context);
	if (FAILED(hr) || device == NULL || context == NULL) {
		snprintf(r->zone_probe_note, sizeof(r->zone_probe_note),
		         "not probed: WARP D3D11 device creation failed (0x%08lx, OK)", (unsigned long)hr);
		if (context != NULL) {
			ID3D11DeviceContext_Release(context);
		}
		if (device != NULL) {
			ID3D11Device_Release(device);
		}
		return;
	}

	struct xrt_display_processor_d3d11 *xdp = NULL;
	xrt_result_t xret = iface->create_dp_d3d11(device, context, NULL, &xdp);
	if (xret != XRT_SUCCESS || xdp == NULL) {
		snprintf(r->zone_probe_note, sizeof(r->zone_probe_note),
		         "not probed: D3D11 DP factory declined (xret=%d, OK)", (int)xret);
		ID3D11DeviceContext_Release(context);
		ID3D11Device_Release(device);
		return;
	}

	struct xrt_dp_local_zone_caps caps;
	memset(&caps, 0, sizeof(caps)); // append contract: caller zeroes, then sets struct_size
	caps.struct_size = (uint32_t)sizeof(caps);
	bool got = xrt_display_processor_d3d11_get_local_zone_caps(xdp, &caps);
	if (!got) {
		snprintf(r->zone_probe_note, sizeof(r->zone_probe_note),
		         "not probed: DP exposes no zone slots (legacy plug-in, OK)");
	} else {
		r->zone_caps_probed = true;
		r->zone_caps = caps;
		bool malformed = caps.supported > 1 ||
		                 (caps.supported == 1 && (caps.zone_grid_width == 0 || caps.zone_grid_height == 0)) ||
		                 caps.wish_fractional > 1 ||
		                 caps.switch_granularity > (uint32_t)XRT_DP_SWITCH_GRANULARITY_CELL_GRID;
		r->zone_caps_malformed = malformed;
		if (malformed) {
			snprintf(r->zone_probe_note, sizeof(r->zone_probe_note),
			         "MALFORMED caps: supported=%u grid=%ux%u wish_fractional=%u granularity=%u",
			         caps.supported, caps.zone_grid_width, caps.zone_grid_height, caps.wish_fractional,
			         caps.switch_granularity);
		} else {
			snprintf(r->zone_probe_note, sizeof(r->zone_probe_note),
			         "supported=%u grid=%ux%u max_mask=%ux%u max_hz=%u wish_fractional=%u granularity=%u",
			         caps.supported, caps.zone_grid_width, caps.zone_grid_height, caps.max_mask_width,
			         caps.max_mask_height, caps.max_update_hz, caps.wish_fractional,
			         caps.switch_granularity);
		}
	}

	xrt_display_processor_d3d11_destroy(&xdp);
	ID3D11DeviceContext_Release(context);
	ID3D11Device_Release(device);
}

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

//! Fallback for a plug-in that doesn't self-report a version through
//! xrt_plugin_iface::version: read the installer-written value from
//! HKLM\Software\DisplayXR\DisplayProcessors\<id>\Version. The discovery
//! registry is the authoritative install record (the NSI writes Version from
//! PROJECT_VERSION, and the loader already uses it for skew detection — #461),
//! so this surfaces a real version even for plug-ins built before adopting the
//! iface field. Leaves @p out untouched on any miss.
static void
read_plugin_version_from_registry(const char *id, char *out, size_t cap)
{
	if (id == NULL || id[0] == '\0') {
		return;
	}
	char subkey[256];
	snprintf(subkey, sizeof(subkey), "Software\\DisplayXR\\DisplayProcessors\\%s", id);

	HKEY key;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
		return;
	}
	char buf[128];
	DWORD len = sizeof(buf);
	DWORD type = 0;
	if (RegQueryValueExA(key, "Version", NULL, &type, (LPBYTE)buf, &len) == ERROR_SUCCESS && type == REG_SZ &&
	    len > 0) {
		buf[(len < sizeof(buf)) ? len : sizeof(buf) - 1] = '\0'; // RegQueryValueEx may omit the NUL
		if (buf[0] != '\0') {
			snprintf(out, cap, "%s", buf);
		}
	}
	RegCloseKey(key);
}
#endif

//! Human label for an xrt_display_claim_confidence value.
static const char *
dp_confidence_label(uint32_t c)
{
	switch (c) {
	case (uint32_t)XRT_DISPLAY_CLAIM_FALLBACK: return "FALLBACK";
	case (uint32_t)XRT_DISPLAY_CLAIM_EDID: return "EDID";
	case (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED: return "VERIFIED";
	default: return "?";
	}
}

/*!
 * Compute what DP each render path would pick and flag divergence. The
 * in-process path uses the active plug-in (scalar dp_factory). The service /
 * shell path uses the registry's primary entry — reproduced here with the same
 * `os_display_edid_enumerate` → `target_plugin_build_descriptors` →
 * `target_plugin_resolve_displays` sequence the compositor builds it from, then
 * reading entries[0] exactly as `comp_dp_factory_for_window(COMP_DP_PRIMARY_
 * MONITOR)` does. Runs after the active plug-in is known; safe headless (no
 * service, no GPU). Off-Windows the EDID enumerator yields no monitors, so the
 * registry is empty and the service path falls back to the scalar — reported as
 * agreement, never a false mismatch.
 */
static void
probe_dp_selection(struct cli_query_result *r, const struct xrt_plugin_iface *active)
{
	r->dp_sel_probed = true;
	snprintf(r->dp_sel_inproc_id, sizeof(r->dp_sel_inproc_id), "%s",
	         (active != NULL && active->id != NULL) ? active->id : "");

	struct os_display_edid_list list = {0};
	os_display_edid_enumerate(&list);

	struct xrt_display_descriptor descs[XRT_DP_REGISTRY_MAX_ENTRIES];
	uint32_t dn = target_plugin_build_descriptors(&list, descs, XRT_DP_REGISTRY_MAX_ENTRIES);
	r->dp_sel_monitor_count = dn;

	struct xrt_dp_factory_registry reg = {0};
	target_plugin_resolve_displays(descs, dn, &reg);
	r->dp_sel_claim_count = reg.entry_count;

	if (reg.entry_count > 0) {
		// The compositor passes COMP_DP_PRIMARY_MONITOR, which selects
		// entries[0]; mirror that exactly.
		const struct xrt_dp_registry_entry *e = &reg.entries[0];
		snprintf(r->dp_sel_service_id, sizeof(r->dp_sel_service_id), "%s", e->plugin_id);
		snprintf(r->dp_sel_service_conf, sizeof(r->dp_sel_service_conf), "%s",
		         dp_confidence_label(e->confidence));
		r->dp_sel_mismatch =
		    r->dp_sel_service_id[0] != '\0' && strcmp(r->dp_sel_service_id, r->dp_sel_inproc_id) != 0;
	} else {
		// Empty registry → comp_dp_factory_for_window returns the scalar,
		// i.e. the same plug-in the in-process path uses. No divergence.
		snprintf(r->dp_sel_service_id, sizeof(r->dp_sel_service_id), "%s", r->dp_sel_inproc_id);
		snprintf(r->dp_sel_service_conf, sizeof(r->dp_sel_service_conf), "%s", "scalar-fallback");
		r->dp_sel_mismatch = false;
	}
}

void
cli_query_fill(struct cli_query_result *r, struct cli_query_handles *h, const struct xrt_instance_info *ii)
{
	memset(r, 0, sizeof(*r));
	memset(h, 0, sizeof(*h));
	snprintf(r->runtime_description, sizeof(r->runtime_description), "%s", u_runtime_description);
	snprintf(r->git_tag, sizeof(r->git_tag), "%s", u_git_tag);
	r->plugin_abi_version = (uint32_t)XRT_PLUGIN_API_VERSION_CURRENT;
	r->result_code = CLI_SELFTEST_INIT_FAIL;

#ifdef XRT_OS_WINDOWS
	read_active_runtime(r);
#endif

	// xrt_instance_create takes a non-const ii but only reads it.
	if (xrt_instance_create((struct xrt_instance_info *)ii, &h->xi) != 0) {
		return; // instance_ok stays false, result_code = INIT_FAIL
	}
	r->instance_ok = true;

	xrt_result_t xret = xrt_instance_create_system(h->xi, &h->xsys, &h->xsysd, &h->xso, NULL);
	if (xret != XRT_SUCCESS || h->xsysd == NULL) {
		r->result_code = CLI_SELFTEST_INIT_FAIL;
		return;
	}
	r->system_ok = true;

	// The head role is the display-processor-backed device the active
	// plug-in created through the builder. No head = no display.
	struct xrt_device *head = h->xsysd->static_roles.head;
	if (head == NULL) {
		r->result_code = CLI_SELFTEST_NO_DP;
		return;
	}
	r->head_ok = true;
	snprintf(r->head_str, sizeof(r->head_str), "%s", head->str);

	// Rendering-mode snapshot incl. per-mode tracking flags (#441).
	r->rendering_mode_count = head->rendering_mode_count;
	if (r->rendering_mode_count > XRT_MAX_RENDERING_MODES) {
		r->rendering_mode_count = XRT_MAX_RENDERING_MODES;
	}
	for (uint32_t m = 0; m < r->rendering_mode_count; m++) {
		r->rendering_modes[m] = head->rendering_modes[m];
	}

	const struct xrt_plugin_iface *iface = target_plugin_get_active();
	if (iface == NULL) {
		r->result_code = CLI_SELFTEST_NO_DP;
		return;
	}
	r->plugin_ok = true;
	snprintf(r->plugin_id, sizeof(r->plugin_id), "%s", iface->id ? iface->id : "");
	snprintf(r->plugin_name, sizeof(r->plugin_name), "%s", iface->display_name ? iface->display_name : "");
	snprintf(r->plugin_vendor, sizeof(r->plugin_vendor), "%s", iface->vendor ? iface->vendor : "");
	snprintf(r->plugin_version, sizeof(r->plugin_version), "%s", iface->version ? iface->version : "");
#ifdef XRT_OS_WINDOWS
	// A plug-in that ships no iface version still has an installer-written
	// Version in its discovery registry key — surface that rather than "?".
	if (r->plugin_version[0] == '\0') {
		read_plugin_version_from_registry(r->plugin_id, r->plugin_version, sizeof(r->plugin_version));
	}
#endif

	// Which DP does each render path pick? Flags the in-process-vs-service
	// divergence that silently kills shell head-tracking (EDID-invisible
	// vendor panels). Independent of the display-info checks below.
	probe_dp_selection(r, iface);

	if (iface->get_display_info == NULL) {
		r->result_code = CLI_SELFTEST_BAD_INFO;
		return;
	}

	struct xrt_plugin_display_info info = {0};
	info.struct_size = (uint32_t)sizeof(info);
	if (!iface->get_display_info(target_plugin_get_active_instance(), head, &info)) {
		r->result_code = CLI_SELFTEST_BAD_INFO;
		return;
	}
	r->display_info = info;
	r->display_info_ok = true;

	if (!(info.display_width_m > 0.0f) || !(info.display_height_m > 0.0f) || info.display_pixel_width == 0 ||
	    info.display_pixel_height == 0) {
		r->result_code = CLI_SELFTEST_BAD_INFO;
		return;
	}
	r->dims_ok = true;
	r->result_code = CLI_SELFTEST_PASS;

	// #224 / ADR-027 P4 — zone-caps probe, after the display-info checks
	// passed. Absence never fails; only malformed caps flip the verdict.
#ifdef XRT_OS_WINDOWS
	probe_zone_caps_d3d11(r, iface);
	if (r->zone_caps_malformed) {
		r->result_code = CLI_SELFTEST_BAD_ZONE_CAPS;
	}
#else
	snprintf(r->zone_probe_note, sizeof(r->zone_probe_note), "not probed: zone-caps probe is Windows-only (OK)");
#endif
}

void
cli_query_teardown(struct cli_query_handles *h)
{
	xrt_space_overseer_destroy(&h->xso);
	xrt_system_devices_destroy(&h->xsysd);
	xrt_system_destroy(&h->xsys);
	xrt_instance_destroy(&h->xi);
}

void
cli_query_run(struct cli_query_result *r)
{
	struct cli_query_handles h;
	cli_query_fill(r, &h, NULL);
	cli_query_teardown(&h);
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

/*!
 * Decode the advisory switch-granularity value (xrt_dp_switch_granularity).
 */
static const char *
zone_granularity_label(uint32_t g)
{
	switch (g) {
	case 0: return "unknown";
	case 1: return "global";
	case 2: return "column-band";
	case 3: return "row-band";
	case 4: return "cell-grid";
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
	// Baseline hint only — the authoritative scale is per rendering mode (below).
	PT("view scale:   (%.3f, %.3f) (baseline hint; see per-mode scale)\n", (double)i->recommended_view_scale_x,
	   (double)i->recommended_view_scale_y);
	PT("screen pos:   (%d, %d)\n", i->display_screen_left, i->display_screen_top);
	char et_buf[64];
	PT("eye-tracking: supported=%s (0x%x) default=%s\n",
	   eye_modes_label(i->supported_eye_tracking_modes, et_buf, sizeof(et_buf)),
	   i->supported_eye_tracking_modes, eye_default_label(i->default_eye_tracking_mode));
	PT("modes:        %u\n", r->rendering_mode_count);
	for (uint32_t m = 0; m < r->rendering_mode_count; m++) {
		const struct xrt_rendering_mode *rm = &r->rendering_modes[m];
		PT("  [%u] %-14s views=%u 3d=%c tracked=%c rot=%c scale=%.3fx%.3f\n", rm->mode_index, rm->mode_name,
		   rm->view_count, rm->hardware_display_3d ? 'y' : 'n',
		   (rm->mode_flags & XRT_RENDERING_MODE_FLAG_HAS_TRACKING) ? 'y' : 'n',
		   (rm->mode_flags & XRT_RENDERING_MODE_FLAG_CAN_ROTATE) ? 'y' : 'n', (double)rm->view_scale_x,
		   (double)rm->view_scale_y);
	}

	P(" :: DP selection (which plug-in each render path picks)\n");
	if (!r->dp_sel_probed) {
		PT("not evaluated\n");
	} else {
		PT("in-process (handle/texture apps): '%s'\n", or_q(r->dp_sel_inproc_id));
		PT("service / shell:                  '%s' (%s)\n", or_q(r->dp_sel_service_id),
		   r->dp_sel_service_conf);
		PT("monitors=%u  claimed=%u\n", r->dp_sel_monitor_count, r->dp_sel_claim_count);
		if (r->dp_sel_mismatch) {
			PT("** MISMATCH: the shell will weave with '%s' while standalone apps use '%s'.\n",
			   r->dp_sel_service_id, r->dp_sel_inproc_id);
			PT("   If '%s' is a non-tracking DP (e.g. sim_display), shell head-tracking is broken\n",
			   r->dp_sel_service_id);
			PT("   even though standalone apps track fine — the registry lost the display to a\n");
			PT("   fallback claim (EDID table stale vs the vendor runtime).\n");
		} else {
			PT("paths agree.\n");
		}
	}

	P(" :: Local zone caps (#224/ADR-027, headless D3D11 WARP probe)\n");
	if (r->zone_caps_probed) {
		const struct xrt_dp_local_zone_caps *z = &r->zone_caps;
		PT("supported:    %u%s\n", z->supported, r->zone_caps_malformed ? "  (MALFORMED — see below)" : "");
		PT("zone grid:    %ux%u\n", z->zone_grid_width, z->zone_grid_height);
		PT("max mask:     %ux%u\n", z->max_mask_width, z->max_mask_height);
		PT("max hz:       %u%s\n", z->max_update_hz, z->max_update_hz == 0 ? " (unlimited)" : "");
		PT("wish:         fractional=%u granularity=%s (%u)\n", z->wish_fractional,
		   zone_granularity_label(z->switch_granularity), z->switch_granularity);
		if (r->zone_caps_malformed) {
			PT("%s\n", r->zone_probe_note);
		}
	} else {
		PT("%s\n", r->zone_probe_note[0] != '\0' ? r->zone_probe_note : "not evaluated");
	}
}

cJSON *
cli_query_info_to_cjson(const struct cli_query_result *r)
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
		cJSON *rms = cJSON_AddArrayToObject(root, "rendering_modes");
		for (uint32_t m = 0; m < r->rendering_mode_count; m++) {
			const struct xrt_rendering_mode *rm = &r->rendering_modes[m];
			cJSON *o = cJSON_CreateObject();
			cJSON_AddNumberToObject(o, "mode_index", (double)rm->mode_index);
			cJSON_AddStringToObject(o, "name", rm->mode_name);
			cJSON_AddNumberToObject(o, "view_count", (double)rm->view_count);
			cJSON_AddBoolToObject(o, "hardware_display_3d", rm->hardware_display_3d);
			cJSON_AddBoolToObject(o, "has_tracking",
			                      (rm->mode_flags & XRT_RENDERING_MODE_FLAG_HAS_TRACKING) != 0);
			cJSON_AddBoolToObject(o, "can_rotate", (rm->mode_flags & XRT_RENDERING_MODE_FLAG_CAN_ROTATE) != 0);
			cJSON *vs = cJSON_AddObjectToObject(o, "view_scale");
			cJSON_AddNumberToObject(vs, "x", (double)rm->view_scale_x);
			cJSON_AddNumberToObject(vs, "y", (double)rm->view_scale_y);
			cJSON_AddItemToArray(rms, o);
		}
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

	// DP-selection divergence probe (in-process vs service/shell).
	{
		cJSON *ds = cJSON_AddObjectToObject(root, "dp_selection");
		cJSON_AddBoolToObject(ds, "probed", r->dp_sel_probed);
		cJSON_AddBoolToObject(ds, "mismatch", r->dp_sel_mismatch);
		cJSON_AddStringToObject(ds, "in_process_plugin_id", r->dp_sel_inproc_id);
		cJSON_AddStringToObject(ds, "service_plugin_id", r->dp_sel_service_id);
		cJSON_AddStringToObject(ds, "service_confidence", r->dp_sel_service_conf);
		cJSON_AddNumberToObject(ds, "monitor_count", (double)r->dp_sel_monitor_count);
		cJSON_AddNumberToObject(ds, "claim_count", (double)r->dp_sel_claim_count);
	}

	// #224 / ADR-027 P4 zone-caps probe.
	{
		cJSON *zc = cJSON_AddObjectToObject(root, "zone_caps");
		cJSON_AddBoolToObject(zc, "probed", r->zone_caps_probed);
		cJSON_AddStringToObject(zc, "note", r->zone_probe_note[0] != '\0' ? r->zone_probe_note : "not evaluated");
		if (r->zone_caps_probed) {
			const struct xrt_dp_local_zone_caps *z = &r->zone_caps;
			cJSON_AddBoolToObject(zc, "malformed", r->zone_caps_malformed);
			cJSON_AddNumberToObject(zc, "supported", (double)z->supported);
			cJSON *g = cJSON_AddObjectToObject(zc, "zone_grid");
			cJSON_AddNumberToObject(g, "width", (double)z->zone_grid_width);
			cJSON_AddNumberToObject(g, "height", (double)z->zone_grid_height);
			cJSON *mm = cJSON_AddObjectToObject(zc, "max_mask");
			cJSON_AddNumberToObject(mm, "width", (double)z->max_mask_width);
			cJSON_AddNumberToObject(mm, "height", (double)z->max_mask_height);
			cJSON_AddNumberToObject(zc, "max_update_hz", (double)z->max_update_hz);
			cJSON_AddNumberToObject(zc, "wish_fractional", (double)z->wish_fractional);
			cJSON_AddNumberToObject(zc, "switch_granularity", (double)z->switch_granularity);
			cJSON_AddStringToObject(zc, "switch_granularity_label",
			                        zone_granularity_label(z->switch_granularity));
		}
	}

	return root;
}

void
cli_query_print_info_json(const struct cli_query_result *r)
{
	cJSON *root = cli_query_info_to_cjson(r);
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

	// #224 / ADR-027 P4 — zone-caps probe. ABSENCE NEVER FAILS: ok stays
	// true for legacy plug-ins / no factory / non-Windows; only a
	// present-but-malformed caps struct fails (BAD_ZONE_CAPS).
	c = &out[n++];
	c->name = "zone_caps";
	c->ok = !r->zone_caps_malformed;
	snprintf(c->detail, sizeof(c->detail), "%s",
	         r->zone_probe_note[0] != '\0' ? r->zone_probe_note : "not evaluated");

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

cJSON *
cli_query_selftest_to_cjson(const struct cli_query_result *r)
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

	return root;
}

void
cli_query_print_selftest_json(const struct cli_query_result *r)
{
	cJSON *root = cli_query_selftest_to_cjson(r);
	char *out = cJSON_Print(root);
	if (out != NULL) {
		printf("%s\n", out);
		cJSON_free(out);
	}
	cJSON_Delete(root);
}
