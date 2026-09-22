// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native-Wayland window chrome: subsurface + wl_shm glue around the
 *         shared displayxr::csd painter. See dxr_wl_chrome.h for the design.
 */

#include "dxr_wl_chrome.h"

#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"

#include <linux/input-event-codes.h> // BTN_LEFT / BTN_RIGHT
#include <sys/mman.h>                // memfd_create, mmap
#include <unistd.h>                  // ftruncate, close

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHROME_INFO(fmt, ...)                                                                                          \
	do {                                                                                                           \
		fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__);                                                   \
		fflush(stdout);                                                                                        \
	} while (0)
#define CHROME_WARN(fmt, ...) fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)

namespace {

//! Resize band on the CONTENT surface's side / bottom edges, logical px.
//! Inside the window (there is no shadow margin to put it in), so it is kept
//! thin: it takes a few pixels of pointer input from the content, which the
//! test apps do not use.
constexpr double kEdgeBand = 6.0;

//! A wl_shm buffer the compositor may still be reading. Freed on release.
struct ShmBuffer
{
	struct wl_buffer *buffer = nullptr;
	void *map = nullptr;
	size_t size = 0;
};

std::vector<ShmBuffer *> g_outstanding; // one chrome per process; see destroy()

void
release_pointer(struct wl_pointer *p)
{
	// wl_pointer.release is v3+; older seats only have the client-side destroy.
	if (wl_pointer_get_version(p) >= WL_POINTER_RELEASE_SINCE_VERSION) {
		wl_pointer_release(p);
	} else {
		wl_pointer_destroy(p);
	}
}

void
free_shm_buffer(ShmBuffer *b)
{
	if (b->buffer != nullptr) {
		wl_buffer_destroy(b->buffer);
	}
	if (b->map != nullptr && b->map != MAP_FAILED) {
		munmap(b->map, b->size);
	}
	for (auto it = g_outstanding.begin(); it != g_outstanding.end(); ++it) {
		if (*it == b) {
			g_outstanding.erase(it);
			break;
		}
	}
	delete b;
}

} // namespace


/*
 *
 * Globals / seat.
 *
 */

bool
DxrWlChrome::on_global(struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	if (strcmp(iface, wl_subcompositor_interface.name) == 0) {
		m_subcompositor =
		    static_cast<struct wl_subcompositor *>(wl_registry_bind(r, name, &wl_subcompositor_interface, 1));
		return true;
	}
	if (strcmp(iface, wl_shm_interface.name) == 0) {
		m_shm = static_cast<struct wl_shm *>(wl_registry_bind(r, name, &wl_shm_interface, 1));
		return true;
	}
	if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
		m_deco_manager = static_cast<struct zxdg_decoration_manager_v1 *>(
		    wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1));
		return true;
	}
	if (strcmp(iface, ext_background_effect_manager_v1_interface.name) == 0) {
		// Background blur behind the translucent bar. Standardised successor
		// of KDE's org_kde_kwin_blur; GNOME offers neither.
		m_blur_manager = static_cast<struct ext_background_effect_manager_v1 *>(
		    wl_registry_bind(r, name, &ext_background_effect_manager_v1_interface, 1));
		static const struct ext_background_effect_manager_v1_listener kBlurListener = {
		    s_blur_capabilities,
		};
		ext_background_effect_manager_v1_add_listener(m_blur_manager, &kBlurListener, this);
		return true;
	}
	if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		m_cursor_manager = static_cast<struct wp_cursor_shape_manager_v1 *>(
		    wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1));
		return true;
	}
	(void)version;
	return false;
}

void
DxrWlChrome::on_seat_capabilities(struct wl_seat *seat, uint32_t caps)
{
	m_seat = seat;
	const bool has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
	if (has_ptr && m_pointer == nullptr) {
		m_pointer = wl_seat_get_pointer(seat);
		static const struct wl_pointer_listener kPtrListener = {
		    s_ptr_enter,       s_ptr_leave,     s_ptr_motion,         s_ptr_button, s_ptr_axis,
		    s_ptr_frame,       s_ptr_axis_source, s_ptr_axis_stop,    s_ptr_axis_discrete,
		};
		wl_pointer_add_listener(m_pointer, &kPtrListener, this);
		if (m_cursor_manager != nullptr) {
			m_cursor_device = wp_cursor_shape_manager_v1_get_pointer(m_cursor_manager, m_pointer);
		}
	} else if (!has_ptr && m_pointer != nullptr) {
		if (m_cursor_device != nullptr) {
			wp_cursor_shape_device_v1_destroy(m_cursor_device);
			m_cursor_device = nullptr;
		}
		release_pointer(m_pointer);
		m_pointer = nullptr;
		m_ptr_surface = nullptr;
	}
}


/*
 *
 * Setup.
 *
 */

void
DxrWlChrome::attach(struct wl_display *display,
                    struct wl_compositor *compositor,
                    struct wl_surface *content,
                    struct xdg_surface *xdg_surface,
                    struct xdg_toplevel *toplevel,
                    struct wp_viewporter *viewporter,
                    const char *title,
                    bool fullscreen)
{
	m_display = display;
	m_compositor = compositor;
	m_content = content;
	m_xdg_surface = xdg_surface;
	m_toplevel = toplevel;
	m_fullscreen = fullscreen;

	const char *env = getenv("DXR_WL_CSD");
	if (env != nullptr && strcmp(env, "0") == 0) {
		m_mode = Mode::Off;
		CHROME_INFO("Wayland chrome: disabled (DXR_WL_CSD=0) — undecorated window, Super+drag to move");
		return;
	}
	m_force_csd = env != nullptr && strcmp(env, "force") == 0;

	// Server-side first, where it is on offer. The answer arrives with the
	// initial configure; until then the mode is Undecided and nothing shows.
	if (m_deco_manager != nullptr && !m_force_csd) {
		m_deco = zxdg_decoration_manager_v1_get_toplevel_decoration(m_deco_manager, toplevel);
		static const struct zxdg_toplevel_decoration_v1_listener kDecoListener = {
		    s_decoration_configure,
		};
		zxdg_toplevel_decoration_v1_add_listener(m_deco, &kDecoListener, this);
		zxdg_toplevel_decoration_v1_set_mode(m_deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	} else {
		m_mode = Mode::ClientSide;
	}

	if (m_subcompositor == nullptr || m_shm == nullptr) {
		if (m_mode == Mode::ClientSide) {
			CHROME_WARN("Wayland chrome: compositor lacks %s — no title bar (Super+drag to move)",
			            m_subcompositor == nullptr ? "wl_subcompositor" : "wl_shm");
			m_mode = Mode::Off;
		}
		return;
	}

	// The chrome surface. Built even for a fullscreen start (not shown), so
	// leaving fullscreen brings the bar back without any setup then.
	m_surface = wl_compositor_create_surface(compositor);
	m_subsurface = wl_subcompositor_get_subsurface(m_subcompositor, m_surface, content);
	// Parent state: applied by the helper's one pre-session commit of the
	// content surface, and never changed afterwards — the bar is always
	// kLogicalHeight LOGICAL px, whatever the scale.
	wl_subsurface_set_position(m_subsurface, 0, -(int32_t)dxr_csd::TitleBar::kLogicalHeight);
	// Desync: the bar repaints on its own commits, never waiting on the WSI's.
	wl_subsurface_set_desync(m_subsurface);
	if (viewporter != nullptr) {
		m_viewport = wp_viewporter_get_viewport(viewporter, m_surface);
	}
	// NO wl_surface_set_opaque_region on this surface, ever: the bar is
	// translucent, and an opaque region would let the compositor skip
	// blending it (and cull whatever is behind).
	if (m_blur_manager != nullptr) {
		m_blur = ext_background_effect_manager_v1_get_background_effect(m_blur_manager, m_surface);
	}

	m_bar.configure(1.0f);
	m_bar.setSurfaceHasAlpha(true); // ARGB8888: translucent material, rounded corners
	m_bar.setTitle(title != nullptr ? title : "");
	CHROME_INFO("Wayland chrome: %s; translucent bar (%.0f %% opacity) %s; title font %s; cursor shapes %s",
	            m_mode != Mode::ClientSide ? "asked the compositor for server-side decorations"
	            : m_force_csd              ? "client-side title bar (forced by DXR_WL_CSD=force)"
	                                       : "client-side title bar (no zxdg_decoration_manager_v1 — expected on GNOME)",
	            (double)(m_bar.style().opacity * 100.f),
	            m_blur != nullptr ? "with ext_background_effect_v1 blur where the capability is offered"
	                              : "without background blur (no ext_background_effect_manager_v1 — expected "
	                                "on GNOME)",
	            m_bar.fontPath().empty() ? "NONE (set DXR_CSD_FONT)" : m_bar.fontPath().c_str(),
	            m_cursor_manager != nullptr ? "wp_cursor_shape_v1" : "unavailable (cursor left to the compositor)");
}

void
DxrWlChrome::s_decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *d, uint32_t mode)
{
	(void)d;
	auto *self = static_cast<DxrWlChrome *>(data);
	const Mode want =
	    mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE ? Mode::ServerSide : Mode::ClientSide;
	if (want == self->m_mode) {
		return;
	}
	self->m_mode = (want == Mode::ClientSide && self->m_surface == nullptr) ? Mode::Off : want;
	CHROME_INFO("Wayland chrome: compositor chose %s decorations",
	            want == Mode::ServerSide ? "SERVER-side (no client title bar drawn)" : "CLIENT-side");
}

void
DxrWlChrome::on_toplevel_states(struct wl_array *states, bool sized)
{
	bool fullscreen = false, maximized = false, activated = false;
	if (states != nullptr) {
		const uint32_t *s = static_cast<const uint32_t *>(states->data);
		const size_t n = states->size / sizeof(uint32_t);
		for (size_t i = 0; i < n; i++) {
			switch (s[i]) {
			case XDG_TOPLEVEL_STATE_FULLSCREEN: fullscreen = true; break;
			case XDG_TOPLEVEL_STATE_MAXIMIZED: maximized = true; break;
			case XDG_TOPLEVEL_STATE_ACTIVATED: activated = true; break;
			case XDG_TOPLEVEL_STATE_TILED_LEFT:
			case XDG_TOPLEVEL_STATE_TILED_RIGHT:
			case XDG_TOPLEVEL_STATE_TILED_TOP:
			case XDG_TOPLEVEL_STATE_TILED_BOTTOM: maximized = true; break;
			default: break;
			}
		}
	}
	if (sized || fullscreen) {
		m_fullscreen = fullscreen;
	}
	m_maximized = maximized;
	m_activated = activated;
	m_bar.setMaximized(maximized);
	m_bar.setFocused(activated);
}

int32_t
DxrWlChrome::bar_logical() const
{
	return (m_mode == Mode::ClientSide && !m_fullscreen) ? (int32_t)dxr_csd::TitleBar::kLogicalHeight : 0;
}

const char *
DxrWlChrome::mode_name() const
{
	switch (m_mode) {
	case Mode::Off: return "none";
	case Mode::ServerSide: return "server-side";
	case Mode::ClientSide: return "client-side";
	default: return "undecided";
	}
}


/*
 *
 * Per-frame.
 *
 */

void
DxrWlChrome::update(int32_t content_w, int32_t content_h, double scale)
{
	if (m_surface == nullptr || content_w <= 0 || content_h <= 0) {
		return;
	}
	const bool want_shown = bar_logical() > 0;
	const int32_t bar = (int32_t)dxr_csd::TitleBar::kLogicalHeight;

	// Window geometry = the visible frame. Pending on the CONTENT surface; the
	// WSI's next present commits it (the app never commits that surface once
	// the session exists). Not touched at all without CSD: an undecorated or
	// server-decorated window keeps the compositor's default (the surface).
	if (m_mode == Mode::ClientSide || m_geom_set) {
		// (m_geom_set with another mode: the compositor switched to server-side
		// decorations after we had drawn our own — hand the geometry back to
		// the plain surface rect.)
		const int32_t gx = 0, gy = want_shown ? -bar : 0;
		const int32_t gw = content_w, gh = content_h + (want_shown ? bar : 0);
		if (!m_geom_set || gx != m_geom_x || gy != m_geom_y || gw != m_geom_w || gh != m_geom_h) {
			xdg_surface_set_window_geometry(m_xdg_surface, gx, gy, gw, gh);
			m_geom_set = true;
			m_geom_x = gx;
			m_geom_y = gy;
			m_geom_w = gw;
			m_geom_h = gh;
			CHROME_INFO("Wayland chrome: window geometry %d,%d %dx%d logical (content %dx%d + %d px bar)", gx,
			            gy, gw, gh, content_w, content_h, want_shown ? bar : 0);
		}
	}

	if (!want_shown) {
		show(false);
		return;
	}

	// Without a viewport the buffer can only be mapped by an integer scale;
	// draw at that scale rather than at a fractional one it cannot express.
	double s = scale > 0.0 ? scale : 1.0;
	if (m_viewport == nullptr) {
		s = std::floor(s + 0.5);
		if (s < 1.0) {
			s = 1.0;
		}
	}
	if (s != m_scale || m_bar.height() == 0) {
		m_scale = s;
		m_bar.configure((float)s);
	}
	const bool size_changed = content_w != m_content_w || content_h != m_content_h;
	m_content_w = content_w;
	m_content_h = content_h;

	const uint32_t want_w = (uint32_t)std::lround((double)content_w * m_scale);
	if (!m_shown || size_changed || m_bar.dirty() || want_w != m_buf_w || m_bar.height() != m_buf_h) {
		repaint();
	}
}

void
DxrWlChrome::show(bool visible)
{
	if (visible || !m_shown || m_surface == nullptr) {
		return;
	}
	// A NULL buffer unmaps the subsurface; desync, so this commit is enough.
	wl_surface_attach(m_surface, nullptr, 0, 0);
	wl_surface_commit(m_surface);
	m_shown = false;
	m_buf_w = m_buf_h = 0;
}

void
DxrWlChrome::repaint()
{
	const uint32_t w = (uint32_t)std::max<long>(1, std::lround((double)m_content_w * m_scale));
	const uint32_t h = m_bar.height();
	m_bar.render(w);

	const size_t stride = (size_t)w * 4;
	const size_t size = stride * h;
	const int fd = memfd_create("dxr-wl-chrome", MFD_CLOEXEC);
	if (fd < 0) {
		CHROME_WARN("Wayland chrome: memfd_create failed — bar not drawn");
		return;
	}
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		CHROME_WARN("Wayland chrome: ftruncate(%zu) failed — bar not drawn", size);
		return;
	}
	auto *b = new ShmBuffer();
	b->size = size;
	b->map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->map == MAP_FAILED) {
		close(fd);
		delete b;
		CHROME_WARN("Wayland chrome: mmap failed — bar not drawn");
		return;
	}
	// wl_shm ARGB8888: little-endian 0xAARRGGBB, premultiplied — exactly what
	// the painter produces, so the rounded corners are real alpha.
	m_bar.pack(static_cast<uint32_t *>(b->map), w, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u);

	struct wl_shm_pool *pool = wl_shm_create_pool(m_shm, fd, (int32_t)size);
	b->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	static const struct wl_buffer_listener kBufListener = {
	    s_buffer_release,
	};
	wl_buffer_add_listener(b->buffer, &kBufListener, b);
	g_outstanding.push_back(b);

	wl_surface_attach(m_surface, b->buffer, 0, 0);
	if (m_viewport != nullptr) {
		wp_viewport_set_destination(m_viewport, m_content_w, (int32_t)dxr_csd::TitleBar::kLogicalHeight);
	} else {
		wl_surface_set_buffer_scale(m_surface, (int32_t)m_scale);
	}
	wl_surface_damage_buffer(m_surface, 0, 0, (int32_t)w, (int32_t)h);
	update_blur_region(); // pending on this surface; rides on the commit below
	wl_surface_commit(m_surface);

	if (!m_shown) {
		CHROME_INFO("Wayland chrome: title bar shown — %ux%u px buffer for %dx%d logical at scale %.4f, "
		            "corner radius %u px",
		            w, h, m_content_w, (int)dxr_csd::TitleBar::kLogicalHeight, m_scale, m_bar.cornerRadius());
	}
	m_shown = true;
	m_buf_w = w;
	m_buf_h = h;
}

void
DxrWlChrome::s_blur_capabilities(void *data, struct ext_background_effect_manager_v1 *m, uint32_t flags)
{
	(void)m;
	auto *self = static_cast<DxrWlChrome *>(data);
	const bool capable = (flags & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR) != 0;
	if (capable != self->m_blur_capable) {
		self->m_blur_capable = capable;
		self->m_blur_w = 0; // force a re-declare on the next repaint
		self->m_bar.invalidate();
		CHROME_INFO("Wayland chrome: compositor background blur %s", capable ? "AVAILABLE" : "unavailable");
	}
}

void
DxrWlChrome::update_blur_region()
{
	if (m_blur == nullptr || m_compositor == nullptr) {
		return;
	}
	const uint32_t radius = m_bar.cornerRadius();
	if (m_blur_capable && m_blur_w == m_content_w && m_blur_radius == radius) {
		return;
	}
	if (!m_blur_capable) {
		if (m_blur_w != 0) {
			ext_background_effect_surface_v1_set_blur_region(m_blur, nullptr);
			m_blur_w = 0;
		}
		return;
	}
	// Surface-local LOGICAL px. A region is rectangles, so the rounded top
	// corners are approximated one logical row at a time — close enough that
	// no blurred square corner shows outside the anti-aliased edge.
	struct wl_region *region = wl_compositor_create_region(m_compositor);
	const int32_t bar = (int32_t)dxr_csd::TitleBar::kLogicalHeight;
	const double r = m_scale > 0.0 ? (double)radius / m_scale : 0.0;
	const int32_t rows = (int32_t)std::ceil(r);
	for (int32_t y = 0; y < rows && y < bar; y++) {
		const double dy = r - ((double)y + 0.5);
		const double inset = r - std::sqrt(std::max(0.0, r * r - dy * dy));
		const int32_t ix = (int32_t)std::ceil(inset);
		if (m_content_w - 2 * ix > 0) {
			wl_region_add(region, ix, y, m_content_w - 2 * ix, 1);
		}
	}
	wl_region_add(region, 0, rows, m_content_w, bar - rows);
	ext_background_effect_surface_v1_set_blur_region(m_blur, region);
	wl_region_destroy(region);
	m_blur_w = m_content_w;
	m_blur_radius = radius;
}

void
DxrWlChrome::s_buffer_release(void *data, struct wl_buffer *b)
{
	(void)b;
	free_shm_buffer(static_cast<ShmBuffer *>(data));
}

bool
DxrWlChrome::take_close_request()
{
	const bool r = m_close_requested;
	m_close_requested = false;
	return r;
}


/*
 *
 * Pointer.
 *
 */

void
DxrWlChrome::set_cursor(uint32_t serial, uint32_t shape)
{
	if (m_cursor_device == nullptr || shape == m_cursor_shape) {
		return;
	}
	wp_cursor_shape_device_v1_set_shape(m_cursor_device, serial, shape);
	m_cursor_shape = shape;
}

uint32_t
DxrWlChrome::content_edge(double x, double y) const
{
	if (bar_logical() == 0 || m_maximized || m_content_w <= 0 || m_content_h <= 0) {
		return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
	}
	const bool left = x < kEdgeBand;
	const bool right = x >= (double)m_content_w - kEdgeBand;
	const bool bottom = y >= (double)m_content_h - kEdgeBand;
	if (bottom && left) {
		return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
	}
	if (bottom && right) {
		return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
	}
	if (bottom) {
		return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
	}
	if (left) {
		return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
	}
	if (right) {
		return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
	}
	return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
}

static uint32_t
edge_cursor(uint32_t edge)
{
	switch (edge) {
	case XDG_TOPLEVEL_RESIZE_EDGE_TOP: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
	case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
	case XDG_TOPLEVEL_RESIZE_EDGE_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
	case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
	case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
	case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
	case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
	case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
	default: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
	}
}

static uint32_t
hit_edge(dxr_csd::Hit h)
{
	switch (h) {
	case dxr_csd::Hit::ResizeTop: return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
	case dxr_csd::Hit::ResizeTopLeft: return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
	case dxr_csd::Hit::ResizeTopRight: return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
	default: return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
	}
}

void
DxrWlChrome::on_pointer_motion(double x, double y)
{
	m_ptr_x = x;
	m_ptr_y = y;
	if (m_ptr_surface == m_surface && m_surface != nullptr) {
		// Surface-local LOGICAL -> the bar raster's device px.
		const dxr_csd::Hit h = m_bar.hitTest((int)(x * m_scale), (int)(y * m_scale), m_buf_w);
		m_bar.setHover(h);
		set_cursor(m_ptr_enter_serial, edge_cursor(hit_edge(h)));
	} else if (m_ptr_surface == m_content && m_content != nullptr) {
		set_cursor(m_ptr_enter_serial, edge_cursor(content_edge(x, y)));
	}
}

void
DxrWlChrome::on_pointer_button(uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	const bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (m_seat == nullptr || m_toplevel == nullptr) {
		return;
	}

	if (m_ptr_surface == m_content && m_content != nullptr) {
		if (pressed && button == BTN_LEFT) {
			const uint32_t edge = content_edge(m_ptr_x, m_ptr_y);
			if (edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
				xdg_toplevel_resize(m_toplevel, m_seat, serial, edge);
			}
		}
		return;
	}
	if (m_ptr_surface != m_surface || m_surface == nullptr) {
		return;
	}

	const int dx = (int)(m_ptr_x * m_scale), dy = (int)(m_ptr_y * m_scale);
	const dxr_csd::Hit h = m_bar.hitTest(dx, dy, m_buf_w);

	if (button == BTN_RIGHT && pressed && h != dxr_csd::Hit::Outside) {
		// GNOME's window menu, as a server-decorated title bar offers.
		// Window-geometry coordinates: the bar starts the frame at y = 0.
		xdg_toplevel_show_window_menu(m_toplevel, m_seat, serial, (int32_t)m_ptr_x, (int32_t)m_ptr_y);
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}

	if (pressed) {
		if (h == dxr_csd::Hit::Drag) {
			if (m_dclick.press(time, (int)m_ptr_x, (int)m_ptr_y, 4)) {
				if (m_maximized) {
					xdg_toplevel_unset_maximized(m_toplevel);
				} else {
					xdg_toplevel_set_maximized(m_toplevel);
				}
				CHROME_INFO("Wayland chrome: double-click — %s", m_maximized ? "restore" : "maximise");
				return;
			}
			// The compositor's own move — the same one Super+drag runs, so
			// the runtime's geometry-service phase tracking is unchanged.
			xdg_toplevel_move(m_toplevel, m_seat, serial);
			return;
		}
		m_dclick.reset();
		if (dxr_csd::IsResize(h)) {
			xdg_toplevel_resize(m_toplevel, m_seat, serial, hit_edge(h));
			return;
		}
		m_bar.setPressed(h); // a button: acts on release, over the same button
		return;
	}

	// Release.
	const dxr_csd::Hit down = m_bar.pressed();
	m_bar.setPressed(dxr_csd::Hit::Outside);
	if (down == dxr_csd::Hit::Outside || down != h) {
		return;
	}
	if (h == dxr_csd::Hit::Close) {
		CHROME_INFO("Wayland chrome: close button");
		m_close_requested = true;
	} else if (h == dxr_csd::Hit::Minimize) {
		CHROME_INFO("Wayland chrome: minimise button");
		xdg_toplevel_set_minimized(m_toplevel);
		m_bar.setHover(dxr_csd::Hit::Outside);
	}
}

void
DxrWlChrome::s_ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)p;
	auto *self = static_cast<DxrWlChrome *>(data);
	self->m_ptr_surface = s;
	self->m_ptr_enter_serial = serial;
	self->m_cursor_shape = 0; // unknown after an enter: force the next set
	self->on_pointer_motion(wl_fixed_to_double(x), wl_fixed_to_double(y));
	if (self->m_cursor_shape == 0) {
		self->set_cursor(serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
	}
}

void
DxrWlChrome::s_ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)p;
	(void)serial;
	(void)s;
	auto *self = static_cast<DxrWlChrome *>(data);
	self->m_ptr_surface = nullptr;
	self->m_bar.setHover(dxr_csd::Hit::Outside);
	self->m_bar.setPressed(dxr_csd::Hit::Outside);
}

void
DxrWlChrome::s_ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)p;
	(void)time;
	static_cast<DxrWlChrome *>(data)->on_pointer_motion(wl_fixed_to_double(x), wl_fixed_to_double(y));
}

void
DxrWlChrome::s_ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	(void)p;
	static_cast<DxrWlChrome *>(data)->on_pointer_button(serial, time, button, state);
}

void
DxrWlChrome::s_ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	(void)data;
	(void)p;
	(void)time;
	(void)axis;
	(void)value;
}

void
DxrWlChrome::s_ptr_frame(void *data, struct wl_pointer *p)
{
	(void)data;
	(void)p;
}

void
DxrWlChrome::s_ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
	(void)data;
	(void)p;
	(void)source;
}

void
DxrWlChrome::s_ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
	(void)data;
	(void)p;
	(void)time;
	(void)axis;
}

void
DxrWlChrome::s_ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete)
{
	(void)data;
	(void)p;
	(void)axis;
	(void)discrete;
}


/*
 *
 * Teardown.
 *
 */

void
DxrWlChrome::destroy()
{
	if (m_cursor_device != nullptr) {
		wp_cursor_shape_device_v1_destroy(m_cursor_device);
		m_cursor_device = nullptr;
	}
	if (m_pointer != nullptr) {
		release_pointer(m_pointer);
		m_pointer = nullptr;
	}
	if (m_blur != nullptr) {
		ext_background_effect_surface_v1_destroy(m_blur);
		m_blur = nullptr;
	}
	if (m_viewport != nullptr) {
		wp_viewport_destroy(m_viewport);
		m_viewport = nullptr;
	}
	if (m_subsurface != nullptr) {
		wl_subsurface_destroy(m_subsurface);
		m_subsurface = nullptr;
	}
	if (m_surface != nullptr) {
		wl_surface_destroy(m_surface);
		m_surface = nullptr;
	}
	while (!g_outstanding.empty()) {
		free_shm_buffer(g_outstanding.back());
	}
	if (m_deco != nullptr) {
		zxdg_toplevel_decoration_v1_destroy(m_deco);
		m_deco = nullptr;
	}
	if (m_deco_manager != nullptr) {
		zxdg_decoration_manager_v1_destroy(m_deco_manager);
		m_deco_manager = nullptr;
	}
	if (m_cursor_manager != nullptr) {
		wp_cursor_shape_manager_v1_destroy(m_cursor_manager);
		m_cursor_manager = nullptr;
	}
	if (m_blur_manager != nullptr) {
		ext_background_effect_manager_v1_destroy(m_blur_manager);
		m_blur_manager = nullptr;
	}
	if (m_shm != nullptr) {
		wl_shm_destroy(m_shm);
		m_shm = nullptr;
	}
	if (m_subcompositor != nullptr) {
		wl_subcompositor_destroy(m_subcompositor);
		m_subcompositor = nullptr;
	}
	m_shown = false;
	m_ptr_surface = nullptr;
	m_seat = nullptr;
}

DxrWlChrome::~DxrWlChrome()
{
	destroy();
}
