// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli catalog <list|show|install|uninstall|update|outdated>`:
 *         the DisplayXR Catalog from the command line (ADR-064 step 4).
 *
 * A front end over `dxr-get`: cli_catalog.c holds the verbs, cli_catalog_os.c
 * finds and runs the engine. This file only joins the two to the CLI, and
 * supplies the one thing the engine does not know: the licence state of an
 * installed plug-in, read the way `<class> list` reads it.
 *
 * `catalog` is distribution, not selection: enabling, preferring and
 * configuring a component stay with the per-class verbs (ADR-054 §2.6).
 *
 * @ingroup targets_cli
 */

#include "cli_common.h"
#include "cli_catalog.h"
#include "cli_component_options.h"

#include "xrt/xrt_component_license.h"

#include "target_plugin_loader.h"
#include "target_input_plugin_loader.h"
#include "target_tracking_plugin_loader.h"
#include "target_conv_status.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>


/*!
 * The licence of an installed plug-in, by its catalog class and the id it
 * registers under. The same call each class's `list` makes (negotiate only),
 * so the `licence` object here is the one `<class> list --json` prints.
 */
static cJSON *
catalog_licence(void *ud, const char *item_class, const char *registered_id, char *cell, size_t cap)
{
	(void)ud;
	struct xrt_component_license_state lic;
	bool found = false;
	if (item_class == NULL || registered_id == NULL) {
		return NULL;
	}
	if (strcmp(item_class, "display_processor") == 0) {
		found = target_plugin_license_state(registered_id, &lic);
	} else if (strcmp(item_class, "input_provider") == 0) {
		found = target_input_plugin_license_state(registered_id, &lic);
	} else if (strcmp(item_class, "tracking_provider") == 0) {
		found = target_tracking_plugin_license_state(registered_id, &lic);
	} else if (strcmp(item_class, "conversion_provider") == 0) {
		found = target_conv_license_state(registered_id, &lic);
	}
	if (!found) {
		return NULL;
	}
	(void)cli_component_license_cell(&lic, cell, cap);
	cJSON *row = cJSON_CreateObject();
	cli_component_license_json(row, &lic);
	cJSON *obj = cJSON_DetachItemFromObjectCaseSensitive(row, "licence");
	cJSON_Delete(row);
	return obj;
}

int
cli_cmd_catalog(int argc, const char **argv)
{
	char engine[1024];
	char why[512];
	const bool have = cli_catalog_os_locate_engine(engine, sizeof(engine), why, sizeof(why));
	const struct cli_catalog_ops ops = {
	    .engine = have ? engine : NULL,
	    .engine_missing = have ? NULL : why,
	    .run = cli_catalog_os_run,
	    .run_elevated = cli_catalog_os_run_elevated,
	    .is_interactive = cli_catalog_os_is_interactive,
	    .signer = cli_catalog_os_signer,
	    .log_open = cli_catalog_os_log_open,
	    .log_take = cli_catalog_os_log_take,
	    .absolute_path = cli_catalog_os_absolute_path,
	    .licence = catalog_licence,
	    .out = stdout,
	    .err = stderr,
	};
	if (argc < 3) {
		cli_catalog_print_usage(stderr);
		return CLI_CATALOG_EXIT_USAGE;
	}
	// Windows: the arguments as UTF-8, from the UTF-16 command line. `argv` there
	// is in the ANSI code page, and the engine is handed UTF-8.
	const char **utf8 = cli_catalog_os_utf8_args(argc);
	return cli_catalog_main(&ops, argc, utf8 != NULL ? utf8 : argv);
}
