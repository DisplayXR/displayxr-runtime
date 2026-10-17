// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_input_providers entry points (ADR-034 Amendment 5, #2181):
 *         which input provider holds this app's controllers / hands / gamepad,
 *         and a per-family request for another.
 *
 * Thin by design. Every rule — the precedence (lock -> per-app pin -> request
 * -> user default -> ProbeOrder), the "only families the provider claims"
 * gate, presence, the per-client generation — lives behind
 * `xrt_system_devices::input_selection` (`xrt/xrt_input_selection.h`):
 * the arbiter in-process, the service over IPC. This file owns struct
 * conversion, the two-call idiom and the providers-changed event.
 *
 * Scope is the CLIENT (this XrInstance): `dynamic_roles_cache` is per
 * `oxr_system`, so a per-session overlay could not be told apart anyway. The
 * setter takes a session only as proof of a running app.
 *
 * In-process the client is implicit (key 0) and keyed by this process's own
 * executable basename; an IPC client's implementation ignores what we pass —
 * the service resolves the key from the peer process, never from the client.
 *
 * @ingroup oxr_api
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"

#include "xrt/xrt_input_selection.h"
#include "xrt/xrt_system.h"

#include "util/u_exe_basename.h"
#include "util/u_trace_marker.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

#ifdef OXR_HAVE_DXR_input_providers

// The families are bit-identical between the two layers; the enum is off by one.
_Static_assert(XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_CONTROLLERS) == 0x1, "family bit mirror");
_Static_assert(XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_HAND_TRACKING) == 0x2, "family bit mirror");
_Static_assert(XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_GAMEPAD) == 0x4, "family bit mirror");
_Static_assert((int)XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR == XRT_INPUT_ROLE_FAMILY_CONTROLLERS + 1, "family mirror");
_Static_assert((int)XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR == XRT_INPUT_ROLE_FAMILY_HAND_TRACKING + 1,
               "family mirror");
_Static_assert((int)XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR == XRT_INPUT_ROLE_FAMILY_GAMEPAD + 1, "family mirror");
_Static_assert(XRT_INPUT_PROVIDER_ID_SIZE == XR_MAX_INPUT_PROVIDER_ID_SIZE_DXR, "size mirror");
_Static_assert(XRT_INPUT_PROVIDER_NAME_SIZE == XR_MAX_INPUT_PROVIDER_NAME_SIZE_DXR, "size mirror");

//! Upper bound on providers one enumeration assembles (the loader's own row cap is lower).
#define ENUM_MAX 64

//! Pages are re-read from the top when the client's generation moves mid-walk; give up after this many.
#define ENUM_RETRIES 3


/*
 *
 * Helpers.
 *
 */

/*!
 * The in-process implicit client. Over IPC the client library ignores it and
 * the service substitutes the connection's own identity.
 */
static void
fill_self_client(struct xrt_input_selection_client *client)
{
	memset(client, 0, sizeof(*client));
	client->key = 0;
	(void)u_exe_basename_self(client->exe_basename, sizeof(client->exe_basename));
}

static XrInputRoleFamilyFlagsDXR
to_xr_families(uint32_t bits)
{
	return (XrInputRoleFamilyFlagsDXR)(bits & XRT_INPUT_ROLE_FAMILY_ALL_BITS);
}

static void
to_xr_properties(const struct xrt_input_provider_info *in, XrInputProviderPropertiesDXR *out)
{
	// type / next belong to the app; everything else is ours.
	memset(out->providerId, 0, sizeof(out->providerId));
	memset(out->displayName, 0, sizeof(out->displayName));
	memcpy(out->providerId, in->id, strnlen(in->id, sizeof(out->providerId) - 1));
	memcpy(out->displayName, in->display_name, strnlen(in->display_name, sizeof(out->displayName) - 1));
	out->supportedFamilies = to_xr_families(in->claimable_families);
	out->currentFamilies = to_xr_families(in->current_families);
	out->requestedFamilies = to_xr_families(in->requested_families);
	out->isPresent = in->present ? XR_TRUE : XR_FALSE;
	out->isUserDefault = in->user_default ? XR_TRUE : XR_FALSE;
	out->isAppPinned = in->app_pinned ? XR_TRUE : XR_FALSE;
}

/*!
 * Read the whole list as one consistent snapshot: page after page, restarting
 * when the client's generation moves between pages. Returns the count written
 * to @p out (<= @p cap); 0 when the system has no selection support.
 */
static XrResult
gather(struct oxr_logger *log, struct oxr_system *sys, struct xrt_input_provider_info *out, uint32_t cap, uint32_t *n)
{
	*n = 0;
	struct xrt_input_selection *xis = sys->xsysd != NULL ? sys->xsysd->input_selection : NULL;
	if (xis == NULL) {
		return XR_SUCCESS; // no selection support on this system: no providers listed
	}
	struct xrt_input_selection_client client;
	fill_self_client(&client);

	struct xrt_input_provider_page page;
	for (int attempt = 0; attempt < ENUM_RETRIES; attempt++) {
		uint32_t got = 0;
		uint64_t generation = 0;
		bool restart = false;
		for (uint32_t start = 0;;) {
			memset(&page, 0, sizeof(page));
			const xrt_result_t xret = xrt_input_selection_enumerate(xis, &client, start, &page);
			if (xret == XRT_ERROR_FEATURE_NOT_SUPPORTED) {
				return XR_SUCCESS; // e.g. a service without selection support
			}
			if (xret != XRT_SUCCESS) {
				return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
				                 "input provider enumeration failed (xrt_result %d)", (int)xret);
			}
			if (start == 0) {
				generation = page.generation;
			} else if (page.generation != generation) {
				restart = true;
				break;
			}
			const uint32_t count = page.count < XRT_INPUT_PROVIDER_PAGE_MAX ? page.count : XRT_INPUT_PROVIDER_PAGE_MAX;
			for (uint32_t i = 0; i < count && got < cap; i++) {
				out[got++] = page.items[i];
			}
			start += count;
			if (count == 0 || start >= page.total || got >= cap) {
				break;
			}
		}
		if (!restart) {
			*n = got;
			return XR_SUCCESS;
		}
	}
	return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "input providers kept changing during enumeration");
}


/*
 *
 * Entry points.
 *
 */

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrEnumerateInputProvidersDXR(XrInstance instance,
                                 XrSystemId systemId,
                                 uint32_t providerCapacityInput,
                                 uint32_t *providerCountOutput,
                                 XrInputProviderPropertiesDXR *providers)
{
	OXR_TRACE_MARKER();

	struct oxr_instance *inst;
	struct oxr_logger log;
	OXR_VERIFY_INSTANCE_AND_INIT_LOG(&log, instance, inst, "xrEnumerateInputProvidersDXR");
	OXR_VERIFY_EXTENSION(&log, inst, DXR_input_providers);
	struct oxr_system *sys = NULL;
	XrResult sret = oxr_system_get_by_id(&log, inst, systemId, &sys);
	if (sret != XR_SUCCESS) {
		return sret;
	}
	OXR_VERIFY_ARG_NOT_NULL(&log, providerCountOutput);
	if (providerCapacityInput != 0 && providers == NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(providers == NULL) with a non-zero capacity");
	}
	for (uint32_t i = 0; i < providerCapacityInput; i++) {
		if (providers[i].type != XR_TYPE_INPUT_PROVIDER_PROPERTIES_DXR) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "(providers[%u].type) is not XR_TYPE_INPUT_PROVIDER_PROPERTIES_DXR", i);
		}
	}

	struct xrt_input_provider_info *list =
	    (struct xrt_input_provider_info *)calloc(ENUM_MAX, sizeof(struct xrt_input_provider_info));
	if (list == NULL) {
		return oxr_error(&log, XR_ERROR_OUT_OF_MEMORY, "input provider list");
	}
	uint32_t n = 0;
	XrResult ret = gather(&log, sys, list, ENUM_MAX, &n);
	if (ret != XR_SUCCESS) {
		free(list);
		return ret;
	}

	*providerCountOutput = n;
	if (providerCapacityInput == 0) {
		free(list);
		return XR_SUCCESS;
	}
	if (providerCapacityInput < n) {
		free(list);
		return oxr_error(&log, XR_ERROR_SIZE_INSUFFICIENT, "(providerCapacityInput == %u) < %u",
		                 providerCapacityInput, n);
	}
	for (uint32_t i = 0; i < n; i++) {
		to_xr_properties(&list[i], &providers[i]);
	}
	free(list);
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrSetInputProviderPreferenceDXR(XrSession session, const XrInputProviderPreferenceInfoDXR *info)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetInputProviderPreferenceDXR");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_input_providers);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, info, XR_TYPE_INPUT_PROVIDER_PREFERENCE_INFO_DXR);

	if (info->family < XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR || info->family > XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(info->family == %d) is not a role family",
		                 (int)info->family);
	}
	if (strnlen(info->providerId, sizeof(info->providerId)) >= sizeof(info->providerId)) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(info->providerId) is not NUL-terminated");
	}
	if (info->persist != XR_TRUE && info->persist != XR_FALSE) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(info->persist == %u) is not an XrBool32",
		                 (unsigned)info->persist);
	}

	struct xrt_input_selection *xis = sess->sys->xsysd != NULL ? sess->sys->xsysd->input_selection : NULL;
	struct xrt_input_selection_client client;
	fill_self_client(&client);

	const enum xrt_input_role_family family = (enum xrt_input_role_family)((int)info->family - 1);
	const xrt_result_t xret =
	    xrt_input_selection_set_preference(xis, &client, family, info->providerId, info->persist == XR_TRUE);
	if (xret == XRT_ERROR_FEATURE_NOT_SUPPORTED) {
		// Normal flow for a system / service without selection support: no oxr_error spam.
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}
	if (xret != XRT_SUCCESS) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "setting the input provider preference failed (%d)",
		                 (int)xret);
	}

	// One-off user action, not per frame: WARN is the lifecycle level here.
	U_LOG_W("XR_DXR_input_providers: family %d -> '%s'%s.", (int)info->family,
	        info->providerId[0] != '\0' ? info->providerId : "<default>", info->persist ? " (persisted)" : "");

	// The roles move on the next xrSyncActions (which raises its own event
	// for the families that actually moved); this one says "your preference
	// was recorded — re-enumerate" even when nothing moved.
	(void)oxr_event_push_XrEventDataInputProvidersChanged(&log, sess->sys->inst,
	                                                      (XrInputRoleFamilyFlagsDXR)XRT_INPUT_ROLE_FAMILY_BIT(family));
	return XR_SUCCESS;
}

#endif // OXR_HAVE_DXR_input_providers


/*
 *
 * Roles-changed hook (called unconditionally from xrSyncActions).
 *
 */

void
oxr_input_providers_roles_changed(struct oxr_logger *log,
                                  struct oxr_system *sys,
                                  const struct xrt_system_roles *old_roles,
                                  const struct xrt_system_roles *new_roles)
{
#ifdef OXR_HAVE_DXR_input_providers
	if (sys == NULL || sys->inst == NULL || !sys->inst->extensions.DXR_input_providers) {
		return;
	}
	if (old_roles->generation_id == 0) {
		return; // the first roles this instance ever saw: nothing "changed"
	}
	uint32_t changed = 0;
	if (old_roles->left != new_roles->left || old_roles->right != new_roles->right) {
		changed |= XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_CONTROLLERS);
	}
	if (old_roles->hand_tracking.unobstructed.left != new_roles->hand_tracking.unobstructed.left ||
	    old_roles->hand_tracking.unobstructed.right != new_roles->hand_tracking.unobstructed.right ||
	    old_roles->hand_tracking.conforming.left != new_roles->hand_tracking.conforming.left ||
	    old_roles->hand_tracking.conforming.right != new_roles->hand_tracking.conforming.right) {
		changed |= XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_HAND_TRACKING);
	}
	if (old_roles->gamepad != new_roles->gamepad) {
		changed |= XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_GAMEPAD);
	}
	if (changed == 0) {
		return; // a generation bump that moved nothing this app binds (e.g. the rig)
	}
	(void)oxr_event_push_XrEventDataInputProvidersChanged(log, sys->inst, (XrInputRoleFamilyFlagsDXR)changed);
#else
	(void)log;
	(void)sys;
	(void)old_roles;
	(void)new_roles;
#endif
}
