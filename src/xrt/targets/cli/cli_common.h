// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common file for the CLI program.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 */

#pragma once

#include <stdbool.h>
#include <string.h>


#ifdef __cplusplus
extern "C" {
#endif


int
cli_cmd_info(int argc, const char **argv);

int
cli_cmd_selftest(int argc, const char **argv);

int
cli_cmd_dp(int argc, const char **argv);

//! `perf` - read/write the persisted performance settings (#1252).
int
cli_cmd_perf(int argc, const char **argv);

int
cli_cmd_input(int argc, const char **argv);

int
cli_cmd_runtime(int argc, const char **argv);

int
cli_cmd_displays(int argc, const char **argv);

int
cli_cmd_probe(int argc, const char **argv);

//! `status [--json] [--watch [--interval ms]]` — the display status snapshot (ADR-051).
int
cli_cmd_status(int argc, const char **argv);

struct xrt_status_snapshot;

/*!
 * Build the HEADLESS status snapshot in this process (an instance + system,
 * no compositor — what a process starting now would get). Shared by `status`
 * and `dp list`'s `screens[]`.
 */
void
cli_status_build_headless(struct xrt_status_snapshot *out);

int
cli_cmd_clients(int argc, const char **argv);

//! `conv list|status|warm|enable|disable|use|reset|models` — conversion providers (ADR-053).
int
cli_cmd_conv(int argc, const char **argv);

struct cli_query_result;

/*!
 * `lift_caps` from the RUNNING service (ADR-053 §7.1: the CLI never loads a
 * second model instance): one DIAG `lift_get_properties` when a service is
 * listening (never launched), recorded with @ref cli_query_set_lift_caps.
 * No service, or a build without IPC: @p r is left as the headless pass
 * filled it (the static dp-lift row note).
 *
 * TODO(#2003): once the service publishes its `dp-lift` conversion row, read
 * that row from the snapshot instead (same `lift_caps` JSON shape).
 */
void
cli_query_lift_caps_from_service(struct cli_query_result *r);

//! `workspace list|set|reset|launch` — workspace controllers + launch settings (dashboard phase 8).
int
cli_cmd_workspace(int argc, const char **argv);

int
cli_cmd_test(int argc, const char **argv);

//! `lift` — XR_DXR_lift (ADR-042): `lift caps`, `lift probe <image|dir>`.
int
cli_cmd_lift(int argc, const char **argv);

//! `camera` — XR_DXR_stereo_camera (ADR-043): `camera list|calib|probe`, over IPC (DIAG).
int
cli_cmd_camera(int argc, const char **argv);

/*!
 * True if @p flag appears anywhere in argv[2..]. Commands take raw
 * argc/argv; this is the one shared option-scan (e.g. `--json`).
 */
static inline bool
cli_has_flag(int argc, const char **argv, const char *flag)
{
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], flag) == 0) {
			return true;
		}
	}
	return false;
}


#ifdef __cplusplus
}
#endif
