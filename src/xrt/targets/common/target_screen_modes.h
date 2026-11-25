// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Builds every claimed screen's rendering-mode table from its plug-in
 *         (ADR-047 Amendment 10) and keeps the process registry
 *         (`util/u_screen_modes.h`) current.
 * @ingroup targets_common
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_screen_list;
struct xrt_dp_factory_registry;
struct xrt_status_snapshot;
struct u_screen_mode_table;

/*!
 * The per-screen sizing opt-out (ADR-047 Amendment 10 decision 1): the
 * environment / settings option naming the screens (by stable key, `;` or `,`
 * separated) that do NOT count toward the instance's worst case. A window on
 * such a screen is resampled from the union of the others. Read by the process
 * that sizes the instance (the app in-process, the service for IPC clients).
 */
#define TARGET_SCREEN_MODES_SIZE_EXCLUDE_ENV "DXR_SIZE_EXCLUDE_SCREENS"

/*!
 * Is the screen @p key named by the opt-out list @p spec (`;` / `,` separated,
 * whitespace trimmed, case-insensitive)? Pure.
 */
bool
target_screen_modes_key_listed(const char *spec, const char *key);

/*!
 * Build one table per screen of @p list into @p out (at most @p cap): the
 * system-default screen from @p head's table, every other screen from its
 * owning plug-in's `get_rendering_modes_for_monitor` (found in @p reg), else a
 * copy of @p head's list. Applies no stored choice (the caller does). Reads the
 * loader's monitor side table, so it must run after the registry was built in
 * this process.
 *
 * @return how many tables were written.
 */
uint32_t
target_screen_modes_build(const struct xrt_screen_list *list,
                          const struct xrt_dp_factory_registry *reg,
                          struct xrt_device *head,
                          struct u_screen_mode_table *out,
                          uint32_t cap);

/*!
 * Build the tables for @p list, apply every screen's stored 3D-mode choice,
 * and publish them to the process registry; registers the choice resolver.
 * With @p initial (the system's first publish, before the instance is sized),
 * a stored choice naming a start-up-only entry is declared for this instance
 * (u_screen_modes_table_promote_choice): `dp mode set` stored it as
 * `applies: restart_app`, and this is that next start.
 * Logs one WARN per screen whose table changed.
 */
void
target_screen_modes_publish(const struct xrt_screen_list *list,
                            const struct xrt_dp_factory_registry *reg,
                            struct xrt_device *head,
                            bool initial);

/*!
 * ADR-066 Part B x ADR-047 Amendment 10: publish @p list's per-screen
 * viewer-tracking decisions (target_viewer_tracking_update_screens; only with
 * @p initial or @p topology_changed, as the screen table only moves then) and
 * THEN its mode tables (target_screen_modes_publish), whose per-entry tracked
 * flags read those decisions. The other order publishes the previous
 * topology's flags (at start-up: none) until the next re-probe.
 */
void
target_screen_modes_publish_with_viewers(const struct xrt_screen_list *list,
                                         const struct xrt_dp_factory_registry *reg,
                                         struct xrt_device *head,
                                         bool initial,
                                         bool topology_changed);

/*!
 * A per-screen viewer-tracking setting may have moved: re-run the per-screen
 * arbitration (target_viewer_tracking_settings_changed) and, when any screen's
 * decision changed, re-publish the mode tables so their tracked flags follow.
 *
 * @return true when the decisions (and so the tables) were re-published.
 */
bool
target_screen_modes_viewer_settings_changed(const struct xrt_screen_list *list,
                                            const struct xrt_dp_factory_registry *reg,
                                            struct xrt_device *head);

/*!
 * The resolver the registry calls for a screen's stored choice (the per-screen
 * 3D-mode store, `u_setting_get_mode_3d_for_screen`).
 */
bool
target_screen_modes_choice(const char *key, char *out_name, size_t cap, uint32_t *out_source);

/*!
 * Fill the `mode_3d` of every screen row of @p out other than the
 * system-default one (which the head device fills): from the process registry
 * when it holds the screen (live state), else from a table built here from
 * @p list / @p reg / @p head (a headless snapshot; @p list may be NULL there,
 * and then such rows stay unknown). A screen that does not fit the running
 * instance's worst case (u_screen_modes_get_instance_worst) reports
 * `restart_app` for its entries.
 */
void
target_screen_modes_fill_status(const struct xrt_screen_list *list,
                                const struct xrt_dp_factory_registry *reg,
                                struct xrt_device *head,
                                struct xrt_status_snapshot *out);

#ifdef __cplusplus
}
#endif
