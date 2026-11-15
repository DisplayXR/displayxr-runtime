// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hosted-window adoption policy, the portable half (#2637).
 *
 * An in-process HOSTED session (legacy app, no window binding) normally gets a
 * runtime-owned full-screen window. When the app's process already owns its own
 * top-level window (a desktop game driven through an OpenXR injector), that puts
 * the 3D picture and the input focus in two different windows. Adoption instead
 * places the runtime's window as an owned, non-activating, click-through popup
 * over the app window's client area (Windows: comp_d3d11_window.cpp).
 *
 * This header holds only the decision: which of the process's top-level
 * windows (if any) to adopt, under `DXR_WIN32_ADOPT_OWN_WINDOW`
 * (unset/"auto" = auto, "1" = force, "0" = off). The window enumeration and
 * the popup live with the window code; this part is unit-tested.
 *
 * @ingroup comp_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum comp_adopt_mode
{
	COMP_ADOPT_MODE_AUTO = 0,
	COMP_ADOPT_MODE_FORCE,
	COMP_ADOPT_MODE_OFF,
};

//! Parse the knob: NULL/""/"auto" = AUTO, "0"/"off"/"false" = OFF, anything else = FORCE.
static inline enum comp_adopt_mode
comp_adopt_mode_parse(const char *s)
{
	if (s == NULL || s[0] == '\0' || strcmp(s, "auto") == 0) {
		return COMP_ADOPT_MODE_AUTO;
	}
	if (strcmp(s, "0") == 0 || strcmp(s, "off") == 0 || strcmp(s, "false") == 0) {
		return COMP_ADOPT_MODE_OFF;
	}
	return COMP_ADOPT_MODE_FORCE;
}

static inline const char *
comp_adopt_mode_str(enum comp_adopt_mode m)
{
	switch (m) {
	case COMP_ADOPT_MODE_AUTO: return "auto";
	case COMP_ADOPT_MODE_FORCE: return "1";
	case COMP_ADOPT_MODE_OFF: return "0";
	}
	return "?";
}

//! One visible, unowned, non-tool top-level window of this process that is not
//! the runtime's own, with its client area in screen coordinates.
struct comp_adopt_candidate
{
	int32_t x, y;
	int32_t w, h;
	bool on_panel;   //!< On the 3D panel's monitor.
	bool foreground; //!< The foreground window.
	//! Topmost and exactly covering its monitor: looks like exclusive
	//! full-screen, where an owned popup cannot sit above it.
	bool exclusive_fullscreen;
};

/*!
 * Pick the window to adopt, or -1 with @p out_why saying why not.
 *
 * AUTO: exactly one candidate on the panel's monitor. FORCE: the foreground
 * candidate, else the largest, on any monitor. Both decline a window that
 * looks exclusive full-screen and a window with an empty client area.
 */
static inline int
comp_adopt_choose(enum comp_adopt_mode mode,
                  const struct comp_adopt_candidate *c,
                  int count,
                  const char **out_why)
{
	const char *why_dummy;
	const char **why = out_why != NULL ? out_why : &why_dummy;
	if (mode == COMP_ADOPT_MODE_OFF) {
		*why = "DXR_WIN32_ADOPT_OWN_WINDOW=0";
		return -1;
	}
	if (count <= 0) {
		*why = "the process owns no visible top-level window";
		return -1;
	}
	int pick = -1;
	if (mode == COMP_ADOPT_MODE_AUTO) {
		int on_panel = 0;
		for (int i = 0; i < count; i++) {
			if (c[i].on_panel) {
				on_panel++;
				pick = i;
			}
		}
		if (on_panel == 0) {
			*why = "none of the process's windows is on the 3D panel";
			return -1;
		}
		if (on_panel > 1) {
			*why = "more than one of the process's windows is on the 3D panel (ambiguous; "
			       "DXR_WIN32_ADOPT_OWN_WINDOW=1 picks the foreground or largest)";
			return -1;
		}
	} else {
		int64_t best_area = -1;
		for (int i = 0; i < count; i++) {
			if (c[i].foreground) {
				pick = i;
				break;
			}
			const int64_t area = (int64_t)c[i].w * (int64_t)c[i].h;
			if (area > best_area) {
				best_area = area;
				pick = i;
			}
		}
	}
	if (c[pick].w <= 0 || c[pick].h <= 0) {
		*why = "the window's client area is empty";
		return -1;
	}
	if (c[pick].exclusive_fullscreen) {
		*why = "the window looks exclusive full-screen (topmost, covering its monitor)";
		return -1;
	}
	*why = mode == COMP_ADOPT_MODE_AUTO ? "the one window on the 3D panel" : "forced";
	return pick;
}

#ifdef __cplusplus
}
#endif
