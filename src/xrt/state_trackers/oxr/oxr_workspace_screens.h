// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_spatial_workspace v25 (ADR-052): wire <-> public conversion
 *         of the workspace screen list and window placements. Header-only so
 *         the round trip is unit-tested without a session
 *         (`tests/tests_workspace_screens_ipc.cpp`).
 * @ingroup oxr_api
 */

#pragma once

#include "shared/ipc_protocol.h"

#include <openxr/openxr.h>
#include <openxr/XR_DXR_spatial_workspace.h>

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The primary screen's desktop origin (the workspace-pixel origin) of @p list; 0,0 when it has none.
static inline void
oxr_workspace_screens_primary_origin(const struct ipc_workspace_screen_list *list, int32_t *out_x, int32_t *out_y)
{
	*out_x = 0;
	*out_y = 0;
	for (uint32_t i = 0; i < list->count && i < IPC_WORKSPACE_MAX_SCREENS; i++) {
		if ((list->screens[i].flags & (uint32_t)XR_WORKSPACE_SCREEN_PRIMARY_BIT_DXR) != 0) {
			*out_x = list->screens[i].desktop_x;
			*out_y = list->screens[i].desktop_y;
			return;
		}
	}
}

/*!
 * Fill one public screen record from the wire form. Leaves @p out's `type` and
 * `next` untouched (the caller validated them).
 */
static inline void
oxr_workspace_screen_from_ipc(const struct ipc_workspace_screen *in,
                              int32_t primary_x,
                              int32_t primary_y,
                              XrWorkspaceScreenDXR *out)
{
	out->screenId = in->id;
	out->desktopOffset.x = in->desktop_x;
	out->desktopOffset.y = in->desktop_y;
	out->desktopExtent.width = (int32_t)in->desktop_w;
	out->desktopExtent.height = (int32_t)in->desktop_h;
	out->workspaceOffset.x = in->desktop_x - primary_x;
	out->workspaceOffset.y = in->desktop_y - primary_y;
	out->physicalSizeMeters.width = in->width_m;
	out->physicalSizeMeters.height = in->height_m;
	out->flags = (XrWorkspaceScreenFlagsDXR)in->flags;
	out->currentModeIndex = in->mode_index;
	out->currentViewCount = in->view_count;
	out->hardware3D = in->hardware_3d != 0 ? XR_TRUE : XR_FALSE;
	out->nominalViewerPosition.x = in->nominal_viewer_m[0];
	out->nominalViewerPosition.y = in->nominal_viewer_m[1];
	out->nominalViewerPosition.z = in->nominal_viewer_m[2];
	memset(out->pluginId, 0, sizeof(out->pluginId));
	snprintf(out->pluginId, sizeof(out->pluginId), "%s", in->plugin_id);
}

//! Public placement -> wire form.
static inline void
oxr_workspace_placement_to_ipc(const XrWorkspaceWindowPlacementDXR *in, struct ipc_workspace_window_placement *out)
{
	memset(out, 0, sizeof(*out));
	out->screen_id = in->screenId;
	out->pose.orientation.x = in->pose.orientation.x;
	out->pose.orientation.y = in->pose.orientation.y;
	out->pose.orientation.z = in->pose.orientation.z;
	out->pose.orientation.w = in->pose.orientation.w;
	out->pose.position.x = in->pose.position.x;
	out->pose.position.y = in->pose.position.y;
	out->pose.position.z = in->pose.position.z;
	out->width_m = in->widthMeters;
	out->height_m = in->heightMeters;
}

//! Wire placement -> public form (leaves `type` / `next` untouched).
static inline void
oxr_workspace_placement_from_ipc(const struct ipc_workspace_window_placement *in, XrWorkspaceWindowPlacementDXR *out)
{
	out->screenId = in->screen_id;
	out->pose.orientation.x = in->pose.orientation.x;
	out->pose.orientation.y = in->pose.orientation.y;
	out->pose.orientation.z = in->pose.orientation.z;
	out->pose.orientation.w = in->pose.orientation.w;
	out->pose.position.x = in->pose.position.x;
	out->pose.position.y = in->pose.position.y;
	out->pose.position.z = in->pose.position.z;
	out->widthMeters = in->width_m;
	out->heightMeters = in->height_m;
}

#ifdef __cplusplus
}
#endif
