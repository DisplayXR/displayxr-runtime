// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The service's live-activation slot (ADR-064 §5,
 *         `docs/specs/runtime/activation-deep-link.md` §6): hands an
 *         activation token to the component instance that is LIVE in this
 *         service, so it learns its new licence state without a restart.
 *
 * The deep-link handler (`displayxr-activate`, `displayxr-cli activate-url`)
 * validates the link and consumes the pending nonce in ITS process; what
 * reaches this module is "this token, for this component", over three DIAG
 * calls (`system_component_activate_put|commit|poll`). It is one slot:
 *
 *     IDLE -> STAGING (put ... put) -> RUNNING (commit) -> DONE (poll) -> IDLE
 *
 *  - ONE activation at a time, service-wide. A second caller gets BUSY and
 *    nothing of its is taken. A component's `activate` is "serialised by the
 *    caller per component"; one slot is that, trivially.
 *  - The component's `activate` MAY BLOCK (a licence server), so it runs on a
 *    worker thread of this module: never on an IPC thread, the render thread
 *    or the component-watch thread. The three DIAG calls only move state.
 *  - The token lives in the slot's buffer from the first put to the end of
 *    the delivery, and is zeroed then, on a discarded staging, and when the
 *    staging connection goes away. It is never logged.
 *  - Nothing here decides anything about a licence: the delivery function
 *    calls the component's own slot and reports what it said.
 *
 * The delivery itself is a function the service supplies (@ref
 * service_component_activate_deliver_fn; `main.c` routes by class), so this
 * state machine is unit-tested with a scripted one.
 *
 * @ingroup targets_service
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_component_license_state;

/*!
 * What the three calls answer. The values ARE `enum
 * ipc_component_activate_status` (ipc_protocol.h; asserted equal where the
 * service wires the two together): this header stays free of the IPC headers
 * so the slot is unit-tested on its own.
 */
enum service_component_activate_status
{
	SERVICE_COMPONENT_ACTIVATE_NONE = 0,        //!< nothing staged or running for this connection
	SERVICE_COMPONENT_ACTIVATE_PENDING = 1,     //!< staged / the component is being asked
	SERVICE_COMPONENT_ACTIVATE_DONE = 2,        //!< the component answered
	SERVICE_COMPONENT_ACTIVATE_NOT_HOSTED = 3,  //!< no live instance; the token was not delivered
	SERVICE_COMPONENT_ACTIVATE_BUSY = 4,        //!< another activation is in progress
	SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST = 5, //!< malformed; the staging was discarded
	SERVICE_COMPONENT_ACTIVATE_UNSUPPORTED = 6, //!< the slot is not armed
};

//! What a delivery did. NOT_HOSTED = no live instance here and nothing was called.
enum service_component_activate_delivery
{
	SERVICE_COMPONENT_ACTIVATE_DELIVERY_CALLED =
	    0, //!< the component's slot was called (or it has none): see result
	SERVICE_COMPONENT_ACTIVATE_DELIVERY_NOT_HOSTED, //!< the token went nowhere
};

/*!
 * Deliver @p token (NUL-terminated, @p token_size bytes) to the live instance
 * of component @p id of class @p class_id (`TARGET_COMPONENT_CLASS_*`). Runs
 * on the module's worker thread and may block. On DELIVERY_CALLED, @p out_result is
 * an `enum target_component_activate_result`, @p out_state the component's
 * state after the call and @p err its one line. Must not keep or log the token.
 */
typedef enum service_component_activate_delivery (*service_component_activate_deliver_fn)(
    void *userdata,
    uint32_t class_id,
    const char *id,
    const char *token,
    uint32_t token_size,
    uint32_t *out_result,
    struct xrt_component_license_state *out_state,
    char *err,
    size_t err_cap);

/*!
 * Arm the slot with @p deliver. Idempotent; false when its lock could not be
 * created (the DIAG calls then answer UNSUPPORTED and the handler delivers in
 * its own process).
 */
bool
service_component_activate_start(service_component_activate_deliver_fn deliver, void *userdata);

/*!
 * Disarm: refuse new work, give a running delivery up to @p grace_ms to
 * finish, and wipe the token buffer. A delivery still blocked in a component
 * after the grace is abandoned (its thread is left to the exiting process).
 */
void
service_component_activate_stop(uint32_t grace_ms);

//! `system_component_activate_put`: returns an `enum service_component_activate_status`.
uint32_t
service_component_activate_put(uint64_t owner, uint32_t offset, const uint8_t *bytes, uint32_t size);

//! `system_component_activate_commit`: returns an `enum service_component_activate_status`.
uint32_t
service_component_activate_commit(uint64_t owner, uint32_t class_id, const char *id, uint32_t token_size);

/*!
 * `system_component_activate_poll`: the slot's status as @p owner sees it.
 * On DONE, @p out_result (an `enum target_component_activate_result`),
 * @p out_state and @p err carry the component's answer, which is handed out
 * once (the slot is then free). Any out pointer may be NULL.
 */
uint32_t
service_component_activate_poll(
    uint64_t owner, uint32_t *out_result, struct xrt_component_license_state *out_state, char *err, size_t err_cap);

//! The connection @p owner closed: drop what it staged (a running delivery finishes; its answer is dropped).
void
service_component_activate_client_gone(uint64_t owner);

#ifdef __cplusplus
}
#endif
