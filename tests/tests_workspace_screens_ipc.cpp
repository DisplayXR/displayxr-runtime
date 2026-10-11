// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief XR_DXR_spatial_workspace v25 (ADR-052) over IPC: the screen set the
 *        service builds, through its `workspace_enumerate_screens` wire form,
 *        into the public XrWorkspaceScreenDXR records a controller reads; the
 *        placement round trip; the SCREENS_CHANGED event's wire slot; and
 *        that every new message fits one IPC buffer.
 */

#include "util/comp_workspace_screens_ipc.h"
#include "oxr_workspace_screens.h"

#include "ipc_protocol_generated.h"

#include "catch_amalgamated.hpp"

#include <cstdio>
#include <cstring>

namespace {

xrt_screen
screen(uint64_t id, int32_t x, int32_t y, uint32_t w, uint32_t h, float wm, float hm, const char *plugin, uint32_t conf)
{
	xrt_screen s{};
	s.id = id;
	s.desktop_left = x;
	s.desktop_top = y;
	s.desktop_width = w;
	s.desktop_height = h;
	s.info.width_m = wm;
	s.info.height_m = hm;
	s.info.nominal_viewer_x_m = 0.01f;
	s.info.nominal_viewer_y_m = 0.02f;
	s.info.nominal_viewer_z_m = 0.65f;
	s.confidence = conf;
	std::snprintf(s.plugin_id, sizeof(s.plugin_id), "%s", plugin);
	return s;
}

} // namespace

TEST_CASE("workspace screens: service set -> wire -> public records")
{
	// The primary is NOT the left-most screen: a second 3D panel sits to its left.
	xrt_screen_list l{};
	l.screens[0] = screen(0x1001, 0, 0, 3840, 2160, 0.60f, 0.3375f, "vendor-a", 100);
	l.screens[0].flags = XRT_SCREEN_FLAG_SYSTEM_DEFAULT | XRT_SCREEN_FLAG_TRACKED;
	l.screens[1] = screen(0x2002, -1920, 0, 1920, 1080, 0.40f, 0.225f, "vendor-b", 50);
	l.count = 2;

	comp_ws_screen_set set{};
	REQUIRE(comp_ws_screens_build(&l, 0x3, COMP_WS_POLICY_CLAIMED, &set) == 2);
	set.screens[0].mode_index = 1;
	set.screens[0].view_count = 2;
	set.screens[0].hardware_3d = true;

	ipc_workspace_screen_list wire{};
	comp_ws_screens_to_ipc(&set, 7, &wire);
	REQUIRE(wire.count == 2);
	CHECK(wire.generation == 7);

	XrWorkspaceScreenDXR out[2];
	for (auto &o : out) {
		std::memset(&o, 0, sizeof(o));
		o.type = XR_TYPE_WORKSPACE_SCREEN_DXR;
	}
	int32_t px, py;
	oxr_workspace_screens_primary_origin(&wire, &px, &py);
	CHECK(px == 0);
	CHECK(py == 0);
	for (uint32_t i = 0; i < wire.count; i++) {
		oxr_workspace_screen_from_ipc(&wire.screens[i], px, py, &out[i]);
	}

	CHECK(out[0].type == XR_TYPE_WORKSPACE_SCREEN_DXR); // untouched
	CHECK(out[0].screenId == 0x1001);
	CHECK(out[0].flags == (XR_WORKSPACE_SCREEN_PRIMARY_BIT_DXR | XR_WORKSPACE_SCREEN_3D_BIT_DXR |
	                       XR_WORKSPACE_SCREEN_PRESENTER_BIT_DXR | XR_WORKSPACE_SCREEN_TRACKED_BIT_DXR));
	CHECK(out[0].desktopExtent.width == 3840);
	CHECK(out[0].physicalSizeMeters.width == 0.60f);
	CHECK(out[0].currentModeIndex == 1);
	CHECK(out[0].currentViewCount == 2);
	CHECK(out[0].hardware3D == XR_TRUE);
	CHECK(out[0].nominalViewerPosition.z == 0.65f);
	CHECK(std::strcmp(out[0].pluginId, "vendor-a") == 0);

	CHECK(out[1].screenId == 0x2002);
	CHECK(out[1].flags == (XR_WORKSPACE_SCREEN_3D_BIT_DXR | XR_WORKSPACE_SCREEN_PRESENTER_BIT_DXR));
	CHECK(out[1].desktopOffset.x == -1920);
	CHECK(out[1].workspaceOffset.x == -1920); // workspace px = desktop - primary origin
	CHECK(out[1].currentViewCount == 0);      // mode fields only for a presenter with a known mode

	// The bits are the same numbers on both sides of the wire.
	CHECK((uint64_t)COMP_WS_SCREEN_PRIMARY == XR_WORKSPACE_SCREEN_PRIMARY_BIT_DXR);
	CHECK((uint64_t)COMP_WS_SCREEN_3D == XR_WORKSPACE_SCREEN_3D_BIT_DXR);
	CHECK((uint64_t)COMP_WS_SCREEN_PRESENTER == XR_WORKSPACE_SCREEN_PRESENTER_BIT_DXR);
	CHECK((uint64_t)COMP_WS_SCREEN_TRACKED == XR_WORKSPACE_SCREEN_TRACKED_BIT_DXR);
}

TEST_CASE("workspace screens: a non-presenter carries no mode, the plug-in id is truncated not overrun")
{
	comp_ws_screen_set set{};
	set.count = 1;
	set.primary_index = 0;
	set.screens[0].id = 5;
	set.screens[0].flags = COMP_WS_SCREEN_3D; // listed, not presenting
	set.screens[0].mode_index = 4;
	set.screens[0].view_count = 4;
	std::memset(set.screens[0].plugin_id, 'x', sizeof(set.screens[0].plugin_id) - 1);

	ipc_workspace_screen_list wire{};
	comp_ws_screens_to_ipc(&set, 1, &wire);
	CHECK(wire.screens[0].mode_index == 0);
	CHECK(wire.screens[0].view_count == 0);
	CHECK(std::strlen(wire.screens[0].plugin_id) == IPC_WORKSPACE_SCREEN_PLUGIN_ID_MAX - 1);
}

TEST_CASE("workspace placement: public -> wire -> public")
{
	XrWorkspaceWindowPlacementDXR in{};
	in.type = XR_TYPE_WORKSPACE_WINDOW_PLACEMENT_DXR;
	in.screenId = 0x2002;
	in.pose.orientation = {0.0f, 0.258819f, 0.0f, 0.9659258f};
	in.pose.position = {0.1f, -0.2f, 0.03f};
	in.widthMeters = 0.25f;
	in.heightMeters = 0.15f;

	ipc_workspace_window_placement wire{};
	oxr_workspace_placement_to_ipc(&in, &wire);
	CHECK(wire.screen_id == 0x2002);
	CHECK(wire.pose.position.y == -0.2f);
	CHECK(wire.width_m == 0.25f);

	XrWorkspaceWindowPlacementDXR out{};
	out.type = XR_TYPE_WORKSPACE_WINDOW_PLACEMENT_DXR;
	oxr_workspace_placement_from_ipc(&wire, &out);
	CHECK(out.type == XR_TYPE_WORKSPACE_WINDOW_PLACEMENT_DXR);
	CHECK(out.screenId == in.screenId);
	CHECK(std::memcmp(&out.pose, &in.pose, sizeof(in.pose)) == 0);
	CHECK(out.widthMeters == in.widthMeters);
	CHECK(out.heightMeters == in.heightMeters);
}

TEST_CASE("workspace SCREENS_CHANGED: wire and public slots line up, the union does not grow")
{
	CHECK((int)IPC_WORKSPACE_INPUT_EVENT_SCREENS_CHANGED == (int)XR_WORKSPACE_INPUT_EVENT_SCREENS_CHANGED_DXR);

	ipc_workspace_input_event ev{};
	ev.event_type = IPC_WORKSPACE_INPUT_EVENT_SCREENS_CHANGED;
	ev.u.screens_changed.generation = 9;
	ev.u.screens_changed.screen_count = 3;

	// What the client bridge does for this event type.
	XrWorkspaceInputEventDXR pub{};
	pub.eventType = (XrWorkspaceInputEventTypeDXR)ev.event_type;
	pub.screensChanged.generation = ev.u.screens_changed.generation;
	pub.screensChanged.screenCount = ev.u.screens_changed.screen_count;
	CHECK(pub.eventType == XR_WORKSPACE_INPUT_EVENT_SCREENS_CHANGED_DXR);
	CHECK(pub.screensChanged.generation == 9);
	CHECK(pub.screensChanged.screenCount == 3);

	// The new member fits the existing union: the public event stride (and so
	// every pre-v25 controller's drain buffer) is unchanged.
	CHECK(sizeof(pub.screensChanged) <= sizeof(pub.windowPoseChanged));
	CHECK(sizeof(ev.u.screens_changed) <= sizeof(ev.u.window_pose_changed));
}

TEST_CASE("workspace v25 messages fit one IPC buffer")
{
	CHECK(sizeof(struct ipc_workspace_enumerate_screens_reply) <= IPC_BUF_SIZE);
	CHECK(sizeof(struct ipc_workspace_set_window_placement_msg) <= IPC_BUF_SIZE);
	CHECK(sizeof(struct ipc_workspace_get_window_placement_reply) <= IPC_BUF_SIZE);
	CHECK(sizeof(struct ipc_workspace_set_overlay_on_screen_msg) <= IPC_BUF_SIZE); // v26
}

TEST_CASE("workspace overlay screen (v26): the chained tag, or none")
{
	XrWorkspaceOverlayInfoDXR info = {XR_TYPE_WORKSPACE_OVERLAY_INFO_DXR};
	uint64_t id = 0xFFFF;

	SECTION("nothing chained: the pre-v26 call (primary), sent as before")
	{
		CHECK_FALSE(oxr_workspace_overlay_screen_from_chain(&info, &id));
		CHECK(id == 0);
	}

	SECTION("a tag behind another struct is found")
	{
		XrWorkspaceOverlayScreenDXR tag = {XR_TYPE_WORKSPACE_OVERLAY_SCREEN_DXR};
		tag.screenId = 0x2002;
		XrWorkspaceCursorDepthDXR other = {XR_TYPE_WORKSPACE_CURSOR_DEPTH_DXR}; // any unrelated struct
		other.next = &tag;
		info.next = &other;
		CHECK(oxr_workspace_overlay_screen_from_chain(&info, &id));
		CHECK(id == 0x2002);
	}

	SECTION("a tag with screenId 0 names the primary")
	{
		XrWorkspaceOverlayScreenDXR tag = {XR_TYPE_WORKSPACE_OVERLAY_SCREEN_DXR};
		tag.screenId = 0;
		info.next = &tag;
		CHECK(oxr_workspace_overlay_screen_from_chain(&info, &id));
		CHECK(id == 0);
	}
}
