// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Presence-gated hand-role arbitration between an input provider
 *         and the qwerty fallback.
 *
 * ADR-034 shipped a one-shot rule: whatever the active input-provider
 * plug-in created at system build claimed the left/right hand roles for
 * the process lifetime. That is wrong for hardware that comes and goes —
 * a registered Ultraleap provider with no Leap Motion plugged in still
 * created devices, displaced qwerty, and left the user with no
 * controllers at all (#823 follow-up).
 *
 * This module makes the *roles* dynamic while leaving the *devices*
 * alone. Both pairs — the provider's and qwerty's — are created once at
 * system build and both live in `xrt_system_devices::xdevs` exactly as
 * before. The arbiter installs its own `xrt_system_devices::get_roles`,
 * which on each call asks the active provider
 * @ref xrt_input_plugin_iface::get_presence and points
 * `xrt_system_roles::left` / `::right` at the provider pair when the
 * hardware is there and at the qwerty pair when it is not, bumping
 * `generation_id` on every flip.
 *
 * That reuses the role-change path Monado already has end to end: the
 * OpenXR state tracker re-reads the roles in `xrSyncActions`, rebinds
 * actions and queues `XrEventDataInteractionProfileChanged` when the
 * generation moves, and the IPC layer forwards `get_roles` to the
 * service. So re-arbitration happens per app start AND mid-session, with
 * no new plumbing and nothing to poll.
 *
 * Scope: a runtime process hosts exactly one system, so the state here
 * is file-scope, mirroring `target_input_plugin_loader.c` and the claim
 * records in `target_builder_input_provider.c`. `get_roles` calls on any
 * other `xrt_system_devices` are forwarded to the implementation the
 * arbiter displaced.
 *
 * @ingroup target_common
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_system_devices;

/*!
 * Forget everything from a previous system build. Called at the top of
 * the input-provider builder pass.
 */
void
t_input_arbiter_reset(void);

/*!
 * Record the input provider's left/right motion controllers. Either may
 * be NULL. Called by @ref t_builder_add_input_provider_devices after the
 * devices are in `xsysd->xdevs`.
 */
void
t_input_arbiter_note_provider_pair(struct xrt_device *left, struct xrt_device *right);

/*!
 * Record the qwerty fallback's left/right emulated controllers. Either
 * may be NULL. Called by @ref t_builder_add_qwerty_input.
 */
void
t_input_arbiter_note_qwerty_pair(struct xrt_device *left, struct xrt_device *right);

/*!
 * Should the provider hold the hand roles at this instant?
 *
 * True when a provider is active, it supplied at least one hand device,
 * and it either reports @ref XRT_INPUT_PROVIDER_PRESENCE_PRESENT or
 * predates the presence slot entirely (no `get_presence` — assume
 * present, the pre-#823-follow-up behaviour). False otherwise, which is
 * the signal for qwerty to keep the roles.
 *
 * Cheap and side-effect free; the arbiter throttles the underlying
 * provider query internally.
 */
bool
t_input_arbiter_provider_holds_roles(void);

/*!
 * Install the dynamic `get_roles` on @p xsysd. Call once, after BOTH the
 * provider pass and the qwerty pass have added their devices, from the
 * builder's `open_system_impl` (the device list is final by then and the
 * indices the roles carry can be resolved).
 *
 * A no-op when neither pair was recorded, or when neither pair is a
 * complete alternative to the other — the pre-existing static roles are
 * left in place rather than replaced with a degenerate arbitration.
 */
void
t_input_arbiter_install(struct xrt_system_devices *xsysd);

/*!
 * Diagnostics for `displayxr-cli` (`selftest` / `info`): whether the
 * arbiter is installed, and whether the provider currently holds the
 * roles. Both out-params are optional.
 */
void
t_input_arbiter_get_status(bool *out_installed, bool *out_provider_holds_roles);

#ifdef __cplusplus
}
#endif
