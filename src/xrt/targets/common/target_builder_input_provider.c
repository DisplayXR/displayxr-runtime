// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared builder helper for input-provider motion controllers.
 * @ingroup target_common
 */

#include "target_builder_input_provider.h"
#include "target_input_arbiter.h"
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
 *
 * "Claimed" here means the provider SUPPLIED a device for that role, not
 * that the device currently holds it: the hand roles are arbitrated
 * against provider presence and can move to qwerty at any time
 * (`target_input_arbiter.h`). Ask @ref t_input_arbiter_provider_holds_roles
 * for the live answer.
 */
static bool g_provider_claimed_left = false;
static bool g_provider_claimed_right = false;
static bool g_provider_claimed_ht_left = false;
static bool g_provider_claimed_ht_right = false;

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

void
t_builder_input_provider_get_ht_claims(bool *out_left, bool *out_right)
{
	if (out_left != NULL) {
		*out_left = g_provider_claimed_ht_left;
	}
	if (out_right != NULL) {
		*out_right = g_provider_claimed_ht_right;
	}
}

/*!
 * Claim hand-tracking roles for @p xdev (#825 Tier 2). Devices
 * self-describe here too: `supported.hand_tracking` gates, and the
 * present `XRT_INPUT_HT_*` input names say which hand and which data
 * source (unobstructed = optical, conforming = controller-derived) the
 * device serves — one device may serve several. First claimant wins,
 * matching the controller-role arbitration above.
 */
static void
claim_hand_tracking_roles(struct u_builder_roles_helper *ubrh, struct xrt_device *xdev)
{
	if (!xdev->supported.hand_tracking) {
		return;
	}

	for (uint32_t i = 0; i < xdev->input_count; i++) {
		switch (xdev->inputs[i].name) {
		case XRT_INPUT_HT_UNOBSTRUCTED_LEFT:
			if (ubrh->hand_tracking.unobstructed.left == NULL) {
				ubrh->hand_tracking.unobstructed.left = xdev;
			}
			break;
		case XRT_INPUT_HT_UNOBSTRUCTED_RIGHT:
			if (ubrh->hand_tracking.unobstructed.right == NULL) {
				ubrh->hand_tracking.unobstructed.right = xdev;
			}
			break;
		case XRT_INPUT_HT_CONFORMING_LEFT:
			if (ubrh->hand_tracking.conforming.left == NULL) {
				ubrh->hand_tracking.conforming.left = xdev;
			}
			break;
		case XRT_INPUT_HT_CONFORMING_RIGHT:
			if (ubrh->hand_tracking.conforming.right == NULL) {
				ubrh->hand_tracking.conforming.right = xdev;
			}
			break;
		default: break;
		}
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
	g_provider_claimed_ht_left = false;
	g_provider_claimed_ht_right = false;

	t_input_arbiter_reset();

	if (target_input_plugin_get_force_qwerty()) {
		// Debug override (registry / config gate): qwerty keeps the
		// hand roles; providers are not even loaded, so behavior is
		// bit-identical to a box with none registered.
		U_LOG_W("input provider builder: ForceQwerty override set — skipping input providers.");
		return XRT_SUCCESS;
	}

	// ADR-034 Amendment 3: every claiming provider's devices are created
	// and stay resident; the arbiter ranks the pairs by ProbeOrder and
	// the hand roles go to the highest-ranked candidate whose hardware
	// is present, falling through the ranks before landing on qwerty.
	int provider_count = target_input_plugin_get_count();
	struct xrt_device *first_left = NULL;
	struct xrt_device *first_right = NULL;

	for (int p = 0; p < provider_count; p++) {
		const struct xrt_input_plugin_iface *iface = target_input_plugin_get_iface(p);
		struct xrt_input_plugin_instance *inst = target_input_plugin_get_instance(p);
		uint32_t priority = target_input_plugin_get_priority(p);

		if (iface == NULL || iface->create_devices == NULL) {
			U_LOG_W("input provider builder: provider '%s' has no create_devices — ignoring.",
			        iface != NULL && iface->id != NULL ? iface->id : "?");
			continue;
		}

		struct xrt_device *devs[XRT_INPUT_PLUGIN_MAX_DEVICES] = {0};
		uint32_t max = XRT_INPUT_PLUGIN_MAX_DEVICES;

		// Never overflow the system device list — the head (and
		// qwerty's three) still need slots after us.
		uint32_t slots_free = 0;
		if (xsysd->xdev_count < XRT_SYSTEM_MAX_DEVICES) {
			slots_free = (uint32_t)(XRT_SYSTEM_MAX_DEVICES - xsysd->xdev_count);
		}
		if (slots_free < max) {
			max = slots_free;
		}
		if (max == 0) {
			U_LOG_W("input provider builder: no free device slots — skipping remaining providers.");
			break;
		}

		uint32_t count = 0;
		xrt_result_t xret = iface->create_devices(inst, devs, max, &count);
		if (xret != XRT_SUCCESS || count == 0) {
			U_LOG_W("input provider builder: '%s' create_devices returned %d (count=%u) — skipping.",
			        iface->id != NULL ? iface->id : "?", (int)xret, count);
			continue; // Non-fatal by contract.
		}
		if (count > max) {
			U_LOG_E("input provider builder: '%s' reported %u devices for a max of %u — clamping.",
			        iface->id != NULL ? iface->id : "?", count, max);
			count = max;
		}

		struct xrt_device *prov_left = NULL;
		struct xrt_device *prov_right = NULL;

		for (uint32_t i = 0; i < count; i++) {
			struct xrt_device *xdev = devs[i];
			if (xdev == NULL) {
				continue;
			}

			xsysd->xdevs[xsysd->xdev_count++] = xdev;

			// An input provider reports physically honest poses in
			// its own tracking volume (ADR-034); anchoring that
			// volume to the world is the runtime's job. On a 3D
			// display the sensor sits on the same desk as the
			// panel, so the volume is bolted to the rig: it follows
			// voluntary navigation and ignores eye-tracked head
			// parallax (ADR-034 Amendment 2). Qwerty is
			// deliberately absent here — its controllers already
			// follow the qwerty HMD.
			if (ubrh->rig_relative_count < ARRAY_SIZE(ubrh->rig_relative)) {
				ubrh->rig_relative[ubrh->rig_relative_count++] = xdev;
			}

			// Devices self-describe via device_type (ADR-034).
			// First claimant wins a role; ANY fills whichever hand
			// is empty (left first, matching reader expectations
			// elsewhere).
			switch (xdev->device_type) {
			case XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER:
				if (prov_left == NULL) {
					prov_left = xdev;
				}
				break;
			case XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER:
				if (prov_right == NULL) {
					prov_right = xdev;
				}
				break;
			case XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER:
				if (prov_left == NULL) {
					prov_left = xdev;
				} else if (prov_right == NULL) {
					prov_right = xdev;
				}
				break;
			default:
				// Future device classes (generic trackers, …)
				// ride along in the device list without
				// claiming a role.
				break;
			}

			// Hand-tracking roles are orthogonal to the controller
			// roles: a device may claim either, both (ultraleap),
			// or neither. These are STATIC roles by contract
			// (`xrt_system_devices`), so unlike the controller
			// roles they are not arbitrated — an absent optical
			// tracker simply reports inactive joints, and qwerty
			// has no hand tracking to fall back to anyway. First
			// claimant across ALL providers wins.
			claim_hand_tracking_roles(ubrh, xdev);

			U_LOG_I("input provider builder: '%s' added device '%s' (type=%d)%s",
			        iface->id != NULL ? iface->id : "?", xdev->str, (int)xdev->device_type,
			        (prov_left == xdev)    ? " [left]"
			        : (prov_right == xdev) ? " [right]"
			                               : "");
		}

		// Hand this provider's pair to the arbiter with its identity
		// and rank; `t_input_arbiter_install()` (called by the builder
		// once qwerty has been added too) makes the assignment
		// re-resolve for the life of the process.
		t_input_arbiter_note_provider_pair(iface, inst, priority, prov_left, prov_right);

		if (first_left == NULL) {
			first_left = prov_left;
		}
		if (first_right == NULL) {
			first_right = prov_right;
		}

		g_provider_claimed_left |= prov_left != NULL;
		g_provider_claimed_right |= prov_right != NULL;

		U_LOG_W("input provider builder: provider '%s' (priority %u) supplied %u device(s)%s%s.",
		        iface->id != NULL ? iface->id : "?", priority, count,
		        prov_left != NULL ? " — left supplied" : "", prov_right != NULL ? ", right supplied" : "");
	}

	// The static role seed: the highest-ranked pair, taken only if any
	// provider hardware is actually there right now. The arbiter refines
	// per-hand and re-resolves from the first xrSyncActions on.
	const bool holds = t_input_arbiter_provider_holds_roles();
	if (holds) {
		if (ubrh->left == NULL) {
			ubrh->left = first_left;
		}
		if (ubrh->right == NULL) {
			ubrh->right = first_right;
		}
	}

	g_provider_claimed_ht_left =
	    ubrh->hand_tracking.unobstructed.left != NULL || ubrh->hand_tracking.conforming.left != NULL;
	g_provider_claimed_ht_right =
	    ubrh->hand_tracking.unobstructed.right != NULL || ubrh->hand_tracking.conforming.right != NULL;

	if (provider_count > 0) {
		U_LOG_W(
		    "input provider builder: %d provider(s) resident%s%s%s%s; hardware %s — %s holds the hand roles.",
		    provider_count, g_provider_claimed_left ? " — left supplied" : "",
		    g_provider_claimed_right ? ", right supplied" : "",
		    g_provider_claimed_ht_left ? ", ht-left claimed" : "",
		    g_provider_claimed_ht_right ? ", ht-right claimed" : "", holds ? "present" : "absent",
		    holds ? "a provider" : "qwerty");
	}

	return XRT_SUCCESS;
}
