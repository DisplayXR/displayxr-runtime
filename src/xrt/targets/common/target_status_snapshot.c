// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Headless builder of the display status snapshot (ADR-051).
 * @ingroup targets_common
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#endif

#include "target_status_snapshot.h"
#include "target_plugin_loader.h"
#include "target_screens.h"

#include "xrt/xrt_compositor.h" // xrt_dp_factory_registry
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_system.h"

#include "os/os_display_desktop.h"
#include "os/os_display_edid.h"
#include "util/u_git_tag.h"
#include "util/u_screen_info.h"
#include "util/u_status_snapshot.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#endif


/*
 *
 * Runtime + plug-ins.
 *
 */

static void
fill_runtime(struct xrt_status_runtime *rt)
{
	(void)snprintf(rt->version, sizeof(rt->version), "%u.%u.%u", (unsigned)u_version_major,
	               (unsigned)u_version_minor, (unsigned)u_version_patch);
	(void)snprintf(rt->git_tag, sizeof(rt->git_tag), "%s", u_git_tag);
	rt->plugin_abi = (uint32_t)XRT_PLUGIN_API_VERSION_CURRENT;
#ifdef XRT_OS_WINDOWS
	// Read only: the OpenXR loader's ActiveRuntime (HKLM). Unset leaves "".
	wchar_t wbuf[1024];
	DWORD bytes = sizeof(wbuf);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, NULL,
	                 wbuf, &bytes) == ERROR_SUCCESS) {
		WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, rt->active_openxr_runtime,
		                    (int)sizeof(rt->active_openxr_runtime), NULL, NULL);
		rt->active_openxr_runtime[sizeof(rt->active_openxr_runtime) - 1] = '\0';
	}
#endif
}

/*!
 * Every registered plug-in, joined with the loader's ADR-045 record, sorted by
 * ProbeOrder — the same merge `displayxr-cli info` reports.
 */
static void
fill_plugins(struct xrt_status_snapshot *out)
{
	struct target_plugin_desc descs[16];
	struct target_plugin_status st[16];
	const int nd = target_plugin_enumerate(descs, 16);
	const int ns = target_plugin_get_status(st, 16);
	const struct xrt_plugin_iface *active = target_plugin_get_active();
	const char *active_id = (active != NULL && active->id != NULL) ? active->id : "";

	struct xrt_status_plugin tmp[32];
	int n = 0;
	for (int i = 0; i < nd && n < 32; i++) {
		struct xrt_status_plugin *p = &tmp[n++];
		memset(p, 0, sizeof(*p));
		(void)snprintf(p->id, sizeof(p->id), "%s", descs[i].id);
		(void)snprintf(p->name, sizeof(p->name), "%s", descs[i].display_name);
		(void)snprintf(p->vendor, sizeof(p->vendor), "%s", descs[i].vendor);
		(void)snprintf(p->version, sizeof(p->version), "%s", descs[i].version);
		p->probe_order = descs[i].probe_order;
		(void)snprintf(p->load, sizeof(p->load), "%s",
		               target_plugin_load_result_str(TARGET_PLUGIN_RESULT_NOT_ATTEMPTED));
		p->fallback = strcmp(descs[i].id, "sim-display") == 0;
		for (int j = 0; j < ns; j++) {
			if (strcmp(st[j].id, descs[i].id) != 0) {
				continue;
			}
			(void)snprintf(p->load, sizeof(p->load), "%s", target_plugin_load_result_str(st[j].result));
			p->platform_state = st[j].platform_state;
			(void)snprintf(p->hint, sizeof(p->hint), "%s", st[j].hint);
			p->fallback = st[j].fallback;
			break;
		}
		p->active = strcmp(p->id, active_id) == 0;
	}
	// Loader records with no registration (e.g. a dev search path).
	for (int j = 0; j < ns && n < 32; j++) {
		bool listed = false;
		for (int i = 0; i < nd; i++) {
			listed |= strcmp(st[j].id, descs[i].id) == 0;
		}
		if (listed) {
			continue;
		}
		struct xrt_status_plugin *p = &tmp[n++];
		memset(p, 0, sizeof(*p));
		(void)snprintf(p->id, sizeof(p->id), "%s", st[j].id);
		(void)snprintf(p->name, sizeof(p->name), "%s", st[j].display_name);
		(void)snprintf(p->version, sizeof(p->version), "%s", st[j].version);
		p->probe_order = st[j].probe_order;
		(void)snprintf(p->load, sizeof(p->load), "%s", target_plugin_load_result_str(st[j].result));
		p->platform_state = st[j].platform_state;
		(void)snprintf(p->hint, sizeof(p->hint), "%s", st[j].hint);
		p->fallback = st[j].fallback;
		p->active = strcmp(p->id, active_id) == 0;
	}
	// Insertion sort by ProbeOrder (n <= 32).
	for (int i = 1; i < n; i++) {
		struct xrt_status_plugin k = tmp[i];
		int j = i - 1;
		while (j >= 0 && tmp[j].probe_order > k.probe_order) {
			tmp[j + 1] = tmp[j];
			j--;
		}
		tmp[j + 1] = k;
	}
	out->plugin_count = (uint32_t)(n < XRT_STATUS_MAX_PLUGINS ? n : XRT_STATUS_MAX_PLUGINS);
	memcpy(out->plugins, tmp, sizeof(out->plugins[0]) * out->plugin_count);
}


/*
 *
 * Screens.
 *
 */

//! EDID manufacturer id (stored little-endian) -> 3-letter PNP code; "" if not A-Z.
static void
pnp_code(uint16_t raw, char out[4])
{
	const uint16_t v = (uint16_t)((raw >> 8) | (raw << 8));
	const int c[3] = {((v >> 10) & 0x1F) + 'A' - 1, ((v >> 5) & 0x1F) + 'A' - 1, (v & 0x1F) + 'A' - 1};
	for (int i = 0; i < 3; i++) {
		if (c[i] < 'A' || c[i] > 'Z') {
			out[0] = '\0';
			return;
		}
		out[i] = (char)c[i];
	}
	out[3] = '\0';
}

/*!
 * What the system says about its default screen — the active plug-in's panel
 * info and the desktop rect the runtime resolves for it (#1301). Mirrors
 * `cli_query_fill` and the native instance's `apply_plugin_display_info`.
 */
static void
resolve_system(struct xrt_system_devices *xsysd, struct target_screens_system *sys)
{
	memset(sys, 0, sizeof(*sys));
	const struct xrt_plugin_iface *iface = target_plugin_get_active();
	struct xrt_device *head = xsysd != NULL ? xsysd->static_roles.head : NULL;
	if (iface == NULL || iface->get_display_info == NULL || head == NULL) {
		return;
	}
	struct xrt_plugin_display_info info;
	memset(&info, 0, sizeof(info));
	info.struct_size = (uint32_t)sizeof(info);
	if (!iface->get_display_info(target_plugin_get_active_instance(), head, &info)) {
		return;
	}
	u_screen_info_from_plugin(&info, XRT_SCREEN_INFO_SOURCE_SYSTEM, &sys->info);
	if (!(sys->info.recommended_view_scale_x > 0.0f) || !(sys->info.recommended_view_scale_y > 0.0f)) {
		float min_x = 1.0f, min_y = 1.0f;
		for (uint32_t i = 0; i < head->rendering_mode_count && i < XRT_MAX_RENDERING_MODES; i++) {
			const struct xrt_rendering_mode *rm = &head->rendering_modes[i];
			if (rm->view_scale_x > 0.0f && rm->view_scale_x < min_x) {
				min_x = rm->view_scale_x;
			}
			if (rm->view_scale_y > 0.0f && rm->view_scale_y < min_y) {
				min_y = rm->view_scale_y;
			}
		}
		sys->info.recommended_view_scale_x = min_x;
		sys->info.recommended_view_scale_y = min_y;
	}
	sys->info_valid = info.display_width_m > 0.0f && info.display_height_m > 0.0f;

	const struct os_display_panel_hint hint = {
	    .screen_left = info.display_screen_left,
	    .screen_top = info.display_screen_top,
	    .pixel_width = info.display_pixel_width,
	    .pixel_height = info.display_pixel_height,
	    .width_m = info.display_width_m,
	    .height_m = info.display_height_m,
	};
	struct os_display_desktop_info desk;
	struct os_display_panel_match match;
	memset(&desk, 0, sizeof(desk));
	memset(&match, 0, sizeof(match));
	if (os_display_desktop_info_for_panel(&hint, &desk, &match)) {
		sys->desktop_left = desk.left;
		sys->desktop_top = desk.top;
		sys->desktop_width = desk.width;
		sys->desktop_height = desk.height;
		sys->native_width = desk.native_width;
		sys->native_height = desk.native_height;
		sys->desktop_scale = (float)desk.scale;
		sys->is_primary = desk.is_primary;
		(void)snprintf(sys->device_name, sizeof(sys->device_name), "%.*s", (int)(sizeof(sys->device_name) - 1),
		               desk.device_name);
	}
}

static uint32_t
apis_of(const struct xrt_dp_registry_entry *e)
{
	uint32_t bits = 0;
	bits |= e->dp_factory_d3d11 != NULL ? XRT_STATUS_API_BIT_D3D11 : 0u;
	bits |= e->dp_factory_d3d12 != NULL ? XRT_STATUS_API_BIT_D3D12 : 0u;
	bits |= e->dp_factory_vk != NULL ? XRT_STATUS_API_BIT_VK : 0u;
	bits |= e->dp_factory_gl != NULL ? XRT_STATUS_API_BIT_GL : 0u;
	bits |= e->dp_factory_metal != NULL ? XRT_STATUS_API_BIT_METAL : 0u;
	return bits;
}

static const struct xrt_dp_registry_entry *
registry_find(const struct xrt_dp_factory_registry *reg, uint64_t id)
{
	for (uint32_t i = 0; i < reg->entry_count && i < XRT_DP_REGISTRY_MAX_ENTRIES; i++) {
		if (reg->entries[i].monitor_id == id) {
			return &reg->entries[i];
		}
	}
	return NULL;
}

#ifdef XRT_OS_WINDOWS
/*!
 * The desktop compositor scale of the monitor at a point (effective DPI / 96),
 * 0 when unknown. Windows only: `os_display_desktop_enumerate` deliberately
 * reports nothing there, so the registry carries no scale. Loaded at run time
 * so no new import is added to the runtime's link line.
 */
static float
win_monitor_scale(int32_t x, int32_t y)
{
	typedef HRESULT(WINAPI * pfn_get_dpi_for_monitor)(HMONITOR, int, UINT *, UINT *);
	static pfn_get_dpi_for_monitor fn = NULL;
	static bool tried = false;
	if (!tried) {
		tried = true;
		HMODULE shcore = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (shcore != NULL) {
			fn = (pfn_get_dpi_for_monitor)(void *)GetProcAddress(shcore, "GetDpiForMonitor");
		}
	}
	if (fn == NULL) {
		return 0.0f;
	}
	POINT pt = {(LONG)x, (LONG)y};
	HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
	UINT dx = 0, dy = 0;
	if (mon == NULL || FAILED(fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)) || dx == 0) {
		return 0.0f;
	}
	return (float)dx / 96.0f;
}

/*!
 * The native (preferred) mode of the monitor whose GDI source is
 * @p gdi_device_name (`\\.\DISPLAY1`): DisplayConfig's target preferred
 * mode, matched to the monitor through its path's source GDI name. Windows
 * only — the EDID reader fills no native mode there, so without this
 * `NOT_NATIVE` could never fire. Plain user32 exports (already on the link
 * line); read-only, no mode is changed.
 *
 * @return true with @p out_w / @p out_h (and @p out_refresh_mhz, the
 *         preferred timing's vsync, 0 when unknown) filled.
 */
static bool
win_preferred_mode(const char *gdi_device_name, uint32_t *out_w, uint32_t *out_h, uint32_t *out_refresh_mhz)
{
	if (gdi_device_name == NULL || gdi_device_name[0] == '\0') {
		return false;
	}
	wchar_t wname[CCHDEVICENAME];
	if (MultiByteToWideChar(CP_UTF8, 0, gdi_device_name, -1, wname, CCHDEVICENAME) == 0) {
		return false;
	}
	UINT32 np = 0, nm = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0) {
		return false;
	}
	DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof(*paths));
	DISPLAYCONFIG_MODE_INFO *modes = calloc(nm > 0 ? nm : 1, sizeof(*modes));
	bool found = false;
	if (paths != NULL && modes != NULL &&
	    QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
		for (UINT32 i = 0; i < np && !found; i++) {
			DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
			memset(&src, 0, sizeof(src));
			src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			src.header.size = sizeof(src);
			src.header.adapterId = paths[i].sourceInfo.adapterId;
			src.header.id = paths[i].sourceInfo.id;
			if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS ||
			    wcscmp(src.viewGdiDeviceName, wname) != 0) {
				continue;
			}
			DISPLAYCONFIG_TARGET_PREFERRED_MODE pref;
			memset(&pref, 0, sizeof(pref));
			pref.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
			pref.header.size = sizeof(pref);
			pref.header.adapterId = paths[i].targetInfo.adapterId;
			pref.header.id = paths[i].targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&pref.header) != ERROR_SUCCESS || pref.width == 0 ||
			    pref.height == 0) {
				continue;
			}
			*out_w = pref.width;
			*out_h = pref.height;
			const DISPLAYCONFIG_RATIONAL *r = &pref.targetMode.targetVideoSignalInfo.vSyncFreq;
			*out_refresh_mhz =
			    r->Denominator != 0 ? (uint32_t)((uint64_t)r->Numerator * 1000u / r->Denominator) : 0u;
			found = true;
		}
	}
	free(paths);
	free(modes);
	return found;
}
#endif

/*!
 * The native mode of a screen row: the registry's device mode where the
 * platform reader fills one (Linux DRM, macOS), DisplayConfig's preferred mode
 * on Windows. Shared by the headless and the service builder.
 */
static void
fill_native(const struct xrt_screen *in, struct xrt_status_screen *s)
{
	s->native.width = in->native_width;
	s->native.height = in->native_height;
#ifdef XRT_OS_WINDOWS
	uint32_t w = 0, h = 0, mhz = 0;
	if (win_preferred_mode(s->device_name, &w, &h, &mhz)) {
		s->native.width = w;
		s->native.height = h;
		if (s->native.refresh_mhz == 0) {
			s->native.refresh_mhz = mhz;
		}
	}
#endif
	s->native.is_native = s->native.width > 0 && s->native.height > 0 && s->native.width == s->desktop.width &&
	                      s->native.height == s->desktop.height;
}

static void
fill_screen(const struct xrt_screen *in,
            uint32_t index,
            const struct xrt_dp_factory_registry *reg,
            struct xrt_status_screen *s)
{
	memset(s, 0, sizeof(*s));
	s->id = in->id;
	s->index = index;
	(void)snprintf(s->device_name, sizeof(s->device_name), "%s", in->device_name);

	s->desktop.left = in->desktop_left;
	s->desktop.top = in->desktop_top;
	s->desktop.width = in->desktop_width;
	s->desktop.height = in->desktop_height;
	s->desktop.scale = in->desktop_scale;

	// Windows has no output name in the EDID record and no scale in the
	// registry: take the GDI name from the monitor at the screen's origin, and
	// the scale from its effective DPI.
	if (s->device_name[0] == '\0') {
		struct os_display_desktop_info dm;
		if (os_display_desktop_info_at(in->desktop_left, in->desktop_top, &dm) && dm.left == in->desktop_left &&
		    dm.top == in->desktop_top) {
			(void)snprintf(s->device_name, sizeof(s->device_name), "%.*s",
			               (int)(sizeof(s->device_name) - 1), dm.device_name);
		}
	}
#ifdef XRT_OS_WINDOWS
	if (!(s->desktop.scale > 0.0f)) {
		s->desktop.scale = win_monitor_scale(in->desktop_left, in->desktop_top);
	}
#endif

	struct xrt_display_descriptor desc;
	struct os_display_edid_monitor mon;
	memset(&desc, 0, sizeof(desc));
	memset(&mon, 0, sizeof(mon));
	const bool have_mon = target_plugin_get_monitor_record(in->id, &desc, &mon);
	if (have_mon) {
		pnp_code(mon.manufacturer_id, s->edid.manufacturer);
		(void)snprintf(s->edid.product, sizeof(s->edid.product), "%04X", (unsigned)mon.product_id);
		s->edid.serial = mon.serial_number;
		s->native.refresh_mhz = mon.refresh_hz * 1000u;
	}
	if (have_mon && mon.display_name[0] != '\0') {
		(void)snprintf(s->friendly_name, sizeof(s->friendly_name), "%s", mon.display_name);
	} else if (have_mon) {
		(void)snprintf(s->friendly_name, sizeof(s->friendly_name), "%s %s",
		               s->edid.manufacturer[0] != '\0' ? s->edid.manufacturer : "???", s->edid.product);
	}

	fill_native(in, s);

	// Millimetres: the plug-in's metres when it described this screen, else EDID.
	const bool from_plugin = (in->info.source == XRT_SCREEN_INFO_SOURCE_SYSTEM ||
	                          in->info.source == XRT_SCREEN_INFO_SOURCE_PLUGIN_MONITOR) &&
	                         in->info.width_m > 0.0f && in->info.height_m > 0.0f;
	if (from_plugin) {
		s->physical.width_mm = (uint32_t)lroundf(in->info.width_m * 1000.0f);
		s->physical.height_mm = (uint32_t)lroundf(in->info.height_m * 1000.0f);
		s->physical.source = XRT_STATUS_MM_SOURCE_PLUGIN;
	} else if (in->physical_width_mm > 0 && in->physical_height_mm > 0) {
		s->physical.width_mm = in->physical_width_mm;
		s->physical.height_mm = in->physical_height_mm;
		s->physical.source = XRT_STATUS_MM_SOURCE_EDID;
	}

	s->roles.os_main = (in->flags & XRT_SCREEN_FLAG_PRIMARY) != 0;
	s->roles.runtime_default = (in->flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0;
	s->roles.vendor_primary = false; // phase 3 (vendor cell)

	(void)snprintf(s->claim.plugin_id, sizeof(s->claim.plugin_id), "%s", in->plugin_id);
	s->claim.confidence = in->confidence;
	const struct xrt_dp_registry_entry *e = registry_find(reg, in->id);
	if (e != NULL) {
		(void)snprintf(s->claim.serial, sizeof(s->claim.serial), "%s", e->serial);
		s->claim.apis = apis_of(e);
		// Display dashboard phase 7: the per-screen preference.
		s->claim.forced = e->forced;
		(void)snprintf(s->claim.preferred_plugin, sizeof(s->claim.preferred_plugin), "%s", e->preferred_plugin);
		s->claim.preferred_source = e->preferred_plugin[0] != '\0'
		                                ? (enum xrt_status_pref_source)e->preferred_source
		                                : XRT_STATUS_PREF_SOURCE_NONE;
	}
	(void)target_plugin_get_monitor_key(in->id, s->key, sizeof(s->key), NULL, 0);
	// The primary (system-default) screen's DP is the session's own: a change
	// applies at the next session / service start. Every other screen's DP is
	// a segment DP the service recreates on its re-probe.
	s->claim.apply = s->roles.runtime_default ? XRT_STATUS_APPLY_NEXT_SESSION : XRT_STATUS_APPLY_LIVE;

	s->layout.width_m = in->info.width_m;
	s->layout.height_m = in->info.height_m;
	s->layout.nominal_viewer_x_m = in->info.nominal_viewer_x_m;
	s->layout.nominal_viewer_y_m = in->info.nominal_viewer_y_m;
	s->layout.nominal_viewer_z_m = in->info.nominal_viewer_z_m;
	s->layout.source = in->info.source;

	s->eye_tracking.supported = in->info.supported_eye_tracking_modes;
	s->eye_tracking.default_mode = in->info.default_eye_tracking_mode;
	s->eye_tracking.state = XRT_STATUS_TRACKING_NO_DP; // headless: nothing is bound
}

#ifdef XRT_PLUGIN_IFACE_HAS_GET_SCREEN_STATUS
/*!
 * The claiming plug-in's vendor cell for one registry entry (ADR-051 D2),
 * through the passive `get_screen_status` slot. False when the plug-in has no
 * such slot or does not answer for this monitor ("no vendor status").
 */
static bool
query_vendor_status(const struct xrt_dp_registry_entry *e, struct xrt_plugin_screen_status *out)
{
	const struct xrt_plugin_iface *iface = (const struct xrt_plugin_iface *)e->owning_iface;
	if (!xrt_plugin_iface_has_get_screen_status(iface)) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	return iface->get_screen_status((struct xrt_plugin_instance *)e->owning_instance, e->monitor_id, out) ==
	       XRT_SUCCESS;
}

//! Fill a screen row's vendor cell from the slot, verbatim.
static void
fill_vendor(const struct xrt_dp_factory_registry *reg, struct xrt_status_screen *s)
{
	const struct xrt_dp_registry_entry *e = registry_find(reg, s->id);
	struct xrt_plugin_screen_status vs;
	if (e == NULL || !query_vendor_status(e, &vs)) {
		return;
	}
	struct xrt_status_vendor *v = &s->vendor;
	v->present = true;
	v->ready = vs.ready;
	v->verified = vs.verified;
	v->calibrated = vs.calibrated;
	switch (vs.tracker) {
	case XRT_PLUGIN_TRACKER_STATE_NONE: v->tracker = XRT_STATUS_VENDOR_TRACKER_NONE; break;
	case XRT_PLUGIN_TRACKER_STATE_OFF: v->tracker = XRT_STATUS_VENDOR_TRACKER_OFF; break;
	case XRT_PLUGIN_TRACKER_STATE_STARTING: v->tracker = XRT_STATUS_VENDOR_TRACKER_STARTING; break;
	case XRT_PLUGIN_TRACKER_STATE_RUNNING: v->tracker = XRT_STATUS_VENDOR_TRACKER_RUNNING; break;
	case XRT_PLUGIN_TRACKER_STATE_DOWN: v->tracker = XRT_STATUS_VENDOR_TRACKER_DOWN; break;
	case XRT_PLUGIN_TRACKER_STATE_UNSUPPORTED: v->tracker = XRT_STATUS_VENDOR_TRACKER_UNSUPPORTED; break;
	default: v->tracker = XRT_STATUS_VENDOR_TRACKER_UNKNOWN; break;
	}
	switch (vs.lens) {
	case XRT_PLUGIN_LENS_STATE_2D: v->lens = XRT_STATUS_VENDOR_LENS_2D; break;
	case XRT_PLUGIN_LENS_STATE_3D: v->lens = XRT_STATUS_VENDOR_LENS_3D; break;
	default: v->lens = XRT_STATUS_VENDOR_LENS_UNKNOWN; break;
	}
	(void)snprintf(v->model, sizeof(v->model), "%.*s", (int)sizeof(vs.model) - 1, vs.model);
	(void)snprintf(v->serial, sizeof(v->serial), "%.*s", (int)sizeof(vs.serial) - 1, vs.serial);
	(void)snprintf(v->dashboard_command, sizeof(v->dashboard_command), "%.*s",
	               (int)sizeof(vs.dashboard_command) - 1, vs.dashboard_command);
	// The worst vendor warning (first of the highest level), verbatim.
	int worst = -1;
	const uint32_t nw = vs.warning_count < XRT_PLUGIN_SCREEN_STATUS_MAX_WARNINGS
	                        ? vs.warning_count
	                        : XRT_PLUGIN_SCREEN_STATUS_MAX_WARNINGS;
	for (uint32_t k = 0; k < nw; k++) {
		if (worst < 0 || vs.warnings[k].level > vs.warnings[worst].level) {
			worst = (int)k;
		}
	}
	if (worst >= 0) {
		const struct xrt_plugin_screen_warning *w = &vs.warnings[worst];
		(void)snprintf(v->worst_warning.code, sizeof(v->worst_warning.code), "%.*s", (int)sizeof(w->code) - 1,
		               w->code);
		v->worst_warning.level = w->level >= XRT_PLUGIN_SCREEN_WARNING_LEVEL_CRITICAL
		                             ? XRT_STATUS_LEVEL_CRITICAL
		                         : w->level == XRT_PLUGIN_SCREEN_WARNING_LEVEL_WARN ? XRT_STATUS_LEVEL_WARN
		                                                                            : XRT_STATUS_LEVEL_INFO;
		(void)snprintf(v->worst_warning.text, sizeof(v->worst_warning.text), "%.*s", (int)sizeof(w->text) - 1,
		               w->text);
	}
}
#endif

//! One row per screen of @p list, in registry order (both builders).
static void
fill_screens(const struct xrt_screen_list *list,
             const struct xrt_dp_factory_registry *reg,
             struct xrt_status_snapshot *out)
{
	for (uint32_t i = 0; i < list->count && i < XRT_STATUS_MAX_SCREENS; i++) {
		fill_screen(&list->screens[i], i, reg, &out->screens[i]);
#ifdef XRT_PLUGIN_IFACE_HAS_GET_SCREEN_STATUS
		fill_vendor(reg, &out->screens[i]);
#endif
		out->screen_count++;
	}
}

void
target_status_snapshot_build_headless(struct xrt_instance *xi,
                                      struct xrt_system_devices *xsysd,
                                      struct xrt_status_snapshot *out)
{
	memset(out, 0, sizeof(*out));
	out->schema = XRT_STATUS_SCHEMA;
	out->source = XRT_STATUS_SOURCE_HEADLESS;

	fill_runtime(&out->runtime);
	fill_plugins(out);

	// The per-monitor registry, resolved exactly as the runtime resolves it
	// (EDID -> descriptors -> every plug-in's claims). It also fills the loader's
	// monitor side table the screen rows read. Claim serials and per-API
	// factories exist only here, not in xrt_screen.
	struct os_display_edid_list edid;
	memset(&edid, 0, sizeof(edid));
	os_display_edid_enumerate(&edid);
	struct xrt_display_descriptor descs[XRT_DP_REGISTRY_MAX_ENTRIES];
	const uint32_t dn = target_plugin_build_descriptors(&edid, descs, XRT_DP_REGISTRY_MAX_ENTRIES);
	struct xrt_dp_factory_registry reg;
	memset(&reg, 0, sizeof(reg));
	target_plugin_resolve_displays(descs, dn, &reg);

	// The screen list: what the instance reports when it has a system with a
	// compositor (the list apps see), else built from the registry above.
	struct xrt_screen_list list;
	memset(&list, 0, sizeof(list));
	if (xi == NULL || xrt_instance_enumerate_displays(xi, &list) != XRT_SUCCESS || list.count == 0) {
		struct target_screens_system sys;
		resolve_system(xsysd, &sys);
		const struct xrt_plugin_iface *active = target_plugin_get_active();
		target_screens_build(&reg, active != NULL ? active->id : NULL, &sys, &list);
	}

	fill_screens(&list, &reg, out);

	u_status_warnings_derive(out);
}


/*
 *
 * Service builder (phase 2).
 *
 */

//! Index of the screen row with id @p id, -1 when none.
static int
screen_index_of(const struct xrt_status_snapshot *out, uint64_t id)
{
	for (uint32_t i = 0; i < out->screen_count; i++) {
		if (out->screens[i].id == id) {
			return (int)i;
		}
	}
	return -1;
}

//! The runtime-default screen (the one the service's own DP weaves), 0 when none.
static uint64_t
default_screen_id(const struct xrt_status_snapshot *out)
{
	for (uint32_t i = 0; i < out->screen_count; i++) {
		if (out->screens[i].roles.runtime_default) {
			return out->screens[i].id;
		}
	}
	return out->screen_count > 0 ? out->screens[0].id : 0;
}

/*!
 * Bind the live DPs to their screens: each screen's `dps[]`, its live
 * tracking state (TRACKING if any bound DP tracks, NOT_TRACKING with the
 * shortest not-tracking time if every answering DP does not, UNKNOWN if none
 * answered, NO_DP if none is bound) and the head's mode on every screen a DP
 * weaves.
 */
static void
merge_live_dps(const struct xrt_status_live *live, struct xrt_status_snapshot *out)
{
	const uint64_t def = default_screen_id(out);
	bool answered[XRT_STATUS_MAX_SCREENS] = {false};
	bool tracking[XRT_STATUS_MAX_SCREENS] = {false};
	uint32_t min_nt_ms[XRT_STATUS_MAX_SCREENS];
	for (uint32_t i = 0; i < XRT_STATUS_MAX_SCREENS; i++) {
		min_nt_ms[i] = UINT32_MAX;
	}

	for (uint32_t d = 0; d < live->dp_count && d < XRT_STATUS_MAX_LIVE_DPS; d++) {
		const struct xrt_status_live_dp *ld = &live->dps[d];
		const int si = screen_index_of(out, ld->screen_id != 0 ? ld->screen_id : def);
		if (si < 0) {
			continue; // a screen the registry no longer lists
		}
		struct xrt_status_screen *s = &out->screens[si];
		bool dup = false;
		for (uint32_t k = 0; k < s->dp_count; k++) {
			dup |= s->dps[k].client_id == ld->dp.client_id && s->dps[k].kind == ld->dp.kind;
		}
		if (!dup && s->dp_count < XRT_STATUS_MAX_SCREEN_DPS) {
			s->dps[s->dp_count++] = ld->dp;
		}
		if (ld->answered) {
			answered[si] = true;
			if (ld->is_tracking) {
				tracking[si] = true;
			} else if (ld->not_tracking_ms < min_nt_ms[si]) {
				min_nt_ms[si] = ld->not_tracking_ms;
			}
		}
	}

	for (uint32_t i = 0; i < out->screen_count; i++) {
		struct xrt_status_screen *s = &out->screens[i];
		if (s->dp_count == 0) {
			s->eye_tracking.state = XRT_STATUS_TRACKING_NO_DP;
			continue;
		}
		if (tracking[i]) {
			s->eye_tracking.state = XRT_STATUS_TRACKING_TRACKING;
		} else if (answered[i]) {
			s->eye_tracking.state = XRT_STATUS_TRACKING_NOT_TRACKING;
			s->eye_tracking.not_tracking_ms = min_nt_ms[i] != UINT32_MAX ? min_nt_ms[i] : 0u;
		} else {
			s->eye_tracking.state = XRT_STATUS_TRACKING_UNKNOWN;
		}
		if (live->mode.valid) {
			s->mode = live->mode;
		}
	}
}

void
target_status_snapshot_build_service(struct xrt_instance *xi,
                                     const struct xrt_dp_factory_registry *reg,
                                     const struct xrt_status_live *live,
                                     struct xrt_status_snapshot *out)
{
	memset(out, 0, sizeof(*out));
	out->schema = XRT_STATUS_SCHEMA;
	out->source = XRT_STATUS_SOURCE_SERVICE;
	if (live != NULL) {
		out->generation = live->generation;
	}

	fill_runtime(&out->runtime);
	fill_plugins(out); // target_plugin_get_status merges the active plug-in's LIVE platform state

	// The screen list the service's apps see (its instance's registry, read
	// under the instance's own lock), joined with the service's DP registry
	// for claim serials + per-API factories. Nothing is re-probed here: a
	// status read never round-trips the plug-ins' probe_displays (D5).
	struct xrt_screen_list *list = calloc(1, sizeof(*list));
	if (list != NULL && xi != NULL && xrt_instance_enumerate_displays(xi, list) == XRT_SUCCESS) {
		struct xrt_dp_factory_registry empty;
		memset(&empty, 0, sizeof(empty));
		fill_screens(list, reg != NULL ? reg : &empty, out);
	}
	free(list);

	if (live != NULL) {
		const uint32_t nc =
		    live->client_count < XRT_STATUS_MAX_CLIENTS ? live->client_count : XRT_STATUS_MAX_CLIENTS;
		const uint64_t def = default_screen_id(out);
		for (uint32_t i = 0; i < nc; i++) {
			out->clients[i] = live->clients[i];
			// A presenting client whose window no segment DP holds is woven by
			// the panel DP on the runtime-default screen.
			if (out->clients[i].owner_screen == 0 &&
			    out->clients[i].presenter != XRT_STATUS_PRESENTER_NONE) {
				out->clients[i].owner_screen = def;
			}
		}
		out->client_count = nc;
		merge_live_dps(live, out);
		out->workspace = live->workspace;
	}

	u_status_warnings_derive(out);
}

uint64_t
target_status_snapshot_change_key(const struct xrt_dp_factory_registry *reg)
{
	// FNV-1a over what the "topology" counter covers beyond the screen list:
	// each plug-in's load result + live platform state + hint, and (with the
	// ADR-051 D2 slot) every claiming plug-in's per-screen change counter.
	uint64_t h = 1469598103934665603ull;
#define MIX(ptr, len)                                                                                                  \
	do {                                                                                                           \
		const unsigned char *b_ = (const unsigned char *)(ptr);                                                \
		for (size_t k_ = 0; k_ < (size_t)(len); k_++) {                                                        \
			h = (h ^ b_[k_]) * 1099511628211ull;                                                           \
		}                                                                                                      \
	} while (0)

	struct target_plugin_status st[16];
	const int ns = target_plugin_get_status(st, 16);
	for (int i = 0; i < ns; i++) {
		MIX(st[i].id, strlen(st[i].id));
		MIX(&st[i].result, sizeof(st[i].result));
		MIX(&st[i].platform_state, sizeof(st[i].platform_state));
		MIX(st[i].hint, strlen(st[i].hint));
	}
#ifdef XRT_PLUGIN_IFACE_HAS_GET_SCREEN_STATUS
	for (uint32_t i = 0; reg != NULL && i < reg->entry_count && i < XRT_DP_REGISTRY_MAX_ENTRIES; i++) {
		struct xrt_plugin_screen_status vs;
		if (query_vendor_status(&reg->entries[i], &vs)) {
			MIX(&reg->entries[i].monitor_id, sizeof(reg->entries[i].monitor_id));
			MIX(&vs.change_counter, sizeof(vs.change_counter));
		}
	}
#else
	(void)reg;
#endif
#undef MIX
	return h;
}
