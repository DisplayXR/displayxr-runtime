// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671): the X11 backend
 *         (desktop Linux, X11 and XWayland).
 * @ingroup aux_util
 *
 * ## What the tracker is given
 *
 * The window's XID, cast to a pointer (`(void *)(uintptr_t)xid`): the app's
 * window from XR_DXR_xlib_window_binding, or the runtime's own XCB window.
 * The window may be a child (a render child, or the runtime's window adopted
 * under the app's own top-level); the backend walks up to its managed
 * top-level itself. The 3D screen is named by its RandR monitor name
 * (`xrt_system_compositor_info::display_device_name` on desktop Linux).
 *
 * ## Its own connection, libxcb loaded at run time
 *
 * The backend never touches the app's Display: it opens its own connection
 * (`xcb_connect(NULL)`, i.e. `$DISPLAY`) and uses it from the tracker thread
 * only. libxcb is dlopen'd, not linked, for the reason os_display_desktop_x11.c
 * gives (aux_util lands inside arbitrary host processes; a box without X
 * libraries just has no tracker). xcb rather than Xlib because an X error on
 * an xcb connection is a value handed back to the caller (or a queued event),
 * never a call into the process-wide Xlib error handler that exits the host
 * (#2311): the app's window can be destroyed under us at any moment, and every
 * request about it can then fail with BadWindow.
 *
 * ## The signals, cheapest first
 *
 * 1. The target is not viewable (unmapped, or an ancestor is): hidden. Most
 *    window managers unmap a minimized client; ICCCM `WM_STATE` = Iconic and
 *    EWMH `_NET_WM_STATE_HIDDEN` say "minimized" for the ones that do not.
 * 2. The client is on another virtual desktop than `_NET_CURRENT_DESKTOP`.
 * 3. The server said `VisibilityNotify(FullyObscured)` for the target. Only an
 *    uncomposited server ever says it: a redirected window is always
 *    "unobscured", so on a compositing desktop this signal is simply absent.
 * 4. The z-order cover check: every managed client above ours in
 *    `_NET_CLIENT_LIST_STACKING`, as opaque rects, against the target's rect
 *    clipped to the 3D screen, with the same rules as Windows where X has the
 *    same facts — skipped: not viewable, minimized, other desktop, same
 *    process (`_NET_WM_PID`), `_NET_WM_WINDOW_OPACITY` below opaque, overlay
 *    window types (dock, notification, tooltip, menus, drag icon, desktop),
 *    and the configurable overlay-process list (process names from
 *    /proc/<pid>/comm). Occluder rects include server-side decorations
 *    (`_NET_FRAME_EXTENTS`) and exclude client-side shadow margins
 *    (`_GTK_FRAME_EXTENTS`), the X counterpart of DWM's extended frame bounds.
 *
 * The backend also selects structure, property and visibility events on the
 * target, its top-level and the root (event masks are per client, so this
 * never changes what the app receives): any of them wakes the tracker for an
 * early re-check, so a restore shows at once rather than at the next period.
 *
 * ## Limits (all on the "visible" side, which keeps the lens in 3D)
 *
 * - Without an EWMH window manager there is no stacking list: only signals 1-3.
 * - Under XWayland, native Wayland windows are not X windows: they never
 *   occlude an X11 window here. A pure Wayland app is not tracked at all (a
 *   Wayland client cannot learn the global stacking order, by design; an
 *   app-owned xdg_toplevel's `suspended` state reaches only the app).
 * - Shaped (non-rectangular) and input-only overlays count as their bounding
 *   rect when they have no opacity hint.
 */

#include "util/u_window_visibility.h"
#include "util/u_window_visibility_posix.h"
#include "util/u_logging.h"
#include "util/u_debug.h"
#include "os/os_display_desktop.h"
#include "os/os_time.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 *
 * Minimal xcb declarations: this file must build on a box with no X
 * development packages. The reply layouts are the X11 wire format, fixed by
 * the protocol.
 *
 */

typedef struct xcb_connection_t wvx_conn;
typedef uint32_t wvx_window;
typedef uint32_t wvx_atom;

typedef struct
{
	unsigned int sequence;
} wvx_cookie;

typedef struct
{
	uint8_t response_type;
	uint8_t pad0;
	uint16_t sequence;
	uint32_t pad[7];
	uint32_t full_sequence;
} wvx_generic_event;

typedef struct
{
	uint8_t response_type;
	uint8_t error_code;
	uint16_t sequence;
	uint32_t resource_id;
	uint16_t minor_code;
	uint8_t major_code;
	uint8_t pad0;
	uint32_t pad[5];
	uint32_t full_sequence;
} wvx_generic_error;

typedef struct
{
	uint8_t response_type;
	uint8_t pad0;
	uint16_t sequence;
	uint32_t length;
	wvx_atom atom;
} wvx_intern_atom_reply;

typedef struct
{
	uint8_t response_type;
	uint8_t format;
	uint16_t sequence;
	uint32_t length;
	wvx_atom type;
	uint32_t bytes_after;
	uint32_t value_len;
	uint8_t pad0[12];
} wvx_get_property_reply;

typedef struct
{
	uint8_t response_type;
	uint8_t backing_store;
	uint16_t sequence;
	uint32_t length;
	uint32_t visual;
	uint16_t klass;
	uint8_t bit_gravity;
	uint8_t win_gravity;
	uint32_t backing_planes;
	uint32_t backing_pixel;
	uint8_t save_under;
	uint8_t map_is_installed;
	uint8_t map_state;
	uint8_t override_redirect;
	uint32_t colormap;
	uint32_t all_event_masks;
	uint32_t your_event_mask;
	uint16_t do_not_propagate_mask;
	uint8_t pad0[2];
} wvx_get_window_attributes_reply;

typedef struct
{
	uint8_t response_type;
	uint8_t depth;
	uint16_t sequence;
	uint32_t length;
	wvx_window root;
	int16_t x;
	int16_t y;
	uint16_t width;
	uint16_t height;
	uint16_t border_width;
	uint8_t pad0[2];
} wvx_get_geometry_reply;

typedef struct
{
	uint8_t response_type;
	uint8_t same_screen;
	uint16_t sequence;
	uint32_t length;
	wvx_window child;
	int16_t dst_x;
	int16_t dst_y;
} wvx_translate_coordinates_reply;

typedef struct
{
	uint8_t response_type;
	uint8_t pad0;
	uint16_t sequence;
	uint32_t length;
	wvx_window root;
	wvx_window parent;
	uint16_t children_len;
	uint8_t pad1[14];
} wvx_query_tree_reply;

typedef struct
{
	uint8_t response_type;
	uint8_t pad0;
	uint16_t sequence;
	wvx_window window;
	uint8_t state;
	uint8_t pad1[3];
} wvx_visibility_notify_event;

typedef struct
{
	uint8_t response_type;
	uint8_t pad0;
	uint16_t sequence;
	wvx_window event;
	wvx_window window;
	uint8_t from_configure;
	uint8_t pad1[3];
} wvx_unmap_notify_event;

#define WVX_MAP_STATE_VIEWABLE 2
#define WVX_CW_EVENT_MASK 2048u
#define WVX_EVENT_MASK_VISIBILITY_CHANGE 65536u
#define WVX_EVENT_MASK_STRUCTURE_NOTIFY 131072u
#define WVX_EVENT_MASK_PROPERTY_CHANGE 4194304u
#define WVX_VISIBILITY_NOTIFY 15
#define WVX_DESTROY_NOTIFY 17
#define WVX_UNMAP_NOTIFY 18
#define WVX_REPARENT_NOTIFY 21
#define WVX_VISIBILITY_FULLY_OBSCURED 2
#define WVX_ATOM_ANY 0u
#define WVX_ATOM_CARDINAL 6u
#define WVX_ATOM_STRING 31u
#define WVX_ATOM_WINDOW 33u
#define WVX_ICCCM_ICONIC 3u
#define WVX_ALL_DESKTOPS 0xFFFFFFFFu

struct wvx_fns
{
	void *lib;
	wvx_conn *(*connect)(const char *, int *);
	void (*disconnect)(wvx_conn *);
	int (*connection_has_error)(wvx_conn *);
	int (*get_file_descriptor)(wvx_conn *);
	int (*flush)(wvx_conn *);
	wvx_generic_event *(*poll_for_event)(wvx_conn *);
	wvx_cookie (*intern_atom)(wvx_conn *, uint8_t, uint16_t, const char *);
	wvx_intern_atom_reply *(*intern_atom_reply)(wvx_conn *, wvx_cookie, wvx_generic_error **);
	wvx_cookie (*get_property)(wvx_conn *, uint8_t, wvx_window, wvx_atom, wvx_atom, uint32_t, uint32_t);
	wvx_get_property_reply *(*get_property_reply)(wvx_conn *, wvx_cookie, wvx_generic_error **);
	void *(*get_property_value)(const wvx_get_property_reply *);
	int (*get_property_value_length)(const wvx_get_property_reply *);
	wvx_cookie (*get_window_attributes)(wvx_conn *, wvx_window);
	wvx_get_window_attributes_reply *(*get_window_attributes_reply)(wvx_conn *, wvx_cookie, wvx_generic_error **);
	wvx_cookie (*get_geometry)(wvx_conn *, uint32_t);
	wvx_get_geometry_reply *(*get_geometry_reply)(wvx_conn *, wvx_cookie, wvx_generic_error **);
	wvx_cookie (*translate_coordinates)(wvx_conn *, wvx_window, wvx_window, int16_t, int16_t);
	wvx_translate_coordinates_reply *(*translate_coordinates_reply)(wvx_conn *, wvx_cookie, wvx_generic_error **);
	wvx_cookie (*query_tree)(wvx_conn *, wvx_window);
	wvx_query_tree_reply *(*query_tree_reply)(wvx_conn *, wvx_cookie, wvx_generic_error **);
	wvx_window *(*query_tree_children)(const wvx_query_tree_reply *);
	wvx_cookie (*change_window_attributes)(wvx_conn *, wvx_window, uint32_t, const void *);
};

static bool
wvx_fns_load(struct wvx_fns *f)
{
	memset(f, 0, sizeof(*f));
	// Versioned soname: the unversioned .so is in the -dev package.
	f->lib = dlopen("libxcb.so.1", RTLD_LAZY | RTLD_LOCAL);
	if (f->lib == NULL) {
		return false;
	}
#define WVX_LOAD(member, name)                                                                                         \
	do {                                                                                                           \
		*(void **)(&f->member) = dlsym(f->lib, name);                                                          \
		if (f->member == NULL) {                                                                               \
			goto fail;                                                                                     \
		}                                                                                                      \
	} while (0)
	WVX_LOAD(connect, "xcb_connect");
	WVX_LOAD(disconnect, "xcb_disconnect");
	WVX_LOAD(connection_has_error, "xcb_connection_has_error");
	WVX_LOAD(get_file_descriptor, "xcb_get_file_descriptor");
	WVX_LOAD(flush, "xcb_flush");
	WVX_LOAD(poll_for_event, "xcb_poll_for_event");
	WVX_LOAD(intern_atom, "xcb_intern_atom");
	WVX_LOAD(intern_atom_reply, "xcb_intern_atom_reply");
	WVX_LOAD(get_property, "xcb_get_property");
	WVX_LOAD(get_property_reply, "xcb_get_property_reply");
	WVX_LOAD(get_property_value, "xcb_get_property_value");
	WVX_LOAD(get_property_value_length, "xcb_get_property_value_length");
	WVX_LOAD(get_window_attributes, "xcb_get_window_attributes");
	WVX_LOAD(get_window_attributes_reply, "xcb_get_window_attributes_reply");
	WVX_LOAD(get_geometry, "xcb_get_geometry");
	WVX_LOAD(get_geometry_reply, "xcb_get_geometry_reply");
	WVX_LOAD(translate_coordinates, "xcb_translate_coordinates");
	WVX_LOAD(translate_coordinates_reply, "xcb_translate_coordinates_reply");
	WVX_LOAD(query_tree, "xcb_query_tree");
	WVX_LOAD(query_tree_reply, "xcb_query_tree_reply");
	WVX_LOAD(query_tree_children, "xcb_query_tree_children");
	WVX_LOAD(change_window_attributes, "xcb_change_window_attributes");
#undef WVX_LOAD
	return true;
fail:
	dlclose(f->lib);
	memset(f, 0, sizeof(*f));
	return false;
}

/*
 *
 * The backend.
 *
 */

enum wvx_atom_id
{
	A_WM_STATE,
	A_WM_CLASS,
	A_NET_CLIENT_LIST_STACKING,
	A_NET_CURRENT_DESKTOP,
	A_NET_WM_DESKTOP,
	A_NET_WM_STATE,
	A_NET_WM_STATE_HIDDEN,
	A_NET_WM_PID,
	A_NET_WM_WINDOW_OPACITY,
	A_NET_WM_WINDOW_TYPE,
	A_NET_FRAME_EXTENTS,
	A_GTK_FRAME_EXTENTS,
	// Window types that float over windows without hiding them.
	A_TYPE_DOCK,
	A_TYPE_DESKTOP,
	A_TYPE_NOTIFICATION,
	A_TYPE_TOOLTIP,
	A_TYPE_POPUP_MENU,
	A_TYPE_DROPDOWN_MENU,
	A_TYPE_COMBO,
	A_TYPE_DND,
	A_COUNT,
};

static const char *const k_atom_names[A_COUNT] = {
    "WM_STATE",
    "WM_CLASS",
    "_NET_CLIENT_LIST_STACKING",
    "_NET_CURRENT_DESKTOP",
    "_NET_WM_DESKTOP",
    "_NET_WM_STATE",
    "_NET_WM_STATE_HIDDEN",
    "_NET_WM_PID",
    "_NET_WM_WINDOW_OPACITY",
    "_NET_WM_WINDOW_TYPE",
    "_NET_FRAME_EXTENTS",
    "_GTK_FRAME_EXTENTS",
    "_NET_WM_WINDOW_TYPE_DOCK",
    "_NET_WM_WINDOW_TYPE_DESKTOP",
    "_NET_WM_WINDOW_TYPE_NOTIFICATION",
    "_NET_WM_WINDOW_TYPE_TOOLTIP",
    "_NET_WM_WINDOW_TYPE_POPUP_MENU",
    "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
    "_NET_WM_WINDOW_TYPE_COMBO",
    "_NET_WM_WINDOW_TYPE_DND",
};

//! Most clients above the target examined per check (bounds a pathological desktop).
#define WVX_MAX_ABOVE 256
//! Most windows in the stacking list read.
#define WVX_MAX_STACK 4096
//! How long a resolved 3D-screen rect is trusted before RandR is read again.
#define WVX_PANEL_TTL_NS (2ull * 1000ull * 1000ull * 1000ull)
//! How long a resolved top-level is trusted without a reparent event.
#define WVX_TOPLEVEL_TTL_NS (2ull * 1000ull * 1000ull * 1000ull)

//! The per-occluder requests, sent together and collected together.
enum wvx_occ_req
{
	R_ATTR,
	R_GEOM,
	R_XLATE,
	R_STATE,
	R_DESKTOP,
	R_TYPE,
	R_PID,
	R_OPACITY,
	R_FRAME,
	R_GTK_FRAME,
	R_CLASS,
	R_OCC_COUNT,
};

struct u_wv_backend
{
	struct wvx_fns f;
	wvx_conn *conn;
	wvx_atom atoms[A_COUNT];
	char extra_overlays[512];
	uint32_t self_pid;

	// Current target (tracker thread only).
	wvx_window target;
	wvx_window root;
	//! The root whose property changes this connection listens to.
	wvx_window root_selected;
	//! The managed top-level (has WM_STATE), or the root's child ancestor.
	wvx_window toplevel;
	uint64_t toplevel_ns;
	bool toplevel_dirty;
	uint32_t target_pid;
	//! Last VisibilityNotify state of the target (-1 = none seen since mapped).
	int visibility;

	// 3D-screen rect cache.
	char panel_name[128];
	struct u_wv_rect panel_rect;
	bool panel_known;
	uint64_t panel_ns;

	// Per-check scratch.
	wvx_cookie occ_cookies[WVX_MAX_ABOVE][R_OCC_COUNT];
	wvx_window occ_windows[WVX_MAX_ABOVE];
	struct u_wv_rect occ[WVX_MAX_ABOVE];
};

static void
wvx_free(void *p)
{
	free(p);
}

static void
wvx_discard_error(wvx_generic_error *e)
{
	free(e);
}

//! Collect a property reply; NULL on error / absence. Caller frees.
static wvx_get_property_reply *
wvx_prop_reply(struct u_wv_backend *b, wvx_cookie c)
{
	wvx_generic_error *e = NULL;
	wvx_get_property_reply *r = b->f.get_property_reply(b->conn, c, &e);
	wvx_discard_error(e);
	if (r != NULL && r->type == 0) {
		free(r);
		return NULL;
	}
	return r;
}

//! The @p i-th 32-bit item of a format-32 property, false when out of range.
static bool
wvx_prop_u32(struct u_wv_backend *b, const wvx_get_property_reply *r, uint32_t i, uint32_t *out)
{
	if (r == NULL || r->format != 32) {
		return false;
	}
	const int bytes = b->f.get_property_value_length(r);
	if (bytes < 0 || (uint64_t)(i + 1) * 4u > (uint64_t)bytes) {
		return false;
	}
	const uint32_t *v = (const uint32_t *)b->f.get_property_value(r);
	*out = v[i];
	return true;
}

static uint32_t
wvx_prop_count32(struct u_wv_backend *b, const wvx_get_property_reply *r)
{
	if (r == NULL || r->format != 32) {
		return 0;
	}
	const int bytes = b->f.get_property_value_length(r);
	return bytes > 0 ? (uint32_t)bytes / 4u : 0;
}

static bool
wvx_prop_has_atom(struct u_wv_backend *b, const wvx_get_property_reply *r, wvx_atom a)
{
	if (a == 0) {
		return false;
	}
	const uint32_t n = wvx_prop_count32(b, r);
	const uint32_t *v = n > 0 ? (const uint32_t *)b->f.get_property_value(r) : NULL;
	for (uint32_t i = 0; i < n; i++) {
		if (v[i] == a) {
			return true;
		}
	}
	return false;
}

static wvx_cookie
wvx_get_prop(struct u_wv_backend *b, wvx_window w, enum wvx_atom_id a, wvx_atom type, uint32_t len32)
{
	return b->f.get_property(b->conn, 0, w, b->atoms[a], type, 0, len32);
}

//! Select the events that should wake the tracker on @p w (0 mask = none).
static void
wvx_select(struct u_wv_backend *b, wvx_window w, uint32_t mask)
{
	if (w == 0) {
		return;
	}
	// Unchecked: a BadWindow comes back as a queued error, which drain frees.
	(void)b->f.change_window_attributes(b->conn, w, WVX_CW_EVENT_MASK, &mask);
}

/*!
 * Walk up from the target: the root, and the top-level — the highest ancestor
 * (or the target) carrying WM_STATE, i.e. the client the window manager
 * manages; failing that, the root's child that contains the target.
 */
static bool
wvx_resolve_toplevel(struct u_wv_backend *b)
{
	wvx_window w = b->target;
	wvx_window managed = 0;
	wvx_window root_child = w;
	for (int depth = 0; depth < 32; depth++) {
		wvx_cookie qt = b->f.query_tree(b->conn, w);
		wvx_cookie ws = wvx_get_prop(b, w, A_WM_STATE, WVX_ATOM_ANY, 2);
		wvx_generic_error *e = NULL;
		wvx_query_tree_reply *t = b->f.query_tree_reply(b->conn, qt, &e);
		wvx_discard_error(e);
		wvx_get_property_reply *s = wvx_prop_reply(b, ws);
		if (s != NULL) {
			managed = w;
			free(s);
		}
		if (t == NULL) {
			return false; // the window is gone
		}
		b->root = t->root;
		const wvx_window parent = t->parent;
		free(t);
		if (parent == 0 || parent == b->root) {
			root_child = w;
			break;
		}
		w = parent;
	}
	const wvx_window top = managed != 0 ? managed : root_child;
	if (top != b->toplevel) {
		if (b->toplevel != 0 && b->toplevel != b->target) {
			wvx_select(b, b->toplevel, 0);
		}
		b->toplevel = top;
		b->target_pid = 0;
		if (top != b->target) {
			wvx_select(b, top, WVX_EVENT_MASK_STRUCTURE_NOTIFY | WVX_EVENT_MASK_PROPERTY_CHANGE);
		}
	}
	if (b->root != 0 && b->root != b->root_selected) {
		// Stacking order and the current desktop live on the root.
		wvx_select(b, b->root, WVX_EVENT_MASK_PROPERTY_CHANGE);
		b->root_selected = b->root;
	}
	b->toplevel_ns = os_monotonic_get_ns();
	b->toplevel_dirty = false;
	return true;
}

//! The 3D screen's rect in root coordinates, by RandR monitor name. False when unknown.
static bool
wvx_panel_rect(struct u_wv_backend *b, const char *panel, struct u_wv_rect *out)
{
	if (panel == NULL || panel[0] == '\0') {
		return false;
	}
	const uint64_t now = os_monotonic_get_ns();
	if (strcmp(panel, b->panel_name) != 0 || now - b->panel_ns > WVX_PANEL_TTL_NS) {
		(void)snprintf(b->panel_name, sizeof(b->panel_name), "%s", panel);
		b->panel_ns = now;
		b->panel_known = false;
		struct os_display_desktop_info mons[16];
		const uint32_t n = os_display_desktop_enumerate(mons, 16);
		for (uint32_t i = 0; i < n; i++) {
			if (strcmp(mons[i].device_name, panel) == 0) {
				b->panel_rect.left = mons[i].left;
				b->panel_rect.top = mons[i].top;
				b->panel_rect.right = mons[i].left + (int32_t)mons[i].width;
				b->panel_rect.bottom = mons[i].top + (int32_t)mons[i].height;
				b->panel_known = true;
				break;
			}
		}
	}
	if (b->panel_known) {
		*out = b->panel_rect;
	}
	return b->panel_known;
}

//! Process base name from /proc (empty when unknown).
static void
wvx_process_name(uint32_t pid, char *out, size_t out_size)
{
	out[0] = '\0';
	if (pid == 0) {
		return;
	}
	char path[64];
	(void)snprintf(path, sizeof(path), "/proc/%u/comm", pid);
	FILE *fp = fopen(path, "r");
	if (fp == NULL) {
		return;
	}
	if (fgets(out, (int)out_size, fp) == NULL) {
		out[0] = '\0';
	}
	fclose(fp);
	size_t len = strlen(out);
	while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
		out[--len] = '\0';
	}
}

/*!
 * Gather and classify every client above the top-level; true when they cover
 * @p t completely. @p out_any says whether any occluder touched it at all.
 */
static bool
wvx_covered(struct u_wv_backend *b,
            const struct u_wv_rect *t,
            const uint32_t *stack,
            uint32_t stack_count,
            uint32_t self_index,
            uint32_t current_desktop,
            bool have_desktop,
            bool *out_any)
{
	*out_any = false;

	// Send everything for every window above, then collect: one effective
	// round trip however many windows there are.
	uint32_t n = 0;
	for (uint32_t i = self_index + 1; i < stack_count && n < WVX_MAX_ABOVE; i++) {
		const wvx_window w = stack[i];
		wvx_cookie *c = b->occ_cookies[n];
		b->occ_windows[n] = w;
		c[R_ATTR] = b->f.get_window_attributes(b->conn, w);
		c[R_GEOM] = b->f.get_geometry(b->conn, w);
		c[R_XLATE] = b->f.translate_coordinates(b->conn, w, b->root, 0, 0);
		c[R_STATE] = wvx_get_prop(b, w, A_NET_WM_STATE, WVX_ATOM_ANY, 64);
		c[R_DESKTOP] = wvx_get_prop(b, w, A_NET_WM_DESKTOP, WVX_ATOM_CARDINAL, 1);
		c[R_TYPE] = wvx_get_prop(b, w, A_NET_WM_WINDOW_TYPE, WVX_ATOM_ANY, 16);
		c[R_PID] = wvx_get_prop(b, w, A_NET_WM_PID, WVX_ATOM_CARDINAL, 1);
		c[R_OPACITY] = wvx_get_prop(b, w, A_NET_WM_WINDOW_OPACITY, WVX_ATOM_CARDINAL, 1);
		c[R_FRAME] = wvx_get_prop(b, w, A_NET_FRAME_EXTENTS, WVX_ATOM_CARDINAL, 4);
		c[R_GTK_FRAME] = wvx_get_prop(b, w, A_GTK_FRAME_EXTENTS, WVX_ATOM_CARDINAL, 4);
		c[R_CLASS] = wvx_get_prop(b, w, A_WM_CLASS, WVX_ATOM_STRING, 64);
		n++;
	}
	(void)b->f.flush(b->conn);

	size_t occ_n = 0;
	for (uint32_t k = 0; k < n; k++) {
		wvx_cookie *c = b->occ_cookies[k];
		wvx_generic_error *e = NULL;
		wvx_get_window_attributes_reply *attr = b->f.get_window_attributes_reply(b->conn, c[R_ATTR], &e);
		wvx_discard_error(e);
		e = NULL;
		wvx_get_geometry_reply *geom = b->f.get_geometry_reply(b->conn, c[R_GEOM], &e);
		wvx_discard_error(e);
		e = NULL;
		wvx_translate_coordinates_reply *xl = b->f.translate_coordinates_reply(b->conn, c[R_XLATE], &e);
		wvx_discard_error(e);
		wvx_get_property_reply *state = wvx_prop_reply(b, c[R_STATE]);
		wvx_get_property_reply *desk = wvx_prop_reply(b, c[R_DESKTOP]);
		wvx_get_property_reply *type = wvx_prop_reply(b, c[R_TYPE]);
		wvx_get_property_reply *pidr = wvx_prop_reply(b, c[R_PID]);
		wvx_get_property_reply *opac = wvx_prop_reply(b, c[R_OPACITY]);
		wvx_get_property_reply *frame = wvx_prop_reply(b, c[R_FRAME]);
		wvx_get_property_reply *gtk = wvx_prop_reply(b, c[R_GTK_FRAME]);
		wvx_get_property_reply *cls = wvx_prop_reply(b, c[R_CLASS]);

		bool occludes = attr != NULL && geom != NULL && xl != NULL;
		struct u_wv_rect r = {0, 0, 0, 0};
		if (occludes) {
			r.left = xl->dst_x;
			r.top = xl->dst_y;
			r.right = r.left + geom->width;
			r.bottom = r.top + geom->height;
			uint32_t ext[4]; // left, right, top, bottom
			if (wvx_prop_u32(b, frame, 0, &ext[0]) && wvx_prop_u32(b, frame, 1, &ext[1]) &&
			    wvx_prop_u32(b, frame, 2, &ext[2]) && wvx_prop_u32(b, frame, 3, &ext[3])) {
				// Server-side decorations are opaque.
				r.left -= (int32_t)ext[0];
				r.right += (int32_t)ext[1];
				r.top -= (int32_t)ext[2];
				r.bottom += (int32_t)ext[3];
			}
			if (wvx_prop_u32(b, gtk, 0, &ext[0]) && wvx_prop_u32(b, gtk, 1, &ext[1]) &&
			    wvx_prop_u32(b, gtk, 2, &ext[2]) && wvx_prop_u32(b, gtk, 3, &ext[3])) {
				// Client-side shadow margins are not.
				r.left += (int32_t)ext[0];
				r.right -= (int32_t)ext[1];
				r.top += (int32_t)ext[2];
				r.bottom -= (int32_t)ext[3];
			}
		}
		const struct u_wv_rect x = u_wv_rect_intersect(&r, t);
		if (occludes && u_wv_rect_empty(&x)) {
			occludes = false;
		}

		bool overlay_type = false;
		if (occludes) {
			for (int a = A_TYPE_DOCK; a <= A_TYPE_DND; a++) {
				if (wvx_prop_has_atom(b, type, b->atoms[a])) {
					overlay_type = true;
					break;
				}
			}
		}

		if (occludes && !overlay_type) {
			uint32_t pid = 0;
			(void)wvx_prop_u32(b, pidr, 0, &pid);
			uint32_t desktop = WVX_ALL_DESKTOPS;
			const bool has_desk = wvx_prop_u32(b, desk, 0, &desktop);
			uint32_t opacity = 0xFFFFFFFFu;
			const bool has_opacity = wvx_prop_u32(b, opac, 0, &opacity);

			// WM_CLASS is "instance\0class\0": the class is the second string.
			char class_name[128];
			class_name[0] = '\0';
			if (cls != NULL && cls->format == 8) {
				const int len = b->f.get_property_value_length(cls);
				const char *v = (const char *)b->f.get_property_value(cls);
				const char *nul = len > 0 ? memchr(v, '\0', (size_t)len) : NULL;
				if (nul != NULL && nul + 1 < v + len) {
					const size_t cl = strnlen(nul + 1, (size_t)(v + len - (nul + 1)));
					const size_t copy = cl < sizeof(class_name) - 1 ? cl : sizeof(class_name) - 1;
					memcpy(class_name, nul + 1, copy);
					class_name[copy] = '\0';
				}
			}

			struct u_wv_window_facts facts;
			memset(&facts, 0, sizeof(facts));
			facts.class_name = class_name;
			facts.process_name = "";
			facts.visible = attr->map_state == WVX_MAP_STATE_VIEWABLE &&
			                !wvx_prop_has_atom(b, state, b->atoms[A_NET_WM_STATE_HIDDEN]);
			// Another virtual desktop is X's "cloaked".
			facts.cloaked =
			    have_desktop && has_desk && desktop != WVX_ALL_DESKTOPS && desktop != current_desktop;
			facts.same_process = pid != 0 && (pid == b->target_pid || pid == b->self_pid);
			facts.layered_not_opaque = has_opacity && opacity != 0xFFFFFFFFu;
			if (u_wv_classify_occluder(&facts, NULL) == U_WV_OCCLUDES) {
				char proc[64];
				wvx_process_name(pid, proc, sizeof(proc));
				facts.process_name = proc;
				occludes = u_wv_classify_occluder(&facts, b->extra_overlays) == U_WV_OCCLUDES;
			} else {
				occludes = false;
			}
		} else {
			occludes = false;
		}

		if (occludes) {
			b->occ[occ_n++] = x;
			*out_any = true;
		}

		free(attr);
		free(geom);
		free(xl);
		free(state);
		free(desk);
		free(type);
		free(pidr);
		free(opac);
		free(frame);
		free(gtk);
		free(cls);
	}

	return u_wv_rect_covered(t, b->occ, occ_n);
}

/*
 *
 * Backend interface.
 *
 */

struct u_wv_backend *
u_wv_backend_create(void)
{
	struct u_wv_backend *b = (struct u_wv_backend *)calloc(1, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	if (!wvx_fns_load(&b->f)) {
		free(b);
		return NULL;
	}
	// NULL honours $DISPLAY: the server the app's window lives on.
	b->conn = b->f.connect(NULL, NULL);
	if (b->conn == NULL || b->f.connection_has_error(b->conn) != 0) {
		if (b->conn != NULL) {
			b->f.disconnect(b->conn);
		}
		dlclose(b->f.lib);
		free(b);
		return NULL;
	}

	wvx_cookie cookies[A_COUNT];
	for (int i = 0; i < A_COUNT; i++) {
		cookies[i] = b->f.intern_atom(b->conn, 0, (uint16_t)strlen(k_atom_names[i]), k_atom_names[i]);
	}
	for (int i = 0; i < A_COUNT; i++) {
		wvx_generic_error *e = NULL;
		wvx_intern_atom_reply *r = b->f.intern_atom_reply(b->conn, cookies[i], &e);
		wvx_discard_error(e);
		b->atoms[i] = r != NULL ? r->atom : 0;
		free(r);
	}

	debug_get_option(b->extra_overlays, sizeof(b->extra_overlays), U_WINDOW_VISIBILITY_ENV_OVERLAYS, "");
	b->self_pid = (uint32_t)getpid();
	b->visibility = -1;
	return b;
}

void
u_wv_backend_set_target(struct u_wv_backend *b, void *native_window)
{
	const wvx_window w = (wvx_window)(uintptr_t)native_window;
	if (b->target != 0) {
		wvx_select(b, b->target, 0);
	}
	if (b->toplevel != 0 && b->toplevel != b->target) {
		wvx_select(b, b->toplevel, 0);
	}
	b->target = w;
	b->toplevel = 0;
	b->toplevel_dirty = true;
	b->target_pid = 0;
	b->visibility = -1;
	if (w != 0) {
		wvx_select(b, w,
		           WVX_EVENT_MASK_VISIBILITY_CHANGE | WVX_EVENT_MASK_STRUCTURE_NOTIFY |
		               WVX_EVENT_MASK_PROPERTY_CHANGE);
		(void)wvx_resolve_toplevel(b);
	}
	(void)b->f.flush(b->conn);
}

bool
u_wv_backend_check(struct u_wv_backend *b, const char *panel, const char **out_reason)
{
	*out_reason = "uncovered";
	if (b->target == 0) {
		return false;
	}
	if (b->f.connection_has_error(b->conn) != 0) {
		*out_reason = "X connection lost";
		return false;
	}
	const uint64_t now = os_monotonic_get_ns();
	if (b->toplevel == 0 || b->toplevel_dirty || now - b->toplevel_ns > WVX_TOPLEVEL_TTL_NS) {
		if (!wvx_resolve_toplevel(b)) {
			*out_reason = "window gone";
			return false;
		}
	}

	const wvx_window top = b->toplevel;
	wvx_cookie c_attr = b->f.get_window_attributes(b->conn, b->target);
	wvx_cookie c_geom = b->f.get_geometry(b->conn, b->target);
	wvx_cookie c_xl = b->f.translate_coordinates(b->conn, b->target, b->root, 0, 0);
	wvx_cookie c_wms = wvx_get_prop(b, top, A_WM_STATE, WVX_ATOM_ANY, 2);
	wvx_cookie c_state = wvx_get_prop(b, top, A_NET_WM_STATE, WVX_ATOM_ANY, 64);
	wvx_cookie c_desk = wvx_get_prop(b, top, A_NET_WM_DESKTOP, WVX_ATOM_CARDINAL, 1);
	wvx_cookie c_pid = wvx_get_prop(b, top, A_NET_WM_PID, WVX_ATOM_CARDINAL, 1);
	wvx_cookie c_cur = wvx_get_prop(b, b->root, A_NET_CURRENT_DESKTOP, WVX_ATOM_CARDINAL, 1);
	wvx_cookie c_stack = wvx_get_prop(b, b->root, A_NET_CLIENT_LIST_STACKING, WVX_ATOM_WINDOW, WVX_MAX_STACK);
	(void)b->f.flush(b->conn);

	wvx_generic_error *e = NULL;
	wvx_get_window_attributes_reply *attr = b->f.get_window_attributes_reply(b->conn, c_attr, &e);
	wvx_discard_error(e);
	e = NULL;
	wvx_get_geometry_reply *geom = b->f.get_geometry_reply(b->conn, c_geom, &e);
	wvx_discard_error(e);
	e = NULL;
	wvx_translate_coordinates_reply *xl = b->f.translate_coordinates_reply(b->conn, c_xl, &e);
	wvx_discard_error(e);
	wvx_get_property_reply *wms = wvx_prop_reply(b, c_wms);
	wvx_get_property_reply *state = wvx_prop_reply(b, c_state);
	wvx_get_property_reply *desk = wvx_prop_reply(b, c_desk);
	wvx_get_property_reply *pidr = wvx_prop_reply(b, c_pid);
	wvx_get_property_reply *cur = wvx_prop_reply(b, c_cur);
	wvx_get_property_reply *stack = wvx_prop_reply(b, c_stack);

	bool hidden = false;
	uint32_t v = 0;
	if (attr == NULL || geom == NULL || xl == NULL) {
		*out_reason = "window gone";
		b->toplevel_dirty = true;
	} else if ((wvx_prop_u32(b, wms, 0, &v) && v == WVX_ICCCM_ICONIC) ||
	           wvx_prop_has_atom(b, state, b->atoms[A_NET_WM_STATE_HIDDEN])) {
		*out_reason = "minimized";
		hidden = true;
	} else if (attr->map_state != WVX_MAP_STATE_VIEWABLE) {
		*out_reason = "not visible";
		hidden = true;
	} else {
		uint32_t desktop = WVX_ALL_DESKTOPS;
		uint32_t current = 0;
		const bool have_current = wvx_prop_u32(b, cur, 0, &current);
		if (have_current && wvx_prop_u32(b, desk, 0, &desktop) && desktop != WVX_ALL_DESKTOPS &&
		    desktop != current) {
			*out_reason = "on another desktop";
			hidden = true;
		} else if (b->visibility == WVX_VISIBILITY_FULLY_OBSCURED) {
			*out_reason = "fully obscured";
			hidden = true;
		} else {
			struct u_wv_rect t = {xl->dst_x, xl->dst_y, xl->dst_x + geom->width, xl->dst_y + geom->height};
			struct u_wv_rect panel_r;
			if (wvx_panel_rect(b, panel, &panel_r)) {
				t = u_wv_rect_intersect(&t, &panel_r);
			}
			if (u_wv_rect_empty(&t)) {
				// Nothing on the 3D screen: the multi-screen coverage gate owns that case.
				*out_reason = "off the 3D screen";
			} else {
				if (b->target_pid == 0) {
					(void)wvx_prop_u32(b, pidr, 0, &b->target_pid);
				}
				const uint32_t n = wvx_prop_count32(b, stack);
				const uint32_t *list = n > 0 ? (const uint32_t *)b->f.get_property_value(stack) : NULL;
				uint32_t self_index = n;
				for (uint32_t i = 0; i < n; i++) {
					if (list[i] == top) {
						self_index = i;
						break;
					}
				}
				if (self_index == n) {
					// Not a managed client in the stacking list (no EWMH
					// window manager, override-redirect): no cover check.
					*out_reason = "uncovered (no stacking order)";
				} else {
					bool any = false;
					if (wvx_covered(b, &t, list, n, self_index, current, have_current, &any)) {
						*out_reason = "fully covered";
						hidden = true;
					} else {
						*out_reason = any ? "partly covered" : "uncovered";
					}
				}
			}
		}
	}

	free(attr);
	free(geom);
	free(xl);
	free(wms);
	free(state);
	free(desk);
	free(pidr);
	free(cur);
	free(stack);
	return hidden;
}

int
u_wv_backend_fd(struct u_wv_backend *b)
{
	return b->f.get_file_descriptor(b->conn);
}

void
u_wv_backend_drain(struct u_wv_backend *b)
{
	wvx_generic_event *ev;
	while ((ev = b->f.poll_for_event(b->conn)) != NULL) {
		const uint8_t type = ev->response_type & 0x7f;
		if (type == WVX_VISIBILITY_NOTIFY) {
			const wvx_visibility_notify_event *vn = (const wvx_visibility_notify_event *)ev;
			if (vn->window == b->target) {
				b->visibility = vn->state;
			}
		} else if (type == WVX_UNMAP_NOTIFY || type == WVX_DESTROY_NOTIFY) {
			const wvx_unmap_notify_event *un = (const wvx_unmap_notify_event *)ev;
			if (un->window == b->target) {
				// Nothing is known about the next mapping's visibility.
				b->visibility = -1;
			}
			b->toplevel_dirty = true;
		} else if (type == WVX_REPARENT_NOTIFY) {
			b->toplevel_dirty = true;
		}
		// Everything else (property changes, configures, queued errors for
		// a window that is gone) only means "look again".
		wvx_free(ev);
	}
}

void
u_wv_backend_destroy(struct u_wv_backend *b)
{
	if (b == NULL) {
		return;
	}
	// Disconnecting drops every event selection this connection made.
	if (b->conn != NULL) {
		b->f.disconnect(b->conn);
	}
	if (b->f.lib != NULL) {
		dlclose(b->f.lib);
	}
	free(b);
}
