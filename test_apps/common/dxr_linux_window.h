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
 * Why one helper: the X11 leg is lifted verbatim out of cube_handle_vk_linux
 * (placement, ICCCM size hints, post-map XMoveWindow) and cube_zones_vk_linux
 * carried a byte-identical copy. Keeping one copy is what makes the Wayland
 * leg a single addition instead of two.
 *
 * ORDERING CONTRACT (both backends):
 *   create instance -> get system -> xrGetSystemProperties (panel rect, INV-1.3)
 *   -> DxrLinuxWindow::create() -> xrCreateSession with session_binding_chain()
 *   -> ... -> destroy() LAST, after the Vulkan instance is gone. The runtime's
 *   VkSurfaceKHR borrows the Xlib Display / wl_display connection for its
 *   lifetime, so the connection must outlive the session.
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
 */
#pragma once

// Xlib first: XR_DXR_xlib_window_binding.h wants the real Display / Window
// types, not its self-contained stand-ins.
#include <X11/Xlib.h>
#include <X11/Xutil.h> // XSizeHints for INV-1.3 window placement

#ifdef DXR_APP_HAVE_WAYLAND
// Likewise: the Wayland binding header forward-declares wl_display/wl_surface,
// so the real definitions have to come first.
#include <wayland-client.h>
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
	Num3
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
	uint32_t panel_height = 0; //!< ...

	const char *title = "DisplayXR";               //!< toplevel title
	const char *app_id = "com.displayxr.test_app"; //!< Wayland xdg app-id

	//! Wayland only. The runtime sizes its WSI swapchain to the PANEL, never to
	//! the surface, and never follows a Wayland resize
	//! (comp_vk_native_compositor.c: `c->settings.preferred`), because on
	//! Wayland `currentExtent` is UINT32_MAX. Fullscreen-on-the-panel is
	//! therefore the only mode where the weave can be 1:1. Clear this to
	//! observe the windowed limitation on purpose.
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

	//! Live window size in surface pixels. X11 reads XGetWindowAttributes;
	//! Wayland returns the last xdg_toplevel.configure size (the compositor
	//! never tells a client its buffer size, and this helper never sets a
	//! buffer scale, so the configure size IS the surface size). False when no
	//! size is known yet — the caller should fall back to its own envelope.
	bool
	current_size(uint32_t *w, uint32_t *h) const;

	/*!
	 * Pointer to the filled window-binding struct, with `.next = next`, ready
	 * to hand to XrSessionCreateInfo::next. Member storage: valid until this
	 * object is destroyed. Returns `next` unchanged if no window exists.
	 */
	const void *
	session_binding_chain(const void *next);

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

	// --- Binding storage (handed to xrCreateSession) ------------------------
	XrXlibWindowBindingCreateInfoDXR m_xlib_binding = {};
	XrWaylandSurfaceBindingCreateInfoDXR m_wl_binding = {};

#ifdef DXR_APP_HAVE_WAYLAND
	// --- Wayland leg -------------------------------------------------------
	struct WlOutput
	{
		struct wl_output *output = nullptr;
		uint32_t name = 0;
		int32_t x = 0, y = 0;
		int32_t width = 0, height = 0;
		int32_t scale = 1;
	};

	struct wl_display *m_wl_display = nullptr;
	struct wl_registry *m_wl_registry = nullptr;
	struct wl_compositor *m_wl_compositor = nullptr;
	struct xdg_wm_base *m_wl_wm_base = nullptr;
	struct wl_seat *m_wl_seat = nullptr;
	struct wl_keyboard *m_wl_keyboard = nullptr;
	struct wl_surface *m_wl_surface = nullptr;
	struct xdg_surface *m_wl_xdg_surface = nullptr;
	struct xdg_toplevel *m_wl_toplevel = nullptr;
	std::vector<WlOutput> m_wl_outputs;

	bool m_wl_configured = false;
	int32_t m_wl_config_w = 0;
	int32_t m_wl_config_h = 0;

	// Per-frame scratch, set by the listeners and consumed by pump().
	std::vector<DxrKey> m_wl_key_queue;
	bool m_wl_close_requested = false;

	bool
	create_wayland(const DxrLinuxWindowDesc &desc);
	void
	destroy_wayland();

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
