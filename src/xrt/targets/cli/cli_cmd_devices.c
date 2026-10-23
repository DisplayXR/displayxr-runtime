// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `devices` (alias `probe`): dump the system the runtime builds.
 *
 * Creates the same headless instance + system `selftest` does (no
 * compositor), then prints what came out of it: the builder that ran, the
 * active display-processor plug-in, the active input provider, every device
 * in the system with its type, and the role assignments. Replaces the Monado
 * `probe` / `test` commands, which listed USB prober entries and auto-probers
 * that DisplayXR no longer has (#703).
 */

#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_input_plugin.h"

#include "target_plugin_loader.h"
#include "target_input_plugin_loader.h"

#include "cli_common.h"

#include <stdio.h>
#include <string.h>


static const char *
device_type_str(enum xrt_device_type type)
{
	switch (type) {
	case XRT_DEVICE_TYPE_HMD: return "head";
	case XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER: return "right controller";
	case XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER: return "left controller";
	case XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER: return "any-hand controller";
	case XRT_DEVICE_TYPE_GAMEPAD: return "gamepad";
	case XRT_DEVICE_TYPE_GENERIC_TRACKER: return "generic tracker";
	case XRT_DEVICE_TYPE_HAND_TRACKER: return "hand tracker";
	case XRT_DEVICE_TYPE_EYE_TRACKER: return "eye tracker";
	case XRT_DEVICE_TYPE_FACE_TRACKER: return "face tracker";
	case XRT_DEVICE_TYPE_BODY_TRACKER: return "body tracker";
	case XRT_DEVICE_TYPE_NAVIGATION: return "navigation";
	case XRT_DEVICE_TYPE_UNKNOWN:
	default: return "unknown";
	}
}

static const char *
str_or(const char *s, const char *fallback)
{
	return (s != NULL && s[0] != '\0') ? s : fallback;
}

static const char *
dev_str(const struct xrt_device *xdev)
{
	return xdev != NULL ? xdev->str : "<none>";
}

static const char *
idx_str(const struct xrt_system_devices *xsysd, int32_t idx, const char *none)
{
	if (idx < 0 || (size_t)idx >= xsysd->xdev_count || xsysd->xdevs[idx] == NULL) {
		return none;
	}
	return xsysd->xdevs[idx]->str;
}

static void
print_builders(struct xrt_instance *xi)
{
	struct xrt_prober *xp = NULL;
	if (xrt_instance_get_prober(xi, &xp) != XRT_SUCCESS || xp == NULL) {
		printf("builders:      <no prober>\n");
		return;
	}

	size_t count = 0;
	struct xrt_builder **builders = NULL;
	if (xrt_prober_get_builders(xp, &count, &builders) != 0) {
		printf("builders:      <unavailable>\n");
		return;
	}

	printf("builders:     ");
	for (size_t i = 0; i < count; i++) {
		printf(" %s%s", builders[i]->identifier, i + 1 < count ? "," : "");
	}
	printf("%s\n", count == 0 ? " <none>" : "");
}

static void
print_plugins(void)
{
	const struct xrt_plugin_iface *dp = target_plugin_get_active();
	if (dp != NULL) {
		printf("display plug-in: %s (%s%s%s)\n", str_or(dp->id, "<no id>"), str_or(dp->display_name, "?"),
		       dp->vendor != NULL ? ", " : "", dp->vendor != NULL ? dp->vendor : "");
	} else {
		printf("display plug-in: <none>\n");
	}

	const struct xrt_input_plugin_iface *in = target_input_plugin_get_active();
	if (in != NULL) {
		printf("input provider:  %s (%s)\n", str_or(in->id, "<no id>"), str_or(in->display_name, "?"));
	} else {
		printf("input provider:  <none claimed> (qwerty is the input floor)\n");
	}
}

static void
print_devices_and_roles(struct xrt_system_devices *xsysd)
{
	printf("devices (%zu):\n", xsysd->xdev_count);
	for (size_t i = 0; i < xsysd->xdev_count; i++) {
		const struct xrt_device *xdev = xsysd->xdevs[i];
		if (xdev == NULL) {
			continue;
		}
		printf("\t%2zu: %-40s [%s]\n", i, xdev->str, device_type_str(xdev->device_type));
	}

	struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
	xrt_system_devices_get_roles(xsysd, &roles);

	printf("roles:\n");
	printf("\thead:                     %s", dev_str(xsysd->static_roles.head));
	if (xsysd->static_roles.head != NULL && xsysd->static_roles.head->hmd != NULL) {
		printf(" (view count %zu)", xsysd->static_roles.head->hmd->view_count);
	}
	printf("\n");
	printf("\teyes:                     %s\n", dev_str(xsysd->static_roles.eyes));
	printf("\tface:                     %s\n", dev_str(xsysd->static_roles.face));
	printf("\tbody:                     %s\n", dev_str(xsysd->static_roles.body));
	printf("\tleft:                     %s\n", idx_str(xsysd, roles.left, "<none>"));
	printf("\tright:                    %s\n", idx_str(xsysd, roles.right, "<none>"));
	printf("\tgamepad:                  %s\n", idx_str(xsysd, roles.gamepad, "<none>"));
	printf("\trig:                      %s\n", idx_str(xsysd, roles.rig, "<runtime fly camera>"));
	printf("\thand_tracking.unobstructed.left:  %s\n",
	       dev_str(xsysd->static_roles.hand_tracking.unobstructed.left));
	printf("\thand_tracking.unobstructed.right: %s\n",
	       dev_str(xsysd->static_roles.hand_tracking.unobstructed.right));
	printf("\thand_tracking.conforming.left:    %s\n", dev_str(xsysd->static_roles.hand_tracking.conforming.left));
	printf("\thand_tracking.conforming.right:   %s\n", dev_str(xsysd->static_roles.hand_tracking.conforming.right));
}

int
cli_cmd_devices(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	struct xrt_instance *xi = NULL;
	if (xrt_instance_create(NULL, &xi) != XRT_SUCCESS || xi == NULL) {
		fprintf(stderr, "devices: xrt_instance_create failed\n");
		return 1;
	}

	struct xrt_system *xsys = NULL;
	struct xrt_system_devices *xsysd = NULL;
	struct xrt_space_overseer *xso = NULL;
	xrt_result_t xret = xrt_instance_create_system(xi, &xsys, &xsysd, &xso, NULL);

	print_builders(xi);
	print_plugins();

	int ret = 0;
	if (xret != XRT_SUCCESS || xsysd == NULL) {
		fprintf(stderr, "devices: xrt_instance_create_system failed (%d)\n", (int)xret);
		ret = 1;
	} else {
		print_devices_and_roles(xsysd);
		if (xsysd->static_roles.head == NULL) {
			fprintf(stderr, "devices: no head device (no display processor)\n");
			ret = 1;
		}
	}

	xrt_space_overseer_destroy(&xso);
	xrt_system_devices_destroy(&xsysd);
	xrt_system_destroy(&xsys);
	xrt_instance_destroy(&xi);

	return ret;
}
