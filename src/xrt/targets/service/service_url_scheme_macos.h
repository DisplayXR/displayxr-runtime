// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS: keep the `displayxr://` URL scheme registered for every user
 *         (`docs/specs/runtime/activation-deep-link.md` §8).
 *
 * A URL scheme on macOS belongs to an app bundle that Launch Services knows,
 * and its database is PER USER. The handler bundle lives in the runtime's
 * `bin/`, a directory Launch Services never scans, so the `.pkg` postinstall
 * can only register it for whoever is at the console at install time: another
 * user of the machine, an install with nobody logged in (MDM, ssh) and a
 * rebuilt Launch Services database would all be left without a handler.
 *
 * The service is started in every user's GUI session by its LaunchAgent, so it
 * re-registers the bundle there, once per start. Only an INSTALLED service
 * does (its executable under `/Library/Application Support/DisplayXR/bin/`): a
 * dev build must not take the scheme from the installed handler.
 *
 * @ingroup targets
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//! Register the installed handler bundle for the current user. Cheap, idempotent, never fails the caller.
void
service_url_scheme_register_macos(void);

#ifdef __cplusplus
}
#endif
