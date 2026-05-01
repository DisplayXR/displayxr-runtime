// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Shell-side chrome render module (Phase 2.C).
 *
 * Owns per-workspace-client chrome swapchains. On client connect the module
 * creates an XrSwapchain via xrCreateWorkspaceClientChromeSwapchainEXT,
 * renders the floating-pill design into image[0], and submits the layout
 * (pose-in-client + size + hit regions) via xrSetWorkspaceClientChromeLayoutEXT.
 *
 * Plain-C interface so main.c (compiled as C) can drive it. Implementation
 * is C++ to use D3D11 / DXGI / COM through wil.
 */
#pragma once

#include <openxr/openxr.h>
#include <openxr/XR_EXT_spatial_workspace.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct shell_openxr_state;
struct shell_chrome;

/*!
 * Phase 2.C C4: controller-defined chrome region IDs the shell stamps into
 * the chrome layout's hitRegions[] array. The runtime echoes the matched id
 * back as @c chromeRegionId on POINTER / POINTER_MOTION events so main.c can
 * dispatch by region (close → exit RPC, grip → drag, etc.). 0 is reserved as
 * XR_NULL_WORKSPACE_CHROME_REGION_ID and means "no chrome region matched."
 */
enum shell_chrome_region_id
{
	SHELL_CHROME_REGION_NONE  = 0,
	SHELL_CHROME_REGION_GRIP  = 1,
	SHELL_CHROME_REGION_CLOSE = 2,
	SHELL_CHROME_REGION_MIN   = 3,
	SHELL_CHROME_REGION_MAX   = 4,
};

/*!
 * Allocate the chrome module, bound to the shell's OpenXR state. The shell's
 * D3D11 device must already be live. Returns NULL on init failure (logged).
 */
struct shell_chrome *shell_chrome_create(struct shell_openxr_state *xr);

/*!
 * Tear down all per-client chrome swapchains and free the module. Safe to
 * pass NULL.
 */
void shell_chrome_destroy(struct shell_chrome *sc);

/*!
 * Call once per new workspace client. Creates a chrome swapchain for the
 * client, renders initial chrome state into image[0], and submits the
 * layout. @p win_w_m / @p win_h_m is the client's window size in meters
 * (used to compute pill width as a fraction of window width). Returns true
 * on success; false (with diagnostic) on retryable failure — callers should
 * try again on the next tick (Phase 2.K connect-time race).
 */
bool shell_chrome_on_client_connected(struct shell_chrome *sc,
                                      XrWorkspaceClientId id,
                                      float win_w_m,
                                      float win_h_m,
                                      const char *icon_png_path);

/*!
 * Call when a client disconnects. Destroys its chrome swapchain.
 */
void shell_chrome_on_client_disconnected(struct shell_chrome *sc, XrWorkspaceClientId id);

/*!
 * Call when a client's window size changes (preset switch, resize, etc.).
 * Re-pushes the layout so the chrome quad scales with the window.
 */
void shell_chrome_on_window_resized(struct shell_chrome *sc,
                                    XrWorkspaceClientId id,
                                    float win_w_m,
                                    float win_h_m);

/*!
 * @return true if a chrome swapchain has been created for the given client.
 * Cheap check (linear scan of internal slot list). main.c calls this from
 * the per-tick lazy retry loop to skip the get_pose IPC round-trip for
 * already-tracked clients — keeps slot-anim set_pose calls smooth.
 */
bool shell_chrome_has(struct shell_chrome *sc, XrWorkspaceClientId id);

/*!
 * Phase 2.C C3.C-4: notify the chrome module which client (if any) the
 * cursor is currently hovering. Seeds per-slot fade animations: hovered
 * slot fades alpha toward 1 over 150 ms; all other slots fade toward 0
 * over 300 ms (matches the runtime's prior in-runtime hover-fade
 * timings). Pass @p hover_id = XR_NULL_WORKSPACE_CLIENT_ID (0) when the
 * cursor is not over any chrome.
 *
 * Cheap (no IPC, no GPU work). The actual fade tween + chrome SRV
 * re-render happen in shell_chrome_tick.
 */
void shell_chrome_set_hover(struct shell_chrome *sc, XrWorkspaceClientId hover_id);

/*!
 * Phase 2.C C3.C-4: process per-slot fade animations and re-render any
 * chrome SRVs whose alpha changed this tick. Call once per main-loop
 * iteration. No-op when no fade is active (idle = zero GPU work).
 */
void shell_chrome_tick(struct shell_chrome *sc);

/*!
 * @return true if any slot is currently mid-fade (alpha != target).
 * Main loop reads this to drop poll cadence from 500 ms (idle) to 16 ms
 * (60 Hz) so the fade tween is smooth — without this the fade is so
 * short (150 / 300 ms) that the next idle-rate tick fires after it has
 * already completed and the chrome appears to snap rather than fade.
 */
bool shell_chrome_is_animating(struct shell_chrome *sc);

/*!
 * @return true if any chrome swapchain is currently registered (non-empty
 * slot list). Main loop reads this to keep the input-drain poll cadence
 * at 60 Hz whenever chrome is alive, so POINTER_HOVER transitions reach
 * shell_chrome_set_hover with low latency. Without this the idle 500 ms
 * poll holds back the hover signal, and the cursor-between-windows case
 * looks like "B waits for A to fade out" — actually just queued events
 * draining late.
 */
bool shell_chrome_has_any(struct shell_chrome *sc);

#ifdef __cplusplus
}
#endif
