// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Headless self-test: discover a display processor and validate it.
 *
 * Creates an instance and system devices with NO compositor (so it runs
 * without a GPU, window, or display) and asserts that:
 *   1. system creation succeeded and produced a head/display device;
 *   2. a vendor plug-in is active — the loader rejects ABI-mismatched
 *      plug-ins (ADR-020 rule 3), so an active iface implies the plug-in
 *      ABI matches @ref XRT_PLUGIN_API_VERSION_CURRENT;
 *   3. the plug-in reports sane physical + pixel display dimensions.
 *
 * This is the runtime's headless smoke test — it exercises the real
 * plug-in discovery path (registry on Windows, JSON manifests on POSIX)
 * end to end without launching an app. Exit code is the contract:
 * 0 = pass, non-zero = a specific failure (see the enum below), so CI can
 * gate on it.
 *
 * @author David Fattal
 */

#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"

#include "target_plugin_loader.h"

#include "cli_common.h"

#include <stdint.h>
#include <stdio.h>

#define P(...) printf(__VA_ARGS__)

enum selftest_result
{
	SELFTEST_PASS = 0,      //!< All checks passed.
	SELFTEST_INIT_FAIL = 1, //!< Instance / system creation failed.
	SELFTEST_NO_DP = 2,     //!< No display processor / plug-in discovered.
	SELFTEST_BAD_INFO = 3,  //!< Plug-in reported invalid display info.
};

int
cli_cmd_selftest(int argc, const char **argv)
{
	struct xrt_instance *xi = NULL;
	struct xrt_system *xsys = NULL;
	struct xrt_system_devices *xsysd = NULL;
	struct xrt_space_overseer *xso = NULL;
	int rc = SELFTEST_INIT_FAIL;

	P(" :: DisplayXR CLI self-test (headless, no compositor)\n");

	if (xrt_instance_create(NULL, &xi) != 0) {
		P("FAIL: xrt_instance_create failed.\n");
		return SELFTEST_INIT_FAIL;
	}

	xrt_result_t xret = xrt_instance_create_system(xi, &xsys, &xsysd, &xso, NULL);
	if (xret != XRT_SUCCESS || xsysd == NULL) {
		P("FAIL: xrt_instance_create_system failed (xret=%d).\n", (int)xret);
		rc = SELFTEST_INIT_FAIL;
		goto out;
	}

	// The head role is the display-processor-backed device the active
	// plug-in created through the builder. No head = discovery produced
	// no display.
	struct xrt_device *head = xsysd->static_roles.head;
	if (head == NULL) {
		P("FAIL: no head/display device — no display processor was discovered.\n");
		rc = SELFTEST_NO_DP;
		goto out;
	}
	P("PASS: display device present: '%s'\n", head->str);

	// Identify the active vendor plug-in. An active iface implies a
	// matching ABI (the loader skips mismatched majors).
	const struct xrt_plugin_iface *iface = target_plugin_get_active();
	if (iface == NULL) {
		P("FAIL: display device exists but no vendor plug-in is active.\n");
		rc = SELFTEST_NO_DP;
		goto out;
	}
	P("PASS: active plug-in: id='%s' name='%s' version='%s' (ABI v%u)\n", iface->id ? iface->id : "?",
	  iface->display_name ? iface->display_name : "?", iface->version ? iface->version : "?",
	  (unsigned)XRT_PLUGIN_API_VERSION_CURRENT);

	// Validate the vendor-neutral display info.
	if (iface->get_display_info == NULL) {
		P("FAIL: plug-in does not implement get_display_info.\n");
		rc = SELFTEST_BAD_INFO;
		goto out;
	}

	struct xrt_plugin_display_info info = {0};
	info.struct_size = (uint32_t)sizeof(info);
	if (!iface->get_display_info(target_plugin_get_active_instance(), head, &info)) {
		P("FAIL: get_display_info returned false.\n");
		rc = SELFTEST_BAD_INFO;
		goto out;
	}

	if (!(info.display_width_m > 0.0f) || !(info.display_height_m > 0.0f) || info.display_pixel_width == 0 ||
	    info.display_pixel_height == 0) {
		P("FAIL: invalid display dimensions: %.4fm x %.4fm, %ux%u px.\n", (double)info.display_width_m,
		  (double)info.display_height_m, info.display_pixel_width, info.display_pixel_height);
		rc = SELFTEST_BAD_INFO;
		goto out;
	}
	P("PASS: display info: %.4fm x %.4fm, %ux%u px, eye-tracking modes=0x%x.\n", (double)info.display_width_m,
	  (double)info.display_height_m, info.display_pixel_width, info.display_pixel_height,
	  info.supported_eye_tracking_modes);

	rc = SELFTEST_PASS;

out:
	xrt_space_overseer_destroy(&xso);
	xrt_system_devices_destroy(&xsysd);
	xrt_system_destroy(&xsys);
	xrt_instance_destroy(&xi);

	if (rc == SELFTEST_PASS) {
		P(" :: SELF-TEST PASSED\n");
	} else {
		P(" :: SELF-TEST FAILED (rc=%d)\n", rc);
	}
	return rc;
}
