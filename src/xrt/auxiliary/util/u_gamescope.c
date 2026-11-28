// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  gamescope (SteamOS Game Mode): detection and the 1:1 verdict.
 * @ingroup aux_util
 */

#include "util/u_gamescope.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

//! Is @p word one of the ':'-separated entries of @p list (ASCII case-insensitive)?
static bool
list_has(const char *list, const char *word)
{
	const size_t wlen = strlen(word);
	const char *p = list;
	while (p != NULL && *p != '\0') {
		const char *end = strchr(p, ':');
		const size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
		if (len == wlen) {
			bool same = true;
			for (size_t i = 0; i < len && same; i++) {
				same = tolower((unsigned char)p[i]) == tolower((unsigned char)word[i]);
			}
			if (same) {
				return true;
			}
		}
		p = end != NULL ? end + 1 : NULL;
	}
	return false;
}

bool
u_gamescope_env_is_gamescope(const char *xdg_current_desktop, const char *gamescope_wayland_display)
{
	if (gamescope_wayland_display != NULL && gamescope_wayland_display[0] != '\0') {
		return true;
	}
	return xdg_current_desktop != NULL && list_has(xdg_current_desktop, "gamescope");
}

bool
u_gamescope_session(void)
{
	return u_gamescope_env_is_gamescope(getenv("XDG_CURRENT_DESKTOP"), getenv("GAMESCOPE_WAYLAND_DISPLAY"));
}

enum u_gamescope_1to1
u_gamescope_check_1to1(
    uint32_t panel_w, uint32_t panel_h, uint32_t root_w, uint32_t root_h, uint32_t win_w, uint32_t win_h)
{
	if (panel_w == 0 || panel_h == 0 || root_w == 0 || root_h == 0) {
		return U_GAMESCOPE_1TO1_UNKNOWN;
	}
	if (root_w != panel_w || root_h != panel_h) {
		return U_GAMESCOPE_1TO1_GAME_RES_SCALED;
	}
	if (win_w != 0 && win_h != 0 && (win_w != root_w || win_h != root_h)) {
		return U_GAMESCOPE_1TO1_WINDOW_SCALED;
	}
	return U_GAMESCOPE_1TO1_YES;
}

const char *
u_gamescope_1to1_str(enum u_gamescope_1to1 v)
{
	switch (v) {
	case U_GAMESCOPE_1TO1_YES: return "1:1";
	case U_GAMESCOPE_1TO1_GAME_RES_SCALED: return "game resolution scaled to the output";
	case U_GAMESCOPE_1TO1_WINDOW_SCALED: return "window scaled to the game resolution";
	case U_GAMESCOPE_1TO1_UNKNOWN:
	default: return "unknown";
	}
}
