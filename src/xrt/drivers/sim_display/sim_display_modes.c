// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's rendering-mode table. See the header.
 * @ingroup drv_sim_display
 */

#include "sim_display_modes.h"

#include "xrt/xrt_config_os.h"

#include <stdio.h>

struct sim_mode_row
{
	const char *name;
	uint32_t view_count;
	float view_scale_x;
	float view_scale_y;
	bool hardware_display_3d;
	uint32_t tile_columns;
	uint32_t tile_rows;
};

static const struct sim_mode_row g_rows[SIM_DISPLAY_BASE_MODE_COUNT + 1] = {
    // 0: 2D (mono, full resolution, 1x1 tile)
    {"2D", 1, 1.0f, 1.0f, false, 1, 1},
    // 1: Anaglyph (stereo, default 3D, 2x1 tiles = side-by-side half-res)
    {"Anaglyph", 2, 0.5f, 0.5f, true, 2, 1},
    // 2: Cropped SBS (stereo, 1x2 tiles = top-bottom half-res)
    {"Cropped SBS", 2, 0.5f, 0.5f, true, 1, 2},
    // 3: Squeezed SBS (stereo, 2x1 tiles = side-by-side full-height)
    {"Squeezed SBS", 2, 0.5f, 1.0f, true, 2, 1},
    // 4: Quad (4 views, 2x2 grid, each quadrant at half-res)
    {"Quad", 4, 0.5f, 0.5f, true, 2, 2},
    // 5: Interlaced (#817): same atlas geometry as Anaglyph (2 full-canvas
    // views, position-preserving), but the DP picks the view per PANEL COLUMN,
    // so the output is sensitive to the weave target's size and its X origin
    // the way a real lenticular weave is. See enum sim_display_output_mode.
    {"Interlaced", 2, 0.5f, 0.5f, true, 2, 1},
    // 6: Full SBS for a stereo desktop compositor: two views, each at the full
    // window size (scale 1.0 x 1.0), 2x1 tiles handed over side by side as they
    // are. The DP reports stereo_layout 3; the runtime presents a target twice
    // the window's width and declares it. Position-preserving geometry, like
    // Anaglyph: each view sees the whole window (the Kooima screen is the full
    // display, not half of it). Twice the panel's width in the atlas, so it is
    // registered only on request (see the header).
    {"Full SBS", 2, 1.0f, 1.0f, true, 2, 1},
};

bool
sim_display_full_sbs_available(void)
{
#ifdef XRT_OS_LINUX_DESKTOP
	return true;
#else
	return false;
#endif
}

uint32_t
sim_display_fill_rendering_modes(struct xrt_rendering_mode *modes, bool want_full_sbs)
{
	uint32_t count = SIM_DISPLAY_BASE_MODE_COUNT;
	if (want_full_sbs && sim_display_full_sbs_available()) {
		count++;
	}
	for (uint32_t i = 0; i < count; i++) {
		const struct sim_mode_row *r = &g_rows[i];
		modes[i].mode_index = i;
		snprintf(modes[i].mode_name, XRT_DEVICE_NAME_LEN, "%s", r->name);
		modes[i].view_count = r->view_count;
		modes[i].view_scale_x = r->view_scale_x;
		modes[i].view_scale_y = r->view_scale_y;
		modes[i].hardware_display_3d = r->hardware_display_3d;
		modes[i].tile_columns = r->tile_columns;
		modes[i].tile_rows = r->tile_rows;
	}
	return count;
}
