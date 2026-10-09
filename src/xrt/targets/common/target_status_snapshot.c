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
#endif

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

	s->native.width = in->native_width;
	s->native.height = in->native_height;
	s->native.is_native = in->native_width > 0 && in->native_height > 0 && in->native_width == in->desktop_width &&
	                      in->native_height == in->desktop_height;

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
	}

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

	for (uint32_t i = 0; i < list.count && i < XRT_STATUS_MAX_SCREENS; i++) {
		fill_screen(&list.screens[i], i, &reg, &out->screens[i]);
		out->screen_count++;
	}

	u_status_warnings_derive(out);
}
