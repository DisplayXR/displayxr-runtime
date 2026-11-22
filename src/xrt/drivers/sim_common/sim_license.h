// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A scripted licence state for the in-tree reference providers
 *         (ADR-054 N5): the hardware-free way to exercise the licence-state
 *         contract of `xrt/xrt_component_license.h` end to end.
 *
 * Header-only (static inline, no dependency beyond the xrt headers), so a
 * reference plug-in includes it without linking anything. It is a SCRIPT, not
 * a licence check: nothing here protects anything, and a real vendor's gate
 * lives in the vendor's own binary.
 *
 * Settings the provider reads through its host and hands to
 * @ref sim_license_load (once, at negotiate):
 *
 * | Key | Type | Effect |
 * |---|---|---|
 * | `LICENSE_SIM` | enum | unset = the provider reports no licence at all (`not_applicable`, the default: nothing
 * changes). `licensed` = licensed, source `hardware`. `trial` = trial, ends 2100-01-01. `expired` = a trial that ended
 * 2000-01-01. `unlicensed` = no entitlement | | `LICENSE_KEY` | secret | with `LICENSE_SIM` = `unlicensed` or
 * `expired`: equal to @ref SIM_LICENSE_TEST_KEY makes the state `licensed`, source `key` |
 *
 * `activate` accepts exactly @ref SIM_LICENSE_TEST_KEY (when `LICENSE_SIM` is
 * set) and flips the state to `licensed`, source `key`, for the life of the
 * process. By default the script keeps nothing on disk.
 *
 * A real provider keeps its activation itself, across processes; the script
 * can stand in for that with an optional LICENCE STORE: a provider that reads
 * a `LICENSE_STORE` setting (a file path) calls @ref sim_license_store_write
 * when `activate` accepts and @ref sim_license_store_read when it loads, so
 * an activation delivered in one process (the `displayxr://activate`
 * handler) is the licence state of the next (ADR-064 §5,
 * activation-deep-link.md §7). The file holds a fixed marker, never the key.
 */

#pragma once

#include "xrt/xrt_component_license.h"
#include "xrt/xrt_component_settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

//! The one token the scripted licence accepts (a setting value or an activation token).
#define SIM_LICENSE_TEST_KEY "DXR-SIM-TEST-KEY"
//! Where the scripted "unlicensed" state sends the user. Never fetched by the runtime.
#define SIM_LICENSE_PURCHASE_URL "https://example.invalid/displayxr/sim-licence"
#define SIM_LICENSE_TRIAL_END_UNIX_S 4102444800ll //!< 2100-01-01T00:00:00Z
#define SIM_LICENSE_EXPIRED_AT_UNIX_S 946684800ll //!< 2000-01-01T00:00:00Z

//! What a licence store file holds once an activation was accepted. Not the key.
#define SIM_LICENSE_STORE_MARKER "displayxr-sim-licence-v1 activated\n"
//! The optional third setting, for a provider that keeps a licence store.
#define SIM_LICENSE_STORE_SETTING_DESC                                                                                 \
	{                                                                                                              \
		"LICENSE_STORE", XRT_COMPONENT_SETTING_TYPE_STRING, "",                                                \
		    "file the scripted licence keeps an accepted activation in (unset = this process only)"            \
	}

//! The two settings a provider adds to its `describe_settings` table.
#define SIM_LICENSE_SETTING_DESCS                                                                                      \
	{"LICENSE_SIM", XRT_COMPONENT_SETTING_TYPE_ENUM, "",                                                           \
	 "scripted licence state: licensed|trial|expired|unlicensed (unset = no licence to report)"},                  \
	{                                                                                                              \
		"LICENSE_KEY", XRT_COMPONENT_SETTING_TYPE_SECRET, "",                                                  \
		    "licence key for the scripted licence (the fixed test key makes unlicensed/expired licensed)"      \
	}

//! The script's state, one per provider (a file-scope static in the plug-in).
struct sim_license
{
	uint32_t scripted; //!< XRT_COMPONENT_LICENSE_* from LICENSE_SIM; NOT_APPLICABLE = not scripted
	bool key_ok;       //!< LICENSE_KEY holds the test key
	bool activated;    //!< activate() accepted the test key in this process
};

//! Read the two settings' values (NULL / "" = unset) into @p c and forget any activation.
static inline void
sim_license_load(struct sim_license *c, const char *license_sim, const char *license_key)
{
	memset(c, 0, sizeof(*c));
	c->scripted = XRT_COMPONENT_LICENSE_NOT_APPLICABLE;
	if (license_sim != NULL) {
		if (strcmp(license_sim, "licensed") == 0) {
			c->scripted = XRT_COMPONENT_LICENSE_LICENSED;
		} else if (strcmp(license_sim, "trial") == 0) {
			c->scripted = XRT_COMPONENT_LICENSE_TRIAL;
		} else if (strcmp(license_sim, "expired") == 0) {
			c->scripted = XRT_COMPONENT_LICENSE_EXPIRED;
		} else if (strcmp(license_sim, "unlicensed") == 0) {
			c->scripted = XRT_COMPONENT_LICENSE_UNLICENSED;
		}
	}
	c->key_ok = license_key != NULL && strcmp(license_key, SIM_LICENSE_TEST_KEY) == 0;
}

//! Copy @p full into @p out within the size the caller announced (its struct_size stays its own).
static inline void
sim_license_write(struct xrt_component_license_state *out, const struct xrt_component_license_state *full)
{
	const uint32_t caller = out->struct_size;
	const size_t n = caller < sizeof(*full) ? caller : sizeof(*full);
	if (n < 2 * sizeof(uint32_t)) {
		return;
	}
	memcpy(out, full, n);
	out->struct_size = caller;
}

/*!
 * The `get_license_state` slot's body. False (nothing written) when the
 * script is off, which is "no licence to report".
 */
static inline bool
sim_license_get(const struct sim_license *c, struct xrt_component_license_state *out)
{
	if (c->scripted == XRT_COMPONENT_LICENSE_NOT_APPLICABLE || out == NULL) {
		return false;
	}
	struct xrt_component_license_state st;
	memset(&st, 0, sizeof(st));
	st.struct_size = (uint32_t)sizeof(st);
	st.state = c->scripted;
	switch (c->scripted) {
	case XRT_COMPONENT_LICENSE_LICENSED:
		st.source = XRT_COMPONENT_LICENSE_SOURCE_HARDWARE;
		(void)snprintf(st.message, sizeof(st.message), "Included with the simulated display");
		break;
	case XRT_COMPONENT_LICENSE_TRIAL:
		st.source = XRT_COMPONENT_LICENSE_SOURCE_TRIAL;
		st.expiry_unix_s = SIM_LICENSE_TRIAL_END_UNIX_S;
		(void)snprintf(st.message, sizeof(st.message), "Simulated trial");
		break;
	case XRT_COMPONENT_LICENSE_EXPIRED:
		st.source = XRT_COMPONENT_LICENSE_SOURCE_TRIAL;
		st.expiry_unix_s = SIM_LICENSE_EXPIRED_AT_UNIX_S;
		(void)snprintf(st.message, sizeof(st.message), "Simulated trial ended");
		break;
	default: (void)snprintf(st.message, sizeof(st.message), "No simulated licence"); break;
	}
	if (xrt_component_license_blocks(st.state)) {
		st.activation = XRT_COMPONENT_LICENSE_ACTIVATION_KEY | XRT_COMPONENT_LICENSE_ACTIVATION_DEEP_LINK;
		(void)snprintf(st.purchase_url, sizeof(st.purchase_url), "%s", SIM_LICENSE_PURCHASE_URL);
		if (c->key_ok || c->activated) {
			st.state = XRT_COMPONENT_LICENSE_LICENSED;
			st.source = XRT_COMPONENT_LICENSE_SOURCE_KEY;
			st.expiry_unix_s = 0;
			(void)snprintf(st.message, sizeof(st.message), "Simulated licence key accepted");
		}
	}
	sim_license_write(out, &st);
	return true;
}

/*!
 * The `activate` slot's body: accepts @ref SIM_LICENSE_TEST_KEY, refuses
 * everything else with one line. Returns true when the state changed to (or
 * already was) licensed by key.
 */
static inline bool
sim_license_activate(struct sim_license *c,
                     const char *token,
                     uint32_t token_size,
                     struct xrt_component_license_state *out_state,
                     char *out_error,
                     uint32_t error_size)
{
	const char *why = NULL;
	if (c->scripted == XRT_COMPONENT_LICENSE_NOT_APPLICABLE) {
		why = "the simulated licence is off (LICENSE_SIM is not set)";
	} else if (token == NULL || token_size != strlen(SIM_LICENSE_TEST_KEY) ||
	           memcmp(token, SIM_LICENSE_TEST_KEY, token_size) != 0) {
		why = "not the simulated test key";
	}
	if (why == NULL) {
		c->activated = true;
	} else if (out_error != NULL && error_size > 0) {
		(void)snprintf(out_error, error_size, "%s", why);
	}
	if (out_state != NULL) {
		(void)sim_license_get(c, out_state);
	}
	return why == NULL;
}

//! Does the licence store at @p path (NULL / "" = none) hold an accepted activation?
static inline bool
sim_license_store_read(const char *path)
{
	if (path == NULL || path[0] == '\0') {
		return false;
	}
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	char buf[sizeof(SIM_LICENSE_STORE_MARKER) + 1];
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	(void)fclose(f);
	return n == sizeof(SIM_LICENSE_STORE_MARKER) - 1 && memcmp(buf, SIM_LICENSE_STORE_MARKER, n) == 0;
}

//! Record an accepted activation in the licence store at @p path (NULL / "" = none). False when not written.
static inline bool
sim_license_store_write(const char *path)
{
	if (path == NULL || path[0] == '\0') {
		return false;
	}
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		return false;
	}
	const size_t n = fwrite(SIM_LICENSE_STORE_MARKER, 1, sizeof(SIM_LICENSE_STORE_MARKER) - 1, f);
	return fclose(f) == 0 && n == sizeof(SIM_LICENSE_STORE_MARKER) - 1;
}
