// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-hypr-geometry-bridge: Hyprland's `j/clients` and
 *         `j/monitors` replies to one org.displayxr.WindowGeometry1 snapshot,
 *         with no I/O.
 *
 * Schema 1 (docs/specs/runtime/wayland-window-geometry.md §2/§4,
 * "Hyprland publisher"):
 *
 * - `layout_mode: "logical"`. Hyprland's `at`/`size` and a monitor's `x`/`y`
 *   are already logical (layout) px; a monitor's `width`/`height` are its
 *   mode in device px, scanout orientation, so the logical size is that,
 *   transposed for a quarter turn, divided by `scale`;
 * - `frame` = `buffer` = `at` + `size` (Hyprland draws its border outside
 *   `at`, and has no client-side shadow to subtract);
 * - `monitor.scale` = `monitor.device_scale` = the fractional output scale;
 * - `buffer_device` = the buffer in device px relative to the monitor;
 * - `focus` = focusHistoryID 0; `hidden` = not mapped, Hyprland-hidden (a
 *   background tab of a group) or on a workspace the monitor is not showing;
 * - `moving` is not sent: Hyprland reports no interactive move or resize;
 * - no titles.
 *
 * @ingroup targets
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//! What a snapshot tells the bridge's main loop, beside the JSON itself.
struct hgb_snapshot_info
{
	int window_count;
	//! A visible floating window exists: Hyprland sends no event while one
	//! is dragged or resized with the mouse, so the loop polls faster.
	bool any_floating_visible;
};

/*!
 * Build a schema-1 snapshot. Returns a newly allocated JSON string (release
 * with @ref hgb_free), or NULL when either reply is not the JSON array
 * Hyprland sends. Windows whose monitor is unknown, or with no pid, are left
 * out. @p info may be NULL.
 */
char *
hgb_build_snapshot(const char *clients_json, const char *monitors_json, struct hgb_snapshot_info *info);

//! Release a string from @ref hgb_build_snapshot. NULL is fine.
void
hgb_free(char *s);

#ifdef __cplusplus
}
#endif
