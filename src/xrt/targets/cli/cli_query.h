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

#include <stdint.h>
#include <stdbool.h>


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

	/* Vendor-neutral display info (valid iff display_info_ok). */
	struct xrt_plugin_display_info display_info;
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

/* Serializers — info dump (all fields) and self-test (per-check verdict). */

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
