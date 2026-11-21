// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `displayxr://activate` handler: one link in, one outcome out
 *         (ADR-064 §5, `docs/specs/runtime/activation-deep-link.md`).
 *
 * This is the code a browser reaches when a page navigates to a
 * `displayxr://` link, so every byte of its input is attacker-controlled.
 * What it does, in this order and no other:
 *
 *  1. refuse to answer at all when too many links failed recently;
 *  2. parse the link with the strict parser (`util/u_activation_url.h`);
 *  3. consume the nonce from the per-user pending store
 *     (`util/u_activation_store.h`): spent by this first use whatever follows;
 *  4. check the optional `item` echo against what the nonce was begun for;
 *  5. hand the token to THE ONE component the nonce was begun for, through
 *     the delivery function, and wipe it;
 *  6. record nonce id + component + outcome (never the token, never the link).
 *
 * Nothing from the link selects the component: the pending record does. The
 * handler never opens, executes or forwards any part of the link, never
 * starts the service, never asks for elevation, and reports every outcome to
 * the caller, which shows it to the user (no silent activation).
 *
 * The delivery is a function pointer so the whole flow is unit-tested with a
 * scripted component; the shipped one is @ref activation_deliver_default
 * (activation_deliver.h).
 *
 * @ingroup targets
 */

#pragma once

#include "xrt/xrt_component_license.h"
#include "util/u_activation_url.h"
#include "util/u_activation_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! How a link ended. Stable names: @ref activation_outcome_str.
enum activation_outcome_kind
{
	ACTIVATION_OUTCOME_ACTIVATED = 0, //!< the component accepted the token
	ACTIVATION_OUTCOME_REFUSED,       //!< the component refused the token (its line is in `error`)
	ACTIVATION_OUTCOME_UNSUPPORTED,   //!< the component has no activation slot
	ACTIVATION_OUTCOME_NOT_LOADED,    //!< the component is not registered any more, or did not load
	ACTIVATION_OUTCOME_UNKNOWN,       //!< handed to the service, which did not report back in time
	ACTIVATION_OUTCOME_BUSY,          //!< the service was busy with another activation; not delivered
	ACTIVATION_OUTCOME_INVALID_LINK,  //!< the link is not a well-formed activation link (`detail` = why)
	ACTIVATION_OUTCOME_NOT_PENDING,   //!< no live pending activation for the nonce (`detail` = why)
	ACTIVATION_OUTCOME_ITEM_MISMATCH, //!< the link's `item` is not what the nonce was begun for
	ACTIVATION_OUTCOME_RATE_LIMITED,  //!< too many failed links recently; the link was not looked at
	ACTIVATION_OUTCOME_STORE_ERROR,   //!< the per-user store could not be located
};

//! What the delivery function reports.
enum activation_deliver_result
{
	ACTIVATION_DELIVER_OK = 0,
	ACTIVATION_DELIVER_REFUSED,
	ACTIVATION_DELIVER_UNSUPPORTED,
	ACTIVATION_DELIVER_NOT_LOADED,
	ACTIVATION_DELIVER_UNKNOWN,
	ACTIVATION_DELIVER_BUSY,
};

//! One link's outcome. Carries no token and no part of the link.
struct activation_outcome
{
	enum activation_outcome_kind kind;
	char detail[32];                                //!< the parser's / the store's reason, "" otherwise
	char cls[U_ACTIVATION_CLASS_MAX];               //!< the component's class verb, once the nonce resolved
	char id[U_ACTIVATION_ID_MAX];                   //!< the component's id, once the nonce resolved
	char nonce_id[U_ACTIVATION_NONCE_ID_CHARS + 1]; //!< loggable id of the nonce, when the link had one
	char via[12];                                   //!< "service" / "process" / "": where the token was delivered
	struct xrt_component_license_state licence;     //!< the component's state after the call (delivered outcomes)
	char error[XRT_COMPONENT_ACTIVATION_ERROR_MAX]; //!< the component's own line when it refused
	//! Set when the caller should show NOTHING (a flood is being ignored). The outcome is still logged.
	bool quiet;
};

/*!
 * Deliver @p token (NUL-terminated, @p token_size bytes, opaque) to component
 * @p id of class @p cls. Returns an @ref activation_deliver_result; writes the
 * component's state to @p out_state, its error line to @p err and where it
 * went ("service" / "process") to @p via. Must not keep, log or echo the token.
 */
typedef int (*activation_deliver_fn)(void *userdata,
                                     const char *cls,
                                     const char *id,
                                     const char *token,
                                     uint32_t token_size,
                                     struct xrt_component_license_state *out_state,
                                     char *err,
                                     size_t err_cap,
                                     char *via,
                                     size_t via_cap);

//! What a handler run is wired to. Zero-fill, then set what differs from the defaults.
struct activation_ops
{
	activation_deliver_fn deliver; //!< required
	void *userdata;
	const char *store_dir; //!< NULL = the user's own (u_activation_store_default_dir)
	int64_t now_unix_s;    //!< 0 = the wall clock
};

/*!
 * Handle one link. @p url may be NULL, empty, enormous or hostile; @p out is
 * always filled. Never throws the token anywhere but @p ops->deliver, and
 * wipes its own copy before returning.
 */
void
activation_handle_url(const char *url, const struct activation_ops *ops, struct activation_outcome *out);

//! "activated" / "refused" / "unsupported" / "not-loaded" / "unknown" / "busy" / "invalid-link" / "not-pending" /
//! "item-mismatch" / "rate-limited" / "store-error".
const char *
activation_outcome_str(enum activation_outcome_kind k);

//! Process exit code for an outcome: 0 only for ACTIVATED.
int
activation_outcome_exit_code(enum activation_outcome_kind k);

//! Human name of a class verb: "display processor", "input provider", "conversion provider", "tracking provider".
const char *
activation_class_noun(const char *cls);

/*!
 * The message a front end shows for @p o: a short @p title and a plain-text
 * @p body (UTF-8, no markup, '\n' line breaks). Text that came from a
 * component (its error line, its licence message) is included only when it is
 * well-formed text, and nothing from the link is ever included.
 */
void
activation_outcome_message(
    const struct activation_outcome *o, char *title, size_t title_cap, char *body, size_t body_cap);

#ifdef __cplusplus
}
#endif
