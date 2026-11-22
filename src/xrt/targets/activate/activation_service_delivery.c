// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The client half of a live activation (see activation_service_delivery.h).
 * @ingroup targets
 */

#include "activation_service_delivery.h"

#include <stdio.h>
#include <string.h>

enum activation_service_delivery
activation_service_deliver(const struct activation_service_transport *t,
                           uint32_t class_id,
                           const char *id,
                           const char *token,
                           uint32_t token_size,
                           int *out_result,
                           struct xrt_component_license_state *out_state,
                           char *err,
                           size_t err_cap)
{
	if (t == NULL || t->put == NULL || t->commit == NULL || t->poll == NULL || t->sleep_ms == NULL ||
	    t->chunk_max == 0 || token == NULL || token_size == 0) {
		return ACTIVATION_SERVICE_NOT_DELIVERED;
	}

	// Stage the token in chunks (it does not fit one IPC message). Nothing is
	// called before the commit, so every failure here is "not delivered". A
	// busy slot is retried for a while: nothing of ours has been taken yet.
	uint32_t waited_ms = 0;
	uint32_t offset = 0;
	while (offset < token_size) {
		uint32_t n = token_size - offset;
		if (n > t->chunk_max) {
			n = t->chunk_max;
		}
		enum activation_service_status st = ACTIVATION_SERVICE_STATUS_REFUSED;
		if (!t->put(t->userdata, offset, token + offset, n, &st)) {
			return ACTIVATION_SERVICE_NOT_DELIVERED; // the connection dropped before any commit
		}
		if (st == ACTIVATION_SERVICE_STATUS_BUSY && offset == 0) {
			if (waited_ms >= ACTIVATION_SERVICE_BUSY_TIMEOUT_MS) {
				return ACTIVATION_SERVICE_BUSY;
			}
			t->sleep_ms(t->userdata, ACTIVATION_SERVICE_POLL_STEP_MS);
			waited_ms += ACTIVATION_SERVICE_POLL_STEP_MS;
			continue;
		}
		if (st != ACTIVATION_SERVICE_STATUS_PENDING) {
			return ACTIVATION_SERVICE_NOT_DELIVERED; // the service discarded the staging
		}
		offset += n;
	}

	// The point of no return. A commit whose reply did not arrive may have
	// been accepted all the same (the service starts the delivery before it
	// answers), so a transport failure is an UNKNOWN outcome: delivering again
	// from this process could hand a one-time token to `activate` twice.
	enum activation_service_status st = ACTIVATION_SERVICE_STATUS_REFUSED;
	if (!t->commit(t->userdata, class_id, id, token_size, &st)) {
		return ACTIVATION_SERVICE_NO_ANSWER;
	}
	if (st != ACTIVATION_SERVICE_STATUS_PENDING) {
		return ACTIVATION_SERVICE_NOT_DELIVERED; // answered: refused before any delivery started
	}

	// From here the service owns the token: its answer is final, and no answer is "unknown".
	for (waited_ms = 0; waited_ms < ACTIVATION_SERVICE_ANSWER_TIMEOUT_MS;
	     waited_ms += ACTIVATION_SERVICE_POLL_STEP_MS) {
		struct xrt_component_license_state state;
		char line[XRT_COMPONENT_ACTIVATION_ERROR_MAX];
		int result = 0;
		xrt_component_license_state_init(&state);
		line[0] = '\0';
		st = ACTIVATION_SERVICE_STATUS_REFUSED;
		if (!t->poll(t->userdata, &st, &result, &state, line, sizeof(line))) {
			return ACTIVATION_SERVICE_NO_ANSWER; // the service went away mid-delivery
		}
		if (st == ACTIVATION_SERVICE_STATUS_PENDING) {
			t->sleep_ms(t->userdata, ACTIVATION_SERVICE_POLL_STEP_MS);
			continue;
		}
		if (st == ACTIVATION_SERVICE_STATUS_NOT_HOSTED) {
			return ACTIVATION_SERVICE_NOT_DELIVERED; // no live instance: the service called nothing
		}
		if (st == ACTIVATION_SERVICE_STATUS_DONE) {
			line[sizeof(line) - 1] = '\0';
			xrt_component_license_state_sanitize(&state);
			if (out_result != NULL) {
				*out_result = result;
			}
			if (out_state != NULL) {
				*out_state = state;
			}
			if (err != NULL && err_cap > 0) {
				(void)snprintf(err, err_cap, "%s", line);
			}
			return ACTIVATION_SERVICE_ANSWERED;
		}
		return ACTIVATION_SERVICE_NO_ANSWER; // an answer that is neither: the token was taken
	}
	return ACTIVATION_SERVICE_NO_ANSWER;
}
