// Copyright 2019-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A cli program to configure and test Monado.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 */

#include "cli_common.h"

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_WINDOWS
#include "util/u_windows.h"
#endif

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
	P("  perf <...>        - Performance settings the runtime reads inside each app.\n");
	P("                      'perf list [--json]', 'perf set <name> <value>', 'perf reset'.\n");
	P("  dp <...>          - List display processors / set the PreferredPlugin override.\n");
	P("                      'dp list [--json]', 'dp use <id>', 'dp reset'; per screen:\n");
	P("                      'dp use <id> --screen <key>', 'dp reset --screen <key>|all'.\n");
	P("  input <...>       - Input providers (motion controllers, ADR-034).\n");
	P("                      'input list [--json]', 'input haptic-test [seconds]'.\n");
	P("  runtime <...>     - Inspect / switch the active OpenXR runtime.\n");
	P("                      'runtime status'   full loader precedence + conflicts,\n");
	P("                      'runtime list'     every runtime on the box (incl. unregistered),\n");
	P("                      'runtime activate [<manifest>]', 'runtime restore'.\n");
	P("  displays [--json] - Enumerate connected displays via EDID (vendor-neutral).\n");
	P("           [--claims] - Also show which plug-in claims each display (loads plug-ins).\n");
	P("  status [--json]   - Display status snapshot (ADR-051): one row per screen with its claim,\n");
	P("                      metres, roles, tracking and warnings, plus the live windows. From the\n");
	P("                      service when reachable (DIAG IPC; non-elevated on Windows), else\n");
	P("                      headless (labelled 'source: headless').\n");
	P("         --watch [--interval ms]  stay connected; poll the service's generation counters\n");
	P("                      every <ms> (default 500, min 100) and print only on a change: one\n");
	P("                      NDJSON line per change with --json, else the table redrawn in place.\n");
	P("                      Ends on Ctrl-C or stdin EOF.\n");
	P("  workspace <...>   - Workspace controllers (role, registration) and their launch settings.\n");
	P("                      'workspace list [--json]', 'workspace set <id> --hotkey <combo> |\n");
	P("                      --no-hotkey | --mode auto|disabled', 'workspace reset <id>',\n");
	P("                      'workspace launch <id>', 'workspace hotkey-suspend on|off'\n");
	P("                      (the last two through the running service).\n");
	P("  clients [--json]  - List the running service's IPC clients with their verified class\n");
	P("                      (#960), presenter, panel lease, window rect and owner screen.\n");
	P("                      Connects over IPC as a DIAG client; non-elevated on Windows.\n");
	P("  lift <...>        - 2D->3D conversion module (XR_DXR_lift, ADR-042), over IPC (DIAG).\n");
	P("                      'lift caps [--json]', 'lift probe <image|frames_dir> [--mode depth|sbs|\n");
	P("                      nview|gaussians] [--n N]' (Windows) — writes lift_out_<i>.png/.ply.\n");
	P("  camera <...>      - Stereo camera sources (XR_DXR_stereo_camera, ADR-043), over IPC (DIAG).\n");
	P("                      'camera list [--json]', 'camera calib <id> [--raw|--rectified]',\n");
	P("                      'camera probe [<id>] [--raw] [--format gray8|nv12|bgra8] [--fps F]\n");
	P("                      [--frames N] [--seconds S] [--out DIR]' — rate, layout, disparity, PNG.\n");
	P("  test              - List found devices and role assignments, for prober testing.\n");
	P("  probe             - Just probe and then exit.\n");

	return 1;
}

int
main(int argc, const char **argv)
{
#ifdef XRT_OS_WINDOWS
	// #1201 — FIRST, before anything touches GDI. A DPI-unaware process is
	// handed virtualised coordinates, so on a 4K panel at 150% scaling this
	// tool would report (and `selftest` would ASSERT) a 2560x1440 display
	// while every DPI-aware app on the same box reads 3840x2160. The embedded
	// manifest normally has this in force already; the call is the backstop.
	u_win_make_process_dpi_aware(NULL);
#endif

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
	if (strcmp(argv[1], "perf") == 0) {
		return cli_cmd_perf(argc, argv);
	}
	if (strcmp(argv[1], "input") == 0) {
		return cli_cmd_input(argc, argv);
	}
	if (strcmp(argv[1], "runtime") == 0) {
		return cli_cmd_runtime(argc, argv);
	}
	if (strcmp(argv[1], "displays") == 0) {
		return cli_cmd_displays(argc, argv);
	}
	if (strcmp(argv[1], "status") == 0) {
		return cli_cmd_status(argc, argv);
	}
	if (strcmp(argv[1], "clients") == 0) {
		return cli_cmd_clients(argc, argv);
	}
	if (strcmp(argv[1], "workspace") == 0) {
		return cli_cmd_workspace(argc, argv);
	}
	if (strcmp(argv[1], "test") == 0) {
		return cli_cmd_test(argc, argv);
	}
	if (strcmp(argv[1], "probe") == 0) {
		return cli_cmd_probe(argc, argv);
	}
	if (strcmp(argv[1], "lift") == 0) {
		return cli_cmd_lift(argc, argv);
	}
	if (strcmp(argv[1], "camera") == 0) {
		return cli_cmd_camera(argc, argv);
	}
	return cli_print_help(argc, argv);
}
