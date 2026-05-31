// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Headless self-test: discover a display processor and validate it.
 *
 * Creates an instance and system devices with NO compositor (so it runs
 * without a GPU, window, or display) and asserts that:
 *   1. system creation succeeded and produced a head/display device;
 *   2. a vendor plug-in is active — the loader rejects ABI-mismatched
 *      plug-ins (ADR-020 rule 3), so an active iface implies the plug-in
 *      ABI matches @ref XRT_PLUGIN_API_VERSION_CURRENT;
 *   3. the plug-in reports sane physical + pixel display dimensions.
 *
 * Thin wrapper over the shared query core (@ref cli_query.h). The exit code
 * is the contract: 0 = pass, non-zero = a specific failure (see
 * @ref cli_selftest_result), so CI can gate on it. `--json` emits the
 * per-check results alongside the same strict exit code.
 *
 * @author David Fattal
 */

#include "cli_common.h"
#include "cli_query.h"


int
cli_cmd_selftest(int argc, const char **argv)
{
	struct cli_query_result r;
	cli_query_run(&r);

	if (cli_has_flag(argc, argv, "--json")) {
		cli_query_print_selftest_json(&r);
	} else {
		cli_query_print_selftest_text(&r);
	}
	return (int)r.result_code;
}
