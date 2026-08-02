// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared builder helper for input-provider motion controllers.
 * @ingroup target_common
 */

#include "target_builder_input_provider.h"
#include "target_input_plugin_loader.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"

#include "util/u_builders.h"
#include "util/u_logging.h"

#include <stddef.h>


/*!
 * Claim record of the last provider pass, for
 * @ref t_builder_input_provider_get_claims. Builders run once per
 * process on the single-threaded system-create path.
 */
static bool g_provider_claimed_left = false;
static bool g_provider_claimed_right = false;

void
t_builder_input_provider_get_claims(bool *out_left, bool *out_right)
{
	if (out_left != NULL) {
		*out_left = g_provider_claimed_left;
	}
	if (out_right != NULL) {
		*out_right = g_provider_claimed_right;
	}
}

xrt_result_t
t_builder_add_input_provider_devices(struct xrt_system_devices *xsysd,
                                     struct u_builder_roles_helper *ubrh,
                                     enum u_logging_level log_level)
{
	(void)log_level;

	g_provider_claimed_left = false;
	g_provider_claimed_right = false;

	if (target_input_plugin_get_force_qwerty()) {
		// Debug override (registry / config gate): qwerty keeps the
		// hand roles; providers are not even loaded, so behavior is
		// bit-identical to a box with none registered.
		U_LOG_W("input provider builder: ForceQwerty override set — skipping input providers.");
		return XRT_SUCCESS;
	}

	const struct xrt_input_plugin_iface *iface = target_input_plugin_get_active();
	if (iface == NULL) {
		return XRT_SUCCESS; // No provider — qwerty claims the hand roles.
	}
	if (iface->create_devices == NULL) {
		U_LOG_W("input provider builder: provider '%s' has no create_devices — ignoring.",
		        iface->id != NULL ? iface->id : "?");
		return XRT_SUCCESS;
	}

	struct xrt_device *devs[XRT_INPUT_PLUGIN_MAX_DEVICES] = {0};
	uint32_t max = XRT_INPUT_PLUGIN_MAX_DEVICES;

	// Never overflow the system device list — the head (and qwerty's
	// three) still need slots after us.
	uint32_t slots_free = 0;
	if (xsysd->xdev_count < XRT_SYSTEM_MAX_DEVICES) {
		slots_free = (uint32_t)(XRT_SYSTEM_MAX_DEVICES - xsysd->xdev_count);
	}
	if (slots_free < max) {
		max = slots_free;
	}
	if (max == 0) {
		U_LOG_W("input provider builder: no free device slots — skipping provider devices.");
		return XRT_SUCCESS;
	}

	uint32_t count = 0;
	xrt_result_t xret = iface->create_devices(target_input_plugin_get_active_instance(), devs, max, &count);
	if (xret != XRT_SUCCESS || count == 0) {
		U_LOG_W(
		    "input provider builder: '%s' create_devices returned %d (count=%u) — "
		    "qwerty keeps the hand roles.",
		    iface->id != NULL ? iface->id : "?", (int)xret, count);
		return XRT_SUCCESS; // Non-fatal by contract.
	}
	if (count > max) {
		U_LOG_E("input provider builder: '%s' reported %u devices for a max of %u — clamping.",
		        iface->id != NULL ? iface->id : "?", count, max);
		count = max;
	}

	for (uint32_t i = 0; i < count; i++) {
		struct xrt_device *xdev = devs[i];
		if (xdev == NULL) {
			continue;
		}

		xsysd->xdevs[xsysd->xdev_count++] = xdev;

		// Devices self-describe via device_type (ADR-034). First
		// claimant wins a role; ANY fills whichever hand is empty
		// (left first, matching reader expectations elsewhere).
		switch (xdev->device_type) {
		case XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER:
			if (ubrh->left == NULL) {
				ubrh->left = xdev;
			}
			break;
		case XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER:
			if (ubrh->right == NULL) {
				ubrh->right = xdev;
			}
			break;
		case XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER:
			if (ubrh->left == NULL) {
				ubrh->left = xdev;
			} else if (ubrh->right == NULL) {
				ubrh->right = xdev;
			}
			break;
		default:
			// Future device classes (generic trackers, …) ride
			// along in the device list without claiming a role.
			break;
		}

		U_LOG_I("input provider builder: added device '%s' (type=%d)%s", xdev->str, (int)xdev->device_type,
		        (ubrh->left == xdev)    ? " [left]"
		        : (ubrh->right == xdev) ? " [right]"
		                                : "");
	}

	g_provider_claimed_left = ubrh->left != NULL;
	g_provider_claimed_right = ubrh->right != NULL;

	U_LOG_W("input provider builder: provider '%s' supplied %u device(s)%s%s.", iface->id != NULL ? iface->id : "?",
	        count, g_provider_claimed_left ? " — left claimed" : "",
	        g_provider_claimed_right ? ", right claimed" : "");

	return XRT_SUCCESS;
}
