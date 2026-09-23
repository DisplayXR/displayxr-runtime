// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Client of the compositor's window-placement service, for an
 *         app-owned Wayland window drag (#1609).
 *
 * WHY AN APP MOVES ITS OWN WINDOW ON WAYLAND. A weaving window must not change
 * its interlace phase while it moves, or the 3D breaks up while dragging — and
 * the only way to guarantee that is for whoever decides the next position to
 * snap it BEFORE the window gets there. Windows does exactly that inside its
 * move loop (the display processor rewrites the proposed position in
 * WM_WINDOWPOSCHANGING), and the X11 leg of this helper does it too
 * (snap -> XMoveWindow). Wayland gives a client no positioning protocol at
 * all, so `xdg_toplevel.move` hands the whole drag to the compositor and the
 * app can only watch. This is the missing piece: the
 * `window-geometry@displayxr.org` GNOME Shell extension exposes a relative
 * move for the CALLING process's own window, so the app can run the drag
 * itself and snap every step.
 *
 * WHAT IT COSTS. The compositor is no longer running the drag, so its drag
 * affordances are not available while the button is held: edge tiling, drag to
 * another workspace, and the "throw at a monitor edge" gestures. And the
 * window follows the pointer with a client round trip instead of being moved
 * inside the compositor's own input handling. Both are measured and stated in
 * the PR rather than assumed; DXR_WL_CLIENT_DRAG=0 switches back to
 * `xdg_toplevel.move` for an A/B.
 *
 * Deliberately tiny: one method call, fire-and-forget, no reply and no JSON.
 * The app never learns its absolute position — it does not need to, because a
 * phase snap only ever depends on the DISPLACEMENT from where the drag began.
 */
#pragma once

#include <cstdint>

/*!
 * Session-bus client for `org.displayxr.WindowPlacement1`. One per app.
 * Every entry point is safe to call when the service is absent; the caller
 * then falls back to a compositor-driven drag.
 */
class DxrWlPlacement
{
public:
	~DxrWlPlacement();

	//! Connect to the session bus and check the service is there. False when
	//! there is no bus, no publisher, or this build has no D-Bus at all.
	bool
	connect();

	//! True once connect() found a live `org.displayxr.WindowPlacement1`.
	bool
	available() const
	{
		return m_available;
	}

	/*!
	 * Move this process's window by @p dx / @p dy LOGICAL px. Fire-and-forget:
	 * a drag issues one of these per pointer motion, and waiting for a reply
	 * would put a round trip in the middle of the motion path.
	 *
	 * @return false when the request could not even be sent.
	 */
	bool
	move_by(int32_t dx, int32_t dy);

	/*!
	 * Drain pending `WindowMoved` signals. Call once per frame (and after a
	 * request) so @ref observed() is current.
	 */
	void
	poll();

	/*!
	 * Ask for the window's frame position now (one bounded round trip). Used
	 * once at the start of a drag to establish the base; the per-motion path
	 * uses the pushed reports instead.
	 */
	bool
	fetch_origin(int32_t *x, int32_t *y);

	/*!
	 * The window's ACHIEVED frame position, as the compositor last reported
	 * it. This is what a drag must close its loop on: integrating one's own
	 * requests runs away the moment a move is not applied — measured on
	 * hardware, a cumulative of +1278 logical px while the window never moved.
	 *
	 * @return false until the first report arrives.
	 */
	bool
	observed(int32_t *x, int32_t *y) const;

	//! How many APPLIED reports have arrived; a caller can tell "nothing yet"
	//! from "reported the same position again".
	uint64_t
	observed_seq() const
	{
		return m_seq;
	}

	//! Every report, applied or not, and the refused subset. The pair
	//! separates "the compositor refused the move" from "no report reached
	//! us", which look identical from a stalled drag but are different bugs.
	uint64_t
	reports() const
	{
		return m_reports;
	}
	uint64_t
	refusals() const
	{
		return m_refusals;
	}

	/*!
	 * Move this process's window so its FRAME's top-left is at @p x / @p y
	 * (logical stage px). Fire-and-forget, and absolute — so re-sending is
	 * idempotent, unlike move_by().
	 */
	bool
	move_to(int32_t x, int32_t y);

	//! Human-readable state for the create log.
	const char *
	describe() const;

	void
	disconnect();

private:
	void *m_conn = nullptr; //!< DBusConnection, opaque so the header stays clean
	bool m_available = false;
	const char *m_why = "not attempted";
	int32_t m_obs_x = 0, m_obs_y = 0;
	bool m_have_obs = false;
	uint64_t m_seq = 0, m_reports = 0, m_refusals = 0;
};
