// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The shipped delivery of the `displayxr://activate` handler (see
 *         activation_deliver.h).
 * @ingroup targets
 */

#include "activation_deliver.h"
#include "activation_service_delivery.h"

#include "xrt/xrt_config_os.h"
#include "os/os_time.h"

#include "target_component_loader.h"
#include "target_conv_status.h"
#include "target_input_plugin_loader.h"
#include "target_plugin_loader.h"
#include "target_tracking_plugin_loader.h"

#include <stdio.h>
#include <string.h>

#ifdef ACTIVATION_HAVE_IPC
#include "xrt/xrt_instance.h"
#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "ipc_client_generated.h"
#include "shared/ipc_protocol.h"

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#endif // ACTIVATION_HAVE_IPC

static uint32_t
class_id_of(const char *cls)
{
	if (strcmp(cls, "dp") == 0) {
		return TARGET_COMPONENT_CLASS_DISPLAY;
	}
	if (strcmp(cls, "input") == 0) {
		return TARGET_COMPONENT_CLASS_INPUT;
	}
	if (strcmp(cls, "conv") == 0) {
		return TARGET_COMPONENT_CLASS_CONVERSION;
	}
	if (strcmp(cls, "tracking") == 0) {
		return TARGET_COMPONENT_CLASS_TRACKING;
	}
	return TARGET_COMPONENT_CLASS_NONE;
}

static int
deliver_result_of(enum target_component_activate_result r)
{
	switch (r) {
	case TARGET_COMPONENT_ACTIVATE_OK: return ACTIVATION_DELIVER_OK;
	case TARGET_COMPONENT_ACTIVATE_REFUSED: return ACTIVATION_DELIVER_REFUSED;
	case TARGET_COMPONENT_ACTIVATE_UNSUPPORTED: return ACTIVATION_DELIVER_UNSUPPORTED;
	case TARGET_COMPONENT_ACTIVATE_BAD_TOKEN: return ACTIVATION_DELIVER_REFUSED;
	default: return ACTIVATION_DELIVER_NOT_LOADED;
	}
}

#ifdef ACTIVATION_HAVE_IPC

/*!
 * One DIAG connection to a service that is ALREADY running. Never starts it:
 * the handler is launched by a browser, and a service it started would live
 * in the browser's job (the rule of cli_ipc.c, #1966).
 */
static bool
diag_connect(struct ipc_connection *ipc_c)
{
	ipc_client_connection_disable_service_launch();
#ifdef XRT_OS_WINDOWS
	if (!WaitNamedPipeA("\\\\.\\pipe\\displayxr\\displayxr_comp_ipc", 1) &&
	    GetLastError() == ERROR_FILE_NOT_FOUND) {
		return false;
	}
#endif
#ifdef XRT_OS_LINUX_DESKTOP
	// A wedged service accepts and never answers (#2515): bound the handshake.
	ipc_client_connection_set_handshake_timeout_ms(5000);
#endif
	struct xrt_instance_info ii;
	memset(&ii, 0, sizeof(ii));
	(void)snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-activate");
	ii.app_info.declared_client_class = XRT_CLIENT_CLASS_DIAG;
	memset(ipc_c, 0, sizeof(*ipc_c));
	return ipc_client_connection_init(ipc_c, U_LOGGING_RAW, &ii) == XRT_SUCCESS;
}

static enum activation_service_status
status_of(uint32_t ipc_status)
{
	switch (ipc_status) {
	case IPC_COMPONENT_ACTIVATE_PENDING: return ACTIVATION_SERVICE_STATUS_PENDING;
	case IPC_COMPONENT_ACTIVATE_DONE: return ACTIVATION_SERVICE_STATUS_DONE;
	case IPC_COMPONENT_ACTIVATE_NOT_HOSTED: return ACTIVATION_SERVICE_STATUS_NOT_HOSTED;
	case IPC_COMPONENT_ACTIVATE_BUSY: return ACTIVATION_SERVICE_STATUS_BUSY;
	default: return ACTIVATION_SERVICE_STATUS_REFUSED; // NONE / BAD_REQUEST / UNSUPPORTED
	}
}

// The DIAG transport of activation_service_deliver(). Anything but
// XRT_SUCCESS is a transport failure: the call may or may not have arrived.

static bool
diag_put(void *ud, uint32_t offset, const char *bytes, uint32_t size, enum activation_service_status *st)
{
	struct ipc_component_activate_chunk chunk;
	memset(&chunk, 0, sizeof(chunk));
	if (size > IPC_COMPONENT_ACTIVATE_CHUNK_MAX) {
		return false;
	}
	chunk.offset = offset;
	chunk.size = size;
	memcpy(chunk.bytes, bytes, size);
	uint32_t status = IPC_COMPONENT_ACTIVATE_UNSUPPORTED;
	const xrt_result_t xret = ipc_call_system_component_activate_put((struct ipc_connection *)ud, &chunk, &status);
	memset(&chunk, 0, sizeof(chunk)); // the token does not linger on this stack
	*st = status_of(status);
	return xret == XRT_SUCCESS;
}

static bool
diag_commit(void *ud, uint32_t class_id, const char *id, uint32_t token_size, enum activation_service_status *st)
{
	struct ipc_component_activate_target target;
	memset(&target, 0, sizeof(target));
	target.component_class = class_id;
	target.token_size = token_size;
	(void)snprintf(target.id, sizeof(target.id), "%s", id);
	uint32_t status = IPC_COMPONENT_ACTIVATE_UNSUPPORTED;
	const xrt_result_t xret =
	    ipc_call_system_component_activate_commit((struct ipc_connection *)ud, &target, &status);
	*st = status_of(status);
	return xret == XRT_SUCCESS;
}

static bool
diag_poll(void *ud,
          enum activation_service_status *st,
          int *result,
          struct xrt_component_license_state *state,
          char *err,
          size_t err_cap)
{
	struct ipc_component_activate_result res;
	memset(&res, 0, sizeof(res));
	if (ipc_call_system_component_activate_poll((struct ipc_connection *)ud, &res) != XRT_SUCCESS) {
		return false;
	}
	*st = status_of(res.status);
	*result = (int)res.result;
	*state = res.state;
	res.error[sizeof(res.error) - 1] = '\0';
	(void)snprintf(err, err_cap, "%s", res.error);
	return true;
}

static void
diag_sleep_ms(void *ud, uint32_t ms)
{
	(void)ud;
	os_nanosleep((int64_t)ms * 1000 * 1000);
}

static enum activation_service_delivery
deliver_via_service(uint32_t class_id,
                    const char *id,
                    const char *token,
                    uint32_t token_size,
                    int *out_result,
                    struct xrt_component_license_state *out_state,
                    char *err,
                    size_t err_cap)
{
	struct ipc_connection ipc_c;
	if (!diag_connect(&ipc_c)) {
		return ACTIVATION_SERVICE_NOT_DELIVERED;
	}
	const struct activation_service_transport t = {
	    .userdata = &ipc_c,
	    .chunk_max = IPC_COMPONENT_ACTIVATE_CHUNK_MAX,
	    .put = diag_put,
	    .commit = diag_commit,
	    .poll = diag_poll,
	    .sleep_ms = diag_sleep_ms,
	};
	int result = TARGET_COMPONENT_ACTIVATE_NOT_LOADED;
	const enum activation_service_delivery ret =
	    activation_service_deliver(&t, class_id, id, token, token_size, &result, out_state, err, err_cap);
	if (ret == ACTIVATION_SERVICE_ANSWERED) {
		*out_result = deliver_result_of((enum target_component_activate_result)result);
	}
	ipc_client_connection_fini(&ipc_c);
	return ret;
}

//! After an in-process delivery: ask a running service to re-select the class. Best effort.
static void
poke_reselect(uint32_t class_id)
{
	struct ipc_connection ipc_c;
	if (!diag_connect(&ipc_c)) {
		return;
	}
	(void)ipc_call_system_request_component_reselect(&ipc_c, class_id);
	ipc_client_connection_fini(&ipc_c);
}

#endif // ACTIVATION_HAVE_IPC

int
activation_deliver_default(void *userdata,
                           const char *cls,
                           const char *id,
                           const char *token,
                           uint32_t token_size,
                           struct xrt_component_license_state *out_state,
                           char *err,
                           size_t err_cap,
                           char *via,
                           size_t via_cap)
{
	(void)userdata;
	xrt_component_license_state_init(out_state);
	if (err != NULL && err_cap > 0) {
		err[0] = '\0';
	}
	if (via != NULL && via_cap > 0) {
		via[0] = '\0';
	}
	const uint32_t class_id = class_id_of(cls);
	if (class_id == TARGET_COMPONENT_CLASS_NONE || token == NULL || token_size == 0 ||
	    token_size >= XRT_COMPONENT_ACTIVATION_TOKEN_MAX || strlen(token) != token_size) {
		return ACTIVATION_DELIVER_NOT_LOADED;
	}

#ifdef ACTIVATION_HAVE_IPC
	int svc_result = ACTIVATION_DELIVER_UNKNOWN;
	switch (deliver_via_service(class_id, id, token, token_size, &svc_result, out_state, err, err_cap)) {
	case ACTIVATION_SERVICE_ANSWERED: (void)snprintf(via, via_cap, "service"); return svc_result;
	// The token may have reached the component (no reply to the commit, or no
	// answer after it): never delivered a second time from here.
	case ACTIVATION_SERVICE_NO_ANSWER: (void)snprintf(via, via_cap, "service"); return ACTIVATION_DELIVER_UNKNOWN;
	case ACTIVATION_SERVICE_BUSY: return ACTIVATION_DELIVER_BUSY;
	default: break; // PROVABLY nothing was called: this process delivers
	}
#endif

	(void)snprintf(via, via_cap, "process");
	enum target_component_activate_result r = TARGET_COMPONENT_ACTIVATE_NOT_LOADED;
	switch (class_id) {
	case TARGET_COMPONENT_CLASS_DISPLAY: r = target_plugin_activate(id, token, out_state, err, err_cap); break;
	case TARGET_COMPONENT_CLASS_INPUT: r = target_input_plugin_activate(id, token, out_state, err, err_cap); break;
	case TARGET_COMPONENT_CLASS_CONVERSION: r = target_conv_activate(id, token, out_state, err, err_cap); break;
	case TARGET_COMPONENT_CLASS_TRACKING:
		r = target_tracking_plugin_activate(id, token, out_state, err, err_cap);
		break;
	default: break;
	}
#ifdef ACTIVATION_HAVE_IPC
	if (r == TARGET_COMPONENT_ACTIVATE_OK) {
		poke_reselect(class_id);
	}
#endif
	return deliver_result_of(r);
}
