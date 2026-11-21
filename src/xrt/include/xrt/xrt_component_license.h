// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The one licence-state contract every component class shares
 *         (ADR-054 N5, ADR-064 §5): the shape a component uses to REPORT
 *         whether this user is entitled to it, and to RECEIVE an activation
 *         token.
 *
 * Install is free; a vendor gates only the licence, and gates it inside its
 * own binary. The runtime carries NO licence logic: it asks the component
 * for a state, shows it, and treats `unlicensed` / `expired` like any other
 * provider that cannot serve (skipped, selection falls through to the next
 * candidate). It never decides the state, never parses an activation token
 * and never stores one.
 *
 * Every class - display processors (`xrt_plugin.h`), input providers
 * (`xrt_input_plugin.h`), conversion providers (`xrt_conversion_plugin.h`),
 * viewer-tracking providers (`xrt_tracking_plugin.h`) - appends the same two
 * optional vtable slots, typed here, under its own ABI rules (ADR-020: a
 * vtable slot is appended under `struct_size`, announced by a feature macro,
 * and does not move the class's API major). A component that fills neither
 * has no licence to report: its state reads `not_applicable` and nothing
 * about it changes.
 *
 * How this relates to the conversion class's earlier vocabulary (nothing
 * there is renamed or removed):
 *  - `XRT_CONV_LICENCE_*` (the licence CLASS of a capability) and
 *    `XRT_CONV_CAP_LICENSED_LOCAL_INSTALL` describe the TERMS a model ships
 *    under (open / commercial / research-only) and feed the
 *    `AllowResearchOnly` gate. They say nothing about this user.
 *  - `XRT_CONV_STATE_UNLICENSED` (== `XRT_DP_LIFT_MODULE_UNLICENSED`) is the
 *    per-capability readiness a conversion provider reports when ITS licence
 *    check refused one capability. It keeps working as is. A conversion
 *    provider whose licence state here is `unlicensed` or `expired` has every
 *    capability read `XRT_CONV_STATE_UNLICENSED` (`XRT_CONV_FAIL_LICENCE`),
 *    which is the mapping of this contract onto that one
 *    (@ref xrt_conv_state_apply_license in `xrt_conversion_plugin.h`).
 *
 * @ingroup xrt_iface
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @name Licence state (`xrt_component_license_state::state`).
 * Stable values (they cross every class's plug-in ABI and the satellite
 * channels). 0 is "nothing to report", so a zero-filled answer changes
 * nothing; an unknown value reads as 0.
 * @{
 */
#define XRT_COMPONENT_LICENSE_NOT_APPLICABLE 0u //!< free / no licence concept; behaves exactly as before
#define XRT_COMPONENT_LICENSE_LICENSED 1u       //!< entitled; `source` says how
#define XRT_COMPONENT_LICENSE_TRIAL 2u          //!< entitled for now; `expiry_unix_s` says until when, if known
#define XRT_COMPONENT_LICENSE_EXPIRED 3u        //!< a trial or term licence ended; the component does not serve
#define XRT_COMPONENT_LICENSE_UNLICENSED 4u     //!< no entitlement; the component does not serve
/*! @} */

/*!
 * @name Where the entitlement comes from (`xrt_component_license_state::source`). Optional.
 * @{
 */
#define XRT_COMPONENT_LICENSE_SOURCE_NONE 0u     //!< not stated
#define XRT_COMPONENT_LICENSE_SOURCE_HARDWARE 1u //!< the connected hardware entitles the user (no key, no purchase)
#define XRT_COMPONENT_LICENSE_SOURCE_KEY 2u      //!< a licence key / activation token
#define XRT_COMPONENT_LICENSE_SOURCE_TRIAL 3u    //!< a time-limited trial
#define XRT_COMPONENT_LICENSE_SOURCE_OEM 4u      //!< came with the machine
/*! @} */

/*!
 * @name How a licence can be activated (`xrt_component_license_state::activation`, bit flags).
 * @{
 */
//! The component accepts a pasted key: either a `secret` setting it declares
//! (`xrt_component_settings.h`) or the `activate` slot.
#define XRT_COMPONENT_LICENSE_ACTIVATION_KEY (1u << 0)
//! The vendor's checkout can hand a token back through a `displayxr://activate`
//! deep link, delivered to the `activate` slot.
#define XRT_COMPONENT_LICENSE_ACTIVATION_DEEP_LINK (1u << 1)
/*! @} */

#define XRT_COMPONENT_LICENSE_MESSAGE_MAX 128u //!< bytes, NUL included
#define XRT_COMPONENT_LICENSE_URL_MAX 256u     //!< bytes, NUL included

/*!
 * Largest activation token, in bytes, NUL included. 2048 because a signed
 * licence is larger than a product key (an RS256 JWT's signature alone is 342
 * characters), because it is the practical ceiling of a deep-link URL, and
 * because it fits one frame of the satellite channels.
 */
#define XRT_COMPONENT_ACTIVATION_TOKEN_MAX 2048u

//! Bytes of the error text `activate` may write, NUL included.
#define XRT_COMPONENT_ACTIVATION_ERROR_MAX 128u

/*!
 * What a component says about its licence. The CALLER zero-fills it and sets
 * @ref struct_size; the component writes only within that size, so the struct
 * can grow at its end. Strings are UTF-8, NUL-terminated, shown verbatim and
 * never parsed.
 */
struct xrt_component_license_state
{
	uint32_t struct_size;  //!< set by the caller: `sizeof` at ITS compile time
	uint32_t state;        //!< XRT_COMPONENT_LICENSE_*
	uint32_t source;       //!< XRT_COMPONENT_LICENSE_SOURCE_* (optional)
	uint32_t activation;   //!< XRT_COMPONENT_LICENSE_ACTIVATION_* flags (0 = neither)
	int64_t expiry_unix_s; //!< when a trial / term licence ends or ended, seconds since the Unix epoch UTC; 0 =
	                       //!< none / unknown
	char message[XRT_COMPONENT_LICENSE_MESSAGE_MAX];  //!< one short line for a human ("Trial, 12 days left"); "" =
	                                                  //!< none
	char purchase_url[XRT_COMPONENT_LICENSE_URL_MAX]; //!< the vendor's purchase / information page (https); "" =
	                                                  //!< none
};

/*!
 * The optional vtable slot through which a component reports its licence
 * state. Fill @p out (zero-filled, `struct_size` preset) and return true;
 * return false for "nothing to report" (`not_applicable`).
 *
 * Contract: answers FROM MEMORY - non-blocking, allocation-free, no I/O, no
 * network - because the runtime calls it on selection paths and, for a
 * conversion provider, with every readiness read. Takes no instance (like
 * `describe_settings`): it is called on a negotiated-only component by the
 * CLI, before `probe()`, and after it by the loader. A component whose
 * entitlement depends on hardware answers what it knows at that point and
 * the definitive state once `probe()` has run; the loader decides on the
 * answer it gets AFTER a successful `probe()`. Callable from any thread.
 *
 * Any gate behind the answer (a hardware check, a key check, a licence
 * server) lives in the component's own binary. The runtime acts on the
 * state; it never computes one.
 */
typedef bool (*xrt_component_get_license_state_fn_t)(struct xrt_component_license_state *out);

/*!
 * The optional vtable slot through which an activation token reaches the
 * component: a key the user pasted (`displayxr-cli <class> activate`), or the
 * token a vendor's checkout returned through a deep link.
 *
 * @p token is an opaque blob of @p token_size bytes (1 ..
 * @ref XRT_COMPONENT_ACTIVATION_TOKEN_MAX - 1), also NUL-terminated at
 * `token[token_size]`. The runtime does not interpret, log or store it;
 * keeping it (or what it unlocks) across processes is the component's
 * business.
 *
 * Returns true when the token was accepted, with the resulting state in
 * @p out_state (zero-filled, `struct_size` preset; may be NULL). Returns
 * false when it was refused, with one line of UTF-8 in @p out_error
 * (@p error_size bytes, may be NULL / 0) and, when @p out_state is not NULL,
 * the unchanged state.
 *
 * MAY BLOCK for a few seconds (a vendor may contact its licence server); it
 * is never called from a render, compositor or IPC-dispatch thread. No
 * instance, any thread, serialised by the caller per component.
 */
typedef bool (*xrt_component_activate_fn_t)(const char *token,
                                            uint32_t token_size,
                                            struct xrt_component_license_state *out_state,
                                            char *out_error,
                                            uint32_t error_size);

/*!
 * Does @p state keep a component from serving? True for `unlicensed` and
 * `expired` - the two states selection skips, falling through to the next
 * candidate. `not_applicable`, `licensed` and `trial` serve.
 */
static inline bool
xrt_component_license_blocks(uint32_t state)
{
	return state == XRT_COMPONENT_LICENSE_UNLICENSED || state == XRT_COMPONENT_LICENSE_EXPIRED;
}

//! Stable lower-case name of a state: "not_applicable", "licensed", "trial", "expired", "unlicensed".
static inline const char *
xrt_component_license_state_str(uint32_t state)
{
	switch (state) {
	case XRT_COMPONENT_LICENSE_LICENSED: return "licensed";
	case XRT_COMPONENT_LICENSE_TRIAL: return "trial";
	case XRT_COMPONENT_LICENSE_EXPIRED: return "expired";
	case XRT_COMPONENT_LICENSE_UNLICENSED: return "unlicensed";
	default: return "not_applicable";
	}
}

//! Stable lower-case name of a source: "hardware", "key", "trial", "oem"; "" when not stated.
static inline const char *
xrt_component_license_source_str(uint32_t source)
{
	switch (source) {
	case XRT_COMPONENT_LICENSE_SOURCE_HARDWARE: return "hardware";
	case XRT_COMPONENT_LICENSE_SOURCE_KEY: return "key";
	case XRT_COMPONENT_LICENSE_SOURCE_TRIAL: return "trial";
	case XRT_COMPONENT_LICENSE_SOURCE_OEM: return "oem";
	default: return "";
	}
}

//! Zero @p st and preset its `struct_size`: what a caller does before either slot.
static inline void
xrt_component_license_state_init(struct xrt_component_license_state *st)
{
	memset(st, 0, sizeof(*st));
	st->struct_size = (uint32_t)sizeof(*st);
}

/*!
 * Make a component's answer safe to use: restore `struct_size`, terminate
 * both strings, and read an unknown state / source as "nothing stated" and
 * unknown activation bits as clear. A third-party binary wrote this struct.
 */
static inline void
xrt_component_license_state_sanitize(struct xrt_component_license_state *st)
{
	st->struct_size = (uint32_t)sizeof(*st);
	st->message[sizeof(st->message) - 1] = '\0';
	st->purchase_url[sizeof(st->purchase_url) - 1] = '\0';
	if (st->state > XRT_COMPONENT_LICENSE_UNLICENSED) {
		st->state = XRT_COMPONENT_LICENSE_NOT_APPLICABLE;
	}
	if (st->source > XRT_COMPONENT_LICENSE_SOURCE_OEM) {
		st->source = XRT_COMPONENT_LICENSE_SOURCE_NONE;
	}
	st->activation &= XRT_COMPONENT_LICENSE_ACTIVATION_KEY | XRT_COMPONENT_LICENSE_ACTIVATION_DEEP_LINK;
}

/*!
 * Ask @p fn (a class's `get_license_state` slot, already `struct_size`-gated
 * by the caller; NULL = the component has none) and return the state, with
 * the absent-slot / false / unknown-value rule applied: @p out is always a
 * valid, sanitised struct, `not_applicable` unless the component said
 * otherwise.
 */
static inline uint32_t
xrt_component_license_query(xrt_component_get_license_state_fn_t fn, struct xrt_component_license_state *out)
{
	xrt_component_license_state_init(out);
	if (fn == NULL) {
		return XRT_COMPONENT_LICENSE_NOT_APPLICABLE;
	}
	if (!fn(out)) {
		xrt_component_license_state_init(out);
		return XRT_COMPONENT_LICENSE_NOT_APPLICABLE;
	}
	xrt_component_license_state_sanitize(out);
	return out->state;
}

#ifdef __cplusplus
}
#endif
