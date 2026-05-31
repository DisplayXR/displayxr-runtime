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

int
cli_cmd_probe(int argc, const char **argv);

int
cli_cmd_test(int argc, const char **argv);

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
