// Copyright 2026, Leia Inc.
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

	/* Head/display device description (valid iff head_ok). */
	char head_str[256];

	/* Rendering-mode snapshot incl. per-mode tracking flags (#441;
	 * valid iff head_ok — count 0 if the device exposes none). */
	uint32_t rendering_mode_count;
	struct xrt_rendering_mode rendering_modes[XRT_MAX_RENDERING_MODES];

	/* Vendor-neutral display info (valid iff display_info_ok). */
	struct xrt_plugin_display_info display_info;

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
