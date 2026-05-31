// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Prints DisplayXR runtime, plug-in, and display info.
 *
 * A one-command diagnostic dump for bug reports: runtime version, which
 * vendor plug-in (display processor) the discovery path selected and its
 * ABI, the physical/pixel display dimensions the plug-in reports, and —
 * on Windows — whether DisplayXR is the registered active OpenXR runtime.
 *
 * Thin wrapper over the shared query core (@ref cli_query.h); `--json`
 * emits the same data as machine-readable JSON.
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author David Fattal
 */

#include "cli_common.h"
#include "cli_query.h"


int
cli_cmd_info(int argc, const char **argv)
{
	struct cli_query_result r;
	cli_query_run(&r);

	if (cli_has_flag(argc, argv, "--json")) {
		cli_query_print_info_json(&r);
	} else {
		cli_query_print_info_text(&r);
	}
	return 0;
}
