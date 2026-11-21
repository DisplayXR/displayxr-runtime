// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The shipped delivery of the `displayxr://activate` handler (see
 *         activation_deliver.h).
 * @ingroup targets
 */

#include "activation_deliver.h"

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

//! How long the service is given to report the component's answer.
#define SERVICE_ANSWER_TIMEOUT_MS 45000u
//! How long a BUSY service (another activation in progress) is retried.
#define SERVICE_BUSY_TIMEOUT_MS 10000u
#define POLL_STEP_MS 100u

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

enum service_delivery
{
	SERVICE_NOT_DELIVERED = 0, //!< no service / no live instance / no such path: nothing was called
	SERVICE_ANSWERED,          //!< the component was asked; the result is final
	SERVICE_NO_ANSWER,         //!< the token was taken and no answer came in time
	SERVICE_BUSY,              //!< another activation held the slot for too long; nothing was taken
};

static enum service_delivery
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
		return SERVICE_NOT_DELIVERED;
	}
	enum service_delivery ret = SERVICE_NOT_DELIVERED;

	// Stage the token in chunks (it does not fit one IPC message). A busy
	// slot is retried for a while: nothing of ours has been taken yet.
	uint32_t waited_ms = 0;
	uint32_t offset = 0;
	while (offset < token_size) {
		struct ipc_component_activate_chunk chunk;
		memset(&chunk, 0, sizeof(chunk));
		chunk.offset = offset;
		chunk.size = token_size - offset;
		if (chunk.size > IPC_COMPONENT_ACTIVATE_CHUNK_MAX) {
			chunk.size = IPC_COMPONENT_ACTIVATE_CHUNK_MAX;
		}
		const uint32_t n = chunk.size;
		memcpy(chunk.bytes, token + offset, n);
		uint32_t status = IPC_COMPONENT_ACTIVATE_UNSUPPORTED;
		const xrt_result_t xret = ipc_call_system_component_activate_put(&ipc_c, &chunk, &status);
		memset(&chunk, 0, sizeof(chunk)); // the token does not linger on this stack
		if (xret != XRT_SUCCESS) {
			break; // the connection dropped: not delivered
		}
		if (status == IPC_COMPONENT_ACTIVATE_BUSY && offset == 0) {
			if (waited_ms >= SERVICE_BUSY_TIMEOUT_MS) {
				ret = SERVICE_BUSY;
				break;
			}
			os_nanosleep((int64_t)POLL_STEP_MS * 1000 * 1000);
			waited_ms += POLL_STEP_MS;
			continue;
		}
		if (status != IPC_COMPONENT_ACTIVATE_PENDING) {
			break; // UNSUPPORTED / BAD_REQUEST / a lost staging: the service discarded it
		}
		offset += n;
	}
	if (offset < token_size) {
		ipc_client_connection_fini(&ipc_c);
		return ret; // NOT_DELIVERED, or BUSY
	}

	struct ipc_component_activate_target target;
	memset(&target, 0, sizeof(target));
	target.component_class = class_id;
	target.token_size = token_size;
	(void)snprintf(target.id, sizeof(target.id), "%s", id);
	uint32_t status = IPC_COMPONENT_ACTIVATE_UNSUPPORTED;
	if (ipc_call_system_component_activate_commit(&ipc_c, &target, &status) != XRT_SUCCESS ||
	    status != IPC_COMPONENT_ACTIVATE_PENDING) {
		// Refused before any delivery started (the service wiped the staging).
		ipc_client_connection_fini(&ipc_c);
		return SERVICE_NOT_DELIVERED;
	}

	// From here the service owns the token: its answer is final, and no answer is "unknown".
	ret = SERVICE_NO_ANSWER;
	for (waited_ms = 0; waited_ms < SERVICE_ANSWER_TIMEOUT_MS; waited_ms += POLL_STEP_MS) {
		struct ipc_component_activate_result res;
		memset(&res, 0, sizeof(res));
		if (ipc_call_system_component_activate_poll(&ipc_c, &res) != XRT_SUCCESS) {
			break; // the service went away mid-delivery
		}
		if (res.status == IPC_COMPONENT_ACTIVATE_PENDING) {
			os_nanosleep((int64_t)POLL_STEP_MS * 1000 * 1000);
			continue;
		}
		if (res.status == IPC_COMPONENT_ACTIVATE_NOT_HOSTED) {
			ret = SERVICE_NOT_DELIVERED; // no live instance: the service called nothing
		} else if (res.status == IPC_COMPONENT_ACTIVATE_DONE) {
			ret = SERVICE_ANSWERED;
			*out_result = deliver_result_of((enum target_component_activate_result)res.result);
			*out_state = res.state;
			xrt_component_license_state_sanitize(out_state);
			res.error[sizeof(res.error) - 1] = '\0';
			if (err != NULL && err_cap > 0) {
				(void)snprintf(err, err_cap, "%s", res.error);
			}
		}
		break;
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
	case SERVICE_ANSWERED: (void)snprintf(via, via_cap, "service"); return svc_result;
	case SERVICE_NO_ANSWER: (void)snprintf(via, via_cap, "service"); return ACTIVATION_DELIVER_UNKNOWN;
	case SERVICE_BUSY: return ACTIVATION_DELIVER_BUSY;
	default: break; // nothing was delivered: this process delivers
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
