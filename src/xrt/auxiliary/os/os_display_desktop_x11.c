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
 * primary flag, EDID physical size, and a name atom per monitor — where the
 * older CRTC/output walk needs three round trips and its own mode bookkeeping.
 * RandR 1.5 is 2015-era and present on every distribution the runtime targets
 * (Ubuntu 22.04 well inside it).
 *
 * ## Both entry points come off ONE walk
 *
 * The point lookup and @ref os_display_desktop_enumerate are two shapes of the
 * same `XRRGetMonitors` result (@ref query_monitors), so they cannot disagree
 * about the layout — which matters, because panel selection consults both.
 *
 * ## An X error on our connection stays on our connection (#2311)
 *
 * Every connection here is private and only queried, so an X error on it just
 * means "that output is gone / unreadable", which every caller already treats
 * as unknown. Left to Xlib, though, it goes to the process-global error
 * handler, and the default one prints "X Error of failed request" and exits
 * the HOST process. Since the in-process display-hotplug sweep (#2305) re-reads
 * RandR around every topology change, an output destroyed between
 * XRRGetMonitors and XRRGetOutputProperty (XWayland destroys a monitor's
 * output with it) killed the app with BadRROutput.
 *
 * So each connection carries its own error hook (@ref x11_open_private): the
 * per-display hook of Xlib's extension interface (XAddExtension + XESetError),
 * which `_XReply` offers every error on that connection before the global
 * handler. Every request here waits for its reply, so every error passes it.
 *
 * Not XSetErrorHandler: that handler is process-wide. Swapped in from the
 * sweep's worker thread it would also swallow the app's own errors raised on
 * its render / event thread meanwhile (a toolkit's error trap then misses
 * them), and a save / restore interleaved with the app or a toolkit doing the
 * same can leave the wrong handler installed for good. The hook is state of a
 * Display only the calling thread uses and dies with XCloseDisplay: no global
 * state, nothing to restore, nothing to race. xcb's checked requests would
 * isolate errors too, at the price of rewriting both readers against two more
 * dlopen'd libraries.
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
#include "os_display_connector_linux.h"
#include "os_display_edid_linux.h"

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

//! XExtCodes; only `extension` is read.
struct os_x_ext_codes
{
	int extension;
	int major_opcode;
	int first_event;
	int first_error;
};

//! An XESetError hook: (display, xError *, codes, ret_code), True suppresses the error.
typedef int (*os_x_error_hook)(os_x_display *, void *, struct os_x_ext_codes *, int *);

struct x11_fns
{
	void *lib_x11;
	void *lib_xrandr;

	os_x_display *(*XOpenDisplay)(const char *);
	int (*XCloseDisplay)(os_x_display *);
	os_x_window (*XDefaultRootWindow)(os_x_display *);
	char *(*XGetAtomName)(os_x_display *, os_x_atom);
	int (*XFree)(void *);
	struct os_x_ext_codes *(*XAddExtension)(os_x_display *);
	os_x_error_hook (*XESetError)(os_x_display *, int, os_x_error_hook);

	struct os_xrr_monitor_info *(*XRRGetMonitors)(os_x_display *, os_x_window, int, int *);
	void (*XRRFreeMonitors)(struct os_xrr_monitor_info *);

	// Optional: only the EDID reader uses these, and a missing one just means
	// "no EDID from the X server".
	os_x_atom (*XInternAtom)(os_x_display *, const char *, int);
	int (*XRRGetOutputProperty)(os_x_display *,
	                            unsigned long,
	                            os_x_atom,
	                            long,
	                            long,
	                            int,
	                            int,
	                            os_x_atom,
	                            os_x_atom *,
	                            int *,
	                            unsigned long *,
	                            unsigned long *,
	                            unsigned char **);
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
	// Required, not optional: no request goes out on a connection without
	// the error hook (#2311). Both are in every libX11 since X11R4.
	LOAD(lib_x11, XAddExtension, "XAddExtension");
	LOAD(lib_x11, XESetError, "XESetError");
	LOAD(lib_xrandr, XRRGetMonitors, "XRRGetMonitors");
	LOAD(lib_xrandr, XRRFreeMonitors, "XRRFreeMonitors");

#undef LOAD

	*(void **)(&f->XInternAtom) = dlsym(f->lib_x11, "XInternAtom");
	*(void **)(&f->XRRGetOutputProperty) = dlsym(f->lib_xrandr, "XRRGetOutputProperty");

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

/*!
 * Every X error on a private connection: suppressed, and the request that
 * raised it returns its failure value (NULL / None / the error code), which
 * the callers already handle as "unknown" (#2311).
 */
static int
x11_private_error_hook(os_x_display *dpy, void *err, struct os_x_ext_codes *codes, int *ret_code)
{
	(void)dpy;
	(void)err;
	(void)codes;
	*ret_code = 0; // what _XReply returns: no reply
	return 1;
}

/*!
 * XOpenDisplay(NULL) with @ref x11_private_error_hook installed on the new
 * connection, before its first request (see "An X error on our connection" at
 * the top). NULL when there is no display, or when the hook cannot be
 * installed: then no request is sent at all.
 */
static os_x_display *
x11_open_private(const struct x11_fns *f)
{
	// NULL honours $DISPLAY.
	os_x_display *dpy = f->XOpenDisplay(NULL);
	if (dpy == NULL) {
		return NULL;
	}
	const struct os_x_ext_codes *codes = f->XAddExtension(dpy);
	if (codes == NULL) {
		f->XCloseDisplay(dpy);
		return NULL;
	}
	(void)f->XESetError(dpy, codes->extension, x11_private_error_hook);
	return dpy;
}

//! Squared distance from (x, y) to the nearest edge of a monitor rect, 0 inside.
static int64_t
rect_distance_sq(const struct os_display_desktop_info *m, int32_t x, int32_t y)
{
	int64_t dx = 0;
	int64_t dy = 0;

	if (x < m->left) {
		dx = (int64_t)m->left - x;
	} else if (x >= m->left + (int64_t)m->width) {
		dx = (int64_t)x - (m->left + (int64_t)m->width - 1);
	}
	if (y < m->top) {
		dy = (int64_t)m->top - y;
	} else if (y >= m->top + (int64_t)m->height) {
		dy = (int64_t)y - (m->top + (int64_t)m->height - 1);
	}

	return dx * dx + dy * dy;
}

/*!
 * The one X11 round trip: open the display, walk RandR's monitor list, fill our
 * own structs.
 *
 * @return the number of monitors written, 0 on any failure (no X libraries, no
 * $DISPLAY, RandR too old, nothing active).
 */
static uint32_t
query_monitors(struct os_display_desktop_info *out_infos, uint32_t max_infos)
{
	if (out_infos == NULL || max_infos == 0) {
		return 0;
	}
	memset(out_infos, 0, sizeof(*out_infos) * max_infos);

	struct x11_fns f;
	if (!x11_fns_load(&f)) {
		return 0;
	}

	uint32_t written = 0;

	// No display (headless, or pure Wayland with no XWayland) is a normal
	// "unknown", not a failure to report.
	os_x_display *dpy = x11_open_private(&f);
	if (dpy != NULL) {
		int count = 0;
		os_x_window root = f.XDefaultRootWindow(dpy);
		// only_active=1: mirrored/disabled outputs are not placeable.
		struct os_xrr_monitor_info *mons = f.XRRGetMonitors(dpy, root, 1, &count);

		if (mons != NULL) {
			for (int i = 0; i < count && written < max_infos; i++) {
				const struct os_xrr_monitor_info *m = &mons[i];
				if (m->width <= 0 || m->height <= 0) {
					continue;
				}

				struct os_display_desktop_info *o = &out_infos[written++];
				o->left = (int32_t)m->x;
				o->top = (int32_t)m->y;
				o->width = (uint32_t)m->width;
				o->height = (uint32_t)m->height;
				o->is_primary = m->primary != 0;

				// The plug-in has no DPI virtualisation to undo here, so
				// it reads the same space we do. NOTE this is NOT device
				// pixels under XWayland at a non-unit scale — the
				// connector's device mode is native_width/height.
				o->width_in_caller_dpi = o->width;
				o->height_in_caller_dpi = o->height;

				// RandR passes the output's EDID physical size straight
				// through. This is the SECOND matching factor: when a
				// plug-in can report neither an identity nor a usable
				// origin, millimetres are the only other property both
				// sides know independently about the same panel.
				o->physical_width_mm = (uint32_t)(m->mwidth > 0 ? m->mwidth : 0);
				o->physical_height_mm = (uint32_t)(m->mheight > 0 ? m->mheight : 0);

				// The monitor's name atom is the RandR output name
				// ("HDMI-1", "eDP-1", "DP-2") — stable across sessions for
				// a given physical connector, which is what makes it
				// usable for re-resolution.
				char *name = f.XGetAtomName(dpy, m->name);
				if (name != NULL) {
					(void)snprintf(o->device_name, sizeof(o->device_name), "%s", name);
					f.XFree(name);
				}
			}

			f.XRRFreeMonitors(mons);
		}

		f.XCloseDisplay(dpy);
	}

	x11_fns_unload(&f);

	// The device mode behind each X11 rect (#1831): under XWayland the rect
	// is in the scaled X screen, and the panel can only be recognised — and
	// a 1:1 presentation only be judged — by what the connector runs.
	os_display_connector_annotate(out_infos, written);

	return written;
}

uint32_t
os_display_x11_read_monitor_edids(struct os_display_randr_edid *out, uint32_t max)
{
	if (out == NULL || max == 0) {
		return 0;
	}
	memset(out, 0, sizeof(*out) * max);

	struct x11_fns f;
	if (!x11_fns_load(&f)) {
		return 0;
	}
	uint32_t written = 0;
	// The output can be destroyed between XRRGetMonitors and its property read
	// (a monitor unplugged mid-sweep): BadRROutput, absorbed by the
	// connection's hook, and that monitor reads as having no EDID (#2311).
	os_x_display *dpy = (f.XInternAtom != NULL && f.XRRGetOutputProperty != NULL) ? x11_open_private(&f) : NULL;
	if (dpy != NULL) {
		// only_if_exists: a server that never published an EDID property
		// has no such atom, and creating one would be a side effect.
		const os_x_atom edid_atom = f.XInternAtom(dpy, "EDID", 1);
		int count = 0;
		struct os_xrr_monitor_info *mons =
		    edid_atom != 0 ? f.XRRGetMonitors(dpy, f.XDefaultRootWindow(dpy), 1, &count) : NULL;
		for (int i = 0; mons != NULL && i < count && written < max; i++) {
			const struct os_xrr_monitor_info *m = &mons[i];
			if (m->width <= 0 || m->height <= 0) {
				continue; // same filter as query_monitors, so names line up
			}
			struct os_display_randr_edid *o = &out[written++];
			char *name = f.XGetAtomName(dpy, m->name);
			if (name != NULL) {
				(void)snprintf(o->name, sizeof(o->name), "%s", name);
				f.XFree(name);
			}
			if (m->noutput < 1 || m->outputs == NULL) {
				continue;
			}
			const unsigned long output = ((const unsigned long *)m->outputs)[0];
			os_x_atom actual_type = 0;
			int actual_format = 0;
			unsigned long nitems = 0, bytes_after = 0;
			unsigned char *prop = NULL;
			// Length is in 32-bit units: 64 = 256 bytes.
			if (f.XRRGetOutputProperty(dpy, output, edid_atom, 0, OS_DISPLAY_RANDR_EDID_MAX / 4, 0, 0,
			                           0 /* AnyPropertyType */, &actual_type, &actual_format, &nitems,
			                           &bytes_after, &prop) == 0 /* Success */
			    && prop != NULL && actual_format == 8 && nitems >= 128) {
				const uint32_t n =
				    nitems > OS_DISPLAY_RANDR_EDID_MAX ? OS_DISPLAY_RANDR_EDID_MAX : (uint32_t)nitems;
				memcpy(o->edid, prop, n);
				o->len = n;
			}
			if (prop != NULL) {
				f.XFree(prop);
			}
		}
		if (mons != NULL) {
			f.XRRFreeMonitors(mons);
		}
		f.XCloseDisplay(dpy);
	}
	x11_fns_unload(&f);
	return written;
}

uint32_t
os_display_desktop_enumerate(struct os_display_desktop_info *out_infos, uint32_t max_infos)
{
	return query_monitors(out_infos, max_infos);
}

bool
os_display_desktop_info_at(int32_t x, int32_t y, struct os_display_desktop_info *out_info)
{
	if (out_info == NULL) {
		return false;
	}
	memset(out_info, 0, sizeof(*out_info));

	struct os_display_desktop_info mons[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	uint32_t count = query_monitors(mons, OS_DISPLAY_DESKTOP_MAX_MONITORS);
	if (count == 0) {
		return false;
	}

	// Nearest rather than strictly-containing, matching the Windows
	// MONITOR_DEFAULTTONEAREST behaviour: a stale origin or a gap in a ragged
	// arrangement still yields a placeable rect.
	uint32_t best = 0;
	int64_t best_d = rect_distance_sq(&mons[0], x, y);
	for (uint32_t i = 1; i < count; i++) {
		int64_t d = rect_distance_sq(&mons[i], x, y);
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}

	*out_info = mons[best];

	return true;
}

bool
os_display_desktop_info_for_handle(void *platform_handle, struct os_display_desktop_info *out_info)
{
	// No monitor handle on this platform: identity is the device name, which
	// os_display_desktop_info_for_device matches against the enumeration.
	(void)platform_handle;
	if (out_info != NULL) {
		memset(out_info, 0, sizeof(*out_info));
	}
	return false;
}
