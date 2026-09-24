// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The present-owner's own client of the GNOME Shell extension
 *         `window-geometry@displayxr.org` (docs/specs/runtime/
 *         wayland-window-geometry.md §2 and §7).
 *
 * WHY THE APP NEEDS ITS OWN. An in-process `_handle` app never asks where its
 * window is: the runtime's compositor does (comp_vk_native_wl_geom.c) and
 * feeds `set_present_origin` itself. A weave present-owner is the other way
 * round. The weave service runs in another process, owns no window and cannot
 * ask the compositor about ours (the extension answers per CALLER pid, and a
 * client may only move its own windows), so the window owner states its
 * geometry through xrWeaveBindWindow2DXR + XrWeaveWindowGeometryDXR, and also
 * owns the drop-time snap move. This is the smallest client that does both:
 *
 *  - GetWindows() + WindowsChanged: this process's window frame/buffer rect,
 *    its monitor rect and fractional scale, and the `moving` / `lattice_drop`
 *    flags;
 *  - WindowPlacement1.MoveWindow(): the drop-time snap.
 *
 * Everything returned is the wire payload, LOGICAL pixels; conversion to
 * device pixels is the caller's, through u_wayland_geom.h — the same header,
 * byte for byte, the runtime converts with.
 */
#pragma once

#include <cstdint>

struct WlOwnWindow
{
	//! Frame rect, LOGICAL, Mutter global stage coordinates.
	int32_t frame_x = 0, frame_y = 0, frame_w = 0, frame_h = 0;
	//! Buffer rect (the committed main surface), LOGICAL; w == 0 when absent.
	int32_t buffer_x = 0, buffer_y = 0, buffer_w = 0, buffer_h = 0;
	//! The window's monitor, LOGICAL, plus Mutter's FRACTIONAL scale.
	bool have_monitor = false;
	int32_t mon_x = 0, mon_y = 0, mon_w = 0, mon_h = 0;
	double mon_scale = 1.0;
	bool focus = false;
	//! Publisher v3+: an interactive grab is running on the window.
	bool have_moving = false;
	bool moving = false;
	//! Publisher v7+: the last drag ended on its drag lattice.
	bool lattice_drop = false;
};

class WlGeometryClient
{
public:
	~WlGeometryClient();

	//! Connect to the session bus and take one snapshot (bounded, ~200 ms).
	//! False when this build has no libdbus or no bus answers.
	bool
	connect();

	bool
	connected() const
	{
		return m_conn != nullptr;
	}

	//! Drain WindowsChanged signals (non-blocking) and look up this
	//! process's window: focused first, else the largest.
	bool
	own_window(WlOwnWindow *out);

	//! Ask the compositor to put this process's window FRAME at (x, y) LOGICAL.
	//! Bounded, blocking; call once per drop, never per frame.
	bool
	move_window(int32_t frame_logical_x, int32_t frame_logical_y);

	void
	disconnect();

private:
	void *m_conn = nullptr;  //!< DBusConnection *
	void *m_state = nullptr; //!< parsed snapshot (opaque)
	int64_t m_next_retry_ns = 0;
	bool m_warned_placement = false;

	bool
	request_snapshot(int timeout_ms);
	void
	parse(const char *json);
};
