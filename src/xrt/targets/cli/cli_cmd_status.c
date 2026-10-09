// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `status [--json]` — the display status snapshot (ADR-051 D4).
 *
 * The one per-screen view the CLI, the Control Panel and agents share: the
 * snapshot from the running service when one is reachable, else the headless
 * one this process builds (labelled `source: headless` — "what a process
 * starting now would get", not what running apps see). Text is design §7,
 * JSON is design §3 (`docs/roadmap/display-dashboard.md`).
 *
 * @author David Fattal
 */

#include "cli_common.h"
#include "cli_query.h" // cli_query_handles + teardown

#include "xrt/xrt_display_status.h"
#include "xrt/xrt_instance.h"
#include "util/u_status_snapshot.h"
#include "target_status_snapshot.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*!
 * The live snapshot from the running service over the session-free DIAG path.
 * Phase 2 implements it (`system_get_status_snapshot` + per-client segments);
 * until then there is no such RPC, so it always reports "no service" and the
 * caller falls back to the headless build.
 */
static bool
status_fetch_service(struct xrt_status_snapshot *out)
{
	(void)out;
	return false;
}

//! Build the headless snapshot: an instance + system with NO compositor, as `info` does.
static void
status_build_headless(struct xrt_status_snapshot *out)
{
	struct cli_query_handles h;
	memset(&h, 0, sizeof(h));
	if (xrt_instance_create(NULL, &h.xi) == XRT_SUCCESS) {
		(void)xrt_instance_create_system(h.xi, &h.xsys, &h.xsysd, &h.xso, NULL);
	}
	target_status_snapshot_build_headless(h.xi, h.xsysd, out);
	cli_query_teardown(&h);
}

int
cli_cmd_status(int argc, const char **argv)
{
	// ~tens of KB: keep it off the stack.
	struct xrt_status_snapshot *snap = calloc(1, sizeof(*snap));
	if (snap == NULL) {
		fprintf(stderr, "status: out of memory\n");
		return 1;
	}
	if (!status_fetch_service(snap)) {
		status_build_headless(snap);
	}

	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *root = u_status_snapshot_to_cjson(snap);
		char *s = cJSON_Print(root);
		if (s != NULL) {
			printf("%s\n", s);
			cJSON_free(s);
		}
		cJSON_Delete(root);
	} else {
		const size_t need = u_status_snapshot_format_text(snap, NULL, 0);
		char *buf = malloc(need + 1);
		if (buf != NULL) {
			(void)u_status_snapshot_format_text(snap, buf, need + 1);
			fputs(buf, stdout);
			free(buf);
		}
	}
	free(snap);
	return 0;
}
