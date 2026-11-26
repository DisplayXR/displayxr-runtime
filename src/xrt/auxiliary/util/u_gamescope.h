// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  gamescope (SteamOS Game Mode): detection, and whether a game's X11
 *         pixels reach the panel 1:1.
 *
 * In Game Mode every app runs under gamescope, Valve's micro-compositor: a
 * nested Xwayland whose root is the GAME resolution, composited fullscreen
 * onto the one output gamescope drives. gamescope scales whatever the game
 * presents to fill that output (bilinear, FSR or NIS, per the user's Steam
 * scaling setting). A weave survives none of them, so 1:1 needs both:
 *
 * 1. the nested root (Steam's per-game "Game Resolution") equal to the 3D
 *    panel's native size, and
 * 2. the game's window (its swapchain) equal to that root — a smaller
 *    window is scaled up to the root like any windowed game.
 *
 * Nothing else about placement matters there: there is one output, gamescope
 * owns it, every game window is fullscreen at the root's origin, and EWMH
 * placement hints do nothing. Direct scanout is impossible (gamescope holds
 * the DRM master).
 *
 * The verdict is pure (@ref u_gamescope_check_1to1) so it is unit-tested; the
 * Vulkan compositor's X11 1:1 gate acts on it.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Do these environment values say the process runs under gamescope?
 * gamescope exports `GAMESCOPE_WAYLAND_DISPLAY` to every child, and SteamOS's
 * gamescope-session sets `XDG_CURRENT_DESKTOP=gamescope` (a colon-separated
 * list; matched per entry, case-insensitively). Either is enough. NULL or
 * empty means unset.
 */
bool
u_gamescope_env_is_gamescope(const char *xdg_current_desktop, const char *gamescope_wayland_display);

//! @ref u_gamescope_env_is_gamescope on this process's environment.
bool
u_gamescope_session(void);

//! Can a gamescope session present to the 3D panel 1:1?
enum u_gamescope_1to1
{
	//! Not enough known to say (panel or root size 0): keep the weave as it is.
	U_GAMESCOPE_1TO1_UNKNOWN = 0,
	//! Root == panel, and the window (when known) == root.
	U_GAMESCOPE_1TO1_YES,
	//! The nested root is not the panel's size: gamescope scales the game
	//! resolution to the output. Fix: the game's resolution in Steam = Native.
	U_GAMESCOPE_1TO1_GAME_RES_SCALED,
	//! Root == panel, but the window is a different size: gamescope scales the
	//! window to the root.
	U_GAMESCOPE_1TO1_WINDOW_SCALED,
};

/*!
 * The verdict for a panel of @p panel_w x @p panel_h device px, a nested X
 * root of @p root_w x @p root_h, and the presenting window @p win_w x
 * @p win_h (0 x 0 = not known yet, which is not a reason to degrade).
 */
enum u_gamescope_1to1
u_gamescope_check_1to1(
    uint32_t panel_w, uint32_t panel_h, uint32_t root_w, uint32_t root_h, uint32_t win_w, uint32_t win_h);

//! Short name of a verdict, for logs. Never NULL.
const char *
u_gamescope_1to1_str(enum u_gamescope_1to1 v);

#ifdef __cplusplus
}
#endif
