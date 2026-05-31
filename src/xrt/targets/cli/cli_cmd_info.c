// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2026, Leia Inc.
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
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author David Fattal
 */

#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_config_os.h"

#include "util/u_git_tag.h"

#include "target_plugin_loader.h"

#include "cli_common.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define P(...) printf(__VA_ARGS__)
#define PT(...) printf("\t" __VA_ARGS__)
#define PTT(...) printf("\t\t" __VA_ARGS__)


#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*!
 * Print the Khronos `ActiveRuntime` value (64-bit view) so the user can
 * see at a glance whether DisplayXR is the system's active OpenXR runtime.
 */
static void
print_active_runtime(void)
{
	wchar_t wbuf[1024];
	DWORD wbuf_bytes = sizeof(wbuf);
	LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Khronos\\OpenXR\\1", L"ActiveRuntime",
	                          RRF_RT_REG_SZ, NULL, wbuf, &wbuf_bytes);
	P(" :: Active OpenXR runtime (HKLM\\Software\\Khronos\\OpenXR\\1\\ActiveRuntime)\n");
	if (rc == ERROR_SUCCESS) {
		char ubuf[1024] = {0};
		WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, ubuf, (int)sizeof(ubuf), NULL, NULL);
		PT("%s\n", ubuf);
	} else {
		PT("<unset> (rc=%ld)\n", rc);
	}
}
#endif


static int
do_exit(struct xrt_instance **xi_ptr, int ret)
{
	xrt_instance_destroy(xi_ptr);

	printf(" :: Exiting '%i'\n", ret);

	return ret;
}

int
cli_cmd_info(int argc, const char **argv)
{
	struct xrt_instance *xi = NULL;
	struct xrt_system *xsys = NULL;
	struct xrt_system_devices *xsysd = NULL;
	struct xrt_space_overseer *xso = NULL;
	xrt_result_t xret = XRT_SUCCESS;
	int ret = 0;

	P(" :: Runtime\n");
	PT("description: '%s'\n", u_runtime_description);
	PT("git-tag:     '%s'\n", u_git_tag);
	PT("plug-in ABI: v%u (runtime speaks XRT_PLUGIN_API_VERSION_CURRENT)\n",
	   (unsigned)XRT_PLUGIN_API_VERSION_CURRENT);

#ifdef XRT_OS_WINDOWS
	print_active_runtime();
#endif

	/*
	 * Initialize the instance and prober.
	 */

	P(" :: Creating instance and prober\n");

	ret = xrt_instance_create(NULL, &xi);
	if (ret != 0) {
		PT("Failed to create instance!\n");
		return do_exit(&xi, 0);
	}

	struct xrt_prober *xp = NULL;
	xret = xrt_instance_get_prober(xi, &xp);
	if (xret != XRT_SUCCESS) {
		PT("No xrt_prober could be created!\n");
		return do_exit(&xi, -1);
	}

	/*
	 * List builders compiled into this runtime.
	 */

	P(" :: Built builders\n");

	size_t builder_count;
	struct xrt_builder **builders;
	size_t num_entries;
	struct xrt_prober_entry **entries;
	struct xrt_auto_prober **auto_probers;
	ret = xrt_prober_get_builders(xp, &builder_count, &builders, &num_entries, &entries, &auto_probers);
	if (ret != 0) {
		PT("Failed to get builders!\n");
		return do_exit(&xi, ret);
	}

	for (size_t i = 0; i < builder_count; i++) {
		struct xrt_builder *builder = builders[i];
		if (builder == NULL) {
			continue;
		}
		PT("%s: %s\n", builder->identifier, builder->name);
	}

	/*
	 * Create the system devices — this runs the vendor plug-in discovery
	 * path and the display-processor-backed head device.
	 */

	P(" :: Display processor\n");

	xret = xrt_instance_create_system(xi, &xsys, &xsysd, &xso, NULL);
	if (xret != XRT_SUCCESS || xsysd == NULL || xsysd->static_roles.head == NULL) {
		PT("No display processor discovered (xret=%d).\n", (int)xret);
		goto out;
	}

	struct xrt_device *head = xsysd->static_roles.head;
	const struct xrt_plugin_iface *iface = target_plugin_get_active();

	if (iface == NULL) {
		PT("device: '%s' (no active vendor plug-in iface)\n", head->str);
		goto out;
	}

	PT("plug-in: id='%s' name='%s' vendor='%s' version='%s'\n", iface->id ? iface->id : "?",
	   iface->display_name ? iface->display_name : "?", iface->vendor ? iface->vendor : "?",
	   iface->version ? iface->version : "?");
	PT("ABI:     v%u (loader-verified match)\n", (unsigned)XRT_PLUGIN_API_VERSION_CURRENT);
	PT("device:  '%s'\n", head->str);

	if (iface->get_display_info != NULL) {
		struct xrt_plugin_display_info info = {0};
		info.struct_size = (uint32_t)sizeof(info);
		if (iface->get_display_info(target_plugin_get_active_instance(), head, &info)) {
			PT("physical:     %.4fm x %.4fm\n", (double)info.display_width_m,
			   (double)info.display_height_m);
			PT("pixels:       %ux%u\n", info.display_pixel_width, info.display_pixel_height);
			PT("viewer:       (%.4f, %.4f, %.4f) m\n", (double)info.nominal_viewer_x_m,
			   (double)info.nominal_viewer_y_m, (double)info.nominal_viewer_z_m);
			PT("view scale:   (%.3f, %.3f)\n", (double)info.recommended_view_scale_x,
			   (double)info.recommended_view_scale_y);
			PT("screen pos:   (%d, %d)\n", info.display_screen_left, info.display_screen_top);
			PT("eye-tracking: supported=0x%x default=%u\n", info.supported_eye_tracking_modes,
			   info.default_eye_tracking_mode);
		} else {
			PT("get_display_info returned false.\n");
		}
	} else {
		PT("plug-in does not implement get_display_info.\n");
	}

out:
	/*
	 * Done.
	 */

	P(" :: All ok, shutting down.\n");

	xrt_space_overseer_destroy(&xso);
	xrt_system_devices_destroy(&xsysd);
	xrt_system_destroy(&xsys);

	return do_exit(&xi, 0);
}
