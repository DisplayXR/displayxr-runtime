// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window chrome (title bar) for the native-Wayland leg of
 *         DxrLinuxWindow — the Wayland GLUE around the shared
 *         displayxr::csd painter (displayxr-common#52, runtime #1654).
 *
 * WHY. GNOME's mutter gives Wayland clients no server-side decorations, so
 * without this a native-Wayland window has no title bar, no close / minimise,
 * and moves only with Super+drag.
 *
 * DESIGN — THE CHROME IS A SUBSURFACE, THE BOUND SURFACE STAYS THE CONTENT.
 *
 *     xdg_toplevel wl_surface  (bound through XR_DXR_wayland_surface_binding;
 *       |                       Vulkan WSI owns attach/commit; exactly the
 *       |                       CONTENT rect, so the swapchain, the atlas and
 *       |                       the weave never contain a chrome pixel)
 *       +-- wl_subsurface at (0, -barH) logical, DESYNC
 *             wl_surface: wl_shm ARGB8888 buffer, device resolution, mapped
 *             with its own wp_viewport onto (w, barH) logical. Alpha is
 *             real, so the rounded top corners are transparent.
 *
 *   The alternative — the chrome surface as the toplevel and the Vulkan
 *   surface as its child — would move the binding off the xdg_toplevel
 *   surface, and with it every configure / viewport / fractional-scale path
 *   the #1653 work validated. This way none of that changes.
 *
 *   xdg_surface.set_window_geometry(0, -barH, w, h + barH) makes the visible
 *   frame bar + content. xdg_toplevel.configure sizes are therefore FRAME
 *   sizes; frame_to_content() takes the bar back off before the helper
 *   declares the content size to the runtime. The subsurface position is
 *   constant (-barH logical never changes with scale), so it is set once,
 *   before the helper's single pre-session commit. Everything later that is
 *   parent state (window geometry) is left pending for the WSI's next present
 *   to commit, like the viewport mapping already is — the app never commits
 *   the bound surface after the session exists.
 *
 * SERVER-SIDE FIRST. With zxdg_decoration_manager_v1 advertised the helper
 * asks for server_side, and draws nothing if it is granted. GNOME does not
 * advertise it, so CSD is the main path. DXR_WL_CSD=0 disables the chrome
 * outright (the pre-#1654 undecorated window); DXR_WL_CSD=force draws it even
 * when server-side decorations are on offer.
 *
 * MATERIAL. The bar is translucent (dxr_csd::Style: a dark tint at about 65 %
 * opacity, rounded top corners), so the chrome surface is ARGB8888,
 * premultiplied, and is NEVER given an opaque region — the compositor must
 * blend it. Where the compositor offers ext_background_effect_manager_v1 with
 * the blur capability, the bar's region (rounded corners approximated per
 * row) is blurred behind it; GNOME offers no client blur protocol, so there it
 * is plain translucency. None of this reaches the 3D content: that is the
 * other surface, opaque, and woven exactly as before.
 *
 * MOVING. A press on the bar calls xdg_toplevel.move — the same compositor
 * move Super+drag performs — so the runtime's geometry-service phase tracking
 * is exactly as before. (Wayland has no client-side positioning, so the X11
 * leg's per-step lattice snap has no Wayland equivalent here.)
 */
#pragma once

#include <wayland-client.h>

#include "csd_titlebar.h"

#include <cstdint>
#include <string>

struct xdg_surface;
struct xdg_toplevel;
struct wp_viewporter;
struct wp_viewport;
struct wl_subcompositor;
struct wl_subsurface;
struct wl_shm;
struct zxdg_decoration_manager_v1;
struct zxdg_toplevel_decoration_v1;
struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;
struct ext_background_effect_manager_v1;
struct ext_background_effect_surface_v1;

class DxrWlChrome
{
public:
	DxrWlChrome() = default;
	~DxrWlChrome();
	DxrWlChrome(const DxrWlChrome &) = delete;
	DxrWlChrome &
	operator=(const DxrWlChrome &) = delete;

	//! Registry hook. True when the global was one of ours (bound).
	bool
	on_global(struct wl_registry *r, uint32_t name, const char *iface, uint32_t version);

	//! Seat hook: take (or drop) the pointer the chrome needs.
	void
	on_seat_capabilities(struct wl_seat *seat, uint32_t caps);

	/*!
	 * Decide the decoration mode and build the chrome. Call once, after the
	 * toplevel exists and BEFORE the helper's single pre-session commit
	 * (the subsurface position is parent state and rides on that commit).
	 *
	 * @param fullscreen the toplevel is being created fullscreen (the chrome
	 *                   is built anyway, just not shown, so leaving
	 *                   fullscreen later brings it back).
	 */
	void
	attach(struct wl_display *display,
	       struct wl_compositor *compositor,
	       struct wl_surface *content,
	       struct xdg_surface *xdg_surface,
	       struct xdg_toplevel *toplevel,
	       struct wp_viewporter *viewporter,
	       const char *title,
	       bool fullscreen);

	/*!
	 * xdg_toplevel.configure states (fullscreen / maximized / tiled /
	 * activated). Call from the configure listener BEFORE frame_to_content().
	 * @p sized is false for a 0x0 ("you choose") configure: mutter's first
	 * configure after a pre-map set_fullscreen carries no fullscreen state
	 * yet, so an unsized configure never un-fullscreens the chrome.
	 */
	void
	on_toplevel_states(struct wl_array *states, bool sized);

	//! Bar height in LOGICAL px while the chrome is shown, else 0.
	int32_t
	bar_logical() const;

	//! Configure (frame) size -> content size, logical px.
	void
	frame_to_content(int32_t *w, int32_t *h) const
	{
		*h -= bar_logical();
		if (*h < 1) {
			*h = 1;
		}
		(void)w;
	}

	/*!
	 * Bring the chrome in line with the content rect and scale: window
	 * geometry (pending on the content surface), and a repaint of the bar if
	 * anything it shows changed. Cheap when nothing did; call after every
	 * configure and every pump.
	 *
	 * @param content_w/h  content size, LOGICAL px
	 * @param scale        logical -> device scale of the output the window is on
	 */
	void
	update(int32_t content_w, int32_t content_h, double scale);

	//! The close button was clicked since the last call.
	bool
	take_close_request();

	//! Human-readable mode for the create log.
	const char *
	mode_name() const;

	void
	destroy();

private:
	//! (`Off`, not `None`: <X11/X.h> #defines None, and this header is
	//! included next to Xlib.)
	enum class Mode
	{
		Undecided,
		Off,        //!< DXR_WL_CSD=0: undecorated, as before #1654
		ServerSide, //!< the compositor draws the decorations
		ClientSide, //!< we draw them (GNOME)
	};

	void
	repaint();
	void
	show(bool visible);
	void
	set_cursor(uint32_t serial, uint32_t shape);
	//! Which resize edge (xdg_toplevel_resize_edge), if any, a point on the
	//! CONTENT surface is on. 0 = none.
	uint32_t
	content_edge(double x, double y) const;
	void
	on_pointer_motion(double x, double y);
	void
	on_pointer_button(uint32_t serial, uint32_t time, uint32_t button, uint32_t state);

	// Listener trampolines.
	static void
	s_decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *d, uint32_t mode);
	static void
	s_ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y);
	static void
	s_ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s);
	static void
	s_ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y);
	static void
	s_ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
	static void
	s_ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value);
	static void
	s_ptr_frame(void *data, struct wl_pointer *p);
	static void
	s_ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source);
	static void
	s_ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis);
	static void
	s_ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete);
	static void
	s_buffer_release(void *data, struct wl_buffer *b);
	static void
	s_blur_capabilities(void *data, struct ext_background_effect_manager_v1 *m, uint32_t flags);

	//! (Re)declare the blur region for the current bar size / corners.
	void
	update_blur_region();

	Mode m_mode = Mode::Undecided;
	bool m_force_csd = false;

	// Globals (bound by on_global, owned here).
	struct wl_subcompositor *m_subcompositor = nullptr;
	struct wl_shm *m_shm = nullptr;
	struct zxdg_decoration_manager_v1 *m_deco_manager = nullptr;
	struct wp_cursor_shape_manager_v1 *m_cursor_manager = nullptr;
	struct ext_background_effect_manager_v1 *m_blur_manager = nullptr;
	bool m_blur_capable = false; //!< manager advertised the blur capability

	// Borrowed from DxrLinuxWindow.
	struct wl_display *m_display = nullptr;
	struct wl_surface *m_content = nullptr;
	struct xdg_surface *m_xdg_surface = nullptr;
	struct xdg_toplevel *m_toplevel = nullptr;
	struct wl_seat *m_seat = nullptr;

	// Owned.
	struct zxdg_toplevel_decoration_v1 *m_deco = nullptr;
	struct wl_surface *m_surface = nullptr; //!< the chrome surface
	struct wl_subsurface *m_subsurface = nullptr;
	struct wp_viewport *m_viewport = nullptr;
	struct wl_pointer *m_pointer = nullptr;
	struct wp_cursor_shape_device_v1 *m_cursor_device = nullptr;
	struct ext_background_effect_surface_v1 *m_blur = nullptr;
	struct wl_compositor *m_compositor = nullptr; //!< borrowed, for wl_region
	int32_t m_blur_w = 0;       //!< logical width the blur region was set for
	uint32_t m_blur_radius = 0; //!< ... and corner radius (device px)

	dxr_csd::TitleBar m_bar;
	dxr_csd::DoubleClick m_dclick;

	// Toplevel state (from configure).
	bool m_fullscreen = false;
	bool m_maximized = false; //!< maximised or tiled on any edge
	bool m_activated = true;

	// What is currently on screen.
	bool m_shown = false;
	int32_t m_content_w = 0, m_content_h = 0; //!< logical
	double m_scale = 1.0;
	uint32_t m_buf_w = 0, m_buf_h = 0; //!< device px of the attached bar buffer
	int32_t m_geom_x = 0, m_geom_y = 0, m_geom_w = 0, m_geom_h = 0;
	bool m_geom_set = false;

	// Pointer.
	struct wl_surface *m_ptr_surface = nullptr; //!< which of our surfaces it is over
	double m_ptr_x = 0.0, m_ptr_y = 0.0;         //!< surface-local, logical
	uint32_t m_ptr_enter_serial = 0;
	uint32_t m_cursor_shape = 0;

	bool m_close_requested = false;
};
