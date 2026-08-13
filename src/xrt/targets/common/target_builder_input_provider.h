// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared builder helper: add input-provider motion controllers.
 *
 * Runs the input-provider plug-in discovery (ADR-034) and, when a
 * provider claims the system, adds its devices to the system-device list
 * and assigns the left/right hand roles. Call BEFORE
 * `t_builder_add_qwerty_input()` — qwerty only claims a hand role the
 * provider left empty (arbitration contract:
 * `docs/specs/runtime/input-provider-discovery.md` §4).
 *
 * Issue #823.
 *
 * @ingroup target_common
 */

#pragma once

#include "xrt/xrt_results.h"

#include "util/u_logging.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_system_devices;
struct u_builder_roles_helper;

/*!
 * Discover the active input provider and add its devices to @p xsysd,
 * claiming `ubrh->left` / `ubrh->right` for left/right motion
 * controllers. Honors the ForceQwerty override (providers skipped
 * entirely). Never fatal: with no provider registered (or on any
 * provider failure) the builder proceeds exactly as before ADR-034 and
 * qwerty claims the hand roles.
 */
xrt_result_t
t_builder_add_input_provider_devices(struct xrt_system_devices *xsysd,
                                     struct u_builder_roles_helper *ubrh,
                                     enum u_logging_level log_level);

/*!
 * Which hand roles did the (last) input-provider pass claim? False/false
 * when no provider ran or none claimed. Diagnostic — lets the headless
 * self-test tell "provider supplied the roles" apart from "qwerty
 * fell back" without guessing from device strings.
 */
void
t_builder_input_provider_get_claims(bool *out_left, bool *out_right);

/*!
 * Which hand-tracking roles did the (last) input-provider pass claim
 * (#825 Tier 2)? A provider device claims one by advertising
 * `supported.hand_tracking` plus the matching `XRT_INPUT_HT_*` input.
 * False/false when no provider ran or its devices carry no hand-tracking
 * inputs — which is a valid configuration, never a failure.
 */
void
t_builder_input_provider_get_ht_claims(bool *out_left, bool *out_right);

#ifdef __cplusplus
}
#endif
