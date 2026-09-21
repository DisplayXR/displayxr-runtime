// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Backend-neutral app-owned window for the desktop-Linux test apps.
 *
 * See dxr_linux_window.h for the contract. The X11 leg started as the code
 * that used to live (twice, verbatim) in cube_handle_vk_linux/main.cpp and
 * cube_zones_vk_linux/main.cpp; the ICCCM size hints and the post-map
 * XMoveWindow survive from it, now followed by the EWMH fullscreen-on-monitor
 * placement a panel-sized window needs under mutter (#729).
 */

#include "dxr_linux_window.h"

#include <X11/keysym.h> // XK_* for the X11 key mapping

#ifdef DXR_APP_HAVE_XRANDR
#include <X11/extensions/Xrandr.h> // XRRGetMonitors — resolve the panel's monitor INDEX
#endif

#ifdef DXR_APP_HAVE_WAYLAND
#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
// THE logical->device conversion, shared verbatim with the runtime so the
// app's output match and the runtime's window-rect conversion cannot drift
// apart (#1595/#1596). Header-only; see src/xrt/auxiliary/util/.
#include "util/u_wayland_geom.h"

#include <linux/input-event-codes.h> // raw evdev keycodes — no xkbcommon dependency
#include <poll.h>                    // non-blocking socket check in pump()
#include <unistd.h>                  // close() the keymap fd we do not read
#endif

#include <chrono> // bounded event-pump budget in the X11 placement handshake
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread> // ...and the 5 ms breather between its polls

// Same shape as the test apps' own macros, so helper output is indistinguishable
// from app output in a run log. INFO flushes: stdout is block-buffered when a
// run is redirected to a file, and these apps are normally ended with a SIGTERM
// (`timeout 20 …`) that discards the buffer — which would take the placement
// line with it, exactly when it is wanted as evidence.
#define DXRW_INFO(fmt, ...)                                                                                            \
	do {                                                                                                           \
		fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__);                                                   \
		fflush(stdout);                                                                                        \
	} while (0)
#define DXRW_WARN(fmt, ...) fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define DXRW_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)


/*
 *
 * Backend selection.
 *
 */

const char *
DxrLinuxWindow::backend_name(DxrWindowBackend b)
{
	switch (b) {
	case DxrWindowBackend::X11: return "x11";
	case DxrWindowBackend::Wayland: return "wayland";
	default: return "auto";
	}
}

bool
DxrLinuxWindow::parse_backend(const char *text, DxrWindowBackend *out)
{
	if (text == nullptr || out == nullptr) {
		return false;
	}
	if (strcmp(text, "x11") == 0) {
		*out = DxrWindowBackend::X11;
		return true;
	}
	if (strcmp(text, "wayland") == 0 || strcmp(text, "wl") == 0) {
		*out = DxrWindowBackend::Wayland;
		return true;
	}
	if (strcmp(text, "auto") == 0) {
		*out = DxrWindowBackend::Auto;
		return true;
	}
	return false;
}

DxrWindowBackend
DxrLinuxWindow::select(DxrWindowBackend requested, bool runtime_has_xlib, bool runtime_has_wayland, std::string *reason)
{
	const char *wl_env = getenv("WAYLAND_DISPLAY");
	const char *x_env = getenv("DISPLAY");
	const bool wl_session = wl_env != nullptr && wl_env[0] != '\0';
	const bool x_session = x_env != nullptr && x_env[0] != '\0';

#ifndef DXR_APP_HAVE_WAYLAND
	// Built without libwayland: the Wayland leg is not compiled in at all.
	if (requested == DxrWindowBackend::Wayland) {
		if (reason != nullptr) {
			*reason = "this binary was built without libwayland-client (no Wayland backend compiled in)";
		}
		return DxrWindowBackend::Auto;
	}
	runtime_has_wayland = false;
#endif

	if (requested == DxrWindowBackend::X11) {
		if (!runtime_has_xlib) {
			if (reason != nullptr) {
				*reason = "--backend=x11 but the runtime does not advertise " +
				          std::string(XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME);
			}
			return DxrWindowBackend::Auto;
		}
		if (!x_session && reason != nullptr) {
			*reason = "explicitly requested (DISPLAY is unset — XOpenDisplay will likely fail)";
		} else if (reason != nullptr) {
			*reason = "explicitly requested";
		}
		return DxrWindowBackend::X11;
	}

	if (requested == DxrWindowBackend::Wayland) {
		if (!runtime_has_wayland) {
			if (reason != nullptr) {
				*reason = "--backend=wayland but the runtime does not advertise " +
				          std::string(XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME);
			}
			return DxrWindowBackend::Auto;
		}
		if (reason != nullptr) {
			*reason = wl_session ? "explicitly requested"
			                     : "explicitly requested (WAYLAND_DISPLAY is unset — connect will likely fail)";
		}
		return DxrWindowBackend::Wayland;
	}

	// Auto. X11 first when it is available at all — including under XWayland,
	// which is the proven, fully-featured path: windowed weaving, app-chosen
	// window position (INV-1.3), and compositor-follows-resize. The native
	// Wayland path is newer and today the runtime only supports FULLSCREEN on
	// it (panel-sized WSI swapchain, no resize follow), so Auto never silently
	// downgrades a working X11 session to it.
	if (x_session && runtime_has_xlib) {
		if (reason != nullptr) {
			*reason = "DISPLAY=" + std::string(x_env) +
			          " resolves and the runtime advertises the xlib binding; X11 is the proven path" +
			          (wl_session ? " (this is a Wayland session, so that is XWayland — pass "
			                        "--backend=wayland for the native path, which is fullscreen-only)"
			                      : "");
		}
		return DxrWindowBackend::X11;
	}
	if (wl_session && runtime_has_wayland) {
		if (reason != nullptr) {
			*reason = "no usable X11 session (DISPLAY unset or xlib binding unavailable)";
		}
		return DxrWindowBackend::Wayland;
	}

	if (reason != nullptr) {
		*reason = "neither backend usable: DISPLAY=" + std::string(x_session ? x_env : "(unset)") +
		          " xlib_binding=" + (runtime_has_xlib ? "yes" : "no") +
		          ", WAYLAND_DISPLAY=" + std::string(wl_session ? wl_env : "(unset)") +
		          " wayland_binding=" + (runtime_has_wayland ? "yes" : "no");
	}
	return DxrWindowBackend::Auto;
}


/*
 *
 * X11 leg.
 *
 */

namespace {

//! Rect of the RandR monitor a fullscreen request was targeted at.
struct X11MonitorRect
{
	int index = -1; //!< RandR monitor index, or -1 when unresolved
	int x = 0, y = 0;
	int width = 0, height = 0;
	std::string name = "?";
};

/*!
 * Resolve the RandR monitor INDEX that owns (@p left, @p top), for the
 * _NET_WM_FULLSCREEN_MONITORS request. Xlib mirror of the runtime's own
 * resolve_monitor_index() in comp_vk_native_window_xcb.c (#723).
 *
 * Prefers a monitor whose origin exactly matches the point; falls back to the
 * monitor CONTAINING it. Returns index -1 when RandR is unavailable (the
 * caller then falls back to plain _NET_WM_STATE_FULLSCREEN, which mutter
 * applies to the output the window currently occupies).
 */
X11MonitorRect
x11_resolve_monitor(Display *dpy, ::Window root, int32_t left, int32_t top)
{
	X11MonitorRect out;
#ifdef DXR_APP_HAVE_XRANDR
	int count = 0;
	XRRMonitorInfo *mons = XRRGetMonitors(dpy, root, True /* active only */, &count);
	if (mons == nullptr) {
		return out;
	}

	int exact = -1;
	int contains = -1;
	for (int i = 0; i < count; i++) {
		if (exact < 0 && mons[i].x == (int)left && mons[i].y == (int)top) {
			exact = i;
		}
		if (contains < 0 && left >= mons[i].x && left < mons[i].x + mons[i].width && top >= mons[i].y &&
		    top < mons[i].y + mons[i].height) {
			contains = i;
		}
	}

	const int chosen = exact >= 0 ? exact : contains;
	if (chosen >= 0) {
		out.index = chosen;
		out.x = mons[chosen].x;
		out.y = mons[chosen].y;
		out.width = mons[chosen].width;
		out.height = mons[chosen].height;
		if (mons[chosen].name != None) {
			char *nm = XGetAtomName(dpy, mons[chosen].name);
			if (nm != nullptr) {
				out.name = nm;
				XFree(nm);
			}
		}
	}
	XRRFreeMonitors(mons);
#else
	(void)dpy;
	(void)root;
	(void)left;
	(void)top;
#endif
	return out;
}

/*!
 * Drain the X event queue for up to @p budget_ms, returning early as soon as
 * @p done() is true. Never blocks indefinitely: XPending never waits, and the
 * deadline is monotonic.
 *
 * Called only during create(), before the app's own pump() exists, so the
 * events drained here are startup noise (Map/Configure/Reparent) that nothing
 * is listening for yet.
 */
void
x11_pump_for(Display *dpy, int budget_ms, const std::function<bool()> &done)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
	for (;;) {
		XSync(dpy, False); // push our requests, take in whatever the WM replied
		while (XPending(dpy) > 0) {
			XEvent ev;
			XNextEvent(dpy, &ev);
		}
		if (done && done()) {
			return;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

//! Root-relative origin of @p win (what the WM actually did with it), as
//! opposed to XGetWindowAttributes' x/y, which are parent-relative and so read
//! as an offset INSIDE the frame once a WM reparents the window.
void
x11_root_origin(Display *dpy, ::Window win, int *out_x, int *out_y)
{
	::Window child = 0;
	int rx = 0;
	int ry = 0;
	if (XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &child) == 0) {
		rx = 0;
		ry = 0;
	}
	*out_x = rx;
	*out_y = ry;
}

//! Post an EWMH client message to the root window (the WM listens for these on
//! SubstructureRedirect|Notify).
void
x11_send_root_message(Display *dpy, ::Window win, Atom type, long d0, long d1, long d2, long d3, long d4)
{
	XEvent ev = {};
	ev.xclient.type = ClientMessage;
	ev.xclient.send_event = True;
	ev.xclient.display = dpy;
	ev.xclient.window = win;
	ev.xclient.message_type = type;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = d0;
	ev.xclient.data.l[1] = d1;
	ev.xclient.data.l[2] = d2;
	ev.xclient.data.l[3] = d3;
	ev.xclient.data.l[4] = d4;
	XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

/*!
 * Ask for an undecorated toplevel through the Motif hints.
 *
 * Belt-and-braces next to _NET_WM_STATE_FULLSCREEN: a fullscreen window is
 * undecorated by EWMH rule anyway, but a WM that ignores EWMH (or that decides
 * not to honour the fullscreen request) would otherwise reparent us into a
 * title bar and steal 74 px off the top of the weave — exactly the #729
 * symptom. Both mutter and KWin honour _MOTIF_WM_HINTS.
 */
void
x11_set_undecorated(Display *dpy, ::Window win)
{
	Atom motif = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
	if (motif == None) {
		return;
	}
	// flags, functions, decorations, input_mode, status — flags=2 is
	// MWM_HINTS_DECORATIONS, decorations=0 is "none".
	unsigned long hints[5] = {2, 0, 0, 0, 0};
	XChangeProperty(dpy, win, motif, motif, 32, PropModeReplace, (const unsigned char *)hints, 5);
}

} // namespace

bool
DxrLinuxWindow::create_x11(const DxrLinuxWindowDesc &desc)
{
	const int32_t screenLeft = desc.panel_left;
	const int32_t screenTop = desc.panel_top;

	// A window asking for exactly the panel's size IS the fullscreen demo
	// mode, and must be genuinely fullscreen on the panel: exact 1:1, no
	// decoration, no offset. Anything smaller is a deliberate windowed run
	// (DXR_CUBE_WINDOW) and keeps the plain create-at-position behaviour.
	// DXR_X11_NO_FULLSCREEN=1 forces the old path for A/B testing.
	const char *no_fs = getenv("DXR_X11_NO_FULLSCREEN");
	const bool fs_opt_out = no_fs != nullptr && no_fs[0] != '\0' && strcmp(no_fs, "0") != 0;
	const bool panel_known = desc.panel_width > 0 && desc.panel_height > 0;
	const bool want_fullscreen =
	    panel_known && !fs_opt_out && desc.width == desc.panel_width && desc.height == desc.panel_height;

	// #1588: a WINDOWED toplevel is undecorated + client-dragged as well, so
	// every move can be routed through the weave's lattice snap. Opt out for
	// the old decorated, WM-dragged behaviour.
	const char *wm_dec = getenv("DXR_X11_WM_DECORATIONS");
	m_x_wm_drag = !want_fullscreen && wm_dec != nullptr && wm_dec[0] != '\0' && strcmp(wm_dec, "0") != 0;
	const bool client_drag = !want_fullscreen && !m_x_wm_drag;
	m_x_client_drag = client_drag;

	m_x_display = XOpenDisplay(nullptr);
	if (m_x_display == nullptr) {
		DXRW_ERROR("XOpenDisplay failed — is DISPLAY set?");
		return false;
	}

	// INV-1.3: open on the 3D panel (#715). (screenLeft, screenTop) is the
	// panel top-left in virtual-desktop pixels (top-down, origin = primary
	// top-left, XrDisplayDesktopPositionDXR); (0,0) = primary/unknown is a
	// safe create position either way.
	int screen = DefaultScreen(m_x_display);
	m_x_window = XCreateSimpleWindow(m_x_display, RootWindow(m_x_display, screen), screenLeft, screenTop, desc.width,
	                                 desc.height, 0, BlackPixel(m_x_display, screen), BlackPixel(m_x_display, screen));
	if (m_x_window == 0) {
		DXRW_ERROR("XCreateSimpleWindow failed");
		return false;
	}

	// WM_NORMAL_HINTS with USPosition|PPosition, so the window manager treats
	// the create-time position as intentional instead of auto-placing the
	// window (ICCCM §4.1.2.3; GNOME/Mutter auto-places without this). Mirrors
	// the runtime's own hosted-window placement (comp_vk_native_window_xcb.c).
	{
		XSizeHints hints = {};
		hints.flags = USPosition | PPosition;
		hints.x = screenLeft;
		hints.y = screenTop;
		XSetWMNormalHints(m_x_display, m_x_window, &hints);
	}

	XStoreName(m_x_display, m_x_window, desc.title);
	// Button + motion events are what the client-owned drag runs on (#1588).
	// Harmless when the drag is off; selecting them unconditionally keeps one
	// input mask for both paths.
	XSelectInput(m_x_display, m_x_window,
	             StructureNotifyMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
	                 Button1MotionMask);

	// Clean close on the window manager's close button.
	m_x_wm_delete = XInternAtom(m_x_display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(m_x_display, m_x_window, &m_x_wm_delete, 1);

	// Decorations off BEFORE the map, so a frame is never created in the first
	// place. Mutter reparenting us into a title bar is what clamped the #729
	// window to 3840x2086 at (3456, 74) — and for a WINDOWED run (#1588) the
	// frame is worse than an offset: it hands the drag to the WM, where the
	// interlace phase cannot be snapped. Undecorated is therefore the default
	// for both, and DXR_X11_WM_DECORATIONS=1 is the escape hatch.
	if (want_fullscreen || client_drag) {
		x11_set_undecorated(m_x_display, m_x_window);
	}

	XMapWindow(m_x_display, m_x_window);
	XFlush(m_x_display);

	// Re-assert the position after mapping — many WMs (Mutter included)
	// ignore the create-time x/y of a freshly mapped toplevel, but honor a
	// post-map ConfigureRequest (this is what `xdotool windowmove` sends).
	XMoveWindow(m_x_display, m_x_window, screenLeft, screenTop);
	XFlush(m_x_display);

	X11MonitorRect mon;
	if (want_fullscreen) {
		::Window root = DefaultRootWindow(m_x_display);
		mon = x11_resolve_monitor(m_x_display, root, screenLeft, screenTop);

		// Let the move land before asking for fullscreen: mutter fullscreens
		// onto whichever output the window CURRENTLY occupies, and a position
		// request issued before the window settles is simply discarded. Wait
		// for the window's root origin to reach the target monitor, with a
		// hard 400 ms ceiling so a WM that never moves us cannot hang startup.
		const int mon_x = mon.index >= 0 ? mon.x : (int)screenLeft;
		const int mon_y = mon.index >= 0 ? mon.y : (int)screenTop;
		const int mon_w = mon.index >= 0 ? mon.width : 1;
		const int mon_h = mon.index >= 0 ? mon.height : 1;
		Display *dpy = m_x_display;
		::Window win = m_x_window;
		x11_pump_for(dpy, 400, [dpy, win, mon_x, mon_y, mon_w, mon_h]() {
			int rx = 0;
			int ry = 0;
			x11_root_origin(dpy, win, &rx, &ry);
			return rx >= mon_x && rx < mon_x + mon_w && ry >= mon_y && ry < mon_y + mon_h;
		});

		// EWMH fullscreen. _NET_WM_STATE add first (source 1 = application),
		// then pin it to the panel's RandR monitor: single-monitor fullscreen
		// means all four edges are the same index. Without RandR we still send
		// the fullscreen request — mutter applies it to the output the window
		// now occupies, which the pump above just made the right one.
		Atom net_wm_state = XInternAtom(m_x_display, "_NET_WM_STATE", False);
		Atom net_wm_state_fullscreen = XInternAtom(m_x_display, "_NET_WM_STATE_FULLSCREEN", False);
		if (net_wm_state != None && net_wm_state_fullscreen != None) {
			x11_send_root_message(m_x_display, m_x_window, net_wm_state, 1 /* _NET_WM_STATE_ADD */,
			                      (long)net_wm_state_fullscreen, 0, 1 /* source: application */, 0);
		}
		if (mon.index >= 0) {
			Atom net_fs_monitors = XInternAtom(m_x_display, "_NET_WM_FULLSCREEN_MONITORS", False);
			if (net_fs_monitors != None) {
				x11_send_root_message(m_x_display, m_x_window, net_fs_monitors, mon.index /* top */,
				                      mon.index /* bottom */, mon.index /* left */,
				                      mon.index /* right */, 1 /* source: application */);
			}
		}
		XFlush(m_x_display);

		// Second bounded wait: let the fullscreen configure arrive, so the log
		// line below (and the runtime's first swapchain sizing) sees the truth.
		const uint32_t want_w = desc.width;
		const uint32_t want_h = desc.height;
		x11_pump_for(dpy, 400, [dpy, win, want_w, want_h]() {
			XWindowAttributes wa = {};
			return XGetWindowAttributes(dpy, win, &wa) != 0 && (uint32_t)wa.width == want_w &&
			       (uint32_t)wa.height == want_h;
		});
	} else {
		// Windowed: no fullscreen handshake, but still give the WM a moment to
		// reparent and place us, so the line below reports where the window
		// really ended up rather than where it was a millisecond after the
		// move request (under mutter those differ by the frame's title bar).
		x11_pump_for(m_x_display, 250, {});
	}

	// One line, after the handshake: what was asked for and what the WM
	// actually did. XTranslateCoordinates is the honest origin (XGetWindow-
	// Attributes' x/y are parent-relative once a WM reparents us), so this
	// proves placement without reaching for xwininfo.
	{
		XWindowAttributes wa = {};
		int rx = 0;
		int ry = 0;
		XGetWindowAttributes(m_x_display, m_x_window, &wa);
		x11_root_origin(m_x_display, m_x_window, &rx, &ry);
		if (want_fullscreen) {
			char where[96];
			if (mon.index >= 0) {
				snprintf(where, sizeof(where), "fullscreen on RandR monitor #%d (%s)", mon.index,
				         mon.name.c_str());
			} else {
				snprintf(where, sizeof(where), "fullscreen on the current output (no RandR monitor at "
				                               "the panel position)");
			}
			DXRW_INFO("Created app-owned X11 window 0x%lx: requested %ux%u at (%d, %d) %s; actual %dx%d "
			          "at (%d, %d)",
			          m_x_window, desc.width, desc.height, screenLeft, screenTop, where, wa.width,
			          wa.height, rx, ry);
		} else {
                  DXRW_INFO("Created app-owned X11 window 0x%lx: requested "
                            "%ux%u at (%d, %d) windowed%s%s; "
                            "actual %dx%d at (%d, %d)",
                            m_x_window, desc.width, desc.height, screenLeft,
                            screenTop,
                            fs_opt_out ? " (DXR_X11_NO_FULLSCREEN)" : "",
                            client_drag ? ", undecorated + client-owned drag "
                                          "(snap offered by the display "
                                          "processor; each landing is "
                                          "verified, see 'drag: placement')"
                                        : " (DXR_X11_WM_DECORATIONS)",
                            wa.width, wa.height, rx, ry);
                }
		m_x_drag_at_x = rx;
		m_x_drag_at_y = ry;
	}

	// DXR_X11_TEST_DRAG=dx,dy,steps — TEST HOOK, off by default. Only meaningful
	// where a drag is possible at all (windowed + client-owned).
	if (client_drag) {
		if (const char *tenv = getenv("DXR_X11_TEST_DRAG")) {
			int dx = 0, dy = 0, steps = 0;
			if (sscanf(tenv, "%d,%d,%d", &dx, &dy, &steps) == 3 && steps > 0) {
				m_x_test_drag_armed = true;
				m_x_test_drag_dx = dx;
				m_x_test_drag_dy = dy;
				m_x_test_drag_steps = steps;
				DXRW_WARN("DXR_X11_TEST_DRAG=%d,%d,%d — TEST HOOK armed; the window will walk "
				          "that offset in %d snapped steps after a warm-up",
				          dx, dy, steps, steps);
			} else {
				DXRW_WARN("DXR_X11_TEST_DRAG=\"%s\" is not dx,dy,steps — ignored", tenv);
			}
		}
	}
	return true;
}

/*
 *
 * X11 client-owned drag (#1588).
 *
 */

void
DxrLinuxWindow::snap_origin(int origin_x, int origin_y, int target_x, int target_y, int *out_x, int *out_y)
{
	int32_t sx = (int32_t)target_x;
	int32_t sy = (int32_t)target_y;
	bool snapped = false;
	if (m_snap_fn != nullptr) {
		snapped = m_snap_fn(m_snap_userdata, (int32_t)origin_x, (int32_t)origin_y, (int32_t)target_x,
		                    (int32_t)target_y, &sx, &sy);
	}
	if (!snapped) {
		sx = (int32_t)target_x;
		sy = (int32_t)target_y;
	}
	if (!m_snap_reported) {
		m_snap_reported = true;
                DXRW_INFO("drag: snap provider %s — %s",
                          m_snap_fn != nullptr ? "installed"
                                               : "ABSENT (identity)",
                          snapped ? "the display processor is OFFERING snapped "
                                    "origins — whether the window "
                                    "actually lands on them is checked per "
                                    "move ('drag: placement')"
                                  : "identity (no DP lattice snap on this "
                                    "runtime, or the runtime refused "
                                    "the snap — see its log); the drag "
                                    "mechanics are unaffected");
        }
	*out_x = (int)sx;
	*out_y = (int)sy;
}

void
DxrLinuxWindow::x11_move_snapped(int target_x, int target_y)
{
	if (m_x_display == nullptr || m_x_window == 0) {
		return;
	}
        x11_check_landing();

        int sx = target_x;
	int sy = target_y;
	snap_origin(m_x_drag_origin_x, m_x_drag_origin_y, target_x, target_y, &sx, &sy);

	if (sx != target_x || sy != target_y) {
		m_x_drag_snapped++;
		// On-change only: with an identity snap this never fires, which is
		// the point — a per-motion log line would be per-event spam.
		DXRW_INFO("drag: raw (%d, %d) -> snapped (%d, %d)", target_x, target_y, sx, sy);
	}
	if (sx == m_x_drag_at_x && sy == m_x_drag_at_y) {
		return; // the lattice swallowed this step; do not churn the WM
	}
	XMoveWindow(m_x_display, m_x_window, sx, sy);
	XFlush(m_x_display);
	m_x_drag_at_x = sx;
	m_x_drag_at_y = sy;
	m_x_drag_moves++;
        m_x_probe_pending = true;
        m_x_probe_want_x = sx;
        m_x_probe_want_y = sy;
}

void DxrLinuxWindow::x11_check_landing() {
  if (!m_x_probe_pending || m_x_display == nullptr || m_x_window == 0) {
    return;
  }
  m_x_probe_pending = false;
  int got_x = 0, got_y = 0;
  x11_root_origin(m_x_display, m_x_window, &got_x, &got_y);
  u_x11_placement_probe_note(&m_x_probe, m_x_probe_want_x, m_x_probe_want_y,
                             got_x, got_y);

  if (!m_x_probe.reported && u_x11_placement_probe_is_quantized(&m_x_probe)) {
    m_x_probe.reported = true;
    // One line, once per process: the claim a snapped drag makes is
    // "the window lands where the lens wants it". It does not, so say so,
    // and name the cause that has actually produced this.
    DXRW_WARN("drag: placement NOT honoured — %u of %u moves landed somewhere "
              "other than the "
              "requested origin (worst %u px); landed positions fall on a %u "
              "px lattice. The "
              "window cannot reach every pixel, so the 3D will stutter while "
              "dragging. Most "
              "likely cause: XWayland is running the X screen at global scale "
              "%u because some "
              "output (often NOT the 3D panel) is scaled above 100%%. Fix: "
              "every output at "
              "100%%, or set DXR_X11_PLACEMENT_QUANTUM=%u so the runtime snaps "
              "on the reachable "
              "lattice. `displayxr-cli info` shows the per-output evidence.",
              m_x_probe.diverged, m_x_probe.moves, m_x_probe.worst_delta,
              m_x_probe.inferred_quantum, m_x_probe.inferred_quantum,
              m_x_probe.inferred_quantum);
  }
}

void
DxrLinuxWindow::x11_drive_test_drag()
{
	// Warm-up: let the session come up and the first frames present before
	// the window starts moving, so the runtime's origin trace is readable.
	const uint64_t kWarmupFrames = 60;
	if (!m_x_test_drag_armed || m_x_test_drag_done || m_x_pump_count < kWarmupFrames) {
		return;
	}

	if (m_x_test_drag_step == 0) {
		// Same bookkeeping a ButtonPress does: latch the drag origin.
		x11_root_origin(m_x_display, m_x_window, &m_x_drag_origin_x, &m_x_drag_origin_y);
		m_x_drag_at_x = m_x_drag_origin_x;
		m_x_drag_at_y = m_x_drag_origin_y;
		m_x_drag_moves = 0;
		m_x_drag_snapped = 0;
		DXRW_INFO("drag: start (TEST HOOK) — grab origin (%d, %d), walking %+d,%+d in %d steps",
		          m_x_drag_origin_x, m_x_drag_origin_y, m_x_test_drag_dx, m_x_test_drag_dy,
		          m_x_test_drag_steps);
	}

	m_x_test_drag_step++;
	// Integer-exact endpoint: step i of N lands on origin + d*i/N, so the last
	// step is origin + d with no rounding residue.
	const int i = m_x_test_drag_step;
	const int n = m_x_test_drag_steps;
	const int tx = m_x_drag_origin_x + (int)((int64_t)m_x_test_drag_dx * i / n);
	const int ty = m_x_drag_origin_y + (int)((int64_t)m_x_test_drag_dy * i / n);
	x11_move_snapped(tx, ty);

	if (i >= n) {
		m_x_test_drag_done = true;
                DXRW_INFO("drag: end (TEST HOOK) — %llu move(s), %llu snapped "
                          "away from the raw target, "
                          "origin (%d, %d) -> (%d, %d); placement so far: %u "
                          "of %u verified moves landed exactly",
                          (unsigned long long)m_x_drag_moves,
                          (unsigned long long)m_x_drag_snapped,
                          m_x_drag_origin_x, m_x_drag_origin_y, m_x_drag_at_x,
                          m_x_drag_at_y, m_x_probe.moves - m_x_probe.diverged,
                          m_x_probe.moves);
        }
}

//! Map an X keysym onto the backend-neutral key identity.
static DxrKey
dxr_key_from_keysym(KeySym ks)
{
	switch (ks) {
	case XK_Escape: return DxrKey::Escape;
	case XK_q:
	case XK_Q: return DxrKey::Q;
	case XK_m:
	case XK_M: return DxrKey::M;
	case XK_o:
	case XK_O: return DxrKey::O;
	case XK_v:
	case XK_V: return DxrKey::V;
	case XK_1: return DxrKey::Num1;
	case XK_2: return DxrKey::Num2;
	case XK_3: return DxrKey::Num3;
	default: return DxrKey::Unknown;
	}
}

void
DxrLinuxWindow::destroy_x11()
{
	if (m_x_display != nullptr) {
		if (m_x_window != 0) {
			XDestroyWindow(m_x_display, m_x_window);
			m_x_window = 0;
		}
		XCloseDisplay(m_x_display);
		m_x_display = nullptr;
	}
}


/*
 *
 * Wayland leg.
 *
 */

#ifdef DXR_APP_HAVE_WAYLAND

void
DxrLinuxWindow::s_registry_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);

	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		self->m_wl_compositor = static_cast<struct wl_compositor *>(
		    wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4));
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		self->m_wl_wm_base =
		    static_cast<struct xdg_wm_base *>(wl_registry_bind(r, name, &xdg_wm_base_interface, 1));
		static const struct xdg_wm_base_listener kWmBaseListener = {
		    s_wm_base_ping,
		};
		xdg_wm_base_add_listener(self->m_wl_wm_base, &kWmBaseListener, self);
	} else if (strcmp(iface, wl_seat_interface.name) == 0 && self->m_wl_seat == nullptr) {
		self->m_wl_seat =
		    static_cast<struct wl_seat *>(wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5));
		static const struct wl_seat_listener kSeatListener = {
		    s_seat_capabilities,
		    s_seat_name,
		};
		wl_seat_add_listener(self->m_wl_seat, &kSeatListener, self);
	} else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
		// #1596. Core wl_output cannot express a fractional scale, so without
		// this the app has no way to convert a logical origin into the device
		// pixels the runtime's panel rect is expressed in.
		self->m_wl_xdg_output_manager = static_cast<struct zxdg_output_manager_v1 *>(
		    wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, version < 2 ? version : 2));
		// Outputs may have arrived before the manager did; give them their
		// xdg_output now rather than relying on registry ordering.
		for (auto &out : self->m_wl_outputs) {
			self->wl_attach_xdg_output(out);
		}
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		WlOutput out = {};
		out.name = name;
		out.output = static_cast<struct wl_output *>(
		    wl_registry_bind(r, name, &wl_output_interface, version < 2 ? version : 2));
		self->m_wl_outputs.push_back(out);
		static const struct wl_output_listener kOutputListener = {
		    s_output_geometry, s_output_mode, s_output_done, s_output_scale, s_output_name, s_output_description,
		};
		wl_output_add_listener(out.output, &kOutputListener, self);
		self->wl_attach_xdg_output(self->m_wl_outputs.back());
	}
}

void
DxrLinuxWindow::wl_attach_xdg_output(WlOutput &out)
{
	if (m_wl_xdg_output_manager == nullptr || out.xdg_output != nullptr || out.output == nullptr) {
		return;
	}
	out.xdg_output = zxdg_output_manager_v1_get_xdg_output(m_wl_xdg_output_manager, out.output);
	static const struct zxdg_output_v1_listener kXdgOutputListener = {
	    s_xdg_output_logical_position, s_xdg_output_logical_size,
	    s_xdg_output_done,             s_xdg_output_name,
	    s_xdg_output_description,
	};
	zxdg_output_v1_add_listener(out.xdg_output, &kXdgOutputListener, this);
}

void
DxrLinuxWindow::s_registry_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)r;
	auto *self = static_cast<DxrLinuxWindow *>(data);
	// Outputs can come and go (hotplug); the surface keeps its fullscreen
	// state, so all we must do is stop tracking the record.
	for (auto it = self->m_wl_outputs.begin(); it != self->m_wl_outputs.end(); ++it) {
		if (it->name == name) {
			if (it->xdg_output != nullptr) {
				zxdg_output_v1_destroy(it->xdg_output);
			}
			self->m_wl_outputs.erase(it);
			return;
		}
	}
}

void
DxrLinuxWindow::s_wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	(void)data;
	// Mandatory: a client that does not pong is killed as unresponsive.
	xdg_wm_base_pong(b, serial);
}

void
DxrLinuxWindow::s_xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);
	xdg_surface_ack_configure(s, serial);
	self->m_wl_configured = true;
}

void
DxrLinuxWindow::s_toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
	(void)t;
	(void)states;
	auto *self = static_cast<DxrLinuxWindow *>(data);
	// 0x0 means "you choose"; keep whatever we asked for.
	if (w > 0 && h > 0) {
		self->m_wl_config_w = w;
		self->m_wl_config_h = h;
	}
}

void
DxrLinuxWindow::s_toplevel_close(void *data, struct xdg_toplevel *t)
{
	(void)t;
	static_cast<DxrLinuxWindow *>(data)->m_wl_close_requested = true;
}

void
DxrLinuxWindow::s_toplevel_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
	(void)data;
	(void)t;
	(void)w;
	(void)h;
}

void
DxrLinuxWindow::s_toplevel_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *caps)
{
	(void)data;
	(void)t;
	(void)caps;
}

void
DxrLinuxWindow::s_output_geometry(void *data,
                                  struct wl_output *o,
                                  int32_t x,
                                  int32_t y,
                                  int32_t pw,
                                  int32_t ph,
                                  int32_t subpixel,
                                  const char *make,
                                  const char *model,
                                  int32_t transform)
{
	(void)pw;
	(void)ph;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.output == o) {
			// LOGICAL, despite the event's name. xdg_output.logical_position
			// supersedes this when the manager exists; this is the seed for a
			// compositor that has no xdg-output.
			out.logical_x = x;
			out.logical_y = y;
			return;
		}
	}
}

void
DxrLinuxWindow::s_output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) {
		return;
	}
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.output == o) {
			// DEVICE pixels — the one core-protocol geometry that already is.
			out.mode_w = w;
			out.mode_h = h;
			// Already milli-hertz on the wire; XrWaylandSurfaceGeometryDXR
			// takes the same unit, so it passes through untouched. The runtime
			// needs it because Wayland has no XCB connection for the RandR
			// query that serves the X11 leg, and its fallback is a hardcoded
			// 60 Hz.
			out.refresh_mhz = refresh;
			return;
		}
	}
}

void
DxrLinuxWindow::s_output_done(void *data, struct wl_output *o)
{
	(void)data;
	(void)o;
}

void
DxrLinuxWindow::s_output_scale(void *data, struct wl_output *o, int32_t factor)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.output == o) {
			// Recorded for diagnostics ONLY — this is an integer by protocol
			// and is 2 on this box's 1.6667 output. Never a conversion factor.
			out.int_scale = factor > 0 ? factor : 1;
			return;
		}
	}
}

void
DxrLinuxWindow::s_output_name(void *data, struct wl_output *o, const char *name)
{
	(void)data;
	(void)o;
	(void)name;
}

void
DxrLinuxWindow::s_output_description(void *data, struct wl_output *o, const char *desc)
{
	(void)data;
	(void)o;
	(void)desc;
}

void
DxrLinuxWindow::s_xdg_output_logical_position(void *data, struct zxdg_output_v1 *o, int32_t x, int32_t y)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.xdg_output == o) {
			out.logical_x = x;
			out.logical_y = y;
			return;
		}
	}
}

void
DxrLinuxWindow::s_xdg_output_logical_size(void *data, struct zxdg_output_v1 *o, int32_t w, int32_t h)
{
	// THE missing number (#1596). Nothing in core wl_output reports an
	// output's logical SIZE, and without it `mode / logical_size` — the only
	// honest fractional scale a client can compute — is unavailable.
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.xdg_output == o) {
			out.logical_w = w;
			out.logical_h = h;
			out.have_logical_size = (w > 0 && h > 0);
			return;
		}
	}
}

void
DxrLinuxWindow::s_xdg_output_done(void *data, struct zxdg_output_v1 *o)
{
	(void)data;
	(void)o;
}

void
DxrLinuxWindow::s_xdg_output_name(void *data, struct zxdg_output_v1 *o, const char *name)
{
	(void)data;
	(void)o;
	(void)name;
}

void
DxrLinuxWindow::s_xdg_output_description(void *data, struct zxdg_output_v1 *o, const char *desc)
{
	(void)data;
	(void)o;
	(void)desc;
}

void
DxrLinuxWindow::s_seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);
	const bool has_kb = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
	if (has_kb && self->m_wl_keyboard == nullptr) {
		self->m_wl_keyboard = wl_seat_get_keyboard(seat);
		static const struct wl_keyboard_listener kKbListener = {
		    s_kb_keymap, s_kb_enter, s_kb_leave, s_kb_key, s_kb_modifiers, s_kb_repeat_info,
		};
		wl_keyboard_add_listener(self->m_wl_keyboard, &kKbListener, self);
	} else if (!has_kb && self->m_wl_keyboard != nullptr) {
		wl_keyboard_release(self->m_wl_keyboard);
		self->m_wl_keyboard = nullptr;
	}
}

void
DxrLinuxWindow::s_seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
	(void)name;
}

void
DxrLinuxWindow::s_kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
	(void)data;
	(void)kb;
	(void)format;
	(void)size;
	// We decode raw evdev keycodes below, so the xkb keymap is unused — but the
	// fd is ours now and leaks one descriptor per keymap change if not closed.
	if (fd >= 0) {
		close(fd);
	}
}

void
DxrLinuxWindow::s_kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)s;
	(void)keys;
}

void
DxrLinuxWindow::s_kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)s;
}

void
DxrLinuxWindow::s_kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	(void)kb;
	(void)serial;
	(void)time;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return;
	}
	auto *self = static_cast<DxrLinuxWindow *>(data);

	// `key` is a raw evdev keycode (linux/input-event-codes.h). Decoding it
	// directly keeps the apps off libxkbcommon for the four or five keys they
	// bind; it is layout-dependent in principle, but these are positional test
	// bindings, and the X11 leg's XLookupKeysym(index 0) is equally so.
	DxrKey k = DxrKey::Unknown;
	switch (key) {
	case KEY_ESC: k = DxrKey::Escape; break;
	case KEY_Q: k = DxrKey::Q; break;
	case KEY_M: k = DxrKey::M; break;
	case KEY_O: k = DxrKey::O; break;
	case KEY_V: k = DxrKey::V; break;
	case KEY_1: k = DxrKey::Num1; break;
	case KEY_2: k = DxrKey::Num2; break;
	case KEY_3: k = DxrKey::Num3; break;
	default: return;
	}
	self->m_wl_key_queue.push_back(k);
}

void
DxrLinuxWindow::s_kb_modifiers(void *data,
                               struct wl_keyboard *kb,
                               uint32_t serial,
                               uint32_t depressed,
                               uint32_t latched,
                               uint32_t locked,
                               uint32_t group)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)depressed;
	(void)latched;
	(void)locked;
	(void)group;
}

void
DxrLinuxWindow::s_kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
	(void)data;
	(void)kb;
	(void)rate;
	(void)delay;
}

bool
DxrLinuxWindow::create_wayland(const DxrLinuxWindowDesc &desc)
{
	m_wl_display = wl_display_connect(nullptr);
	if (m_wl_display == nullptr) {
		DXRW_ERROR("wl_display_connect failed — is WAYLAND_DISPLAY set?");
		return false;
	}

	m_wl_registry = wl_display_get_registry(m_wl_display);
	static const struct wl_registry_listener kRegistryListener = {
	    s_registry_global,
	    s_registry_global_remove,
	};
	wl_registry_add_listener(m_wl_registry, &kRegistryListener, this);

	// First roundtrip: globals. Second: the per-output geometry/mode/scale
	// bursts the first one only triggered. Third: the zxdg_output_v1 bursts
	// (#1596) — an xdg_output created during the registry callback, or during
	// the manager's own bind, only answers on the NEXT round trip, and without
	// its logical_size there is no fractional scale and therefore no
	// device-pixel panel match.
	wl_display_roundtrip(m_wl_display);
	wl_display_roundtrip(m_wl_display);
	wl_display_roundtrip(m_wl_display);

	if (m_wl_compositor == nullptr) {
		DXRW_ERROR("Wayland compositor advertises no wl_compositor");
		return false;
	}
	if (m_wl_wm_base == nullptr) {
		DXRW_ERROR("Wayland compositor advertises no xdg_wm_base — xdg-shell is required");
		return false;
	}

	m_wl_surface = wl_compositor_create_surface(m_wl_compositor);
	if (m_wl_surface == nullptr) {
		DXRW_ERROR("wl_compositor_create_surface failed");
		return false;
	}

	m_wl_xdg_surface = xdg_wm_base_get_xdg_surface(m_wl_wm_base, m_wl_surface);
	static const struct xdg_surface_listener kXdgSurfaceListener = {
	    s_xdg_surface_configure,
	};
	xdg_surface_add_listener(m_wl_xdg_surface, &kXdgSurfaceListener, this);

	m_wl_toplevel = xdg_surface_get_toplevel(m_wl_xdg_surface);
	static const struct xdg_toplevel_listener kToplevelListener = {
	    s_toplevel_configure,
	    s_toplevel_close,
	    s_toplevel_configure_bounds,
	    s_toplevel_wm_capabilities,
	};
	xdg_toplevel_add_listener(m_wl_toplevel, &kToplevelListener, this);
	xdg_toplevel_set_title(m_wl_toplevel, desc.title);
	xdg_toplevel_set_app_id(m_wl_toplevel, desc.app_id);

	m_wl_config_w = (int32_t)desc.width;
	m_wl_config_h = (int32_t)desc.height;

	if (desc.fullscreen_on_wayland) {
		/*
		 * INV-1.3 substitute. A Wayland client cannot place itself, so the
		 * only way to land on the 3D panel is to go fullscreen on the
		 * wl_output that IS the panel.
		 *
		 * The match is made in DEVICE PIXELS (#1596). It used to compare
		 * `wl_output.geometry`'s LOGICAL origin against the runtime's
		 * device-pixel panel rect, which on a scaled desktop can never
		 * succeed: on the measured box the DS1 sits at logical x=1728 and
		 * device x=3456, so the comparison was 1728 == 3456 and the app
		 * fullscreened on whatever output the compositor chose — the laptop.
		 *
		 * SIZE is the reliable half and needs no conversion at all: an
		 * output's `wl_output.mode` is device pixels by protocol and the
		 * runtime's `displayPixelWidth/Height` is device pixels by contract.
		 * The converted ORIGIN is the corroborating half, and it is only
		 * required as a TIE-BREAK, because a single size match is already
		 * unambiguous and the two coordinate spaces can legitimately disagree
		 * (an XWayland root scales the whole layout by one integer factor
		 * while each output has its own). Same rule, and the same reason, as
		 * the runtime's own `OS_DISPLAY_DESKTOP_RULE_PIXEL_MATCH`.
		 */
		struct wl_output *chosen = nullptr;
		if (desc.panel_width != 0 && desc.panel_height != 0) {
			const WlOutput *size_match = nullptr;
			const WlOutput *exact_match = nullptr;
			size_t size_match_count = 0;
			bool any_logical_size = false;

			for (const auto &out : m_wl_outputs) {
				// mode / logical_size, never the integer wl_output.scale.
				// With no xdg_output the scale is unresolvable and only the
				// size half of the match can run — still device-vs-device,
				// still correct, just without the origin tie-break.
				struct u_wl_monitor mon = {};
				mon.logical_x = out.logical_x;
				mon.logical_y = out.logical_y;
				mon.logical_w = out.have_logical_size ? out.logical_w : 0;
				mon.logical_h = out.have_logical_size ? out.logical_h : 0;
				mon.mode_w = out.mode_w;
				mon.mode_h = out.mode_h;
				any_logical_size |= out.have_logical_size;

				bool origin_agrees = false;
				if (!u_wl_monitor_is_panel(&mon, desc.panel_left, desc.panel_top, desc.panel_width,
				                           desc.panel_height, &origin_agrees)) {
					continue;
				}
				size_match_count++;
				if (size_match == nullptr) {
					size_match = &out;
				}
				if (origin_agrees && exact_match == nullptr) {
					exact_match = &out;
				}
			}

			const WlOutput *picked = exact_match != nullptr ? exact_match : nullptr;
			if (picked == nullptr && size_match_count == 1) {
				picked = size_match;
			}

			if (picked != nullptr) {
				chosen = picked->output;
				m_wl_refresh_mhz = picked->refresh_mhz > 0 ? (uint32_t)picked->refresh_mhz : 0;
				// Device-pixel mode size — the buffer size that lands 1:1 on
				// this output. See m_wl_fullscreen_mode_w in the header.
				m_wl_fullscreen_mode_w = picked->mode_w;
				m_wl_fullscreen_mode_h = picked->mode_h;

				if (exact_match != nullptr) {
					DXRW_INFO("Wayland: fullscreen on the wl_output matching the 3D panel rect "
					          "%ux%u+%d+%d in device pixels (logical origin %d,%d)",
					          desc.panel_width, desc.panel_height, desc.panel_left,
					          desc.panel_top, picked->logical_x, picked->logical_y);
				} else {
					DXRW_WARN("Wayland: one wl_output is the 3D panel's size (%ux%u device px) but "
					          "its converted origin does not match the runtime's %d,%d — taking it "
					          "anyway, since the size match is unambiguous. The two coordinate "
					          "spaces disagreeing is expected when the runtime resolved the panel "
					          "through XWayland's RandR view of the layout.",
					          desc.panel_width, desc.panel_height, desc.panel_left, desc.panel_top);
				}
			} else if (size_match_count > 1) {
				DXRW_WARN("Wayland: %zu wl_outputs are %ux%u device px and none has the runtime's "
				          "origin %d,%d — cannot tell which is the 3D panel, so going fullscreen on "
				          "the compositor's choice. INV-1.3 placement is not guaranteed.",
				          size_match_count, desc.panel_width, desc.panel_height, desc.panel_left,
				          desc.panel_top);
			} else {
				DXRW_WARN("Wayland: no wl_output is %ux%u device pixels (%zu output(s) seen%s) — "
				          "going fullscreen on the compositor's choice. INV-1.3 placement is not "
				          "guaranteed, and the runtime will refuse to weave into the resample that "
				          "follows (#1595).",
				          desc.panel_width, desc.panel_height, m_wl_outputs.size(),
				          any_logical_size ? "" : ", none reporting a logical size — is "
				                                  "zxdg_output_manager_v1 advertised?");
			}
		}
		xdg_toplevel_set_fullscreen(m_wl_toplevel, chosen);
	} else {
		// Windowed is supported from extension spec v2: the size below is
		// declared through XrWaylandSurfaceGeometryDXR, so the runtime sizes
		// its swapchain to this surface instead of resizing it to the panel.
		// What windowed still needs, and fullscreen does not, is the
		// compositor geometry service for the weave PHASE — Wayland never
		// tells a client where its surface is.
		DXRW_WARN("Wayland: windowed mode — the surface size is declared to the runtime "
		          "(XrWaylandSurfaceGeometryDXR), so the swapchain follows the window. The weave PHASE still "
		          "needs the window-geometry service (window-geometry@displayxr.org); without it the runtime "
		          "weaves display-scoped, which is wrong for a window that is not at the panel origin.");
	}

	// The role is attached and the state requested; commit so the compositor
	// sends the initial configure. NOTE: this is the ONLY commit this helper
	// ever performs — once the session exists, Mesa's WSI owns attach/damage/
	// commit on this surface and a second commit from the app is a bug.
	wl_surface_commit(m_wl_surface);

	// Block until the initial xdg_surface.configure has been acked. Doing this
	// BEFORE xrCreateSession is mandatory: the runtime calls
	// vkCreateWaylandSurfaceKHR synchronously inside the session create
	// (comp_vk_native_target.cpp:1797) and the WSI attaches a buffer at the
	// first present — attaching to a role-less or unconfigured surface is a
	// protocol error that kills the connection.
	for (int i = 0; i < 100 && !m_wl_configured; i++) {
		if (wl_display_roundtrip(m_wl_display) < 0) {
			DXRW_ERROR("Wayland connection error while waiting for the initial configure");
			return false;
		}
	}
	if (!m_wl_configured) {
		DXRW_ERROR("No xdg_surface.configure after 100 roundtrips — giving up");
		return false;
	}

	// Refresh for the geometry struct. Fullscreen took it from the matched
	// output above; windowed has no wl_surface.enter listener here, so fall
	// back to the first output that reported a mode. Wrong only on a
	// mixed-refresh multi-head desktop, and 0 (unknown) is always safe — the
	// runtime then keeps its 60 Hz default.
	if (m_wl_refresh_mhz == 0) {
		for (const auto &out : m_wl_outputs) {
			if (out.refresh_mhz > 0) {
				m_wl_refresh_mhz = (uint32_t)out.refresh_mhz;
				break;
			}
		}
	}

	{
		uint32_t dw = 0, dh = 0;
		wl_declared_size(&dw, &dh);
		if ((int32_t)dw != m_wl_config_w || (int32_t)dh != m_wl_config_h) {
			DXRW_WARN("Wayland: declaring a %ux%u BUFFER for a %dx%d logical surface — the desktop is "
			          "fractionally scaled (%.3fx). The buffer is the output's mode size, so it still "
			          "lands 1:1 on the panel; the configure size would have been upscaled.",
			          dw, dh, m_wl_config_w, m_wl_config_h,
			          m_wl_config_w > 0 ? (double)dw / (double)m_wl_config_w : 0.0);
		}
	}

	DXRW_INFO("Created app-owned Wayland surface %p (xdg toplevel, %s), configure size %dx%d @ %u mHz",
	          (void *)m_wl_surface, desc.fullscreen_on_wayland ? "fullscreen" : "windowed", m_wl_config_w,
	          m_wl_config_h, m_wl_refresh_mhz);

	// A fullscreen surface only weaves 1:1 when the buffer we declare covers
	// the panel exactly. It normally does (the declared size is the matched
	// output's mode), so this fires when no output matched — then the buffer
	// is the LOGICAL configure size and the compositor will resample it.
	// One WARN, at create time — never per frame.
	if (desc.fullscreen_on_wayland && desc.panel_width > 0 && desc.panel_height > 0) {
		uint32_t dw = 0, dh = 0;
		wl_declared_size(&dw, &dh);
		if (dw != desc.panel_width || dh != desc.panel_height) {
			DXRW_WARN("Wayland: fullscreen buffer will be %ux%u but the 3D panel is %ux%u — no wl_output "
			          "matched, so the declared size is the LOGICAL configure size and the compositor "
			          "will resample it. The weave CANNOT be 1:1 in this session; expect the runtime "
			          "to present flat 2D rather than weave into the resample (look for its NOT_1TO1 "
			          "line, #1595). This warning is advisory — the app cannot refuse on the "
			          "runtime's behalf, and no longer has to.",
			          dw, dh, desc.panel_width, desc.panel_height);
		}
	}

	return true;
}

void
DxrLinuxWindow::destroy_wayland()
{
	if (m_wl_toplevel != nullptr) {
		xdg_toplevel_destroy(m_wl_toplevel);
		m_wl_toplevel = nullptr;
	}
	if (m_wl_xdg_surface != nullptr) {
		xdg_surface_destroy(m_wl_xdg_surface);
		m_wl_xdg_surface = nullptr;
	}
	if (m_wl_surface != nullptr) {
		wl_surface_destroy(m_wl_surface);
		m_wl_surface = nullptr;
	}
	if (m_wl_keyboard != nullptr) {
		wl_keyboard_release(m_wl_keyboard);
		m_wl_keyboard = nullptr;
	}
	if (m_wl_seat != nullptr) {
		wl_seat_destroy(m_wl_seat);
		m_wl_seat = nullptr;
	}
	for (auto &out : m_wl_outputs) {
		if (out.xdg_output != nullptr) {
			zxdg_output_v1_destroy(out.xdg_output);
			out.xdg_output = nullptr;
		}
		if (out.output != nullptr) {
			wl_output_destroy(out.output);
		}
	}
	m_wl_outputs.clear();
	if (m_wl_xdg_output_manager != nullptr) {
		zxdg_output_manager_v1_destroy(m_wl_xdg_output_manager);
		m_wl_xdg_output_manager = nullptr;
	}
	if (m_wl_wm_base != nullptr) {
		xdg_wm_base_destroy(m_wl_wm_base);
		m_wl_wm_base = nullptr;
	}
	if (m_wl_compositor != nullptr) {
		wl_compositor_destroy(m_wl_compositor);
		m_wl_compositor = nullptr;
	}
	if (m_wl_registry != nullptr) {
		wl_registry_destroy(m_wl_registry);
		m_wl_registry = nullptr;
	}
	if (m_wl_display != nullptr) {
		wl_display_disconnect(m_wl_display);
		m_wl_display = nullptr;
	}
}

#endif // DXR_APP_HAVE_WAYLAND


/*
 *
 * Public surface.
 *
 */

DxrLinuxWindow::~DxrLinuxWindow()
{
	destroy();
}

bool
DxrLinuxWindow::create(DxrWindowBackend backend, const DxrLinuxWindowDesc &desc)
{
	m_desc = desc;
	m_backend = backend;

	if (backend == DxrWindowBackend::X11) {
		if (!create_x11(desc)) {
			destroy_x11();
			m_backend = DxrWindowBackend::Auto;
			return false;
		}
		return true;
	}

#ifdef DXR_APP_HAVE_WAYLAND
	if (backend == DxrWindowBackend::Wayland) {
		if (!create_wayland(desc)) {
			destroy_wayland();
			m_backend = DxrWindowBackend::Auto;
			return false;
		}
		return true;
	}
#endif

	DXRW_ERROR("DxrLinuxWindow::create called with an unresolved backend");
	m_backend = DxrWindowBackend::Auto;
	return false;
}

void
DxrLinuxWindow::pump(const std::function<void(DxrKey)> &on_key, bool *running)
{
	if (m_backend == DxrWindowBackend::X11) {
		// Pump pending X events. The runtime borrows this Display's connection
		// for its XCB surface but never reads events from it, so the app owns
		// the queue.
		if (m_x_display == nullptr) {
			return;
		}
		m_x_pump_count++;

		// Motion coalescing (#1588): the X server can queue dozens of
		// MotionNotify per frame and acting on each would issue dozens of
		// XMoveWindow for one visible step. Only the LAST one matters — the
		// target is derived from the absolute pointer position, not from a
		// delta chain — so the whole queue is drained first and one move is
		// issued below.
		bool have_motion = false;
		int motion_root_x = 0;
		int motion_root_y = 0;

		while (XPending(m_x_display) > 0) {
			XEvent ev;
			XNextEvent(m_x_display, &ev);
			if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == m_x_wm_delete) {
				DXRW_INFO("Window closed by user — exiting");
				if (running != nullptr) {
					*running = false;
				}
			} else if (ev.type == KeyPress && on_key) {
				DxrKey k = dxr_key_from_keysym(XLookupKeysym(&ev.xkey, 0));
				if (k != DxrKey::Unknown) {
					on_key(k);
				}
			} else if (ev.type == ButtonPress && ev.xbutton.button == Button1 && m_x_client_drag &&
			           !m_x_test_drag_armed) {
				// There is no title bar to aim at (the window is
				// undecorated precisely so the WM does not own the drag),
				// so button 1 ANYWHERE in the window starts a move. That
				// is a test-app affordance, not a product one.
				m_x_dragging = true;
				m_x_drag_ptr_x = ev.xbutton.x_root;
				m_x_drag_ptr_y = ev.xbutton.y_root;
				x11_root_origin(m_x_display, m_x_window, &m_x_drag_origin_x, &m_x_drag_origin_y);
				m_x_drag_at_x = m_x_drag_origin_x;
				m_x_drag_at_y = m_x_drag_origin_y;
				m_x_drag_moves = 0;
				m_x_drag_snapped = 0;
				// Grab so motion OUTSIDE the window keeps arriving: the
				// pointer routinely leaves a window being dragged fast.
				XGrabPointer(m_x_display, m_x_window, False,
				             ButtonReleaseMask | PointerMotionMask | Button1MotionMask,
				             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
				DXRW_INFO("drag: start — grab origin (%d, %d), pointer (%d, %d)", m_x_drag_origin_x,
				          m_x_drag_origin_y, m_x_drag_ptr_x, m_x_drag_ptr_y);
			} else if (ev.type == MotionNotify && m_x_dragging) {
				have_motion = true;
				motion_root_x = ev.xmotion.x_root;
				motion_root_y = ev.xmotion.y_root;
			} else if (ev.type == ButtonRelease && ev.xbutton.button == Button1 && m_x_dragging) {
				m_x_dragging = false;
				XUngrabPointer(m_x_display, CurrentTime);
				XFlush(m_x_display);
                                DXRW_INFO("drag: end — %llu move(s), %llu "
                                          "snapped away from the raw target, "
                                          "origin (%d, %d) -> (%d, %d); "
                                          "placement so far: %u of %u verified "
                                          "moves landed exactly",
                                          (unsigned long long)m_x_drag_moves,
                                          (unsigned long long)m_x_drag_snapped,
                                          m_x_drag_origin_x, m_x_drag_origin_y,
                                          m_x_drag_at_x, m_x_drag_at_y,
                                          m_x_probe.moves - m_x_probe.diverged,
                                          m_x_probe.moves);
                        }
		}

		if (have_motion && m_x_dragging) {
			// Absolute, not incremental: origin + (pointer now - pointer at
			// grab). A snap that holds the window back for a few pixels
			// therefore never makes the window lag the pointer permanently.
			x11_move_snapped(m_x_drag_origin_x + (motion_root_x - m_x_drag_ptr_x),
			                 m_x_drag_origin_y + (motion_root_y - m_x_drag_ptr_y));
		}

		x11_drive_test_drag();
		return;
	}

#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland) {
		if (m_wl_display == nullptr) {
			return;
		}
		// Strictly non-blocking. wl_display_dispatch() would sleep until the
		// compositor says something, stalling the render loop; and
		// wl_display_read_events() on its own blocks too. The prepare_read /
		// poll(timeout 0) / read_events dance is the documented way to drain
		// the socket without ever waiting.
		while (wl_display_prepare_read(m_wl_display) != 0) {
			if (wl_display_dispatch_pending(m_wl_display) < 0) {
				goto wl_connection_lost;
			}
		}
		wl_display_flush(m_wl_display);
		{
			struct pollfd pfd = {};
			pfd.fd = wl_display_get_fd(m_wl_display);
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0) {
				if (wl_display_read_events(m_wl_display) < 0) {
					goto wl_connection_lost;
				}
			} else {
				wl_display_cancel_read(m_wl_display);
			}
		}
		if (wl_display_dispatch_pending(m_wl_display) < 0) {
			goto wl_connection_lost;
		}
		wl_display_flush(m_wl_display);

		if (m_wl_close_requested) {
			m_wl_close_requested = false;
			DXRW_INFO("Window closed by user — exiting");
			if (running != nullptr) {
				*running = false;
			}
		}
		// A configure may have changed the size in the dispatch above. The
		// runtime has no other way to learn it — Wayland gives the WSI no
		// currentExtent — so republish before the next frame is drawn.
		publish_wayland_geometry_if_changed();

		if (on_key) {
			for (DxrKey k : m_wl_key_queue) {
				on_key(k);
			}
		}
		m_wl_key_queue.clear();
		return;

	wl_connection_lost:
		DXRW_ERROR("Wayland connection lost — exiting");
		if (running != nullptr) {
			*running = false;
		}
		return;
	}
#endif
	(void)on_key;
	(void)running;
}

bool
DxrLinuxWindow::current_size(uint32_t *w, uint32_t *h) const
{
	if (w == nullptr || h == nullptr) {
		return false;
	}

	if (m_backend == DxrWindowBackend::X11) {
		XWindowAttributes wa = {};
		if (m_x_display != nullptr && m_x_window != 0 && XGetWindowAttributes(m_x_display, m_x_window, &wa) &&
		    wa.width > 0 && wa.height > 0) {
			*w = (uint32_t)wa.width;
			*h = (uint32_t)wa.height;
			return true;
		}
		return false;
	}

#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland) {
		// The last xdg_toplevel.configure. This helper never calls
		// wl_surface_set_buffer_scale, so the surface's buffer size is its
		// logical size and the configure value is directly comparable to the
		// X11 XGetWindowAttributes reading.
		if (m_wl_config_w > 0 && m_wl_config_h > 0) {
			*w = (uint32_t)m_wl_config_w;
			*h = (uint32_t)m_wl_config_h;
			return true;
		}
		if (m_desc.width > 0 && m_desc.height > 0) {
			*w = m_desc.width;
			*h = m_desc.height;
			return true;
		}
	}
#endif
	return false;
}

const void *
DxrLinuxWindow::session_binding_chain(const void *next)
{
	if (m_backend == DxrWindowBackend::X11 && m_x_window != 0) {
		m_xlib_binding = {};
		m_xlib_binding.type = XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR;
		m_xlib_binding.next = next;
		m_xlib_binding.xDisplay = m_x_display;
		m_xlib_binding.window = m_x_window;
		return &m_xlib_binding;
	}

#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland && m_wl_surface != nullptr) {
		// Spec v2: declare the SIZE. A wl_surface has none of its own — the
		// WSI reports currentExtent == UINT32_MAX and the buffer the runtime
		// attaches is what defines the surface — so omitting this does not
		// leave the window alone, it lets the runtime resize it to the panel.
		// The size is the configure this helper already acked in create().
		uint32_t decl_w = 0, decl_h = 0;
		wl_declared_size(&decl_w, &decl_h);
		m_wl_geometry = {};
		m_wl_geometry.type = XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR;
		m_wl_geometry.next = next;
		m_wl_geometry.width = decl_w;
		m_wl_geometry.height = decl_h;
		m_wl_geometry.refreshMilliHertz = m_wl_refresh_mhz;
		m_wl_published_w = decl_w;
		m_wl_published_h = decl_h;

		m_wl_binding = {};
		m_wl_binding.type = XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR;
		m_wl_binding.next = &m_wl_geometry;
		m_wl_binding.wlDisplay = m_wl_display;
		m_wl_binding.wlSurface = m_wl_surface;
		return &m_wl_binding;
	}
#endif
	return next;
}

void
DxrLinuxWindow::wl_declared_size(uint32_t *w, uint32_t *h) const
{
	*w = 0;
	*h = 0;
#ifdef DXR_APP_HAVE_WAYLAND
	// Fullscreen on a matched output: declare the output's MODE, in device
	// pixels. That is the buffer that maps 1:1 onto the panel; the configure
	// size is logical and, on a fractionally-scaled desktop, smaller.
	if (m_wl_fullscreen_mode_w > 0 && m_wl_fullscreen_mode_h > 0) {
		*w = (uint32_t)m_wl_fullscreen_mode_w;
		*h = (uint32_t)m_wl_fullscreen_mode_h;
		return;
	}
	// Windowed (or fullscreen with no output match): the configure size is the
	// only thing known. It equals the buffer size at desktop scale 1.0, which
	// is the only scale at which a Wayland weave can be 1:1 anyway — #817 has
	// the runtime refuse window geometry at any other scale.
	if (m_wl_config_w > 0 && m_wl_config_h > 0) {
		*w = (uint32_t)m_wl_config_w;
		*h = (uint32_t)m_wl_config_h;
	}
#endif
}

void
DxrLinuxWindow::attach_session(XrInstance instance, XrSession session)
{
	m_session = session;

#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend != DxrWindowBackend::Wayland || instance == XR_NULL_HANDLE) {
		return;
	}
	// Resolved, not linked: an older runtime (extension spec v1) has no such
	// function and xrGetInstanceProcAddr answers XR_ERROR_FUNCTION_UNSUPPORTED.
	// That is a degraded session, not a broken one — the create-time size still
	// applies, only later resizes stop being followed. Log once.
	PFN_xrVoidFunction fn = nullptr;
	if (xrGetInstanceProcAddr(instance, "xrSetWaylandSurfaceGeometryDXR", &fn) == XR_SUCCESS && fn != nullptr) {
		m_pfn_set_wl_geometry = reinterpret_cast<PFN_xrSetWaylandSurfaceGeometryDXR>(fn);
		DXRW_INFO("Wayland: xrSetWaylandSurfaceGeometryDXR resolved — surface resizes will be followed");
	} else {
		DXRW_WARN("Wayland: this runtime has no xrSetWaylandSurfaceGeometryDXR "
		          "(XR_DXR_wayland_surface_binding spec < 2). The size declared at session create still "
		          "applies; later xdg_toplevel.configure resizes will NOT be followed.");
	}
#else
	(void)instance;
#endif
}

void
DxrLinuxWindow::publish_wayland_geometry_if_changed()
{
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_pfn_set_wl_geometry == nullptr || m_session == XR_NULL_HANDLE) {
		return;
	}
	uint32_t w = 0, h = 0;
	wl_declared_size(&w, &h);
	if (w == 0 || h == 0) {
		return;
	}
	if (w == m_wl_published_w && h == m_wl_published_h) {
		return;
	}
	const XrResult res = m_pfn_set_wl_geometry(m_session, w, h, m_wl_refresh_mhz);
	if (res != XR_SUCCESS) {
		DXRW_WARN("xrSetWaylandSurfaceGeometryDXR(%ux%u) failed: %d", w, h, (int)res);
		return;
	}
	m_wl_published_w = w;
	m_wl_published_h = h;
	DXRW_INFO("Wayland: declared new surface geometry %ux%u @ %u mHz", w, h, m_wl_refresh_mhz);
#endif
}

bool
DxrLinuxWindow::force_declare_geometry(uint32_t width, uint32_t height)
{
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend != DxrWindowBackend::Wayland || width == 0 || height == 0) {
		return false;
	}
	// Pretend a configure arrived. On Wayland this is not a lie: the buffer
	// the runtime attaches defines the surface, so declaring a new size IS how
	// a client resizes itself. Drop any fullscreen mode override — the test
	// hook is asking for this exact buffer size, not the output's.
	m_wl_fullscreen_mode_w = 0;
	m_wl_fullscreen_mode_h = 0;
	m_wl_config_w = (int32_t)width;
	m_wl_config_h = (int32_t)height;
	publish_wayland_geometry_if_changed();
	return m_wl_published_w == width && m_wl_published_h == height;
#else
	(void)width;
	(void)height;
	return false;
#endif
}

void
DxrLinuxWindow::set_snap_provider(SnapWindowOriginFn fn, void *userdata)
{
	m_snap_fn = fn;
	m_snap_userdata = userdata;
}

const char *
DxrLinuxWindow::required_openxr_extension() const
{
	switch (m_backend) {
	case DxrWindowBackend::X11: return XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME;
	case DxrWindowBackend::Wayland: return XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME;
	default: return nullptr;
	}
}

std::string
DxrLinuxWindow::describe() const
{
	char buf[192];
	if (m_backend == DxrWindowBackend::X11) {
		snprintf(buf, sizeof(buf), "X11 Display %p, Window 0x%lx", (void *)m_x_display, m_x_window);
		return buf;
	}
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland) {
		snprintf(buf, sizeof(buf), "wl_display %p, wl_surface %p", (void *)m_wl_display, (void *)m_wl_surface);
		return buf;
	}
#endif
	return "no window";
}

void
DxrLinuxWindow::destroy()
{
	destroy_x11();
#ifdef DXR_APP_HAVE_WAYLAND
	destroy_wayland();
#endif
	m_backend = DxrWindowBackend::Auto;
}
