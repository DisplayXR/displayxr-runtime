// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2026, The DisplayXR Project
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

#include <cjson/cJSON.h>

#include <stdio.h>


int
cli_cmd_info(int argc, const char **argv)
{
	struct cli_query_result r;
	// Fill, PRINT, flush, then tear down (#1892): a plug-in worker thread can
	// end the process during teardown; printing afterwards lost the tail of
	// the JSON about one run in ten, with exit code 0.
	struct cli_query_handles h;
	cli_query_fill(&r, &h, NULL);
	// ADR-053 §7.1: live lift caps from the running service, never a second module.
	cli_query_lift_caps_from_service(&r);

	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *root = cli_query_info_to_cjson(&r);
		cli_camera_calibration_annotate_info(root);
		char *out = cJSON_Print(root);
		if (out != NULL) {
			printf("%s\n", out);
			fflush(stdout); // #1892: the tail must be out before any teardown
			cJSON_free(out);
		}
		cJSON_Delete(root);
	} else {
		cli_query_print_info_text(&r);
	}
	fflush(stdout);
	fflush(stderr);
	cli_query_teardown(&h);
	return 0;
}
