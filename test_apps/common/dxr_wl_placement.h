// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Client of the compositor's drag lattice, for a phase-snapped Wayland
 *         window drag (#1609).
 *
 * THE PROBLEM. A weaving window must not change its interlace phase while it
 * moves, or the 3D breaks up during a drag. The fix every platform uses is to
 * snap each proposed position BEFORE the window gets there: Windows does it in
 * WM_WINDOWPOSCHANGING, the X11 leg of this helper does it before every
 * XMoveWindow. On Wayland the compositor runs the drag and the client cannot
 * see a single intermediate position.
 *
 * THE MECHANISM. mutter has exactly one hook into its own drag,
 * Meta.ExternalConstraint, and the `window-geometry@displayxr.org` GNOME Shell
 * extension installs one. At the press, this app sends it a TABLE: the
 * displacements from the drag start that its display processor calls
 * phase-correct (probed through xrWeaveSnapWindowRectDXR, the same oracle the
 * X11 drag uses), restricted to positions the compositor can place. mutter
 * then keeps running the drag — its feel, edge tiling and workspace drag all
 * survive — and replaces each proposed position with the nearest table entry.
 * No call into the app happens during the grab, so a busy app can never stall
 * the desktop's move path.
 *
 * WHAT THE TABLE IS NOT. It carries no lens pitch, slant or viewing distance:
 * it is this app's own transient answer set, derived per drag from the public
 * snap. The vendor deliberately retired raw lens geometry from its public API;
 * do not "improve" this into publishing it.
 */
#pragma once

#include <cstdint>
#include <vector>

class DxrWlPlacement
{
public:
	~DxrWlPlacement();

	//! Connect to the session bus and ask the publisher what it supports.
	//! False when there is no bus, no publisher, or no drag-lattice support
	//! (an older extension, or a mutter without Meta.ExternalConstraint).
	bool
	connect();

	//! The publisher can constrain a drag to a lattice.
	bool
	has_drag_lattice() const
	{
		return m_lattice;
	}

	/*!
	 * Send a table of allowed displacements (LOGICAL px, from the drag start).
	 *
	 * @param extend  false = a new drag: the publisher takes the window's
	 *                position NOW as the drag start. true = more of the same
	 *                drag (answering DragLatticeNeeded): the start is kept.
	 * @param cell    bucket size the table was sampled at, logical px.
	 * @param min_dx..max_dy  the displacement range the table covers; outside
	 *                it the publisher lets the move through and asks for more.
	 * @param[out] start_x,start_y  the drag origin the publisher recorded
	 *                (frame top-left, logical stage px); may be null.
	 * @return true when the publisher accepted it. One bounded round trip —
	 *         called at a press, before the grab starts, never during one.
	 */
	bool
	set_drag_lattice(bool extend,
	                 int32_t cell,
	                 int32_t min_dx,
	                 int32_t min_dy,
	                 int32_t max_dx,
	                 int32_t max_dy,
	                 const std::vector<int32_t> &dx,
	                 const std::vector<int32_t> &dy,
	                 int32_t *start_x = nullptr,
	                 int32_t *start_y = nullptr);

	/*!
	 * TEST ONLY: move this process's window so its frame is at (@p x, @p y)
	 * logical — a MOVE action, so it passes through the same constraint a
	 * drag does. Lets the lattice be verified with no human at the mouse.
	 */
	bool
	test_move_to(int32_t x, int32_t y);

	/*!
	 * Drain pending signals. Returns true, once, when the publisher asked for
	 * more of the table (the drag left its coverage), with the displacement
	 * it was at.
	 */
	bool
	poll_needed(int32_t *dx, int32_t *dy);

	//! Human-readable state for the create log.
	const char *
	describe() const
	{
		return m_why;
	}

	void
	disconnect();

private:
	void *m_conn = nullptr; //!< DBusConnection, opaque so the header stays clean
	bool m_lattice = false;
	const char *m_why = "not attempted";
};
