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
#include "util/u_setting.h" // #1252 settings chain (env > per-user > machine)
#ifdef XRT_OS_WINDOWS
#include "util/u_windows.h" // #1201 DPI awareness reporting
#endif

#include "target_plugin_loader.h"
#include "target_input_plugin_loader.h"
#include "target_builder_input_provider.h"
#include "target_input_arbiter.h"

#include <cjson/cJSON.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h> // WIN32_LEAN_AND_MEAN + COBJMACROS are set at the top of the file
#include <d3d11.h>   // WARP device for the headless zone-caps probe (#224 / ADR-027 P4)
#include <dxgi.h>    // adapter enumeration for the #918 GPU-topology probe
#include <stdlib.h>  // getenv (DXR_WEAVE_ON_SCANOUT)
#include "xrt/xrt_display_processor_d3d11.h"
#include "d3d/d3d_scanout_helpers.h" // panel rect -> scanout adapter LUID (#918)
#include "d3d/d3d_render_adapter.h"  // ADR-037 §2 resolver — the service's ingest adapter (#1153)
#endif


/*
 *
 * Query.
 *
 */

//! Placeholder for an empty/absent string, shared by the query core (whose
//! notes quote device names) and the serializers below.
static const char *
or_q(const char *s)
{
	return (s != NULL && s[0] != '\0') ? s : "?";
}

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
	HRESULT hr =
	    D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, &fl, &context);
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

//! Pack a Windows LUID the way Vulkan reports deviceLUID (raw bytes, LowPart
//! first) — the same packing @ref d3d_scanout_adapter_luid returns, so the two
//! can be compared directly.
static uint64_t
pack_luid(LUID luid)
{
	uint64_t packed = 0;
	memcpy(&packed, &luid, sizeof(packed));
	return packed;
}

//! Defined with the serializers below; the probe composes a LUID-bearing line too.
static const char *
luid_label(uint64_t packed, char *buf, size_t cap);

/*!
 * #918 Phase 2b PR 6 — the SERVICE output-device split, stated from the two
 * inputs this process can legitimately see.
 *
 * `displayxr-cli` is HEADLESS: no compositor, no service, no IPC connection. It
 * cannot report what the running service actually decided, and must not invent
 * it. But the service's Stage A takes exactly two inputs — the topology (render
 * adapter != scanout adapter, resolved above) and `DXR_WEAVE_ON_SCANOUT` — so
 * "would it engage, given this environment" is answerable and is the question a
 * bug report needs answered. The wording keeps the distinction visible.
 *
 * #918 Phase 3 inverted the env's role: it is a KILL SWITCH now, not an opt-in,
 * so on an ordinary box with no env set the answer is "would engage", and the
 * interesting bug report is the one where it says KILLED.
 *
 * The ingress policy (`DXR_SPLIT_INGRESS`) rides along because it is the other
 * env var that changes what the split costs, and reading it from a bug report is
 * how a stray `staged` gets spotted.
 */
static void
describe_service_split(struct cli_query_result *r)
{
	char ing_buf[64];
	const char *ing = u_setting_get_raw("DXR_SPLIT_INGRESS", ing_buf, sizeof(ing_buf), NULL);
	const bool staged = ing != NULL && strcmp(ing, "staged") == 0;
	snprintf(r->gpu_split_ingress, sizeof(r->gpu_split_ingress), "%s%s", staged ? "staged" : "adaptive",
	         (ing != NULL && ing[0] != '\0') ? "" : " (default)");

	/*
	 * #918 Phase 3 — the split is the DEFAULT (ADR-037 §1), so the question this
	 * answers inverted: not "did someone turn it on" but "did someone turn it
	 * off". The kill switch is a false spelling of DXR_WEAVE_ON_SCANOUT; every
	 * other value, including unset, leaves the split allowed.
	 *
	 * Mirrors comp_split_gate_parse_requested's leading-character test rather
	 * than calling it, for the same reason this whole file re-derives the
	 * service's Stage A: displayxr-cli is headless and links no compositor.
	 */
	const char *w = r->gpu_weave_env_set ? r->gpu_weave_env : NULL;
	const bool killed = w != NULL && (w[0] == '0' || w[0] == 'f' || w[0] == 'F' || w[0] == 'n' || w[0] == 'N' ||
	                                  strcmp(w, "off") == 0 || strcmp(w, "OFF") == 0);
	if (killed) {
		snprintf(r->gpu_service_split, sizeof(r->gpu_service_split),
		         "service split: KILLED (DXR_WEAVE_ON_SCANOUT=%s — the default is ON since #918 Phase 3)", w);
	} else if (!r->gpu_split_applies) {
		snprintf(r->gpu_service_split, sizeof(r->gpu_service_split),
		         "service split: allowed but INERT (render and scanout are the same adapter — Stage A "
		         "no-ops, reason=same_adapter)");
	} else {
		snprintf(r->gpu_service_split, sizeof(r->gpu_service_split),
		         "service split: WOULD ENGAGE (default on, render != scanout); ingress %s",
		         r->gpu_split_ingress);
	}
}

/*!
 * ADR-037 §7 / #1153 — which adapter the service will create its INGEST device
 * on, resolved with the same `d3d_render_adapter` unit the service itself calls.
 *
 * Unlike the split line above, this is not a "would" statement: the resolver is
 * deterministic and reads the same environment the service reads, so running it
 * here answers the question exactly. That matters most for the override arm —
 * `DXR_D3D_FORCE_GPU=scanout` deliberately puts ingest on a different adapter
 * from the clients, and being able to confirm the override took effect BEFORE
 * starting the service is what makes the arm buildable.
 *
 * @param panel_left/@param panel_top/@param panel_w/@param panel_h The panel
 *        rect, consulted only by the `scanout` keyword (zeroes are fine).
 */
static void
describe_service_ingest(
    struct cli_query_result *r, int32_t panel_left, int32_t panel_top, uint32_t panel_w, uint32_t panel_h)
{
	uint64_t luid = 0;
	const char *provenance = NULL;
	if (!d3d_render_adapter_luid(panel_left, panel_top, panel_w, panel_h, &luid, &provenance)) {
		snprintf(r->gpu_service_ingest, sizeof(r->gpu_service_ingest),
		         "service ingest: UNRESOLVED — the service would fall back to D3D_DRIVER_TYPE_HARDWARE");
		return;
	}

	r->gpu_ingest_resolved = true;
	r->gpu_ingest_luid = luid;
	snprintf(r->gpu_ingest_provenance, sizeof(r->gpu_ingest_provenance), "%s",
	         provenance != NULL ? provenance : "unknown");
	for (uint32_t a = 0; a < r->gpu_adapter_count; a++) {
		if (r->gpu_adapters[a].luid == luid) {
			snprintf(r->gpu_ingest_name, sizeof(r->gpu_ingest_name), "%s", r->gpu_adapters[a].name);
			break;
		}
	}

	/*
	 * The forced case is called out because it is the one configuration in
	 * which ingest may legitimately NOT be the adapter the clients are on.
	 *
	 * "Was it forced" is read off the PROVENANCE, never off getenv: the
	 * resolver ignores a value it cannot parse (and a stray trailing space is
	 * enough — observed while testing this line), and a report that says
	 * "FORCED" for an override that did not take is worse than no report.
	 */
	// #1252: "was it set" now spans the whole settings chain, not just getenv —
	// otherwise a value set from the Control Panel would read as unset here.
	// The provenance the resolver returned names the source it actually used
	// ("env-forced:" / "user-forced:" / "machine-forced:"), so match the shared
	// "-forced" suffix rather than one specific source.
	char force_buf[64];
	const char *force = u_setting_get_raw("DXR_D3D_FORCE_GPU", force_buf, sizeof(force_buf), NULL);
	const bool env_set = force != NULL && force[0] != '\0';
	const bool honoured = strstr(r->gpu_ingest_provenance, "-forced") != NULL;
	char lb[32];
	snprintf(r->gpu_service_ingest, sizeof(r->gpu_service_ingest),
	         "service ingest: '%s' LUID=%s (%s) — the adapter clients must share (ADR-037 §7)%s%s%s",
	         r->gpu_ingest_name[0] != '\0' ? r->gpu_ingest_name : "<not enumerated>",
	         luid_label(luid, lb, sizeof(lb)), r->gpu_ingest_provenance, env_set ? "; DXR_D3D_FORCE_GPU=" : "",
	         env_set ? force : "",
	         !env_set ? "" : (honoured ? " HONOURED" : " set but NOT honoured — see the resolver's WARN"));
}

/*!
 * #1201 — read the AUTHORITATIVE current display mode for the panel the
 * plug-in describes, so the `display_dims` check can prove its number.
 *
 * `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)` reports the mode the adapter
 * is actually scanning out, in true pixels, *regardless of the calling
 * process's DPI awareness* — unlike the GDI monitor rects the EDID enumerator
 * reads, which a DPI-unaware process is handed pre-divided by the scale
 * factor. That independence is the whole reason it is usable as the oracle
 * here: were it DPI-sensitive too, both sides of the comparison would be wrong
 * together and the check would pass on a scaled box exactly as before.
 *
 * The monitor is picked by the plug-in's own `display_screen_left/top`, so a
 * DP that claims a secondary display is compared against THAT display; both
 * zero means "no preference" and resolves to the primary monitor, which is
 * what such a plug-in (sim_display) sizes itself from.
 *
 * Sets `native_probed` false and returns quietly when the monitor cannot be
 * resolved — absence never fails.
 */
static void
probe_native_display_mode(struct cli_query_result *r, const struct xrt_plugin_display_info *info)
{
	POINT pt = {(LONG)info->display_screen_left, (LONG)info->display_screen_top};
	HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
	if (mon == NULL) {
		return;
	}

	MONITORINFOEXW mi;
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(mon, (LPMONITORINFO)&mi)) {
		return;
	}

	DEVMODEW dm;
	memset(&dm, 0, sizeof(dm));
	dm.dmSize = sizeof(dm);
	if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
		return;
	}
	if (dm.dmPelsWidth == 0 || dm.dmPelsHeight == 0) {
		return;
	}

	r->native_probed = true;
	r->native_pixel_width = (uint32_t)dm.dmPelsWidth;
	r->native_pixel_height = (uint32_t)dm.dmPelsHeight;
	r->native_refresh_hz = (uint32_t)dm.dmDisplayFrequency;
	WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, r->native_device, (int)sizeof(r->native_device), NULL, NULL);
}

/*!
 * #918 — GPU topology. Enumerates the hardware adapters, names the one that
 * scans out the 3D panel, names the one the runtime would suggest to render
 * on, and states whether the weave-on-scanout split has anything to do on this
 * box (i.e. whether the session pays a cross-adapter present).
 *
 * The render suggestion mirrors `oxr_d3d.cpp`'s dGPU classification — the
 * hardware adapter with the MOST dedicated VRAM — deliberately NOT
 * EnumAdapterByGpuPreference, whose answer a per-app UserGpuPreferences
 * registry entry can reorder.
 *
 * Purely informational: nothing here can fail the self-test. Unresolvable is
 * reported as unresolvable, never guessed.
 */
static void
probe_gpu_topology(struct cli_query_result *r, const struct xrt_plugin_display_info *info)
{
	r->gpu_probed = true;

	// #1252: through the settings chain, not getenv alone — otherwise this
	// report would disagree with what the runtime does the moment anyone sets
	// the lever from the Control Panel instead of the environment.
	char weave_buf[64];
	enum u_setting_source weave_src = U_SETTING_SOURCE_DEFAULT;
	const char *env = u_setting_get_raw("DXR_WEAVE_ON_SCANOUT", weave_buf, sizeof(weave_buf), &weave_src);
	r->gpu_weave_env_set = env != NULL && env[0] != '\0';
	if (r->gpu_weave_env_set) {
		snprintf(r->gpu_weave_env, sizeof(r->gpu_weave_env), "%s", env);
	}
	snprintf(r->gpu_weave_source, sizeof(r->gpu_weave_source), "%s", u_setting_source_str(weave_src));
	// Answered from the env alone here, so every early return below still
	// reports it; re-stated at the end once the topology is known.
	describe_service_split(r);

	IDXGIFactory1 *factory = NULL;
	if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)) || factory == NULL) {
		snprintf(r->gpu_note, sizeof(r->gpu_note), "DXGI factory unavailable — adapters not enumerated");
		snprintf(r->gpu_verdict, sizeof(r->gpu_verdict),
		         "weave-on-scanout topology: unknown (DXGI factory unavailable)");
		snprintf(r->gpu_service_ingest, sizeof(r->gpu_service_ingest),
		         "service ingest: unknown (DXGI factory unavailable)");
		return;
	}

	uint64_t best_vram = 0;
	for (UINT i = 0;; i++) {
		IDXGIAdapter1 *adapter = NULL;
		if (FAILED(IDXGIFactory1_EnumAdapters1(factory, i, &adapter)) || adapter == NULL) {
			break;
		}
		DXGI_ADAPTER_DESC1 d;
		memset(&d, 0, sizeof(d));
		if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &d)) && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
			const uint64_t luid = pack_luid(d.AdapterLuid);
			const uint64_t vram = (uint64_t)d.DedicatedVideoMemory;

			if (r->gpu_adapter_count < CLI_MAX_GPU_ADAPTERS) {
				struct cli_gpu_adapter *e = &r->gpu_adapters[r->gpu_adapter_count++];
				WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, e->name, (int)sizeof(e->name), NULL,
				                    NULL);
				e->luid = luid;
				e->dedicated_vram_bytes = vram;
			}

			// Default render suggestion: most dedicated VRAM wins.
			if (!r->gpu_render_resolved || vram > best_vram) {
				r->gpu_render_resolved = true;
				r->gpu_render_luid = luid;
				best_vram = vram;
				WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, r->gpu_render_name,
				                    (int)sizeof(r->gpu_render_name), NULL, NULL);
			}
		}
		IDXGIAdapter1_Release(adapter);
	}
	IDXGIFactory1_Release(factory);

	// #1153 — resolved HERE, above every remaining early return, so the ingest
	// line survives an unresolvable panel rect: the ranking needs nothing but
	// DXGI, and only the `scanout` keyword degrades without the rect.
	describe_service_ingest(
	    r, info != NULL ? info->display_screen_left : 0, info != NULL ? info->display_screen_top : 0,
	    info != NULL ? info->display_pixel_width : 0, info != NULL ? info->display_pixel_height : 0);

	// The panel's scanout adapter, from the plug-in's own panel rect.
	uint64_t scanout_luid = 0;
	if (info != NULL &&
	    d3d_scanout_adapter_luid(info->display_screen_left, info->display_screen_top, info->display_pixel_width,
	                             info->display_pixel_height, &scanout_luid)) {
		r->gpu_scanout_resolved = true;
		r->gpu_scanout_luid = scanout_luid;
		for (uint32_t a = 0; a < r->gpu_adapter_count; a++) {
			if (r->gpu_adapters[a].luid == scanout_luid) {
				snprintf(r->gpu_scanout_name, sizeof(r->gpu_scanout_name), "%s",
				         r->gpu_adapters[a].name);
				break;
			}
		}
	}

	if (!r->gpu_scanout_resolved) {
		if (r->gpu_note[0] == '\0') {
			snprintf(r->gpu_note, sizeof(r->gpu_note),
			         "panel's scanout adapter unresolvable (panel %ux%u at %d,%d)",
			         info != NULL ? info->display_pixel_width : 0,
			         info != NULL ? info->display_pixel_height : 0,
			         info != NULL ? (int)info->display_screen_left : 0,
			         info != NULL ? (int)info->display_screen_top : 0);
		}
		snprintf(r->gpu_verdict, sizeof(r->gpu_verdict),
		         "weave-on-scanout topology: unknown (scanout adapter unresolvable)");
		return;
	}

	if (!r->gpu_render_resolved) {
		snprintf(r->gpu_verdict, sizeof(r->gpu_verdict),
		         "weave-on-scanout topology: unknown (no hardware render adapter found)");
		return;
	}

	r->gpu_split_applies = r->gpu_render_luid != r->gpu_scanout_luid;
	if (r->gpu_split_applies) {
		snprintf(r->gpu_verdict, sizeof(r->gpu_verdict),
		         "weave-on-scanout topology: APPLIES (render != scanout)");
	} else {
		snprintf(r->gpu_verdict, sizeof(r->gpu_verdict), "weave-on-scanout topology: does not apply (%s)",
		         r->gpu_adapter_count <= 1 ? "single adapter" : "same adapter");
	}
	describe_service_split(r);
}

/*!
 * Read `ActiveRuntime` and judge it.
 *
 * The verdict is the point: a runtime that starts perfectly is still useless
 * if apps resolve to somebody else's manifest, and that is invisible from
 * every other probe in this file. Compare against the installer's own
 * `InstallPath` rather than sniffing the path for "DisplayXR", so a competing
 * runtime that merely lives under a similarly-named folder cannot pass.
 */
static void
read_active_runtime(struct cli_query_result *r)
{
	r->active_runtime_queried = true;
	r->active_runtime_ok = true; // Absence never fails; see CLI_SELFTEST_RUNTIME_HIJACKED.

	wchar_t wbuf[1024];
	DWORD wbuf_bytes = sizeof(wbuf);
	LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ,
	                          NULL, wbuf, &wbuf_bytes);
	if (rc != ERROR_SUCCESS) {
		snprintf(r->active_runtime_note, sizeof(r->active_runtime_note),
		         "ActiveRuntime is unset (OK — from-source or CI box; installed runtimes set it)");
		return;
	}
	WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, r->active_runtime, (int)sizeof(r->active_runtime), NULL, NULL);
	r->active_runtime_set = r->active_runtime[0] != '\0';
	if (!r->active_runtime_set) {
		snprintf(r->active_runtime_note, sizeof(r->active_runtime_note), "ActiveRuntime is empty");
		return;
	}

	// Resolve where this install's manifest should be, from the record the
	// installer wrote. No InstallPath means we are not an installed runtime
	// and have no standing to call the key wrong.
	wchar_t install[MAX_PATH];
	DWORD install_bytes = sizeof(install);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\Runtime", L"InstallPath", RRF_RT_REG_SZ, NULL,
	                 install, &install_bytes) != ERROR_SUCCESS) {
		snprintf(r->active_runtime_note, sizeof(r->active_runtime_note),
		         "%s (not evaluated — no installed DisplayXR to compare against)", r->active_runtime);
		return;
	}
	wchar_t expect[MAX_PATH];
	_snwprintf_s(expect, MAX_PATH, _TRUNCATE, L"%s\\DisplayXR_win64.json", install);

	if (_wcsicmp(expect, wbuf) == 0) {
		snprintf(r->active_runtime_note, sizeof(r->active_runtime_note), "%s (this install)", r->active_runtime);
		return;
	}

	// Kept short on purpose: this lands in a 256-byte check detail, and the
	// hijacker's path is the one part that must survive verbatim.
	r->active_runtime_ok = false;
	snprintf(r->active_runtime_note, sizeof(r->active_runtime_note),
	         "HIJACKED — apps resolve to '%s', not this install. Fix (elevated): displayxr-cli runtime activate",
	         r->active_runtime);
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

/*!
 * #1252 — resolve every allow-listed performance setting through the same chain
 * the runtime uses, recording where each value came from.
 *
 * The provenance is the point. Without it a reader cannot tell a value that is
 * merely this process's environment (and therefore says nothing about any other
 * process) from one in the per-user or machine store (which every app on the
 * box will see). Flattening those two into "the current value" is exactly the
 * trap `docs/roadmap/control-panel-performance-settings.md` is about.
 *
 * Platform-independent on purpose — it sits outside the Windows-only block
 * above, because the per-user store works everywhere the runtime does.
 */
static void
collect_settings(struct cli_query_result *r)
{
	r->setting_count = 0;

	char path[512];
	if (u_setting_user_path(path, sizeof(path))) {
		snprintf(r->settings_user_file, sizeof(r->settings_user_file), "%s", path);
	}
	char written[32];
	if (u_setting_user_written(written, sizeof(written)) != NULL) {
		snprintf(r->settings_user_written, sizeof(r->settings_user_written), "%s", written);
	}

	uint32_t n = u_setting_managed_count();
	for (uint32_t i = 0; i < n && r->setting_count < CLI_MAX_SETTINGS; i++) {
		const char *name = u_setting_managed_name(i);
		if (name == NULL) {
			continue;
		}
		struct cli_setting_row *row = &r->settings[r->setting_count++];
		memset(row, 0, sizeof(*row));
		snprintf(row->name, sizeof(row->name), "%s", name);

		char buf[128];
		enum u_setting_source src = U_SETTING_SOURCE_DEFAULT;
		const char *val = u_setting_get_raw(name, buf, sizeof(buf), &src);
		if (val != NULL && val[0] != '\0') {
			snprintf(row->value, sizeof(row->value), "%s", val);
			row->set = true;
		}
		snprintf(row->source, sizeof(row->source), "%s", u_setting_source_str(src));
	}
}

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

/*!
 * ADR-034 / #823 — input-provider checks. ABSENCE NEVER FAILS: with no
 * provider registered, the ForceQwerty override set, or the provider's
 * hardware not actually plugged in, the checks are skipped with an
 * informational note (qwerty keeping the hand roles is the normal
 * configuration in all three cases). Only a registered, non-overridden
 * provider whose hardware IS present and which then failed to produce
 * left+right motion-controller role devices with a valid interaction
 * profile flips the verdict — applied at the end of cli_query_fill so
 * display failures keep their more fundamental result codes.
 */
static void
probe_input_providers(struct cli_query_result *r, struct cli_query_handles *h)
{
	struct target_input_plugin_desc descs[16];
	r->input_provider_count = target_input_plugin_enumerate(descs, 16);
	r->input_force_qwerty = target_input_plugin_get_force_qwerty();

	if (r->input_provider_count == 0) {
		snprintf(r->input_note, sizeof(r->input_note),
		         "not evaluated: no input provider registered (OK — qwerty keeps the hand roles)");
		return;
	}
	if (r->input_force_qwerty) {
		snprintf(r->input_note, sizeof(r->input_note),
		         "not evaluated: ForceQwerty override set (OK — qwerty keeps the hand roles)");
		return;
	}

	const struct xrt_input_plugin_iface *iface = target_input_plugin_get_active();
	r->input_provider_active = iface != NULL;
	if (iface != NULL) {
		snprintf(r->input_provider_id, sizeof(r->input_provider_id), "%s", iface->id ? iface->id : "");
	}

	// Nobody claimed the system. That is only a fault if a provider
	// failed to LOAD (bad entry point, ABI-major mismatch); providers
	// declining cleanly — no hardware of their type, or sim-input's
	// DXR_SIM_INPUT opt-in left unset — is the ordinary state of most
	// boxes and leaves qwerty correctly holding the hand roles.
	if (!r->input_provider_active) {
		int declined = 0;
		int failed = 0;
		target_input_plugin_get_scan_result(&declined, &failed);
		if (failed > 0) {
			r->input_evaluated = true;
			snprintf(r->input_note, sizeof(r->input_note),
			         "FAIL: %d registered provider(s) failed to load (%d declined cleanly)", failed,
			         declined);
		} else {
			snprintf(r->input_note, sizeof(r->input_note),
			         "not evaluated: all %d registered provider(s) declined "
			         "(OK — qwerty keeps the hand roles)",
			         declined);
		}
		return;
	}

	// Presence-gated arbitration: a loaded provider whose hardware is
	// unplugged is SUPPOSED to leave the hand roles to qwerty, so there
	// is nothing to assert about role devices in that state.
	r->input_provider_present = t_input_arbiter_provider_holds_roles();
	if (!r->input_provider_present) {
		snprintf(r->input_note, sizeof(r->input_note),
		         "not evaluated: provider '%s' loaded but its hardware is absent "
		         "(OK — qwerty holds the hand roles)",
		         r->input_provider_id);
		return;
	}

	r->input_evaluated = true;

	bool claimed_left = false;
	bool claimed_right = false;
	t_builder_input_provider_get_claims(&claimed_left, &claimed_right);

	struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
	xrt_system_devices_get_roles(h->xsysd, &roles);

	if (claimed_left && roles.left >= 0 && (uint32_t)roles.left < h->xsysd->xdev_count) {
		struct xrt_device *xdev = h->xsysd->xdevs[roles.left];
		bool type_ok = xdev->device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER ||
		               xdev->device_type == XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER;
		r->input_left_ok = type_ok && xdev->name != 0;
		snprintf(r->input_left_str, sizeof(r->input_left_str), "%s", xdev->str);
	}
	if (claimed_right && roles.right >= 0 && (uint32_t)roles.right < h->xsysd->xdev_count) {
		struct xrt_device *xdev = h->xsysd->xdevs[roles.right];
		bool type_ok = xdev->device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER ||
		               xdev->device_type == XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER;
		r->input_right_ok = type_ok && xdev->name != 0;
		snprintf(r->input_right_str, sizeof(r->input_right_str), "%s", xdev->str);
	}

	// #825 Tier 2 — hand-tracking role arbitration. Expected iff the
	// provider's role device advertises supported.hand_tracking; ok iff
	// the builder actually filled the matching static hand-tracking role
	// (either data source). Providers without hand tracking expect
	// nothing and pass.
	if (roles.left >= 0 && (uint32_t)roles.left < h->xsysd->xdev_count) {
		r->input_ht_expected_left = claimed_left && h->xsysd->xdevs[roles.left]->supported.hand_tracking;
	}
	if (roles.right >= 0 && (uint32_t)roles.right < h->xsysd->xdev_count) {
		r->input_ht_expected_right = claimed_right && h->xsysd->xdevs[roles.right]->supported.hand_tracking;
	}
	bool ht_claimed_left = false;
	bool ht_claimed_right = false;
	t_builder_input_provider_get_ht_claims(&ht_claimed_left, &ht_claimed_right);
	r->input_ht_left_ok = ht_claimed_left && (h->xsysd->static_roles.hand_tracking.unobstructed.left != NULL ||
	                                          h->xsysd->static_roles.hand_tracking.conforming.left != NULL);
	r->input_ht_right_ok = ht_claimed_right && (h->xsysd->static_roles.hand_tracking.unobstructed.right != NULL ||
	                                            h->xsysd->static_roles.hand_tracking.conforming.right != NULL);

	const bool ht_ok = (!r->input_ht_expected_left || r->input_ht_left_ok) &&
	                   (!r->input_ht_expected_right || r->input_ht_right_ok);

	if (!r->input_left_ok || !r->input_right_ok) {
		snprintf(r->input_note, sizeof(r->input_note),
		         "FAIL: provider '%s' active but roles incomplete (left=%s right=%s)", r->input_provider_id,
		         r->input_left_ok ? "ok" : "missing", r->input_right_ok ? "ok" : "missing");
	} else if (!ht_ok) {
		snprintf(
		    r->input_note, sizeof(r->input_note),
		    "FAIL: provider '%s' devices advertise hand tracking but roles unfilled (ht-left=%s ht-right=%s)",
		    r->input_provider_id,
		    !r->input_ht_expected_left ? "n/a"
		    : r->input_ht_left_ok      ? "ok"
		                               : "missing",
		    !r->input_ht_expected_right ? "n/a"
		    : r->input_ht_right_ok      ? "ok"
		                                : "missing");
	} else {
		snprintf(r->input_note, sizeof(r->input_note), "provider '%s': left='%s' right='%s'%s",
		         r->input_provider_id, r->input_left_str, r->input_right_str,
		         (r->input_ht_left_ok || r->input_ht_right_ok) ? " (+hand-tracking roles)" : "");
	}
}

#ifdef XRT_OS_WINDOWS
//! UTF-16 -> UTF-8 into a caller buffer. Empty string on any failure.
static void
w_to_utf8(const WCHAR *w, char *out, int out_size)
{
	out[0] = '\0';
	if (w == NULL) {
		return;
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, out_size, NULL, NULL);
	if (n <= 0) {
		out[0] = '\0';
	}
}

/*!
 * #1234 / #902 — is `VK_LAYER_DXR_queue_lock` still reachable by the Vulkan
 * loader on this box?
 *
 * Deliberately answered from the REGISTRATION, not by loading `vulkan-1.dll`
 * and enumerating. Two reasons: this tool's whole value is that it needs no
 * GPU and no Vulkan, and post-#1229 a tool that loads `vulkan-1` is one more
 * process a stray loader beside the runtime can poison. The cost is that this
 * reimplements a slice of loader lookup rather than asking the loader — so it
 * checks the three things that actually rot (the value, the manifest, the
 * library path) and reports what it found rather than claiming a verdict the
 * loader would necessarily agree with.
 *
 * INFORMATIONAL ONLY — see the field comment in cli_query.h. Every exit path
 * writes `vk_layer_note`.
 */
static void
probe_vk_queue_lock_layer(struct cli_query_result *r)
{
	r->vk_layer_probed = true;

	HKEY key = NULL;
	// KEY_WOW64_64KEY: the loader reads the 64-bit view, and so must we —
	// a 32-bit build of this tool would otherwise look in the WOW6432Node
	// redirect and report a perfectly registered layer as missing.
	LSTATUS ls = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Khronos\\Vulkan\\ExplicitLayers", 0,
	                           KEY_READ | KEY_WOW64_64KEY, &key);
	if (ls != ERROR_SUCCESS) {
		snprintf(r->vk_layer_note, sizeof(r->vk_layer_note),
		         "NOT registered: no HKLM\\Software\\Khronos\\Vulkan\\ExplicitLayers key "
		         "(#868 late-weave repaint unavailable on a single-graphics-queue GPU)");
		return;
	}

	// The value NAME is the manifest path; the DWORD data is the enable flag
	// (0 = enabled). Ours is whatever path the installer wrote, so match on
	// the filename rather than assuming an install directory.
	static const WCHAR *k_manifest_leaf = L"VkLayer_DXR_queue_lock.json";
	WCHAR name[1024];
	DWORD enable_data = 0;
	bool found = false;
	for (DWORD i = 0;; i++) {
		DWORD name_len = (DWORD)(sizeof(name) / sizeof(name[0]));
		DWORD type = 0;
		DWORD data = 0;
		DWORD data_len = sizeof(data);
		ls = RegEnumValueW(key, i, name, &name_len, NULL, &type, (LPBYTE)&data, &data_len);
		if (ls == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (ls != ERROR_SUCCESS) {
			continue; // a value we cannot read is not a reason to stop looking
		}
		size_t len = wcslen(name);
		size_t leaf = wcslen(k_manifest_leaf);
		if (len >= leaf && _wcsicmp(name + (len - leaf), k_manifest_leaf) == 0) {
			found = true;
			enable_data = (type == REG_DWORD) ? data : 0;
			w_to_utf8(name, r->vk_layer_manifest, (int)sizeof(r->vk_layer_manifest));
			break;
		}
	}
	RegCloseKey(key);

	if (!found) {
		snprintf(r->vk_layer_note, sizeof(r->vk_layer_note),
		         "NOT registered: no ExplicitLayers value names %s "
		         "(#868 late-weave repaint unavailable on a single-graphics-queue GPU)",
		         "VkLayer_DXR_queue_lock.json");
		return;
	}
	r->vk_layer_registered = true;
	r->vk_layer_enabled = (enable_data == 0);

	FILE *f = fopen(r->vk_layer_manifest, "rb");
	if (f == NULL) {
		snprintf(r->vk_layer_note, sizeof(r->vk_layer_note), "registered but the manifest is MISSING: %s",
		         r->vk_layer_manifest);
		return;
	}
	char json[4096];
	size_t got = fread(json, 1, sizeof(json) - 1, f);
	fclose(f);
	json[got] = '\0';

	cJSON *root = cJSON_Parse(json);
	cJSON *layer = (root != NULL) ? cJSON_GetObjectItemCaseSensitive(root, "layer") : NULL;
	cJSON *lib = (layer != NULL) ? cJSON_GetObjectItemCaseSensitive(layer, "library_path") : NULL;
	if (!cJSON_IsString(lib) || lib->valuestring == NULL || lib->valuestring[0] == '\0') {
		cJSON_Delete(root);
		snprintf(r->vk_layer_note, sizeof(r->vk_layer_note),
		         "registered but the manifest has no usable layer.library_path: %s", r->vk_layer_manifest);
		return;
	}
	r->vk_layer_manifest_ok = true;
	snprintf(r->vk_layer_library, sizeof(r->vk_layer_library), "%s", lib->valuestring);
	cJSON_Delete(root);

	// What the loader accepts is narrower than "a path", and NOT simply
	// "absolute only" - see the measured note in
	// src/xrt/targets/vk_layer/CMakeLists.txt. library_path is resolved
	// against the manifest's own directory ONLY when it contains the platform
	// directory symbol, which on Windows is a BACKSLASH. So:
	//   "C:\\dir\\x.dll"  absolute            -> ok
	//   ".\\x.dll"          relative + backslash -> ok (joined to the manifest dir)
	//   "./x.dll" / "x.dll"  no backslash        -> handed to LoadLibraryEx verbatim,
	//                                              which rejects it with error 87 and
	//                                              surfaces as VK_ERROR_OUT_OF_HOST_MEMORY
	//                                              naming neither layer nor path.
	// Flagging the second form would be a FALSE ALARM against the shape the
	// package deliberately ships, and would invite someone to "fix" a working
	// manifest into a broken one.
	const char *p = r->vk_layer_library;
	bool absolute = (p[0] != '\0' && p[1] == ':') || (p[0] == '\\' && p[1] == '\\');
	char resolved[1024];
	if (absolute) {
		snprintf(resolved, sizeof(resolved), "%s", p);
	} else if (strchr(p, '\\') != NULL) {
		char dir[512];
		snprintf(dir, sizeof(dir), "%s", r->vk_layer_manifest);
		char *slash = strrchr(dir, '\\');
		if (slash != NULL) {
			*slash = '\0';
		} else {
			dir[0] = '\0';
		}
		// Drop a leading ".\\" so the reported path reads as a path and not
		// as "...\\.\\name.dll"; both resolve, only one is legible in a bug report.
		const char *rel = p;
		if (rel[0] == '.' && rel[1] == '\\') {
			rel += 2;
		}
		snprintf(resolved, sizeof(resolved), "%s\\%s", dir, rel);
	} else {
		snprintf(r->vk_layer_note, sizeof(r->vk_layer_note),
		         "registered but library_path '%s' has no directory separator — the loader "
		         "hands it to LoadLibraryEx verbatim, which fails with error 87 (the app sees "
		         "VK_ERROR_OUT_OF_HOST_MEMORY naming neither layer nor path)",
		         r->vk_layer_library);
		return;
	}
	if (GetFileAttributesA(resolved) == INVALID_FILE_ATTRIBUTES) {
		snprintf(r->vk_layer_note, sizeof(r->vk_layer_note), "registered but the layer DLL is MISSING: %s",
		         resolved);
		return;
	}
	r->vk_layer_library_ok = true;

	if (!r->vk_layer_enabled) {
		snprintf(r->vk_layer_note, sizeof(r->vk_layer_note),
		         "registered and present but DISABLED (ExplicitLayers value is non-zero): %s",
		         r->vk_layer_manifest);
		return;
	}

	// Reachable. Deliberately not phrased as "repaint is on": the tier also
	// depends on whether the driver hands out a dedicated queue, which is a
	// device-time fact this headless tool cannot see.
	snprintf(r->vk_layer_note, sizeof(r->vk_layer_note), "VK_LAYER_DXR_queue_lock reachable (%s)", resolved);
}
#endif

void
cli_query_fill(struct cli_query_result *r, struct cli_query_handles *h, const struct xrt_instance_info *ii)
{
	memset(r, 0, sizeof(*r));
	memset(h, 0, sizeof(*h));
	snprintf(r->runtime_description, sizeof(r->runtime_description), "%s", u_runtime_description);
	snprintf(r->git_tag, sizeof(r->git_tag), "%s", u_git_tag);
	r->plugin_abi_version = (uint32_t)XRT_PLUGIN_API_VERSION_CURRENT;
	r->result_code = CLI_SELFTEST_INIT_FAIL;
	// Default-true so the ActiveRuntime verdict is a PASS everywhere the
	// concept does not exist (non-Windows); read_active_runtime() below is
	// the only thing that may clear it.
	r->active_runtime_ok = true;

#ifdef XRT_OS_WINDOWS
	read_active_runtime(r);
	// #1201 — record what this process is entitled to SEE before it looks at
	// anything. `unaware` here is the tell that every pixel dimension below
	// came through DPI virtualisation.
	{
		enum u_win_dpi_awareness aware = u_win_get_process_dpi_awareness();
		snprintf(r->dpi_awareness, sizeof(r->dpi_awareness), "%s", u_win_dpi_awareness_to_string(aware));
		r->dpi_aware_ok = aware == U_WIN_DPI_PER_MONITOR_AWARE;
	}
#else
	snprintf(r->dpi_awareness, sizeof(r->dpi_awareness), "n/a");
	r->dpi_aware_ok = true;
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

	// ADR-034 / #823 — input-provider checks (fields only; the verdict
	// is applied at the end so display failures keep their codes).
	probe_input_providers(r, h);

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

	/* #1212 — was a BETTER-ranked plug-in present and rejected?
	 *
	 * plugin_ok above is true for ANY plug-in that wins discovery, including
	 * the vendor-neutral sim_display at ProbeOrder 200. That is why a Leia
	 * device whose vendor plug-in failed to load used to report a green PASS
	 * while rendering nothing. Discovery attempts in ascending ProbeOrder and
	 * returns on first success, so anything it rejected out-ranked the winner.
	 *
	 * ABSENCE NEVER FAILS — same rule zone_caps and input_providers already
	 * follow — so a hardware-free dev box, CI, and a machine that legitimately
	 * has only sim_display all stay green. */
	struct target_plugin_discovery_summary disc;
	target_plugin_get_discovery_summary(&disc);
	r->vendor_dp_ok = (disc.rejected_count == 0);
	if (r->vendor_dp_ok) {
		snprintf(r->vendor_dp_note, sizeof(r->vendor_dp_note),
		         "no better-ranked plug-in failed to load (active ProbeOrder=%u, %d declined their probe)",
		         (unsigned)disc.active_probe_order, disc.declined_count);
	} else {
		snprintf(r->vendor_dp_note, sizeof(r->vendor_dp_note),
		         "'%s' (ProbeOrder=%u) out-ranks the active plug-in but was rejected: %s — the runtime "
		         "fell back to '%s'",
		         disc.best_rejected_id, (unsigned)disc.best_rejected_order, disc.best_rejected_reason,
		         iface->id ? iface->id : "?");
	}

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

	// #1252 — the allow-listed performance settings, resolved through the same
	// chain the runtime uses. Platform-independent, and deliberately BEFORE the
	// GPU probe so it is reported even where that probe does not run.
	collect_settings(r);

	// #918 — GPU topology. Runs BEFORE the dims check so a box with a
	// half-broken plug-in still gets its adapter list dumped; the scanout
	// resolution simply reports "unresolvable" when the rect is unusable.
#ifdef XRT_OS_WINDOWS
	probe_gpu_topology(r, &r->display_info);
#else
	snprintf(r->gpu_verdict, sizeof(r->gpu_verdict), "weave-on-scanout topology: n/a (Windows-only)");
	snprintf(r->gpu_service_split, sizeof(r->gpu_service_split), "service split: n/a (Windows-only)");
	snprintf(r->gpu_service_ingest, sizeof(r->gpu_service_ingest), "service ingest: n/a (Windows-only)");
#endif

	if (!(info.display_width_m > 0.0f) || !(info.display_height_m > 0.0f) || info.display_pixel_width == 0 ||
	    info.display_pixel_height == 0) {
		r->result_code = CLI_SELFTEST_BAD_INFO;
		snprintf(r->dims_note, sizeof(r->dims_note), "reported dimensions are not sane");
		return;
	}

	// #1201 — sane is not the same as CORRECT. Prove the reported pixel size
	// against the mode the adapter is actually scanning out; a check that
	// passes on a wrong number is worse than no check.
#ifdef XRT_OS_WINDOWS
	probe_native_display_mode(r, &info);
#endif
	r->dims_verdict = cli_dims_compare(r->native_probed, info.display_pixel_width, info.display_pixel_height,
	                                   r->native_pixel_width, r->native_pixel_height);
	switch (r->dims_verdict) {
	case CLI_DIMS_MATCH:
		snprintf(r->dims_note, sizeof(r->dims_note), "%ux%u px == %s current mode (%ux%u @ %u Hz)",
		         info.display_pixel_width, info.display_pixel_height, or_q(r->native_device),
		         r->native_pixel_width, r->native_pixel_height, r->native_refresh_hz);
		break;
	case CLI_DIMS_MISMATCH: {
		uint32_t pct = cli_dims_scaling_percent(info.display_pixel_width, info.display_pixel_height,
		                                        r->native_pixel_width, r->native_pixel_height);
		if (pct != 0) {
			snprintf(r->dims_note, sizeof(r->dims_note),
			         "reported %ux%u px but %s is %ux%u — exactly %u%% scaling, i.e. LOGICAL "
			         "coordinates (process is %s)",
			         info.display_pixel_width, info.display_pixel_height, or_q(r->native_device),
			         r->native_pixel_width, r->native_pixel_height, pct, or_q(r->dpi_awareness));
		} else {
			snprintf(r->dims_note, sizeof(r->dims_note), "reported %ux%u px but %s is %ux%u @ %u Hz",
			         info.display_pixel_width, info.display_pixel_height, or_q(r->native_device),
			         r->native_pixel_width, r->native_pixel_height, r->native_refresh_hz);
		}
		break;
	}
	default:
		snprintf(r->dims_note, sizeof(r->dims_note), "%ux%u px (no authoritative mode to compare: %s)",
		         info.display_pixel_width, info.display_pixel_height,
#ifdef XRT_OS_WINDOWS
		         "monitor unresolvable"
#else
		         "Windows-only probe"
#endif
		);
		break;
	}

	if (r->dims_verdict == CLI_DIMS_MISMATCH) {
		r->result_code = CLI_SELFTEST_DIMS_MISMATCH;
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

	// #1234 / #902 — can the Vulkan loader still reach the queue-lock layer?
	// Pure reporting: never touches result_code. See the field comment in
	// cli_query.h for why this must not be fatal.
#ifdef XRT_OS_WINDOWS
	probe_vk_queue_lock_layer(r);
#else
	snprintf(r->vk_layer_note, sizeof(r->vk_layer_note),
	         "not probed: the queue-lock layer is a Windows-only registration (OK)");
#endif

	// ADR-034 / #823 — a registered, non-overridden input provider whose
	// hardware is actually PRESENT must have produced left+right
	// motion-controller role devices — and (#825 Tier 2) hand-tracking
	// roles for any role device that advertises hand tracking. Providers
	// that declined, or whose hardware is unplugged, leave input_evaluated
	// false and pass: the arbiter giving the roles to qwerty is the
	// correct outcome there, not a fault. Applied last so display
	// failures keep their more fundamental codes.

	// #1201 — a DPI-unaware process reads VIRTUALISED geometry, so every
	// pixel dimension above is suspect. On a 100%-scaled box the numbers come
	// out right anyway and the dims check cannot see the regression, which is
	// exactly why awareness is asserted in its own right.
	if (r->result_code == CLI_SELFTEST_PASS && !r->dpi_aware_ok) {
		r->result_code = CLI_SELFTEST_NOT_DPI_AWARE;
	}

	if (r->result_code == CLI_SELFTEST_PASS && r->input_evaluated &&
	    !(r->input_provider_active && r->input_left_ok && r->input_right_ok &&
	      (!r->input_ht_expected_left || r->input_ht_left_ok) &&
	      (!r->input_ht_expected_right || r->input_ht_right_ok))) {
		r->result_code = CLI_SELFTEST_BAD_INPUT;
	}

	// #1212 — a better-ranked plug-in was present and rejected, so the
	// runtime is running on a fallback DP. Applied last so a more
	// fundamental display failure keeps its own code: this is a
	// misconfiguration verdict, not a broken-runtime one, and everything
	// above it still describes a runtime that came up.
	if (r->result_code == CLI_SELFTEST_PASS && !r->vendor_dp_ok) {
		r->result_code = CLI_SELFTEST_VENDOR_DP_REJECTED;
	}

	// Another runtime holds ActiveRuntime. Applied last, for the same reason
	// as the check above: everything before it describes a runtime that came
	// up correctly, and this one says apps will never reach it.
	if (r->result_code == CLI_SELFTEST_PASS && !r->active_runtime_ok) {
		r->result_code = CLI_SELFTEST_RUNTIME_HIJACKED;
	}
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
 * Render a packed adapter LUID the same way the runtime's session logs do
 * (`HighPart:LowPart`), so a CLI dump and an app log can be grepped against
 * each other without conversion.
 */
static const char *
luid_label(uint64_t packed, char *buf, size_t cap)
{
	snprintf(buf, cap, "%08lx:%08lx", (unsigned long)(packed >> 32), (unsigned long)(packed & 0xffffffffu));
	return buf;
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
	// #1201 — the reported pixels CHECKED against the mode the adapter is
	// actually scanning out, plus this process's own DPI awareness. A bug
	// report that quotes a panel resolution has to carry the evidence for it;
	// this tool used to quote a DPI-virtualised number with none.
	if (r->native_probed) {
		PT("panel mode:   %ux%u @ %u Hz on %s  [%s]\n", r->native_pixel_width, r->native_pixel_height,
		   r->native_refresh_hz, or_q(r->native_device),
		   r->dims_verdict == CLI_DIMS_MATCH ? "matches reported pixels" : "** MISMATCH vs reported pixels **");
	} else {
		PT("panel mode:   not probed\n");
	}
	PT("DPI aware:    %s\n", or_q(r->dpi_awareness));
	if (r->dims_verdict == CLI_DIMS_MISMATCH) {
		PT("** %s\n", r->dims_note);
	}
	PT("viewer:       (%.4f, %.4f, %.4f) m\n", (double)i->nominal_viewer_x_m, (double)i->nominal_viewer_y_m,
	   (double)i->nominal_viewer_z_m);
	// Baseline hint only — the authoritative scale is per rendering mode (below).
	PT("view scale:   (%.3f, %.3f) (baseline hint; see per-mode scale)\n", (double)i->recommended_view_scale_x,
	   (double)i->recommended_view_scale_y);
	PT("screen pos:   (%d, %d)\n", i->display_screen_left, i->display_screen_top);
	char et_buf[64];
	PT("eye-tracking: supported=%s (0x%x) default=%s\n",
	   eye_modes_label(i->supported_eye_tracking_modes, et_buf, sizeof(et_buf)), i->supported_eye_tracking_modes,
	   eye_default_label(i->default_eye_tracking_mode));
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
		PT("service / shell:                  '%s' (%s)\n", or_q(r->dp_sel_service_id), r->dp_sel_service_conf);
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

	P(" :: GPU topology (#918 — does the weave cross adapters to reach the panel?)\n");
	if (!r->gpu_probed) {
		PT("%s\n", r->gpu_verdict[0] != '\0' ? r->gpu_verdict : "not evaluated");
	} else {
		char lb[32];
		PT("adapters:     %u\n", r->gpu_adapter_count);
		for (uint32_t a = 0; a < r->gpu_adapter_count; a++) {
			const struct cli_gpu_adapter *g = &r->gpu_adapters[a];
			PT("  [%u] %-32s LUID=%s  dedicated VRAM %llu MB\n", a, g->name,
			   luid_label(g->luid, lb, sizeof(lb)),
			   (unsigned long long)(g->dedicated_vram_bytes / (1024 * 1024)));
		}
		if (r->gpu_scanout_resolved) {
			PT("panel scanout: '%s' LUID=%s\n",
			   r->gpu_scanout_name[0] != '\0' ? r->gpu_scanout_name : "<not enumerated>",
			   luid_label(r->gpu_scanout_luid, lb, sizeof(lb)));
		} else {
			PT("panel scanout: UNRESOLVED (%s)\n", or_q(r->gpu_note));
		}
		if (r->gpu_render_resolved) {
			PT("render (default suggestion): '%s' LUID=%s\n", r->gpu_render_name,
			   luid_label(r->gpu_render_luid, lb, sizeof(lb)));
		} else {
			PT("render (default suggestion): <none>\n");
		}
		PT("%s\n", r->gpu_verdict);
		PT("DXR_WEAVE_ON_SCANOUT=%s (kill switch; the split is ON by default)\n",
		   r->gpu_weave_env_set ? r->gpu_weave_env : "<unset>");
		PT("%s\n", r->gpu_service_split);
		PT("%s\n", r->gpu_service_ingest);
	}

	// #1234 / #902 - VK late-weave repaint reachability.
	P(" :: VK late-weave repaint (#868/#902)\n");
	PT("%s\n", r->vk_layer_note[0] != '\0' ? r->vk_layer_note : "not evaluated");
	if (r->vk_layer_registered) {
		PT("manifest:     %s%s\n", r->vk_layer_manifest, r->vk_layer_enabled ? "" : "  (DISABLED)");
		if (r->vk_layer_library[0] != '\0') {
			PT("library_path: %s%s\n", r->vk_layer_library, r->vk_layer_library_ok ? "" : "  (UNUSABLE)");
		}
	}

	P(" :: Input providers (ADR-034)\n");
	PT("registered:   %d%s\n", r->input_provider_count,
	   r->input_force_qwerty ? "  (ForceQwerty override SET)" : "");
	if (r->input_evaluated) {
		PT("active:       %s\n", r->input_provider_active ? r->input_provider_id : "<none claimed>");
		PT("hardware:     %s\n", r->input_provider_present ? "present (provider holds the hand roles)"
		                                                   : "absent (qwerty holds the hand roles)");
		PT("left:         %s\n", r->input_left_ok ? r->input_left_str : "<missing>");
		PT("right:        %s\n", r->input_right_ok ? r->input_right_str : "<missing>");
		PT("hand-track:   left=%s right=%s\n",
		   !r->input_ht_expected_left ? "n/a"
		   : r->input_ht_left_ok      ? "ok"
		                              : "MISSING",
		   !r->input_ht_expected_right ? "n/a"
		   : r->input_ht_right_ok      ? "ok"
		                               : "MISSING");
	} else {
		PT("%s\n", r->input_note[0] != '\0' ? r->input_note : "not evaluated");
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
			cJSON_AddBoolToObject(o, "can_rotate",
			                      (rm->mode_flags & XRT_RENDERING_MODE_FLAG_CAN_ROTATE) != 0);
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

		// #1201 — the proof behind `pixel_width`/`pixel_height`: the mode the
		// adapter is scanning out, and whether the plug-in's number equals it.
		cJSON *nm = cJSON_AddObjectToObject(d, "native_mode");
		cJSON_AddBoolToObject(nm, "probed", r->native_probed);
		if (r->native_probed) {
			cJSON_AddStringToObject(nm, "device", r->native_device);
			cJSON_AddNumberToObject(nm, "pixel_width", (double)r->native_pixel_width);
			cJSON_AddNumberToObject(nm, "pixel_height", (double)r->native_pixel_height);
			cJSON_AddNumberToObject(nm, "refresh_hz", (double)r->native_refresh_hz);
		}
		cJSON_AddBoolToObject(nm, "matches_reported", r->dims_verdict == CLI_DIMS_MATCH);
		cJSON_AddStringToObject(nm, "note", r->dims_note);
	} else {
		cJSON_AddNullToObject(root, "display");
	}

	// #1201 — this process's DPI awareness. Anything but "per-monitor-aware"
	// on Windows means every pixel dimension above came through DPI
	// virtualisation and must not be trusted.
	cJSON_AddStringToObject(root, "dpi_awareness", r->dpi_awareness);

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

	// #1234 / #902 - VK late-weave repaint reachability (informational).
	{
		cJSON *vl = cJSON_AddObjectToObject(root, "vk_repaint_tier");
		cJSON_AddBoolToObject(vl, "probed", r->vk_layer_probed);
		cJSON_AddBoolToObject(vl, "registered", r->vk_layer_registered);
		cJSON_AddBoolToObject(vl, "enabled", r->vk_layer_enabled);
		cJSON_AddBoolToObject(vl, "manifest_ok", r->vk_layer_manifest_ok);
		cJSON_AddBoolToObject(vl, "library_ok", r->vk_layer_library_ok);
		cJSON_AddStringToObject(vl, "manifest", r->vk_layer_manifest);
		cJSON_AddStringToObject(vl, "library_path", r->vk_layer_library);
		cJSON_AddStringToObject(vl, "note", r->vk_layer_note);
	}

	// #918 GPU topology.
	{
		char lb[32];
		cJSON *gt = cJSON_AddObjectToObject(root, "gpu_topology");
		cJSON_AddBoolToObject(gt, "probed", r->gpu_probed);
		cJSON_AddStringToObject(gt, "verdict", r->gpu_verdict);
		cJSON_AddBoolToObject(gt, "applies", r->gpu_split_applies);
		cJSON_AddStringToObject(gt, "weave_on_scanout_env",
		                        r->gpu_weave_env_set ? r->gpu_weave_env : "<unset>");
		cJSON_AddStringToObject(gt, "service_split", r->gpu_service_split);
		cJSON_AddStringToObject(gt, "split_ingress", r->gpu_split_ingress);
		cJSON_AddStringToObject(gt, "service_ingest", r->gpu_service_ingest);
		if (r->gpu_note[0] != '\0') {
			cJSON_AddStringToObject(gt, "note", r->gpu_note);
		}
		cJSON *ads = cJSON_AddArrayToObject(gt, "adapters");
		for (uint32_t a = 0; a < r->gpu_adapter_count; a++) {
			const struct cli_gpu_adapter *g = &r->gpu_adapters[a];
			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "name", g->name);
			cJSON_AddStringToObject(o, "luid", luid_label(g->luid, lb, sizeof(lb)));
			cJSON_AddNumberToObject(o, "dedicated_vram_bytes", (double)g->dedicated_vram_bytes);
			cJSON_AddItemToArray(ads, o);
		}
		cJSON *sc = cJSON_AddObjectToObject(gt, "scanout");
		cJSON_AddBoolToObject(sc, "resolved", r->gpu_scanout_resolved);
		if (r->gpu_scanout_resolved) {
			cJSON_AddStringToObject(sc, "name", r->gpu_scanout_name);
			cJSON_AddStringToObject(sc, "luid", luid_label(r->gpu_scanout_luid, lb, sizeof(lb)));
		}
		cJSON *rn = cJSON_AddObjectToObject(gt, "render_suggestion");
		cJSON_AddBoolToObject(rn, "resolved", r->gpu_render_resolved);
		if (r->gpu_render_resolved) {
			cJSON_AddStringToObject(rn, "name", r->gpu_render_name);
			cJSON_AddStringToObject(rn, "luid", luid_label(r->gpu_render_luid, lb, sizeof(lb)));
		}
		// ADR-037 §7 / #1153 — the service's ingest adapter, as the resolver
		// answers it in THIS environment (so a DXR_D3D_FORCE_GPU arm is
		// verifiable without starting the service).
		cJSON *ig = cJSON_AddObjectToObject(gt, "service_ingest_adapter");
		cJSON_AddBoolToObject(ig, "resolved", r->gpu_ingest_resolved);
		if (r->gpu_ingest_resolved) {
			cJSON_AddStringToObject(ig, "name", r->gpu_ingest_name);
			cJSON_AddStringToObject(ig, "luid", luid_label(r->gpu_ingest_luid, lb, sizeof(lb)));
			cJSON_AddStringToObject(ig, "provenance", r->gpu_ingest_provenance);
		}
	}

	// ADR-034 / #823 input-provider checks.
	{
		cJSON *ip = cJSON_AddObjectToObject(root, "input_providers");
		cJSON_AddNumberToObject(ip, "registered", (double)r->input_provider_count);
		cJSON_AddBoolToObject(ip, "force_qwerty", r->input_force_qwerty);
		cJSON_AddBoolToObject(ip, "hardware_present", r->input_provider_present);
		cJSON_AddBoolToObject(ip, "evaluated", r->input_evaluated);
		cJSON_AddStringToObject(ip, "note", r->input_note[0] != '\0' ? r->input_note : "not evaluated");
		if (r->input_evaluated) {
			cJSON_AddBoolToObject(ip, "active", r->input_provider_active);
			cJSON_AddStringToObject(ip, "active_id", r->input_provider_id);
			cJSON_AddBoolToObject(ip, "left_ok", r->input_left_ok);
			cJSON_AddStringToObject(ip, "left", r->input_left_str);
			cJSON_AddBoolToObject(ip, "right_ok", r->input_right_ok);
			cJSON_AddStringToObject(ip, "right", r->input_right_str);
			// #825 Tier 2 hand-tracking roles.
			cJSON_AddBoolToObject(ip, "ht_expected_left", r->input_ht_expected_left);
			cJSON_AddBoolToObject(ip, "ht_left_ok", r->input_ht_left_ok);
			cJSON_AddBoolToObject(ip, "ht_expected_right", r->input_ht_expected_right);
			cJSON_AddBoolToObject(ip, "ht_right_ok", r->input_ht_right_ok);
		}
	}

	// #224 / ADR-027 P4 zone-caps probe.
	{
		cJSON *zc = cJSON_AddObjectToObject(root, "zone_caps");
		cJSON_AddBoolToObject(zc, "probed", r->zone_caps_probed);
		cJSON_AddStringToObject(zc, "note",
		                        r->zone_probe_note[0] != '\0' ? r->zone_probe_note : "not evaluated");
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

	/*
	 * #918 GPU topology. Printed in the human dump since Phase 0; serialized
	 * here so the Control Panel (which only parses `--json`) can show the same
	 * answer instead of being blind to the one thing a hybrid box needs.
	 *
	 * The split is deliberate and load-bearing: everything at the top level is
	 * a MACHINE fact — the same in any process on this box — while everything
	 * under `env_scoped` is read from THIS process's environment and is only
	 * true of a process launched the same way. A GUI that renders the second
	 * kind as if it were the first is exactly the trap
	 * `docs/roadmap/control-panel-performance-settings.md` is about, so the
	 * grouping (and its `scope` string) is what stops a consumer conflating
	 * them. Do not flatten this object.
	 */
	{
		char lb[32];
		cJSON *g = cJSON_AddObjectToObject(root, "gpu");
		cJSON_AddBoolToObject(g, "probed", r->gpu_probed);
		cJSON_AddStringToObject(g, "note", r->gpu_note);

		if (r->gpu_probed) {
			cJSON_AddStringToObject(g, "verdict", r->gpu_verdict);
			cJSON_AddBoolToObject(g, "split_applies", r->gpu_split_applies);

			cJSON *arr = cJSON_AddArrayToObject(g, "adapters");
			for (uint32_t a = 0; a < r->gpu_adapter_count; a++) {
				const struct cli_gpu_adapter *ga = &r->gpu_adapters[a];
				cJSON *o = cJSON_CreateObject();
				cJSON_AddNumberToObject(o, "index", (double)a);
				cJSON_AddStringToObject(o, "name", ga->name);
				cJSON_AddStringToObject(o, "luid", luid_label(ga->luid, lb, sizeof(lb)));
				cJSON_AddNumberToObject(o, "dedicated_vram_mb",
				                        (double)(ga->dedicated_vram_bytes / (1024 * 1024)));
				cJSON_AddItemToArray(arr, o);
			}

			cJSON *sc = cJSON_AddObjectToObject(g, "scanout");
			cJSON_AddBoolToObject(sc, "resolved", r->gpu_scanout_resolved);
			if (r->gpu_scanout_resolved) {
				cJSON_AddStringToObject(sc, "name", r->gpu_scanout_name);
				cJSON_AddStringToObject(sc, "luid", luid_label(r->gpu_scanout_luid, lb, sizeof(lb)));
			}

			cJSON *rd = cJSON_AddObjectToObject(g, "render");
			cJSON_AddBoolToObject(rd, "resolved", r->gpu_render_resolved);
			if (r->gpu_render_resolved) {
				cJSON_AddStringToObject(rd, "name", r->gpu_render_name);
				cJSON_AddStringToObject(rd, "luid", luid_label(r->gpu_render_luid, lb, sizeof(lb)));
			}

			// #1153 — the adapter clients must share (ADR-037 §7). Its
			// `provenance` already names the rule that decided ("most VRAM" /
			// "env-forced: scanout"), which is the shape every other setting
			// wants; surface it rather than re-deriving.
			cJSON *ig = cJSON_AddObjectToObject(g, "service_ingest");
			cJSON_AddBoolToObject(ig, "resolved", r->gpu_ingest_resolved);
			if (r->gpu_ingest_resolved) {
				cJSON_AddStringToObject(ig, "name", r->gpu_ingest_name);
				cJSON_AddStringToObject(ig, "luid", luid_label(r->gpu_ingest_luid, lb, sizeof(lb)));
			}
			cJSON_AddStringToObject(ig, "provenance", r->gpu_ingest_provenance);
			cJSON_AddStringToObject(ig, "line", r->gpu_service_ingest);

			// Derived from the topology AND the resolved kill switch, so it
			// lives here rather than under `performance`. `weave_on_scanout`
			// carries its own source: "env" means this process's environment
			// and says nothing about another process, while "user"/"machine"
			// are shared and do describe what other apps will see.
			cJSON *sp = cJSON_AddObjectToObject(g, "split");
			cJSON_AddBoolToObject(sp, "weave_on_scanout_set", r->gpu_weave_env_set);
			if (r->gpu_weave_env_set) {
				cJSON_AddStringToObject(sp, "weave_on_scanout", r->gpu_weave_env);
			} else {
				cJSON_AddNullToObject(sp, "weave_on_scanout");
			}
			cJSON_AddStringToObject(sp, "weave_on_scanout_source", r->gpu_weave_source);
			cJSON_AddStringToObject(sp, "ingress", r->gpu_split_ingress);
			cJSON_AddStringToObject(sp, "service_split", r->gpu_service_split);
		}
	}

	/*
	 * #1252 — the allow-listed performance settings, resolved through the same
	 * chain the runtime uses (env > per-user file > machine default). This is
	 * what the Control Panel's three controls read and write.
	 *
	 * Every row carries its `source`, and consumers must not drop it: a value
	 * sourced from `env` is a property of THIS process and says nothing about
	 * any other, while `user` / `machine` / `default` are machine-wide.
	 */
	{
		cJSON *pf = cJSON_AddObjectToObject(root, "performance");
		cJSON_AddStringToObject(pf, "note",
		                        "Resolved as a process starting now would resolve it: environment first, "
		                        "then the per-user file, then the machine default. A row whose source is "
		                        "\"env\" reflects THIS process's environment only.");
		cJSON_AddStringToObject(pf, "user_file", r->settings_user_file);
		if (r->settings_user_written[0] != '\0') {
			cJSON_AddStringToObject(pf, "user_written", r->settings_user_written);
		} else {
			cJSON_AddNullToObject(pf, "user_written");
		}

		cJSON *arr = cJSON_AddArrayToObject(pf, "levers");
		for (uint32_t i = 0; i < r->setting_count; i++) {
			const struct cli_setting_row *row = &r->settings[i];
			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "name", row->name);
			if (row->set) {
				cJSON_AddStringToObject(o, "value", row->value);
			} else {
				cJSON_AddNullToObject(o, "value");
			}
			cJSON_AddStringToObject(o, "source", row->source);
			cJSON_AddItemToArray(arr, o);
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

	// #1212 — the check that stops a sim fallback reporting PASS. See
	// cli_query_fill() for why plugin_ok alone cannot express this.
	// ABSENCE NEVER FAILS: ok stays true when nothing better-ranked was
	// rejected, so hardware-free boxes and CI are unaffected.
	c = &out[n++];
	c->name = "vendor_dp";
	c->ok = !r->plugin_ok || r->vendor_dp_ok;
	snprintf(c->detail, sizeof(c->detail), "%s",
	         r->vendor_dp_note[0] != '\0' ? r->vendor_dp_note : "not evaluated");

	c = &out[n++];
	c->name = "display_info";
	c->ok = r->display_info_ok;
	snprintf(c->detail, sizeof(c->detail), "%s",
	         r->display_info_ok ? "get_display_info returned valid struct" : "get_display_info missing/false");

	// #1201 — this check PROVES the pixel dimensions against the display
	// mode the adapter is scanning out; it does not merely print them. It
	// FAILS on a mismatch, which is how a DPI-awareness regression (or any
	// other divergence) gets caught instead of laundered into a green PASS.
	// Absence of an authoritative mode never fails.
	c = &out[n++];
	c->name = "display_dims";
	c->ok = r->dims_ok;
	if (r->display_info_ok) {
		const struct xrt_plugin_display_info *i = &r->display_info;
		snprintf(c->detail, sizeof(c->detail), "%.4fm x %.4fm, %s", (double)i->display_width_m,
		         (double)i->display_height_m,
		         r->dims_note[0] != '\0' ? r->dims_note : "dimensions not evaluated");
	} else {
		snprintf(c->detail, sizeof(c->detail), "%s", "not evaluated");
	}

	// #1201 — the tool's own entitlement to the numbers above. FAILS when
	// Windows says this process is not per-monitor aware; "n/a" elsewhere.
	c = &out[n++];
	c->name = "dpi_awareness";
	c->ok = r->dpi_aware_ok;
	snprintf(c->detail, sizeof(c->detail), "process is %s%s", or_q(r->dpi_awareness),
	         r->dpi_aware_ok ? "" : " — Win32/GDI geometry is DPI-virtualised, panel dims cannot be trusted");

	// #224 / ADR-027 P4 — zone-caps probe. ABSENCE NEVER FAILS: ok stays
	// true for legacy plug-ins / no factory / non-Windows; only a
	// present-but-malformed caps struct fails (BAD_ZONE_CAPS).
	c = &out[n++];
	c->name = "zone_caps";
	c->ok = !r->zone_caps_malformed;
	snprintf(c->detail, sizeof(c->detail), "%s",
	         r->zone_probe_note[0] != '\0' ? r->zone_probe_note : "not evaluated");

	// ADR-034 / #823 — input-provider check. ABSENCE NEVER FAILS: ok
	// stays true with no provider registered, ForceQwerty set, every
	// provider declining, or the active provider's hardware unplugged.
	// A provider that IS present must yield left+right role devices —
	// and (#825 Tier 2) hand-tracking roles wherever a role device
	// advertises hand tracking.
	// #918 — GPU topology. INFORMATIONAL ONLY: this check is always ok, it
	// exists so `selftest` output carries the one verdict line that says
	// whether this box pays a cross-adapter present to reach the panel.
	c = &out[n++];
	c->name = "gpu_topology";
	c->ok = true;
	snprintf(c->detail, sizeof(c->detail), "%s", r->gpu_verdict[0] != '\0' ? r->gpu_verdict : "not evaluated");

	// #1234 - VK late-weave repaint reachability. INFORMATIONAL ONLY, for the
	// same reason gpu_topology is: the tier also depends on whether the driver
	// hands out a dedicated queue, which is decided at compositor create on a
	// real device and is invisible from here. A from-source build and CI's own
	// selftest gate both legitimately lack the registration, so failing on it
	// would break the very gate this check exists to sharpen.
	c = &out[n++];
	c->name = "vk_repaint_tier";
	c->ok = true;
	snprintf(c->detail, sizeof(c->detail), "%s", r->vk_layer_note[0] != '\0' ? r->vk_layer_note : "not evaluated");

	// The "can apps even reach us?" check. ABSENCE NEVER FAILS: an unset key
	// (CI, from-source dev boxes) is a PASS. FAILS only when the key is set
	// and points at a different runtime — the one condition under which every
	// other check here can be green while no app on the box loads DisplayXR.
	c = &out[n++];
	c->name = "active_runtime";
	c->ok = r->active_runtime_ok;
	snprintf(c->detail, sizeof(c->detail), "%s",
	         r->active_runtime_note[0] != '\0' ? r->active_runtime_note : "not evaluated");

	c = &out[n++];
	c->name = "input_providers";
	c->ok = !r->input_evaluated || (r->input_provider_active && r->input_left_ok && r->input_right_ok &&
	                                (!r->input_ht_expected_left || r->input_ht_left_ok) &&
	                                (!r->input_ht_expected_right || r->input_ht_right_ok));
	snprintf(c->detail, sizeof(c->detail), "%s", r->input_note[0] != '\0' ? r->input_note : "not evaluated");

	return n;
}

void
cli_query_print_selftest_text(const struct cli_query_result *r)
{
	P(" :: DisplayXR CLI self-test (headless, no compositor)\n");

	struct check checks[16];
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

	struct check checks[16];
	int n = build_checks(r, checks);
	cJSON *arr = cJSON_AddArrayToObject(root, "checks");
	for (int i = 0; i < n; i++) {
		cJSON *c = cJSON_CreateObject();
		cJSON_AddStringToObject(c, "name", checks[i].name);
		cJSON_AddBoolToObject(c, "ok", checks[i].ok);
		cJSON_AddStringToObject(c, "detail", checks[i].detail);
		cJSON_AddItemToArray(arr, c);
	}

	cJSON_AddStringToObject(root, "dpi_awareness", r->dpi_awareness);
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
