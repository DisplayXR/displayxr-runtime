// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Backend-neutral app-owned window for the desktop-Linux test apps.
 *
 * One object that owns either an X11 toplevel (XR_DXR_xlib_window_binding) or
 * a native Wayland xdg-shell toplevel (XR_DXR_wayland_surface_binding), picked
 * at RUNTIME so a single binary works in either session. The window-binding
 * struct it hands back is chained onto XrSessionCreateInfo by the app.
 *
 * Why one helper: the X11 leg is lifted out of cube_handle_vk_linux
 * (placement, ICCCM size hints) and cube_zones_vk_linux carried a
 * byte-identical copy. Keeping one copy is what makes the Wayland leg a single
 * addition instead of two.
 *
 * X11 PLACEMENT CONTRACT (INV-1.3, #729):
 *   A panel-sized window (desc.width/height == desc.panel_width/height) is made
 *   genuinely FULLSCREEN on the panel's monitor, because mutter discards
 *   client-requested geometry for such a window AND decorates it — the observed
 *   result of asking for 3840x2160+3456+0 was a 3840x2086 client at (3456, 74)
 *   under a mutter-x11-frames parent. Fullscreen is the one WM-cooperative
 *   placement primitive, and it is what makes window origin ≡ panel origin
 *   (which the weave phase depends on). The recipe, validated on GNOME 50 /
 *   XWayland: map -> XMoveWindow onto the target output -> pump events briefly
 *   -> EWMH _NET_WM_STATE_FULLSCREEN + _NET_WM_FULLSCREEN_MONITORS. Order
 *   matters: a position request before the window is mapped is discarded, and
 *   mutter fullscreens onto whichever output the window currently occupies.
 *   A windowed size keeps the old behaviour; DXR_X11_NO_FULLSCREEN=1 opts out.
 *
 * X11 CLIENT-OWNED DRAG (#1588):
 *   A WINDOWED X11 toplevel is undecorated too, and this helper — not the
 *   window manager — moves it. The reason is the weave: a WM-owned drag
 *   (mutter's _NET_WM_MOVERESIZE grab) cannot be intercepted by the client, so
 *   the window lands on an arbitrary pixel every frame and the lenticular
 *   interlace phase re-lands with it, which reads as a shimmer/stutter. Windows
 *   avoids this by snapping the window's position to the lens lattice DURING
 *   the drag (WM_WINDOWPOSCHANGING -> the DP's snap_window_rect); the only way
 *   to get the same hook under mutter is to own the drag. So: no decorations,
 *   a button-1 pointer grab anywhere in the window, and every move routed
 *   through the app-installed snap provider (see set_snap_provider(), which
 *   the cube apps back with xrWeaveSnapWindowRectDXR) before XMoveWindow.
 *   Post-map client moves ARE honoured by mutter (verified, #729).
 *   DXR_X11_WM_DECORATIONS=1 restores the decorated, WM-dragged window.
 *
 * ORDERING CONTRACT (both backends):
 *   create instance -> get system -> xrGetSystemProperties (panel rect, INV-1.3)
 *   -> DxrLinuxWindow::create() -> xrCreateSession with session_binding_chain()
 *   -> ... -> destroy() LAST, after the Vulkan instance is gone. The runtime's
 *   VkSurfaceKHR borrows the Xlib Display / wl_display connection for its
 *   lifetime, so the connection must outlive the session.
 *
 * WAYLAND WINDOW CHROME (#1654):
 *   GNOME's mutter gives Wayland clients no server-side decorations, so a
 *   WINDOWED native-Wayland toplevel gets a client-side title bar: the shared
 *   displayxr::csd painter (displayxr-common#52) in a wl_subsurface ABOVE the
 *   bound surface — see dxr_wl_chrome.h. The bound surface stays exactly the
 *   content rect (the bar never reaches the swapchain, the atlas or the
 *   weave); window geometry is bar + content, so xdg_toplevel.configure sizes
 *   are frame sizes and the helper subtracts the bar before declaring the
 *   content size. Fullscreen hides the bar. Server-side decorations are
 *   preferred where zxdg_decoration_manager_v1 offers them; DXR_WL_CSD=0
 *   turns the chrome off, DXR_WL_CSD=force draws it regardless.
 *
 * WAYLAND CONTRACT (see docs/specs/extensions/XR_DXR_wayland_surface_binding.md):
 *   - The surface must already have an xdg role and its first configure must be
 *     ACKED before xrCreateSession: the runtime calls vkCreateWaylandSurfaceKHR
 *     synchronously inside the session create
 *     (src/xrt/compositor/vk_native/comp_vk_native_target.cpp:1797) and Mesa's
 *     WSI attaches a buffer on the first present.
 *   - The app must NEVER attach a wl_buffer, commit, or install a
 *     wl_surface_frame callback once the session exists — the WSI owns those.
 *     This helper commits exactly once, during create(), before the session.
 *   - The runtime never pumps the Wayland queue (there is no wl_display_* call
 *     anywhere in src/), so pump() must be called every frame.
 *   - A wl_surface has NO intrinsic size: the WSI reports
 *     `currentExtent == UINT32_MAX` and the buffer the runtime attaches is what
 *     DEFINES the surface. So the app must DECLARE its size — this helper
 *     chains XrWaylandSurfaceGeometryDXR at session create and republishes
 *     through xrSetWaylandSurfaceGeometryDXR on every later configure that
 *     changes it (extension spec v2). Call attach_session() right after
 *     xrCreateSession to arm that; against an older runtime the function is
 *     simply absent and the helper logs once and stays quiet.
 */
#pragma once

#include "u_x11_scale.h" // placement-landing probe (shared with the runtime)

// Xlib first: XR_DXR_xlib_window_binding.h wants the real Display / Window
// types, not its self-contained stand-ins.
#include <X11/Xlib.h>
#include <X11/Xutil.h> // XSizeHints for INV-1.3 window placement

#ifdef DXR_APP_HAVE_WAYLAND
// Likewise: the Wayland binding header forward-declares wl_display/wl_surface,
// so the real definitions have to come first.
#include <wayland-client.h>
#ifdef DXR_APP_HAVE_WL_CHROME
#include "dxr_wl_chrome.h" // title bar for the native-Wayland leg (#1654)
#endif
#endif

#include <openxr/openxr.h>
#include <openxr/XR_DXR_xlib_window_binding.h>
#include <openxr/XR_DXR_wayland_surface_binding.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

//! Which app-owned window backend (and therefore which binding extension).
enum class DxrWindowBackend
{
	Auto,   //!< pick from the session environment + what the runtime advertises
	X11,    //!< XR_DXR_xlib_window_binding
	Wayland //!< XR_DXR_wayland_surface_binding
};

/*!
 * Backend-neutral key identity. Only the handful of keys the Linux test apps
 * bind — the Wayland leg decodes raw evdev keycodes (no xkbcommon), so this is
 * deliberately not a general keysym.
 *
 * NOTE: the idle enumerator is `Unknown`, not `None`, because <X11/X.h> does
 * `#define None 0L` and would mangle the enumerator.
 */
enum class DxrKey
{
	Unknown,
	Escape,
	Q,
	M,
	O,
	V,
	Num1,
	Num2,
	Num3,
	F11 //!< handled INSIDE pump() (toggle_fullscreen); never reaches on_key
};

//! What to create. Sizes are panel pixels; the panel rect comes from
//! XrDisplayDesktopPositionDXR + XrDisplayInfoDXR (INV-1.3).
struct DxrLinuxWindowDesc
{
	uint32_t width = 0;  //!< requested window size, pixels
	uint32_t height = 0; //!< requested window size, pixels

	int32_t panel_left = 0;    //!< 3D panel top-left in virtual-desktop pixels
	int32_t panel_top = 0;     //!< ...
	uint32_t panel_width = 0;  //!< 3D panel size in pixels (0 = unknown)
	uint32_t panel_height = 0; //!< ... a window of exactly this size goes
	                           //!< fullscreen on the panel's monitor (X11, #729)

	const char *title = "DisplayXR";               //!< toplevel title
	const char *app_id = "com.displayxr.test_app"; //!< Wayland xdg app-id

	//! Wayland only. Fullscreen-on-the-panel is the INV-1.3 substitute (a
	//! Wayland client cannot place itself), so it stays the default. Clearing
	//! it gives a windowed toplevel, which IS supported from extension spec
	//! v2: this helper declares `width`/`height` through
	//! XrWaylandSurfaceGeometryDXR so the runtime sizes its WSI swapchain to
	//! the surface instead of the panel. Note the weave PHASE still needs the
	//! compositor geometry service for a windowed surface — the size comes
	//! from here, the position does not.
	bool fullscreen_on_wayland = true;
};

/*!
 * App-owned toplevel window, X11 or Wayland.
 *
 * Not copyable; one instance per app. All state is owned inline (the binding
 * struct handed to xrCreateSession is member storage, so it stays alive for
 * the duration of the call).
 */
class DxrLinuxWindow
{
public:
	DxrLinuxWindow() = default;
	~DxrLinuxWindow();
	DxrLinuxWindow(const DxrLinuxWindow &) = delete;
	DxrLinuxWindow &operator=(const DxrLinuxWindow &) = delete;

	/*!
	 * Resolve `requested` against the session environment and what the runtime
	 * advertises. Never touches any connection — safe to call before create().
	 *
	 * @param runtime_has_xlib   XR_DXR_xlib_window_binding was enumerated
	 * @param runtime_has_wayland XR_DXR_wayland_surface_binding was enumerated
	 * @param reason             filled with a one-line human explanation (may be null)
	 * @return the chosen backend, or DxrWindowBackend::Auto when nothing is usable
	 *         (the caller should treat that as a hard error and print `reason`).
	 */
	static DxrWindowBackend
	select(DxrWindowBackend requested, bool runtime_has_xlib, bool runtime_has_wayland, std::string *reason);

	//! Parse "x11" / "wayland" / "auto"; returns false on anything else.
	static bool
	parse_backend(const char *text, DxrWindowBackend *out);

	static const char *
	backend_name(DxrWindowBackend b);

	//! Bring the window up. On Wayland this includes the full xdg-shell
	//! handshake: role, fullscreen request, commit, and the first
	//! xdg_surface.configure ACKED — the session may be created straight after.
	bool
	create(DxrWindowBackend backend, const DxrLinuxWindowDesc &desc);

	/*!
	 * Drain the window system's event queue. Call once per frame, never
	 * blocking.
	 *
	 * @param on_key  invoked for each key press (may be empty)
	 * @param running cleared when the user asks to close the window
	 */
	void
	pump(const std::function<void(DxrKey)> &on_key, bool *running);

	/*!
	 * F11: toggle fullscreen. Onto the 3D panel's output / RandR monitor when
	 * one was matched at create, else wherever the compositor puts it. pump()
	 * calls this itself on F11, on both backends, so apps need not.
	 *
	 * Wayland: xdg_toplevel.set_fullscreen(panel wl_output) / unset. The
	 * declared buffer follows (the panel's MODE while fullscreen on it, the
	 * windowed configure x scale after), and the title bar hides / returns.
	 * X11: _NET_WM_STATE_FULLSCREEN + _NET_WM_FULLSCREEN_MONITORS, and the
	 * client-owned drag is disabled while fullscreen.
	 *
	 * @return false when there is no window to toggle.
	 */
	bool
	toggle_fullscreen();

	bool
	is_fullscreen() const;

	//! Live window size in BUFFER pixels — the space the runtime's swapchain
	//! and every rect handed to it live in. X11 reads XGetWindowAttributes;
	//! Wayland returns the size declared through XrWaylandSurfaceGeometryDXR
	//! (the matched output's mode when fullscreen, else the last
	//! xdg_toplevel.configure), never the logical configure size of a
	//! fractionally-scaled fullscreen surface. False when no size is known
	//! yet — the caller should fall back to its own envelope.
	bool
	current_size(uint32_t *w, uint32_t *h) const;

	/*!
	 * Pointer to the filled window-binding struct, with `.next = next`, ready
	 * to hand to XrSessionCreateInfo::next. Member storage: valid until this
	 * object is destroyed. Returns `next` unchanged if no window exists.
	 *
	 * On Wayland the returned chain is TWO structs: the binding, followed by
	 * XrWaylandSurfaceGeometryDXR carrying the acked configure size and the
	 * matched output's refresh (spec v2). Without the second one the runtime
	 * would size its swapchain to the panel and thereby resize the surface.
	 */
	const void *
	session_binding_chain(const void *next);

	/*!
	 * Arm the mid-session geometry channel. Call once, right after
	 * xrCreateSession; no-op on X11.
	 *
	 * Resolves xrSetWaylandSurfaceGeometryDXR through xrGetInstanceProcAddr.
	 * An older runtime returns NULL for it — that is not an error, it just
	 * means the session is stuck with the size declared at create; the helper
	 * logs once and never asks again.
	 */
	void
	attach_session(XrInstance instance, XrSession session);

	/*!
	 * Republish the surface size to the runtime NOW, whatever the compositor
	 * last configured.
	 *
	 * pump() already does this for every real xdg_toplevel.configure, so apps
	 * do not need it. It exists for the test hook that exercises the runtime's
	 * resize-follow without a user dragging a window edge (see
	 * DXR_CUBE_TEST_RESIZE in the Linux cube apps).
	 *
	 * @return false when there is no Wayland session to publish to.
	 */
	bool
	force_declare_geometry(uint32_t width, uint32_t height);

	/*!
	 * Drag-time window-origin snap provider (#1588).
	 *
	 * A DELIBERATELY PLAIN function pointer: the snap math belongs to the
	 * vendor display processor and reaches this helper through the app's
	 * OpenXR session, but the helper itself must stay a window-system object
	 * — it does not know what a session is and never calls OpenXR for this.
	 *
	 * @param userdata    whatever was handed to set_snap_provider()
	 * @param origin_x/y  the window's root origin when the drag STARTED
	 *                    (the phase reference — the snap is absolute, and the
	 *                    DP wants to know where the travel began)
	 * @param target_x/y  the proposed new root origin, pointer-derived
	 * @param out_x/y     receives the lattice-snapped root origin
	 * @return false when nothing was snapped; the helper then moves to the
	 *         raw target and the out params are ignored.
	 */
	typedef bool (*SnapWindowOriginFn)(void *userdata,
	                                   int32_t origin_x,
	                                   int32_t origin_y,
	                                   int32_t target_x,
	                                   int32_t target_y,
	                                   int32_t *out_x,
	                                   int32_t *out_y);

	/*!
	 * Install (or clear, with @p fn nullptr) the snap provider. Call any time;
	 * with none installed every drag move is an identity snap, which is the
	 * correct behaviour against a runtime whose DP cannot snap — the drag
	 * mechanics are the same either way.
	 */
	void
	set_snap_provider(SnapWindowOriginFn fn, void *userdata);

	//! Name of the binding extension this backend needs enabled at
	//! xrCreateInstance, or nullptr when no window exists.
	const char *
	required_openxr_extension() const;

	DxrWindowBackend
	backend() const
	{
		return m_backend;
	}

	//! Human-readable identity of the bound window, for the session-create log.
	std::string
	describe() const;

	void
	destroy();

private:
	DxrWindowBackend m_backend = DxrWindowBackend::Auto;
	DxrLinuxWindowDesc m_desc = {};

	// --- X11 leg -----------------------------------------------------------
	Display *m_x_display = nullptr;
	::Window m_x_window = 0;
	Atom m_x_wm_delete = 0;

	// --- Snap provider (#1588) ---------------------------------------------
	SnapWindowOriginFn m_snap_fn = nullptr;
	void *m_snap_userdata = nullptr;
	//! One-shot: the first move of the first drag says whether anything snaps.
	bool m_snap_reported = false;

	// --- X11 client-owned drag (#1588) --------------------------------------
	//! Windowed run with the WM's decorations + WM drag left in place
	//! (DXR_X11_WM_DECORATIONS=1). No client drag then.
	bool m_x_wm_drag = false;
	/*!
	 * This window owns its drag: windowed AND undecorated. FALSE for a
	 * fullscreen window as well as for the DXR_X11_WM_DECORATIONS opt-out —
	 * a fullscreen window must not be draggable at all, or a stray click
	 * would slide the panel-sized weave off the panel.
	 */
	bool m_x_client_drag = false;
	//! Fullscreen now (created panel-sized, or toggled by F11).
	bool m_x_fullscreen = false;
	bool m_x_dragging = false;
	int m_x_drag_ptr_x = 0;    //!< pointer root position at the grab
	int m_x_drag_ptr_y = 0;    //!< ...
	int m_x_drag_origin_x = 0; //!< window root origin at the grab (snap origin)
	int m_x_drag_origin_y = 0; //!< ...
	int m_x_drag_at_x = 0;     //!< where the window was last moved to
	int m_x_drag_at_y = 0;     //!< ...
	uint64_t m_x_drag_moves = 0;    //!< XMoveWindow calls this drag
	uint64_t m_x_drag_snapped = 0;  //!< ...of which the snap changed the point

        /*!
         * Landing check: did the window go where the snap asked? A move is
         * asynchronous, so the previous request is compared against the
         * window's real origin just before the next move is issued (a pump
         * later, by which time the server has placed it). Accumulates across
         * drags; reports once. This is what would have caught the XWayland 2 px
         * quantum in minutes: a snap the environment silently rounds away is
         * indistinguishable from a working one unless someone reads the window
         * back.
         */
        u_x11_placement_probe m_x_probe = {};
        bool m_x_probe_pending = false;
        int m_x_probe_want_x = 0;
        int m_x_probe_want_y = 0;
        void x11_check_landing();

        // --- X11 programmatic drag test hook (DXR_X11_TEST_DRAG, #1588) ---------
	// Off by default. Walks the window along a straight path through the very
	// same snap -> XMoveWindow code the pointer drag uses, so the mechanics
	// are verifiable with nobody at the mouse. Not a fake X event: it drives
	// the same code path one step per pump().
	bool m_x_test_drag_armed = false;
	bool m_x_test_drag_done = false;
	int m_x_test_drag_dx = 0;
	int m_x_test_drag_dy = 0;
	int m_x_test_drag_steps = 0;
	int m_x_test_drag_step = 0;
	uint64_t m_x_pump_count = 0;
	uint64_t m_test_fs_pumps = 0; //!< DXR_TEST_FULLSCREEN_TOGGLE counter

	//! Run the snap provider, or identity when there is none / it declines.
	//! Reports once, the first time it is asked, what it resolved to.
	void
	snap_origin(int origin_x, int origin_y, int target_x, int target_y, int *out_x, int *out_y);

	//! snap_origin() + XMoveWindow, skipping a move that would not change the
	//! window's position. Logs only when the snap actually moved the point.
	void
	x11_move_snapped(int target_x, int target_y);

	//! One step of DXR_X11_TEST_DRAG, called from pump().
	void
	x11_drive_test_drag();

	// --- Binding storage (handed to xrCreateSession) ------------------------
	XrXlibWindowBindingCreateInfoDXR m_xlib_binding = {};
	XrWaylandSurfaceBindingCreateInfoDXR m_wl_binding = {};
	XrWaylandSurfaceGeometryDXR m_wl_geometry = {};

	// --- Mid-session geometry channel (spec v2) -----------------------------
	XrSession m_session = XR_NULL_HANDLE;
	PFN_xrSetWaylandSurfaceGeometryDXR m_pfn_set_wl_geometry = nullptr;
	//! Last size actually published, so pump() only calls on a real change.
	uint32_t m_wl_published_w = 0;
	uint32_t m_wl_published_h = 0;

	//! Push the current configure size to the runtime when it differs from
	//! what was last published. Cheap; safe to call every frame.
	void
	publish_wayland_geometry_if_changed();

#ifdef DXR_APP_HAVE_WAYLAND
	// --- Wayland leg -------------------------------------------------------
	/*!
	 * One output, with EVERY field's coordinate space in its name (#1596).
	 *
	 * The bug this naming exists to prevent: the panel match used to compare
	 * `wl_output.geometry`'s LOGICAL origin against the runtime's DEVICE-pixel
	 * panel rect, so on the measured box it compared 1728 against 3456 and
	 * could never succeed — the app then fullscreened on whatever output the
	 * compositor picked (the laptop) and wove into a resample.
	 */
	struct WlOutput
	{
		struct wl_output *output = nullptr;
		//! zxdg_output_v1, when the compositor advertises the manager. It is
		//! the ONLY source of an output's logical SIZE, and therefore the only
		//! way to derive a fractional scale (mode / logical_size).
		struct zxdg_output_v1 *xdg_output = nullptr;
		uint32_t name = 0;

		//! Position in the global LOGICAL layout. Seeded from
		//! wl_output.geometry, superseded by xdg_output.logical_position.
		int32_t logical_x = 0, logical_y = 0;
		//! Size in LOGICAL px, from xdg_output.logical_size only.
		int32_t logical_w = 0, logical_h = 0;
		bool have_logical_size = false;

		//! Current mode, DEVICE px (wl_output.mode). Directly comparable with
		//! the runtime's panel size, which is device px by contract.
		int32_t mode_w = 0, mode_h = 0;

		/*!
		 * wl_output.scale. An INTEGER by protocol, and therefore NOT the
		 * conversion factor: this box's 1.6667 laptop advertises 2. Kept only
		 * so a diagnostic can say what the compositor claimed; every
		 * conversion goes through u_wl_monitor_scale().
		 */
		int32_t int_scale = 1;

		int32_t refresh_mhz = 0; //!< wl_output.mode refresh, milli-hertz
	};

#ifdef DXR_APP_HAVE_WL_CHROME
	//! Title bar (client-side decorations) — see dxr_wl_chrome.h, #1654.
	DxrWlChrome m_wl_chrome;
#endif

	//! Logical -> device scale of the surface, for the chrome's raster.
	double
	wl_surface_scale() const
	{
		return m_wl_pref_scale_120 > 0 ? (double)m_wl_pref_scale_120 / 120.0 : 1.0;
	}

	struct wl_display *m_wl_display = nullptr;
	struct wl_registry *m_wl_registry = nullptr;
	struct wl_compositor *m_wl_compositor = nullptr;
	struct xdg_wm_base *m_wl_wm_base = nullptr;
	struct wl_seat *m_wl_seat = nullptr;
	//! zxdg_output_manager_v1 (#1596). Absent on a compositor that does not
	//! advertise it — the match then falls back to mode size alone, which is
	//! still device-vs-device and still correct, just without the origin as a
	//! tie-break.
	struct zxdg_output_manager_v1 *m_wl_xdg_output_manager = nullptr;
	struct wl_keyboard *m_wl_keyboard = nullptr;
	/*!
	 * Buffer -> surface mapping. The buffer the runtime attaches is in DEVICE
	 * pixels (logical size x output scale); without a mapping the compositor
	 * treats buffer pixels as LOGICAL pixels, so at 200 % a panel-mode buffer
	 * becomes a surface twice the output and spills onto the next monitor.
	 * wp_viewport's destination (= the configure size) is the mapping that
	 * works at any scale; set_buffer_scale is the integer-only fallback.
	 * wp_fractional_scale_v1 supplies a windowed surface's preferred scale.
	 * All optional; NULL when the compositor does not advertise them.
	 */
	struct wp_viewporter *m_wl_viewporter = nullptr;
	struct wp_viewport *m_wl_viewport = nullptr;
	struct wp_fractional_scale_manager_v1 *m_wl_frac_manager = nullptr;
	struct wp_fractional_scale_v1 *m_wl_frac = nullptr;
	//! wp_fractional_scale_v1.preferred_scale, in 120ths; 0 = not received.
	uint32_t m_wl_pref_scale_120 = 0;
	//! Mapping last applied to the surface (so it is re-sent only on change).
	int32_t m_wl_map_dst_w = 0, m_wl_map_dst_h = 0, m_wl_map_buffer_scale = 1;
	struct wl_surface *m_wl_surface = nullptr;
	struct xdg_surface *m_wl_xdg_surface = nullptr;
	struct xdg_toplevel *m_wl_toplevel = nullptr;
	std::vector<WlOutput> m_wl_outputs;

	bool m_wl_configured = false;
	//! CONTENT size, LOGICAL px: the last xdg_toplevel.configure minus the
	//! title bar (#1654) — the size of the bound surface, never the frame.
	int32_t m_wl_config_w = 0;
	int32_t m_wl_config_h = 0;
	//! Refresh of the output the surface went fullscreen on (0 = unknown).
	uint32_t m_wl_refresh_mhz = 0;
	/*!
	 * Mode size, in DEVICE pixels, of the output this surface went fullscreen
	 * on; 0 when windowed or when no output matched.
	 *
	 * This is what a fullscreen surface must declare, and it is NOT the
	 * configure size. xdg_toplevel.configure is in LOGICAL units, so on a
	 * fractionally-scaled desktop (this box runs 166.67%) a fullscreen
	 * toplevel is configured at 1728x1080 while the panel is 2880x1800. What
	 * reaches the panel 1:1 is a buffer of the output's MODE size: the
	 * compositor maps that whole buffer onto the output, so buffer pixels and
	 * panel pixels line up exactly. Declaring the logical size instead would
	 * hand the weaver a 1728x1080 image for Mutter to upscale — a resample,
	 * which destroys the interlace.
	 */
	int32_t m_wl_fullscreen_mode_w = 0;
	int32_t m_wl_fullscreen_mode_h = 0;

	//! The wl_output matched to the 3D panel at create (NULL when none), and
	//! its mode — the F11 target and its 1:1 buffer size.
	struct wl_output *m_wl_panel_output = nullptr;
	int32_t m_wl_panel_mode_w = 0, m_wl_panel_mode_h = 0;
	//! Fullscreen now, per the last sized configure (or the create request).
	bool m_wl_fullscreen = false;
	//! We sent unset_fullscreen and have not seen its configure yet.
	bool m_wl_unfullscreen_pending = false;
	//! The current / last fullscreen request targeted the panel output.
	bool m_wl_fs_on_panel = true;
	//! Last windowed CONTENT size, logical — where leaving fullscreen returns.
	int32_t m_wl_windowed_w = 0, m_wl_windowed_h = 0;

	//! Apply a fullscreen state change from a configure. @p cfg_w/h is the
	//! configure size (may be 0x0 = "you choose").
	void
	wl_set_fullscreen_state(bool fs, int32_t *cfg_w, int32_t *cfg_h);

	//! Size to declare to the runtime: the fullscreen output mode when there
	//! is one, else the acked configure size.
	void
	wl_declared_size(uint32_t *w, uint32_t *h) const;

	//! Map the declared (device-pixel) buffer onto the configure (logical)
	//! size: wp_viewport destination, else an integer set_buffer_scale.
	//! Pending surface state only — the WSI's next present commits it.
	void
	wl_apply_buffer_mapping();

	// Per-frame scratch, set by the listeners and consumed by pump().
	std::vector<DxrKey> m_wl_key_queue;
	bool m_wl_close_requested = false;

	bool
	create_wayland(const DxrLinuxWindowDesc &desc);
	void
	destroy_wayland();

	//! Give an output its zxdg_output_v1 once the manager exists. Idempotent,
	//! and called from both directions (output-first and manager-first) so
	//! registry ordering cannot leave an output without a logical size.
	void
	wl_attach_xdg_output(WlOutput &out);

	// Static trampolines (wayland-client listeners are C function pointers).
	static void
	s_registry_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version);
	static void
	s_registry_global_remove(void *data, struct wl_registry *r, uint32_t name);
	static void
	s_wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial);
	static void
	s_xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial);
	static void
	s_frac_preferred_scale(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale_120);
	static void
	s_toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states);
	static void
	s_toplevel_close(void *data, struct xdg_toplevel *t);
	static void
	s_toplevel_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h);
	static void
	s_toplevel_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *caps);
	static void
	s_output_geometry(void *data,
	                  struct wl_output *o,
	                  int32_t x,
	                  int32_t y,
	                  int32_t pw,
	                  int32_t ph,
	                  int32_t subpixel,
	                  const char *make,
	                  const char *model,
	                  int32_t transform);
	static void
	s_output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh);
	static void
	s_output_done(void *data, struct wl_output *o);
	static void
	s_output_scale(void *data, struct wl_output *o, int32_t factor);
	static void
	s_output_name(void *data, struct wl_output *o, const char *name);
	static void
	s_output_description(void *data, struct wl_output *o, const char *desc);
	static void
	s_xdg_output_logical_position(void *data, struct zxdg_output_v1 *o, int32_t x, int32_t y);
	static void
	s_xdg_output_logical_size(void *data, struct zxdg_output_v1 *o, int32_t w, int32_t h);
	static void
	s_xdg_output_done(void *data, struct zxdg_output_v1 *o);
	static void
	s_xdg_output_name(void *data, struct zxdg_output_v1 *o, const char *name);
	static void
	s_xdg_output_description(void *data, struct zxdg_output_v1 *o, const char *desc);
	static void
	s_seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps);
	static void
	s_seat_name(void *data, struct wl_seat *seat, const char *name);
	static void
	s_kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size);
	static void
	s_kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s, struct wl_array *keys);
	static void
	s_kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s);
	static void
	s_kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
	static void
	s_kb_modifiers(void *data,
	               struct wl_keyboard *kb,
	               uint32_t serial,
	               uint32_t depressed,
	               uint32_t latched,
	               uint32_t locked,
	               uint32_t group);
	static void
	s_kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay);
#endif // DXR_APP_HAVE_WAYLAND

	bool
	create_x11(const DxrLinuxWindowDesc &desc);
	void
	destroy_x11();
};
