// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The per-family input-provider precedence (ADR-034 Amendment 5,
 *         #2181): pure, header-only, shared by the plug-in loader's policy
 *         module and the arbiter's per-client walk (whose tests compile the
 *         arbiter standalone, without the loader).
 * @ingroup target_common
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Where a family's effective preference came from (@ref target_input_resolve_preference).
enum target_input_pref_source
{
	TARGET_INPUT_PREF_NONE = 0,         //!< default walk (ProbeOrder)
	TARGET_INPUT_PREF_APP_PIN = 1,      //!< the user's per-app pin
	TARGET_INPUT_PREF_REQUEST = 2,      //!< the client's non-persisted session request
	TARGET_INPUT_PREF_USER_DEFAULT = 3, //!< `PreferredPlugin` (already priority 0 in the global rank)
};

/*!
 * Everything the per-family precedence needs, gathered by the caller. A
 * `*_claims` flag says whether that candidate supplies THIS family — a
 * preference never applies to a family its provider does not claim (ADR-034
 * Amendment 5 decision 2), so an unclaimed level falls through to the next.
 */
struct target_input_pref_inputs
{
	//! `DXR_INPUT_PROVIDERS=0` or `ForceQwerty`: no provider is resident; every pin is moot.
	bool providers_disabled;
	//! HKLM `InputProviders\Policy\Lock` = 1: the per-user pin and the app's request are ignored.
	bool lock;
	const char *app_pin; //!< NULL / "" = none
	bool app_pin_claims;
	const char *request; //!< NULL / "" = none
	bool request_claims;
	const char *user_default; //!< NULL / "" = none
	bool user_default_claims;
};

/*!
 * The precedence of ADR-034 Amendment 5 decision 3 for ONE family, pure:
 * `Policy\Lock` -> per-app pin -> session request -> `PreferredPlugin` ->
 * ProbeOrder, with `DXR_INPUT_PROVIDERS=0` / `ForceQwerty` beating all. Writes
 * the winning id to @p out ("" for NONE) and returns its source. Presence is
 * NOT decided here: the arbiter hoists the winner and its walk still skips it
 * while absent.
 */
static inline enum target_input_pref_source
target_input_resolve_preference(const struct target_input_pref_inputs *in, char *out, size_t cap)
{
	if (out != NULL && cap > 0) {
		out[0] = '\0';
	}
	if (in == NULL || in->providers_disabled) {
		return TARGET_INPUT_PREF_NONE;
	}
	const char *id = NULL;
	enum target_input_pref_source src = TARGET_INPUT_PREF_NONE;
	// Under Policy\Lock the per-user pin and the app's own request are both
	// invisible: the machine's PreferredPlugin (which the caller read with the
	// lock applied) and ProbeOrder decide.
	if (!in->lock && in->app_pin != NULL && in->app_pin[0] != '\0' && in->app_pin_claims) {
		id = in->app_pin;
		src = TARGET_INPUT_PREF_APP_PIN;
	} else if (!in->lock && in->request != NULL && in->request[0] != '\0' && in->request_claims) {
		id = in->request;
		src = TARGET_INPUT_PREF_REQUEST;
	} else if (in->user_default != NULL && in->user_default[0] != '\0' && in->user_default_claims) {
		id = in->user_default;
		src = TARGET_INPUT_PREF_USER_DEFAULT;
	}
	if (id != NULL && out != NULL && cap > 0) {
		snprintf(out, cap, "%s", id);
	}
	return src;
}

//! Stable lower-case name of a @ref target_input_pref_source ("none", "app_pin", "request", "user_default").
static inline const char *
target_input_pref_source_str(enum target_input_pref_source source)
{
	switch (source) {
	case TARGET_INPUT_PREF_APP_PIN: return "app_pin";
	case TARGET_INPUT_PREF_REQUEST: return "request";
	case TARGET_INPUT_PREF_USER_DEFAULT: return "user_default";
	case TARGET_INPUT_PREF_NONE:
	default: return "none";
	}
}

#ifdef __cplusplus
}
#endif
