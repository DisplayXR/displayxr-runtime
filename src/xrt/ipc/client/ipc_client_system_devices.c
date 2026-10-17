// Copyright 2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  IPC Client system devices.
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc_client
 */

#include "ipc_client.h"
#include "ipc_client_generated.h"

#include "util/u_system_helpers.h"

#include "xrt/xrt_input_selection.h"

#include <string.h>


struct ipc_client_system_devices
{
	//! @public Base
	struct u_system_devices base;

	//! Connection to service.
	struct ipc_connection *ipc_c;

	struct xrt_reference feature_use[XRT_DEVICE_FEATURE_MAX_ENUM];

	/*!
	 * ADR-034 Amendment 5 (#2181): per-client input-provider selection,
	 * forwarded to the service. The connection IS the client, so the
	 * @ref xrt_input_selection_client the OpenXR layer passes is ignored —
	 * the service keys on the peer it resolved at accept.
	 */
	struct xrt_input_selection input_selection;
};


/*
 *
 * Helpers
 *
 */

static inline struct ipc_client_system_devices *
ipc_system_devices(struct xrt_system_devices *xsysd)
{
	return (struct ipc_client_system_devices *)xsysd;
}


/*
 *
 * Member functions.
 *
 */

static xrt_result_t
ipc_client_system_devices_get_roles(struct xrt_system_devices *xsysd, struct xrt_system_roles *out_roles)
{
	struct ipc_client_system_devices *usysd = ipc_system_devices(xsysd);

	return ipc_call_system_devices_get_roles(usysd->ipc_c, out_roles);
}

static inline struct ipc_client_system_devices *
ipc_system_devices_from_selection(struct xrt_input_selection *xis)
{
	return container_of(xis, struct ipc_client_system_devices, input_selection);
}

static xrt_result_t
ipc_client_input_selection_enumerate(struct xrt_input_selection *xis,
                                     const struct xrt_input_selection_client *client,
                                     uint32_t start,
                                     struct xrt_input_provider_page *out_page)
{
	(void)client; // the service resolves the client from the connection
	struct ipc_client_system_devices *icsd = ipc_system_devices_from_selection(xis);
	return ipc_call_input_providers_enumerate(icsd->ipc_c, start, out_page);
}

static xrt_result_t
ipc_client_input_selection_set_preference(struct xrt_input_selection *xis,
                                          const struct xrt_input_selection_client *client,
                                          enum xrt_input_role_family family,
                                          const char *provider_id,
                                          bool persist)
{
	(void)client;
	struct ipc_client_system_devices *icsd = ipc_system_devices_from_selection(xis);
	struct ipc_input_provider_id id;
	memset(&id, 0, sizeof(id));
	if (provider_id != NULL) {
		const size_t len = strnlen(provider_id, sizeof(id.id));
		if (len >= sizeof(id.id)) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED; // cannot name a provider in a truncated id
		}
		memcpy(id.id, provider_id, len);
	}
	return ipc_call_input_set_provider_preference(icsd->ipc_c, (uint32_t)family, &id, persist);
}

static xrt_result_t
ipc_client_system_devices_feature_inc(struct xrt_system_devices *xsysd, enum xrt_device_feature_type type)
{
	struct ipc_client_system_devices *usysd = ipc_system_devices(xsysd);
	xrt_result_t xret;

	assert(type < XRT_DEVICE_FEATURE_MAX_ENUM);

	// If it wasn't zero nothing to do.
	if (!xrt_reference_inc_and_was_zero(&usysd->feature_use[type])) {
		return XRT_SUCCESS;
	}

	xret = ipc_call_system_devices_begin_feature(usysd->ipc_c, type);
	IPC_CHK_ALWAYS_RET(usysd->ipc_c, xret, "ipc_call_system_devices_begin_feature");
}

static xrt_result_t
ipc_client_system_devices_feature_dec(struct xrt_system_devices *xsysd, enum xrt_device_feature_type type)
{
	struct ipc_client_system_devices *usysd = ipc_system_devices(xsysd);
	xrt_result_t xret;

	assert(type < XRT_DEVICE_FEATURE_MAX_ENUM);

	// If it is not zero we are done.
	if (!xrt_reference_dec_and_is_zero(&usysd->feature_use[type])) {
		return XRT_SUCCESS;
	}

	xret = ipc_call_system_devices_end_feature(usysd->ipc_c, type);
	IPC_CHK_ALWAYS_RET(usysd->ipc_c, xret, "ipc_call_system_devices_end_feature");
}


static void
ipc_client_system_devices_destroy(struct xrt_system_devices *xsysd)
{
	struct ipc_client_system_devices *usysd = ipc_system_devices(xsysd);

	u_system_devices_close(&usysd->base.base);

	free(usysd);
}


/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_system_devices *
ipc_client_system_devices_create(struct ipc_connection *ipc_c)
{
	struct ipc_client_system_devices *icsd = U_TYPED_CALLOC(struct ipc_client_system_devices);
	icsd->base.base.get_roles = ipc_client_system_devices_get_roles;
	icsd->base.base.destroy = ipc_client_system_devices_destroy;
	icsd->base.base.feature_inc = ipc_client_system_devices_feature_inc;
	icsd->base.base.feature_dec = ipc_client_system_devices_feature_dec;
	icsd->ipc_c = ipc_c;
	// Holders / detach are service-side concerns (DIAG + the connection's own teardown).
	icsd->input_selection.enumerate = ipc_client_input_selection_enumerate;
	icsd->input_selection.set_preference = ipc_client_input_selection_set_preference;
	icsd->base.base.input_selection = &icsd->input_selection;

	return &icsd->base.base;
}
