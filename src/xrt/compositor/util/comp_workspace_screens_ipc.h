// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The workspace screen set (ADR-052) in its `workspace_enumerate_screens`
 *         wire form. Header-only: the service fills the reply with it and the
 *         unit test round-trips it (`tests/tests_workspace_screens_ipc.cpp`).
 * @ingroup comp_util
 */

#pragma once

#include "util/comp_workspace_screens.h"
#include "shared/ipc_protocol.h"

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void
comp_ws_screens_to_ipc(const struct comp_ws_screen_set *set, uint32_t generation, struct ipc_workspace_screen_list *out)
{
	memset(out, 0, sizeof(*out));
	out->generation = generation;
	if (set == NULL) {
		return;
	}
	for (uint32_t i = 0; i < set->count && i < COMP_WS_MAX_SCREENS && out->count < IPC_WORKSPACE_MAX_SCREENS; i++) {
		const struct comp_ws_screen *s = &set->screens[i];
		struct ipc_workspace_screen *w = &out->screens[out->count++];
		w->id = s->id;
		w->desktop_x = s->desktop.x;
		w->desktop_y = s->desktop.y;
		w->desktop_w = s->desktop.w;
		w->desktop_h = s->desktop.h;
		w->width_m = s->width_m;
		w->height_m = s->height_m;
		w->flags = s->flags; // COMP_WS_SCREEN_* == XR_WORKSPACE_SCREEN_*_BIT_DXR
		const bool presenter = (s->flags & COMP_WS_SCREEN_PRESENTER) != 0;
		w->mode_index = presenter ? s->mode_index : 0;
		w->view_count = presenter ? s->view_count : 0;
		w->hardware_3d = presenter && s->hardware_3d ? 1u : 0u;
		w->nominal_viewer_m[0] = s->nominal_viewer_m[0];
		w->nominal_viewer_m[1] = s->nominal_viewer_m[1];
		w->nominal_viewer_m[2] = s->nominal_viewer_m[2];
		snprintf(w->plugin_id, sizeof(w->plugin_id), "%s", s->plugin_id);
	}
}

#ifdef __cplusplus
}
#endif
