// Copyright 2019-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A cli program to configure and test Monado.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 */

#include "cli_common.h"

#include "xrt/xrt_config_os.h"

#include <string.h>
#include <stdio.h>


#define P(...) fprintf(stderr, __VA_ARGS__)

static int
cli_print_help(int argc, const char **argv)
{
	if (argc >= 2) {
		P("Unknown command '%s'\n\n", argv[1]);
	}

	P("DisplayXR-CLI 0.0.1\n");
	P("Usage: %s command [options] [file]\n", argv[0]);
	P("\n");
	P("Commands:\n");
	P("  info [--json]     - Print runtime, plug-in, and display info (for bug reporting).\n");
	P("  selftest [--json] - Headless self-test: discover a display processor and validate\n");
	P("                      its display info. Exits 0 on success, non-zero on failure.\n");
	P("  dp <...>          - List display processors / set the PreferredPlugin override.\n");
	P("                      'dp list [--json]', 'dp use <id>', 'dp reset'.\n");
	P("  runtime <...>     - Show / set DisplayXR as the active OpenXR runtime.\n");
	P("                      'runtime status', 'runtime activate'.\n");
	P("  test              - List found devices and role assignments, for prober testing.\n");
	P("  probe             - Just probe and then exit.\n");

	return 1;
}

int
main(int argc, const char **argv)
{
	if (argc <= 1) {
		return cli_print_help(argc, argv);
	}

	if (strcmp(argv[1], "info") == 0) {
		return cli_cmd_info(argc, argv);
	}
	if (strcmp(argv[1], "selftest") == 0) {
		return cli_cmd_selftest(argc, argv);
	}
	if (strcmp(argv[1], "dp") == 0) {
		return cli_cmd_dp(argc, argv);
	}
	if (strcmp(argv[1], "runtime") == 0) {
		return cli_cmd_runtime(argc, argv);
	}
	if (strcmp(argv[1], "test") == 0) {
		return cli_cmd_test(argc, argv);
	}
	if (strcmp(argv[1], "probe") == 0) {
		return cli_cmd_probe(argc, argv);
	}
	return cli_print_help(argc, argv);
}
