// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `input` subcommand — list registered input providers (ADR-034).
 *
 *   input list [--json]   Enumerate registered input-provider plug-ins
 *                         with ProbeOrder + which one the loader would
 *                         select. Mirror of `dp list` for the second
 *                         plug-in type; enumeration reads the discovery
 *                         root without loading any provider DLL.
 *
 * Issue #823.
 *
 * @author David Fattal
 */

#include "cli_common.h"
#include "cli_query.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"

#include "os/os_time.h"

#include "target_input_plugin_loader.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PROVIDERS 16

#define P(...) printf(__VA_ARGS__)


/*!
 * Predict which provider the loader would select: the lowest ProbeOrder
 * among enabled entries (first wins on ties). Returns the index into
 * @p list, or -1.
 */
static int
predict_active(const struct target_input_plugin_desc *list, int n)
{
	int best = -1;
	for (int i = 0; i < n; i++) {
		if (!list[i].enabled) {
			continue;
		}
		if (best < 0 || list[i].probe_order < list[best].probe_order) {
			best = i;
		}
	}
	return best;
}

static int
cmd_list(int argc, const char **argv)
{
	struct target_input_plugin_desc list[MAX_PROVIDERS];
	int n = target_input_plugin_enumerate(list, MAX_PROVIDERS);

	bool force_qwerty = target_input_plugin_get_force_qwerty();
	int active = predict_active(list, n);

	if (cli_has_flag(argc, argv, "--json")) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddBoolToObject(root, "force_qwerty", force_qwerty);
		cJSON *arr = cJSON_AddArrayToObject(root, "providers");
		for (int i = 0; i < n; i++) {
			cJSON *p = cJSON_CreateObject();
			cJSON_AddStringToObject(p, "id", list[i].id);
			cJSON_AddStringToObject(p, "display_name", list[i].display_name);
			cJSON_AddStringToObject(p, "vendor", list[i].vendor);
			cJSON_AddStringToObject(p, "version", list[i].version);
			cJSON_AddNumberToObject(p, "probe_order", (double)list[i].probe_order);
			cJSON_AddStringToObject(p, "binary_path", list[i].binary_path);
			cJSON_AddBoolToObject(p, "enabled", list[i].enabled);
			cJSON_AddBoolToObject(p, "active", i == active && !force_qwerty);
			cJSON_AddItemToArray(arr, p);
		}
		char *out = cJSON_Print(root);
		if (out != NULL) {
			printf("%s\n", out);
			cJSON_free(out);
		}
		cJSON_Delete(root);
		return 0;
	}

	P(" :: Registered input providers\n");
	if (n == 0) {
		P("\t(none — discovery root absent or empty; qwerty keeps the hand roles)\n");
		return 0;
	}
	if (force_qwerty) {
		P("\tForceQwerty override: SET — providers are skipped, qwerty claims the hand roles.\n");
	}
	for (int i = 0; i < n; i++) {
		P("\t%s id='%s' name='%s' ProbeOrder=%u%s\n", (i == active && !force_qwerty) ? "* " : "  ", list[i].id,
		  list[i].display_name[0] ? list[i].display_name : "?", list[i].probe_order,
		  list[i].enabled ? "" : " [disabled]");
		P("\t     %s\n", list[i].binary_path);
	}
	P("\t('*' = the provider the loader would select.)\n");
	return 0;
}

/*!
 * `input haptic-test [seconds]` — headless haptics driver for the
 * net_input round-trip (#823 Phase 2). Builds the runtime system
 * in-process (which hosts the active input provider, e.g. the net_input
 * hub), then fires a vibration on the left/right role devices twice a
 * second for the window. Pair with
 * `scripts/net_input_feeder.py --assert-haptic`: the feeder streams
 * poses in and must see the HAPTIC events come back. Exits 0 iff both
 * hand roles existed to fire at.
 */
static int
cmd_haptic_test(int argc, const char **argv)
{
	int seconds = 5;
	if (argc >= 4) {
		int v = atoi(argv[3]);
		if (v > 0 && v <= 300) {
			seconds = v;
		}
	}

	struct cli_query_result r;
	struct cli_query_handles h;
	cli_query_fill(&r, &h, NULL);
	if (!r.system_ok) {
		P("FAIL: system creation failed — cannot run haptic test.\n");
		cli_query_teardown(&h);
		return 1;
	}

	// Roles are re-resolved every iteration, not captured once: they are
	// arbitrated against provider presence (`target_input_arbiter.h`) and
	// for net_input "present" means "a feeder is connected" — which
	// normally happens AFTER the runtime is up, i.e. after this loop has
	// already started. Re-reading is also what a real app does (every
	// xrSyncActions), so this doubles as a live check of the flip.
	struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
	struct xrt_device *left = NULL;
	struct xrt_device *right = NULL;

	P("Firing haptics on the hand-role devices for %d s (2 Hz)...\n", seconds);

	struct xrt_output_value value = {0};
	uint32_t tracked_samples = 0;
	uint32_t fired = 0;
	uint64_t last_generation = 0;
	for (int i = 0; i < seconds * 2; i++) {
		xrt_system_devices_get_roles(h.xsysd, &roles);
		left =
		    (roles.left >= 0 && (uint32_t)roles.left < h.xsysd->xdev_count) ? h.xsysd->xdevs[roles.left] : NULL;
		right = (roles.right >= 0 && (uint32_t)roles.right < h.xsysd->xdev_count) ? h.xsysd->xdevs[roles.right]
		                                                                          : NULL;
		if (roles.generation_id != last_generation) {
			last_generation = roles.generation_id;
			P("roles (generation %u): left='%s' right='%s'\n", (unsigned)roles.generation_id,
			  left != NULL ? left->str : "<none>", right != NULL ? right->str : "<none>");
		}
		if (left == NULL || right == NULL) {
			os_nanosleep(500 * 1000 * 1000);
			continue;
		}
		fired++;

		value.vibration.amplitude = 0.25f + 0.75f * (float)(i % 4) / 3.0f;
		value.vibration.frequency = 160.0f;
		value.vibration.duration_ns = 100 * 1000 * 1000;
		xrt_device_set_output(left, XRT_OUTPUT_NAME_SIMPLE_VIBRATION, &value);
		xrt_device_set_output(right, XRT_OUTPUT_NAME_SIMPLE_VIBRATION, &value);

		// Exercise the pose path too: predict the left grip slightly
		// into the future, exactly like a frame loop would.
		xrt_device_update_inputs(left);
		struct xrt_space_relation rel = {0};
		int64_t at = (int64_t)os_monotonic_get_ns() + 10 * 1000 * 1000;
		if (xrt_device_get_tracked_pose(left, XRT_INPUT_SIMPLE_GRIP_POSE, at, &rel) == XRT_SUCCESS &&
		    (rel.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) != 0) {
			tracked_samples++;
			P("left grip @+10ms: (%.3f, %.3f, %.3f)\n", (double)rel.pose.position.x,
			  (double)rel.pose.position.y, (double)rel.pose.position.z);
		}

		os_nanosleep(500 * 1000 * 1000);
	}

	P("Done — fired %u haptic event(s) per hand; %u tracked pose sample(s).\n", fired, tracked_samples);
	cli_query_teardown(&h);
	if (fired == 0) {
		P("FAIL: the hand roles were never populated during the window.\n");
		return 1;
	}
	return 0;
}

static int
print_usage(void)
{
	P("Usage: displayxr-cli input <list|haptic-test>\n");
	P("  list [--json]          Enumerate registered input providers + predicted active one.\n");
	P("  haptic-test [seconds]  Headless: fire vibrations on the hand-role devices (default 5 s).\n");
	P("                         Pair with scripts/net_input_feeder.py --assert-haptic.\n");
	return 1;
}

int
cli_cmd_input(int argc, const char **argv)
{
	if (argc < 3) {
		return print_usage();
	}
	if (strcmp(argv[2], "list") == 0) {
		return cmd_list(argc, argv);
	}
	if (strcmp(argv[2], "haptic-test") == 0) {
		return cmd_haptic_test(argc, argv);
	}
	P("error: unknown 'input' subcommand '%s'.\n\n", argv[2]);
	return print_usage();
}
