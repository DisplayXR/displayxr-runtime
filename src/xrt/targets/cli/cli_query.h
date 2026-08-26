// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared runtime/plug-in/display query core for the CLI.
 *
 * One headless discovery pass that fills a self-contained snapshot, plus
 * text + JSON serializers over it. `info` and `selftest` are thin wrappers:
 * they run the query and pick a serializer; the boilerplate that creates an
 * instance, runs vendor plug-in discovery with no compositor, and reads the
 * vendor-neutral display info lives here once. The Control Panel (issue
 * #378) and the session-free MCP tools consume the same `--json` shape.
 *
 * @author David Fattal
 */

#pragma once

#include "cli_dims_check.h"

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_display_zones.h"

#include <stdint.h>
#include <stdbool.h>

typedef struct cJSON cJSON;

struct xrt_instance;
struct xrt_instance_info;
struct xrt_system;
struct xrt_system_devices;
struct xrt_space_overseer;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Self-test verdict. The integer value is the CLI's exit-code contract for
 * `selftest`, so CI can gate on it: 0 = pass, non-zero = a specific failure.
 */
enum cli_selftest_result
{
	CLI_SELFTEST_PASS = 0,      //!< All checks passed.
	CLI_SELFTEST_INIT_FAIL = 1, //!< Instance / system creation failed.
	CLI_SELFTEST_NO_DP = 2,     //!< No display processor / plug-in discovered.
	CLI_SELFTEST_BAD_INFO = 3,  //!< Plug-in reported invalid display info.
	//! Plug-in's get_local_zone_caps returned a MALFORMED struct (#224 /
	//! ADR-027 P4 probe). NOTE: caps *absence* (legacy plug-in, no D3D11
	//! factory, probe device failure, non-Windows) never fails — only a
	//! present-but-invalid answer does.
	CLI_SELFTEST_BAD_ZONE_CAPS = 4,
	//! An input provider is registered (and not ForceQwerty-overridden)
	//! but did not yield left+right motion-controller role devices with a
	//! valid interaction profile (ADR-034 / #823). NOTE: provider
	//! *absence* never fails — qwerty keeping the hand roles is the
	//! normal no-provider configuration.
	CLI_SELFTEST_BAD_INPUT = 5,
	//! The plug-in's reported panel pixels do NOT match the authoritative
	//! display mode (#1201). The classic cause is a DPI-unaware process
	//! reading virtualised coordinates, but any divergence fails: the whole
	//! point of the check is that `display_dims` must PROVE its number
	//! rather than print it. Absence of an authoritative mode (non-Windows,
	//! monitor unresolvable) never fails.
	CLI_SELFTEST_DIMS_MISMATCH = 6,
	//! This process is not per-monitor DPI aware, so every pixel dimension
	//! it reports came through DPI virtualisation (#1201). Caught even on a
	//! 100%-scaled box, where the numbers happen to come out right and the
	//! dims check above therefore cannot see the regression. Windows-only.
	CLI_SELFTEST_NOT_DPI_AWARE = 7,
	//! A better-ranked plug-in was present at discovery and failed to load,
	//! so the runtime silently fell back to a worse one — in practice the
	//! vendor-neutral sim_display (#1212). Absence of a better-ranked
	//! candidate never fails, so hardware-free dev boxes and CI stay green;
	//! what fails is the case where a vendor plug-in WAS there and got
	//! rejected (the classic being an ABI-rotted hand-built plug-in), which
	//! previously reported PASS over a black screen.
	CLI_SELFTEST_VENDOR_DP_REJECTED = 8,
};

//! Hardware adapters reported by the GPU-topology probe (#918).
#define CLI_MAX_GPU_ADAPTERS 8

/*!
 * One hardware DXGI adapter, as reported by the #918 GPU-topology probe.
 * Software adapters (WARP / Basic Render Driver) are skipped.
 */
struct cli_gpu_adapter
{
	char name[128];
	//! Packed exactly as Vulkan reports `VkPhysicalDeviceIDProperties::deviceLUID`
	//! (raw LUID bytes, LowPart first) — the same packing
	//! @ref d3d_scanout_adapter_luid returns.
	uint64_t luid;
	uint64_t dedicated_vram_bytes;
};

/*!
 * A fully self-contained snapshot of runtime / plug-in / display state.
 * All pointers from the runtime are copied into fixed buffers and the
 * instance is destroyed before @ref cli_query_run returns, so the result
 * stays valid with no dangling iface pointers and no live runtime objects.
 */
struct cli_query_result
{
	/* Runtime. */
	char runtime_description[256];
	char git_tag[128];
	uint32_t plugin_abi_version; //!< XRT_PLUGIN_API_VERSION_CURRENT.

	/* Windows `ActiveRuntime`. `queried` is false on non-Windows. */
	bool active_runtime_queried;
	bool active_runtime_set;
	char active_runtime[1024];

	/* Per-stage outcomes (mirror the self-test checks). */
	bool instance_ok;
	bool system_ok;
	bool head_ok;
	bool plugin_ok;
	bool display_info_ok;
	bool dims_ok;
	enum cli_selftest_result result_code;

	/* Active vendor plug-in identity (valid iff plugin_ok). */
	char plugin_id[64];
	char plugin_name[128];
	char plugin_vendor[64];
	char plugin_version[64];

	/* Discovery outcome (#1212). `vendor_dp_ok` is false only when a
	 * BETTER-RANKED plug-in than the active one was attempted and failed —
	 * i.e. the runtime fell back. Absence of such a candidate is fine. */
	bool vendor_dp_ok;
	char vendor_dp_note[256];

	/* Head/display device description (valid iff head_ok). */
	char head_str[256];

	/* Rendering-mode snapshot incl. per-mode tracking flags (#441;
	 * valid iff head_ok — count 0 if the device exposes none). */
	uint32_t rendering_mode_count;
	struct xrt_rendering_mode rendering_modes[XRT_MAX_RENDERING_MODES];

	/* Vendor-neutral display info (valid iff display_info_ok). */
	struct xrt_plugin_display_info display_info;

	/* #1201 — the authoritative panel mode, so `display_dims` can PROVE the
	 * plug-in's `display_pixel_*` instead of printing it. Windows-only:
	 * `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)` on the monitor the
	 * plug-in points at (`display_screen_left/top`, else the primary),
	 * which returns TRUE pixels regardless of this process's DPI awareness
	 * — that is exactly what makes it a usable oracle for a bug whose whole
	 * signature is DPI-virtualised coordinates.
	 *
	 * native_probed — a mode was read (false ⟹ non-Windows, or the monitor
	 *                 could not be resolved; NEVER a failure).
	 * dims_verdict  — the comparison (@ref cli_dims_compare); MISMATCH is
	 *                 the only value that fails, as DIMS_MISMATCH.
	 * dims_note     — human-readable outcome, reused by both serializers. */
	bool native_probed;
	uint32_t native_pixel_width;
	uint32_t native_pixel_height;
	uint32_t native_refresh_hz;
	char native_device[40]; //!< GDI device name, e.g. DISPLAY1.
	enum cli_dims_verdict dims_verdict;
	char dims_note[224];

	/* #1201 — this process's own DPI awareness, so the tool's output says
	 * whether it is entitled to be believed. Windows-only ("n/a" elsewhere).
	 * A tool that reports the panel MUST report this too: it is the single
	 * fact that separates "the panel is 2560x1440" from "I am not allowed to
	 * see that the panel is 3840x2160". */
	char dpi_awareness[32];
	//! False only on Windows, and only when the process is NOT per-monitor
	//! aware. Non-Windows leaves it true — awareness is a Windows concept.
	bool dpi_aware_ok;

	/* #224 / ADR-027 P4 zone-caps probe (Windows-only: WARP D3D11 device +
	 * the plug-in's create_dp_d3d11 + get_local_zone_caps, headless).
	 * probed   — the DP answered the caps query (false ⟹ legacy plug-in /
	 *            no D3D11 factory / probe device failure / non-Windows;
	 *            never a failure).
	 * malformed — the answer violated the contract (supported > 1, a
	 *            supported DP with a zero grid, wish_fractional > 1,
	 *            switch_granularity out of range) ⟹ BAD_ZONE_CAPS.
	 * note     — human-readable probe outcome for the serializers. */
	bool zone_caps_probed;
	bool zone_caps_malformed;
	char zone_probe_note[128];
	struct xrt_dp_local_zone_caps zone_caps;

	/* Input-provider checks (ADR-034 / #823). Absence never fails: with
	 * no provider registered — or the ForceQwerty override set, or the
	 * provider's hardware simply not plugged in — the fields stay "not
	 * evaluated" and the verdict is untouched. Only a registered,
	 * non-overridden provider whose hardware IS present and which then
	 * fails to produce left+right motion-controller role devices (valid
	 * `xrt_device_name` interaction profile) flips the verdict to
	 * BAD_INPUT.
	 *
	 * "Provider registered but its hardware is absent" is the normal
	 * state of any box whose tracker is unplugged, and it is a PASS: the
	 * arbiter hands the hand roles to qwerty, which is exactly right. */
	int input_provider_count;    //!< registered providers (enumerated, unloaded)
	bool input_force_qwerty;     //!< ForceQwerty override set
	bool input_provider_active;  //!< a provider claimed the system
	bool input_provider_present; //!< …and its hardware is there right now
	bool input_evaluated;        //!< checks ran (provider present, no override)
	bool input_left_ok;
	bool input_right_ok;
	char input_provider_id[64];
	char input_left_str[256];
	char input_right_str[256];
	char input_note[160];

	/* Hand-tracking role checks (#825 Tier 2), same absence-never-fails
	 * rule: `expected` = a provider role device advertises
	 * `supported.hand_tracking`; only expected-but-unfilled static
	 * hand-tracking roles flip the verdict to BAD_INPUT. A provider whose
	 * devices carry no hand-tracking inputs (net_input feeder) leaves all
	 * four fields false and passes. */
	bool input_ht_expected_left;
	bool input_ht_expected_right;
	bool input_ht_left_ok;
	bool input_ht_right_ok;

	/* DP-factory selection divergence probe. Two render paths choose the
	 * display processor differently: the in-process handle/texture path reads
	 * the scalar dp_factory (== the active plug-in, `plugin_id` above), while
	 * the D3D11 service / shell path reads the per-monitor DP registry's
	 * PRIMARY entry (`comp_dp_factory_for_window` with COMP_DP_PRIMARY_MONITOR
	 * → entries[0]). On a single display they MUST agree; a mismatch means
	 * standalone apps and the shell weave with different DPs — e.g. Leia
	 * in-process but sim_display in the shell, which silently drops shell
	 * head-tracking. Reproduced headlessly here: the CLI runs in-process, so
	 * it computes BOTH selections with no service running.
	 * probed   — the probe ran (registry resolution executed).
	 * mismatch — in-process plug-in id != service (registry primary) plug-in id. */
	bool dp_sel_probed;
	bool dp_sel_mismatch;
	uint32_t dp_sel_monitor_count; //!< EDID monitors enumerated.
	uint32_t dp_sel_claim_count;   //!< registry entries (monitors a plug-in claimed).
	char dp_sel_inproc_id[64];     //!< in-process path plug-in id (mirrors plugin_id).
	char dp_sel_service_id[64];    //!< service path plug-in id (registry primary; == in-proc on empty registry).
	char dp_sel_service_conf[24];  //!< service claim confidence label (FALLBACK/EDID/VERIFIED/scalar-fallback).

	/* #918 GPU topology — does this box pay a cross-adapter present to get
	 * the woven frame onto the panel? Windows-only: `gpu_probed` stays false
	 * everywhere else and the serializers print nothing but an "n/a" line.
	 *
	 * scanout — the adapter that owns the output the panel is scanned out
	 *           on (`d3d_scanout_adapter_luid` over the plug-in's panel
	 *           rect). Unresolvable is reported as such, never guessed.
	 * render  — the adapter the runtime would suggest by default, i.e. the
	 *           hardware adapter with the MOST dedicated VRAM (mirrors the
	 *           dGPU classification in oxr_d3d.cpp; deliberately not
	 *           EnumAdapterByGpuPreference, which a per-app
	 *           UserGpuPreferences entry can reorder).
	 * applies — render != scanout, i.e. the session pays the cross-adapter
	 *           present unless the split moves the weave. Since #918 Phase 3
	 *           the split is ON by default, so `applies` is now "the split has
	 *           something to do here", not "the flag would have something to
	 *           move if it were set". */
	bool gpu_probed;
	uint32_t gpu_adapter_count;
	struct cli_gpu_adapter gpu_adapters[CLI_MAX_GPU_ADAPTERS];
	bool gpu_scanout_resolved;
	uint64_t gpu_scanout_luid;
	char gpu_scanout_name[128];
	bool gpu_render_resolved;
	uint64_t gpu_render_luid;
	char gpu_render_name[128];
	bool gpu_split_applies;
	char gpu_verdict[192]; //!< the one-line verdict, reused verbatim by selftest.
	bool gpu_weave_env_set;
	//! DXR_WEAVE_ON_SCANOUT value when set. Since #918 Phase 3 this is a KILL
	//! SWITCH, not an opt-in — unset means the split is allowed.
	char gpu_weave_env[64];
	/*!
	 * #918 Phase 2b — the SERVICE split, as far as a headless tool can honestly
	 * answer it: this process is not the service, holds no IPC connection, and
	 * must not pretend to know what the running service decided. What it does
	 * know is the two inputs — the topology verdict above and the flag — and
	 * that the service's Stage A takes exactly those. So this is a WOULD-ENGAGE
	 * statement, and it says so.
	 */
	char gpu_service_split[160];
	//! DXR_SPLIT_INGRESS as set, or the default the service would pick.
	char gpu_split_ingress[32];
	/*!
	 * ADR-037 §7 / #1153 — the adapter the service creates its INGEST device
	 * on, i.e. the adapter clients' shared textures must land on. Resolved by
	 * the same `d3d_render_adapter` unit the service itself calls, so
	 * `DXR_D3D_FORCE_GPU` shows up here exactly as the service would honour it
	 * (which is the point: the override arm has to be checkable before the
	 * service is started). Distinct from `gpu_render_*` above, which is the
	 * unforced "most dedicated VRAM" default suggestion.
	 */
	bool gpu_ingest_resolved;
	uint64_t gpu_ingest_luid;
	char gpu_ingest_name[128];
	char gpu_ingest_provenance[64]; //!< which rule decided ("most VRAM", "env-forced: scanout", …).
	char gpu_service_ingest[224];   //!< the composed one-liner both serializers print.
	char gpu_note[128]; //!< why the probe could not answer, when it could not.
};

/*!
 * Live runtime objects created by @ref cli_query_fill, kept so the caller
 * decides when (or whether) to tear them down. The Android diag bridge
 * skips teardown entirely — its short-lived process exits instead, because
 * vendor plug-in destroy can hang (displayxr-leia-plugin#39).
 */
struct cli_query_handles
{
	struct xrt_instance *xi;
	struct xrt_system *xsys;
	struct xrt_system_devices *xsysd;
	struct xrt_space_overseer *xso;
};

/*!
 * Run the headless discovery pass and fill @p out. Creates an instance and
 * system devices with NO compositor, runs the real plug-in discovery path,
 * reads display info, then tears everything down. Emits nothing to stdout
 * (so `--json` output stays clean). Always safe to call; failures are
 * recorded in the per-stage booleans and @ref cli_query_result::result_code.
 */
void
cli_query_run(struct cli_query_result *out);

/*!
 * As @ref cli_query_run but leaves the runtime objects alive in @p h; pair
 * with @ref cli_query_teardown. The snapshot in @p out is self-contained
 * either way (no pointers into the live objects). @p ii is optional (NULL
 * on desktop); Android callers must pass one carrying vm + context —
 * android_instance_base_init dereferences it.
 */
void
cli_query_fill(struct cli_query_result *out, struct cli_query_handles *h, const struct xrt_instance_info *ii);

void
cli_query_teardown(struct cli_query_handles *h);

/* Serializers — info dump (all fields) and self-test (per-check verdict). */

/* cJSON tree builders (caller owns the returned object); the print_*_json
 * functions are thin printf wrappers over these. */
cJSON *
cli_query_info_to_cjson(const struct cli_query_result *r);

cJSON *
cli_query_selftest_to_cjson(const struct cli_query_result *r);

void
cli_query_print_info_text(const struct cli_query_result *r);

void
cli_query_print_info_json(const struct cli_query_result *r);

void
cli_query_print_selftest_text(const struct cli_query_result *r);

void
cli_query_print_selftest_json(const struct cli_query_result *r);

#ifdef __cplusplus
}
#endif
