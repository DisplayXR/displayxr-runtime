// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The client half of a live activation through the running service:
 *         stage the token, commit it, wait for the component's answer
 *         (`docs/specs/runtime/activation-deep-link.md` §6).
 *
 * Pure over an injected transport so the one rule that matters is unit-tested
 * without a service: a token may be single use at the vendor's licence
 * server, so the caller may deliver it somewhere else ONLY when this function
 * can say that the service called nothing
 * (@ref ACTIVATION_SERVICE_NOT_DELIVERED). The commit call is the point of no
 * return: once it has been SENT, the service may have taken the token whether
 * or not its reply arrived, so a transport failure on commit (or any later
 * one) is @ref ACTIVATION_SERVICE_NO_ANSWER, never "not delivered".
 *
 * The shipped transport is the DIAG IPC of activation_deliver.c.
 *
 * @ingroup targets
 */

#pragma once

#include "xrt/xrt_component_license.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! What the service says to a put / commit / poll (the IPC_COMPONENT_ACTIVATE_* values, transport-neutral).
enum activation_service_status
{
	ACTIVATION_SERVICE_STATUS_PENDING = 0, //!< put: staged; commit: taken; poll: the component is being asked
	ACTIVATION_SERVICE_STATUS_DONE,        //!< poll: the component answered
	ACTIVATION_SERVICE_STATUS_NOT_HOSTED,  //!< poll: no live instance; the service called nothing
	ACTIVATION_SERVICE_STATUS_BUSY,        //!< another activation holds the slot; nothing was taken
	ACTIVATION_SERVICE_STATUS_REFUSED,     //!< anything else: the request was discarded, nothing was called
};

//! How a delivery through the service ended.
enum activation_service_delivery
{
	ACTIVATION_SERVICE_NOT_DELIVERED = 0, //!< PROVABLY nothing was called: the caller may deliver elsewhere
	ACTIVATION_SERVICE_ANSWERED,          //!< the component was asked; the result is final
	ACTIVATION_SERVICE_NO_ANSWER,         //!< the token MAY have been delivered; the outcome is unknown
	ACTIVATION_SERVICE_BUSY,              //!< the slot stayed busy; nothing was taken
};

/*!
 * One connection to the service. Each call returns false for a TRANSPORT
 * failure (the request may or may not have arrived) and true when the service
 * answered, with its answer in the out parameters.
 */
struct activation_service_transport
{
	void *userdata;
	uint32_t chunk_max; //!< largest put, in bytes
	bool (*put)(
	    void *userdata, uint32_t offset, const char *bytes, uint32_t size, enum activation_service_status *st);
	bool (*commit)(
	    void *userdata, uint32_t class_id, const char *id, uint32_t token_size, enum activation_service_status *st);
	//! On DONE: @p result is a `target_component_activate_result`, with the component's state and error line.
	bool (*poll)(void *userdata,
	             enum activation_service_status *st,
	             int *result,
	             struct xrt_component_license_state *state,
	             char *err,
	             size_t err_cap);
	void (*sleep_ms)(void *userdata, uint32_t ms);
};

//! Bounds of @ref activation_service_deliver, in milliseconds.
#define ACTIVATION_SERVICE_ANSWER_TIMEOUT_MS 45000u //!< the component's answer
#define ACTIVATION_SERVICE_BUSY_TIMEOUT_MS 10000u   //!< a busy slot is retried this long
#define ACTIVATION_SERVICE_POLL_STEP_MS 100u

/*!
 * Stage @p token over @p t, commit it for component @p id of class
 * @p class_id, and wait for the answer. @p out_result (a
 * `target_component_activate_result`), @p out_state and @p err are written
 * only for @ref ACTIVATION_SERVICE_ANSWERED.
 */
enum activation_service_delivery
activation_service_deliver(const struct activation_service_transport *t,
                           uint32_t class_id,
                           const char *id,
                           const char *token,
                           uint32_t token_size,
                           int *out_result,
                           struct xrt_component_license_state *out_state,
                           char *err,
                           size_t err_cap);

#ifdef __cplusplus
}
#endif
