// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux implementation of the desktop-rect resolver (X11/RandR).
 * @ingroup aux_os
 *
 * Follow-on to #1301's Windows half; see #715 for the panel-origin plumbing
 * this consumes.
 *
 * ## Why dlopen instead of linking
 *
 * `aux_os`'s own build rule is "avoid linking with libraries that bring in DSOs
 * as this library is used everywhere, including libraries loaded in by
 * applications like the OpenXR runtime library" — and it means it: aux_os ends
 * up inside arbitrary host processes. Hard-linking libX11/libXrandr would force
 * that dependency on every consumer, including headless and Wayland-only ones
 * that have no use for it.
 *
 * So the X11 entry points are resolved at runtime and their absence is a normal
 * outcome, not an error: a box with no X libraries, or a session with no
 * DISPLAY, reports "unknown" and the caller publishes zeros. Same shape as the
 * Windows half resolving `SetThreadDpiAwarenessContext` dynamically.
 *
 * ## RandR 1.5 `XRRGetMonitors`, not CRTC walking
 *
 * `XRRGetMonitors` returns exactly this struct's shape in one call — rect,
 * primary flag, and a name atom per monitor — where the older CRTC/output walk
 * needs three round trips and its own mode bookkeeping. RandR 1.5 is 2015-era
 * and present on every distribution the runtime targets (Ubuntu 22.04 well
 * inside it).
 *
 * ## Wayland
 *
 * Not handled here, and not handleable: a Wayland client is deliberately denied
 * any global coordinate space — it cannot learn or set its own surface's
 * absolute position, by design. Under a Wayland session without XWayland this
 * reports "unknown", which is the honest answer. Under XWayland the X11 path
 * works and reports the XWayland view of the layout.
 */

#include "os_display_desktop.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdint.h>

// Minimal local declarations so this file needs no X11 headers at build time —
// it must compile on boxes with no X development packages installed.
typedef struct _XDisplay os_x_display;
typedef unsigned long os_x_window;
typedef unsigned long os_x_atom;

struct os_xrr_monitor_info
{
	os_x_atom name;
	int primary;
	int automatic;
	int noutput;
	int x;
	int y;
	int width;
	int height;
	int mwidth;
	int mheight;
	void *outputs;
};

struct x11_fns
{
	void *lib_x11;
	void *lib_xrandr;

	os_x_display *(*XOpenDisplay)(const char *);
	int (*XCloseDisplay)(os_x_display *);
	os_x_window (*XDefaultRootWindow)(os_x_display *);
	char *(*XGetAtomName)(os_x_display *, os_x_atom);
	int (*XFree)(void *);

	struct os_xrr_monitor_info *(*XRRGetMonitors)(os_x_display *, os_x_window, int, int *);
	void (*XRRFreeMonitors)(struct os_xrr_monitor_info *);
};

static bool
x11_fns_load(struct x11_fns *f)
{
	memset(f, 0, sizeof(*f));

	// Versioned sonames: the unversioned .so lives in the -dev package, which a
	// runtime box has no reason to have installed.
	f->lib_x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
	if (f->lib_x11 == NULL) {
		return false;
	}
	f->lib_xrandr = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
	if (f->lib_xrandr == NULL) {
		dlclose(f->lib_x11);
		f->lib_x11 = NULL;
		return false;
	}

#define LOAD(handle, member, name)                                                                                     \
	do {                                                                                                           \
		*(void **)(&f->member) = dlsym(f->handle, name);                                                       \
		if (f->member == NULL) {                                                                               \
			goto fail;                                                                                     \
		}                                                                                                      \
	} while (0)

	LOAD(lib_x11, XOpenDisplay, "XOpenDisplay");
	LOAD(lib_x11, XCloseDisplay, "XCloseDisplay");
	LOAD(lib_x11, XDefaultRootWindow, "XDefaultRootWindow");
	LOAD(lib_x11, XGetAtomName, "XGetAtomName");
	LOAD(lib_x11, XFree, "XFree");
	LOAD(lib_xrandr, XRRGetMonitors, "XRRGetMonitors");
	LOAD(lib_xrandr, XRRFreeMonitors, "XRRFreeMonitors");

#undef LOAD

	return true;

fail:
	dlclose(f->lib_xrandr);
	dlclose(f->lib_x11);
	memset(f, 0, sizeof(*f));
	return false;
}

static void
x11_fns_unload(struct x11_fns *f)
{
	if (f->lib_xrandr != NULL) {
		dlclose(f->lib_xrandr);
	}
	if (f->lib_x11 != NULL) {
		dlclose(f->lib_x11);
	}
	memset(f, 0, sizeof(*f));
}

//! Squared distance from (x, y) to the nearest edge of a monitor rect, 0 inside.
static int64_t
rect_distance_sq(const struct os_xrr_monitor_info *m, int32_t x, int32_t y)
{
	int64_t dx = 0;
	int64_t dy = 0;

	if (x < m->x) {
		dx = (int64_t)m->x - x;
	} else if (x >= m->x + m->width) {
		dx = (int64_t)x - (m->x + m->width - 1);
	}
	if (y < m->y) {
		dy = (int64_t)m->y - y;
	} else if (y >= m->y + m->height) {
		dy = (int64_t)y - (m->y + m->height - 1);
	}

	return dx * dx + dy * dy;
}

bool
os_display_desktop_info_at(int32_t x, int32_t y, struct os_display_desktop_info *out_info)
{
	if (out_info == NULL) {
		return false;
	}
	memset(out_info, 0, sizeof(*out_info));

	struct x11_fns f;
	if (!x11_fns_load(&f)) {
		return false;
	}

	bool ok = false;

	// NULL honours $DISPLAY. No display (headless, or pure Wayland with no
	// XWayland) is a normal "unknown", not a failure to report.
	os_x_display *dpy = f.XOpenDisplay(NULL);
	if (dpy != NULL) {
		int count = 0;
		os_x_window root = f.XDefaultRootWindow(dpy);
		// only_active=1: mirrored/disabled outputs are not placeable.
		struct os_xrr_monitor_info *mons = f.XRRGetMonitors(dpy, root, 1, &count);

		if (mons != NULL && count > 0) {
			// Nearest rather than strictly-containing, matching the Windows
			// MONITOR_DEFAULTTONEAREST behaviour: a stale origin or a gap in
			// a ragged arrangement still yields a placeable rect.
			int best = 0;
			int64_t best_d = rect_distance_sq(&mons[0], x, y);
			for (int i = 1; i < count; i++) {
				int64_t d = rect_distance_sq(&mons[i], x, y);
				if (d < best_d) {
					best_d = d;
					best = i;
				}
			}

			const struct os_xrr_monitor_info *m = &mons[best];
			out_info->left = (int32_t)m->x;
			out_info->top = (int32_t)m->y;
			out_info->width = (uint32_t)(m->width > 0 ? m->width : 0);
			out_info->height = (uint32_t)(m->height > 0 ? m->height : 0);
			out_info->is_primary = m->primary != 0;

			// X11 root coordinates are device pixels with no scaling layer, so
			// the plug-in reads the same space we do.
			out_info->width_in_caller_dpi = out_info->width;
			out_info->height_in_caller_dpi = out_info->height;

			// The monitor's name atom is the RandR output name ("HDMI-1",
			// "eDP-1", "DP-2") — stable across sessions for a given physical
			// connector, which is what makes it usable for re-resolution.
			char *name = f.XGetAtomName(dpy, m->name);
			if (name != NULL) {
				(void)snprintf(out_info->device_name, sizeof(out_info->device_name), "%s", name);
				f.XFree(name);
			}

			ok = out_info->width > 0 && out_info->height > 0;
		}

		if (mons != NULL) {
			f.XRRFreeMonitors(mons);
		}
		f.XCloseDisplay(dpy);
	}

	x11_fns_unload(&f);

	if (!ok) {
		memset(out_info, 0, sizeof(*out_info));
	}

	return ok;
}
