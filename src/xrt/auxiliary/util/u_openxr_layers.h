// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Find a registered OpenXR implicit API layer by name (diagnostics only).
 *
 * The Khronos loader loads every enabled implicit layer into EVERY OpenXR app,
 * whichever runtime is active. A third-party layer can therefore act on shared
 * hardware behind DisplayXR's back — the motivating case is a hand-tracking
 * vendor's layer that resets its tracking service to head-mounted mode each
 * time any OpenXR app starts, which swaps left/right for a desk-mounted
 * sensor. DisplayXR cannot (and must not) unload such a layer; it can only
 * tell the user it is there and how to turn it off. This is that lookup.
 *
 * Read-only and advisory: nothing here ever changes the registry or the
 * environment, and no caller may fail on its answer.
 *
 * Windows: `HKLM` (64-bit view) and `HKCU`
 * `Software\Khronos\OpenXR\1\ApiLayers\Implicit` — value name = manifest path,
 * `REG_DWORD` 0 = enabled. Elsewhere: not implemented (always "not found").
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Ultraleap's hand-tracking API layer. Its installer registers it as an
 * IMPLICIT layer, so the loader puts it in every OpenXR app — DisplayXR ones
 * included — and each load switches the Ultraleap tracking service back to
 * Head-mounted mode. For a Leap on the desk (what DisplayXR's `ultraleap`
 * input provider expects) that swaps the hands and turns sideways motion
 * into depth.
 */
#define U_OPENXR_LAYER_ULTRALEAP_HAND_TRACKING "XR_APILAYER_ULTRALEAP_hand_tracking"

//! The variable its manifest declares as `disable_environment` (used when the manifest could not say).
#define U_OPENXR_LAYER_ULTRALEAP_DISABLE_ENV "DISABLE_XR_APILAYER_ULTRALEAP_HAND_TRACKING_1"

//! What @ref u_openxr_implicit_layer_find learned about one layer.
struct u_openxr_implicit_layer
{
	//! A registered implicit-layer manifest declares the name.
	bool found;
	//! Its registry value is 0 (the loader's "enabled").
	bool enabled_in_registry;
	//! Its `disable_environment` variable is set in this process.
	bool disabled_by_env;
	//! It declares an `enable_environment` variable that is NOT set (the loader skips it).
	bool not_enabled_by_env;
	//! found && enabled_in_registry && !disabled_by_env && !not_enabled_by_env: the loader will load it.
	bool active;
	//! "HKLM" or "HKCU".
	char hive[8];
	char manifest_path[512];
	//! The manifest's `disable_environment` variable name ("" if it declares none).
	char disable_env[128];
	char enable_env[128];
};

/*!
 * Does this manifest text (an OpenXR API-layer JSON manifest) declare
 * @p layer_name? On a match, copies its `disable_environment` /
 * `enable_environment` variable names (empty when absent). Pure, so it is
 * unit-tested on every platform.
 */
bool
u_openxr_layer_manifest_declares(const char *json_text,
                                 const char *layer_name,
                                 char *out_disable_env,
                                 size_t disable_env_size,
                                 char *out_enable_env,
                                 size_t enable_env_size);

/*!
 * Look for a registered implicit layer named @p layer_name. Returns
 * `out->found`. When several registrations declare it, an active one wins
 * (the one the loader will load). Never fails loudly; a missing key, an
 * unreadable manifest or a non-Windows build is simply "not found".
 */
bool
u_openxr_implicit_layer_find(const char *layer_name, struct u_openxr_implicit_layer *out);

/*!
 * One-line advisory for an ACTIVE Ultraleap layer (@p layer from
 * @ref u_openxr_implicit_layer_find): what it does to a desk-mounted Leap and
 * the workaround. Shared by `displayxr-cli` and the `ultraleap` provider so
 * the two say the same thing.
 */
void
u_openxr_layer_ultraleap_advice(const struct u_openxr_implicit_layer *layer, char *buf, size_t size);

#ifdef __cplusplus
}
#endif
