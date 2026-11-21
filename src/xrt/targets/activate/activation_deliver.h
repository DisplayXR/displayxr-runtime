// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The shipped delivery of the `displayxr://activate` handler: gets an
 *         activation token to the component's `activate` slot, exactly once
 *         (`docs/specs/runtime/activation-deep-link.md` §6).
 *
 * A token may be single-use at the vendor's licence server, so a link results
 * in AT MOST ONE call of the component's `activate`:
 *
 *  1. When a service is running and hosts a LIVE instance of the component
 *     (a conversion provider, in-service or in its satellite host; the active
 *     display processor; a resident input provider), the service calls that
 *     instance's slot (DIAG `system_component_activate_put|commit|poll`), so
 *     the running instance learns its new state and selection follows.
 *  2. Otherwise (no service, a service without a live instance, the
 *     tracking class) the component is loaded here, negotiate only, and its
 *     slot is called in this process. A component keeps its activation
 *     across processes itself; the service is then asked to re-select the
 *     class (`system_request_component_reselect`).
 *
 * Step 2 runs only when step 1 reported that nothing was delivered. Once the
 * service has taken the token, its answer is final: a timeout is reported as
 * "unknown", never retried here. The service is never started by this code.
 *
 * @ingroup targets
 */

#pragma once

#include "activation_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

//! The shipped @ref activation_deliver_fn. @p userdata is unused.
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
                           size_t via_cap);

/*!
 * Raise the platform's minimal native message for @p o (Windows: a message
 * box; desktop Linux: a desktop notification through `notify-send`, else a
 * `zenity` / `kdialog` box, each run by absolute path with an argument vector,
 * never a shell). Nothing from the link is in the text. False when no way to
 * show a message exists on this machine. macOS shows its own alert from the
 * handler app bundle and has no implementation here.
 */
bool
activation_notify(const struct activation_outcome *o);

//! Can @ref activation_notify show something on this machine? Checked BEFORE a link is handled.
bool
activation_notify_available(void);

#ifdef __cplusplus
}
#endif
