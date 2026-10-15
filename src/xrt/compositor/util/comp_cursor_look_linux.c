// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-drawn cursor's default look, desktop-Linux half: the
 *         user's cursor theme (XR_DXR_cursor_depth v5, ADR-046 Amendment 2
 *         A.6 / B; #2102).
 * @author Byungju Lee
 * @ingroup comp_util
 *
 * Neither X11 nor Wayland lets one client read the cursor image another one
 * set (a Wayland compositor draws its own; XFixesGetCursorImage needs the X
 * server to be the one drawing it), so this does not follow the live shape the
 * way macOS and Windows do. It loads the shape the desktop shows over an
 * ordinary window - the theme's "left_ptr" - at the size the desktop draws it,
 * with libXcursor's XcursorLibraryLoadImage, which reads the theme files and
 * needs no display connection.
 *
 * Theme and size: XCURSOR_THEME / XCURSOR_SIZE when set (X11 sessions and most
 * desktops export them), else GNOME's org.gnome.desktop.interface cursor-theme /
 * cursor-size (a GNOME Wayland session exports neither variable), else
 * libXcursor's default theme at 24. Read once per process.
 *
 * libXcursor, GIO (GSettings) and libX11 (Xft.dpi) are all dlopen()ed: none of
 * them becomes a dependency of the .deb (package_deb_linux.sh's STABLE_SONAMES),
 * and without them the look stays the classic arrow. A loaded image is cached by
 * backing scale; a scale that failed is not retried every frame. The
 * process-wide state (the libXcursor entry points, the theme, the failed scale,
 * the backing scale) is guarded: two compositors can load the look at once.
 *
 * Backing scale: a compositor that knows the scale of the monitor its window is
 * on passes it (vk_native on native Wayland, through the window-geometry GNOME
 * extension). comp_cursor_look_platform_backing_scale() is the fallback:
 * Xft.dpi / 96 from the X resource database, else 1.
 */

#include "comp_cursor_look.h"

#include "util/u_cursor_depth.h"
#include "util/u_logging.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! Largest theme image we load, per side (accessibility sizes reach 256).
#define CURSOR_LOOK_MAX_PX 256

//! The cursor size GNOME and libXcursor default to, in OS points.
#define CURSOR_LOOK_DEFAULT_SIZE 24

//! dlsym() into a function pointer without ISO C's object-to-function cast (the
//! os_display_*_linux.c pattern).
#define LOOK_DLSYM(lib, fn, name) (*(void **)(&(fn)) = dlsym((lib), (name)))


/*
 *
 * libXcursor (dlopen). The struct mirrors <X11/Xcursor/Xcursor.h>'s
 * XcursorImage, so building this needs no libxcursor-dev.
 *
 */

struct xcursor_image
{
	uint32_t version;
	uint32_t size; //!< nominal size
	uint32_t width;
	uint32_t height;
	uint32_t xhot;
	uint32_t yhot;
	uint32_t delay;
	uint32_t *pixels; //!< ARGB32, premultiplied
};

typedef struct xcursor_image *(*pfn_xcursor_library_load_image)(const char *library, const char *theme, int size);
typedef void (*pfn_xcursor_image_destroy)(struct xcursor_image *image);

//! Guards the libXcursor entry points, the theme, the failed scale and the one-time notice.
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

//! Called under s_lock.
static struct
{
	bool tried;
	pfn_xcursor_library_load_image load_image;
	pfn_xcursor_image_destroy image_destroy;
} s_xcursor;

static bool
xcursor_available(void)
{
	if (!s_xcursor.tried) {
		s_xcursor.tried = true;
		void *lib = dlopen("libXcursor.so.1", RTLD_NOW | RTLD_LOCAL);
		if (lib != NULL) {
			LOOK_DLSYM(lib, s_xcursor.load_image, "XcursorLibraryLoadImage");
			LOOK_DLSYM(lib, s_xcursor.image_destroy, "XcursorImageDestroy");
		}
		if (s_xcursor.load_image == NULL || s_xcursor.image_destroy == NULL) {
			s_xcursor.load_image = NULL;
			U_LOG_W("cursor_draw: libXcursor.so.1 not available - the default look stays the classic arrow");
		}
	}
	return s_xcursor.load_image != NULL;
}


/*
 *
 * Theme and size: the environment, else GSettings (GIO, dlopen).
 *
 */

typedef void *(*pfn_g_settings_schema_source_get_default)(void);
typedef void *(*pfn_g_settings_schema_source_lookup)(void *source, const char *schema_id, int recursive);
typedef int (*pfn_g_settings_schema_has_key)(void *schema, const char *name);
typedef void (*pfn_g_settings_schema_unref)(void *schema);
typedef void *(*pfn_g_settings_new)(const char *schema_id);
typedef char *(*pfn_g_settings_get_string)(void *settings, const char *key);
typedef int (*pfn_g_settings_get_int)(void *settings, const char *key);
typedef void (*pfn_g_object_unref)(void *object);
typedef void (*pfn_g_free)(void *mem);

#define GNOME_INTERFACE_SCHEMA "org.gnome.desktop.interface"

//! GNOME's cursor theme and size, when GSettings has the schema. Never aborts:
//! g_settings_new() does on a missing schema, so the schema is looked up first.
static void
gsettings_cursor(char *theme, size_t theme_size, int *size)
{
	void *gio = dlopen("libgio-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
	if (gio == NULL) {
		return;
	}
	pfn_g_settings_schema_source_get_default source_get_default = NULL;
	pfn_g_settings_schema_source_lookup source_lookup = NULL;
	pfn_g_settings_schema_has_key has_key = NULL;
	pfn_g_settings_schema_unref schema_unref = NULL;
	pfn_g_settings_new settings_new = NULL;
	pfn_g_settings_get_string get_string = NULL;
	pfn_g_settings_get_int get_int = NULL;
	pfn_g_object_unref object_unref = NULL;
	pfn_g_free g_free_fn = NULL;
	LOOK_DLSYM(gio, source_get_default, "g_settings_schema_source_get_default");
	LOOK_DLSYM(gio, source_lookup, "g_settings_schema_source_lookup");
	LOOK_DLSYM(gio, has_key, "g_settings_schema_has_key");
	LOOK_DLSYM(gio, schema_unref, "g_settings_schema_unref");
	LOOK_DLSYM(gio, settings_new, "g_settings_new");
	LOOK_DLSYM(gio, get_string, "g_settings_get_string");
	LOOK_DLSYM(gio, get_int, "g_settings_get_int");
	LOOK_DLSYM(gio, object_unref, "g_object_unref");
	LOOK_DLSYM(gio, g_free_fn, "g_free");
	if (source_get_default == NULL || source_lookup == NULL || has_key == NULL || schema_unref == NULL ||
	    settings_new == NULL || get_string == NULL || get_int == NULL || object_unref == NULL ||
	    g_free_fn == NULL) {
		return; // the library stays loaded: GLib does not support being unloaded
	}
	void *source = source_get_default();
	void *schema = source != NULL ? source_lookup(source, GNOME_INTERFACE_SCHEMA, 1) : NULL;
	if (schema == NULL) {
		return;
	}
	const bool have_theme = has_key(schema, "cursor-theme") != 0;
	const bool have_size = has_key(schema, "cursor-size") != 0;
	schema_unref(schema);
	if (!have_theme && !have_size) {
		return;
	}
	void *settings = settings_new(GNOME_INTERFACE_SCHEMA);
	if (settings == NULL) {
		return;
	}
	if (have_theme && theme[0] == '\0') {
		char *t = get_string(settings, "cursor-theme");
		if (t != NULL) {
			snprintf(theme, theme_size, "%s", t);
			g_free_fn(t);
		}
	}
	if (have_size && *size <= 0) {
		*size = get_int(settings, "cursor-size");
	}
	object_unref(settings);
}

//! The cursor theme ("" = libXcursor's default) and size in OS points, read once.
//! Called under s_lock.
static void
cursor_theme(const char **out_theme, int *out_size)
{
	static bool read = false;
	static char theme[128];
	static int size;
	if (!read) {
		read = true;
		const char *e = getenv("XCURSOR_THEME");
		if (e != NULL) {
			snprintf(theme, sizeof(theme), "%s", e);
		}
		e = getenv("XCURSOR_SIZE");
		size = e != NULL ? atoi(e) : 0;
		if (theme[0] == '\0' || size <= 0) {
			gsettings_cursor(theme, sizeof(theme), &size);
		}
		if (size <= 0 || size > CURSOR_LOOK_MAX_PX) {
			size = CURSOR_LOOK_DEFAULT_SIZE;
		}
	}
	*out_theme = theme;
	*out_size = size;
}


/*
 *
 * The platform half (comp_cursor_look.h).
 *
 */

//! Marks a look that holds the theme image (comp_cursor_look::os_cursor): nothing to release.
static char s_theme_marker;

//! Loads the theme image into @p look at @p scale. Called under s_lock.
static bool
load_theme_locked(struct comp_cursor_look *look, float scale)
{
	static float failed_scale = -1.0f;
	if (scale == failed_scale || !xcursor_available()) {
		return false; // not retried every frame
	}

	const char *theme = NULL;
	int size_pt = 0;
	cursor_theme(&theme, &size_pt);
	const int want_px = (int)lrintf((float)size_pt * scale);
	const char *theme_arg = theme[0] != '\0' ? theme : NULL;
	struct xcursor_image *img = s_xcursor.load_image("left_ptr", theme_arg, want_px);
	if (img == NULL) {
		img = s_xcursor.load_image("default", theme_arg, want_px);
	}
	bool ok = img != NULL && img->pixels != NULL && img->width > 0 && img->height > 0 &&
	          img->width <= CURSOR_LOOK_MAX_PX && img->height <= CURSOR_LOOK_MAX_PX && img->size > 0;
	uint8_t *rgba = ok ? malloc((size_t)img->width * img->height * 4) : NULL;
	ok = ok && rgba != NULL;
	if (!ok) {
		free(rgba);
		if (img != NULL) {
			s_xcursor.image_destroy(img);
		}
		failed_scale = scale;
		// comp_cursor_look_update() keeps the image the look already holds, else draws the arrow.
		const bool keep_theme = look->os_cursor == &s_theme_marker && look->rgba != NULL;
		U_LOG_W("cursor_draw: could not load the cursor theme '%s' left_ptr at %d px - %s",
		        theme_arg != NULL ? theme_arg : "default", want_px,
		        keep_theme ? "keeping the theme image loaded for an earlier scale"
		                   : "the default look is the classic arrow");
		return false;
	}
	u_cursor_draw_from_argb32(img->pixels, img->width * img->height, rgba);

	free(look->rgba);
	look->rgba = rgba;
	look->w = img->width;
	look->h = img->height;
	look->aspect = (float)img->width / (float)img->height;
	look->hot_u = (float)img->xhot / (float)img->width;
	look->hot_v = (float)img->yhot / (float)img->height;
	look->height_pt = u_cursor_theme_height_pt(img->height, img->size, (uint32_t)size_pt);
	look->os_cursor = &s_theme_marker;
	look->os_scale = scale;
	look->generation++;
	static bool announced = false; // process-wide: one notice, whichever compositor loads it first
	if (!announced) {
		announced = true;
		U_LOG_W("cursor_draw: default look = the cursor theme '%s' left_ptr (%ux%u px, nominal %u, for %d pt at "
		        "scale %.2f, hotspot %u,%u; one-time notice, later reloads log at INFO)",
		        theme_arg != NULL ? theme_arg : "default", img->width, img->height, img->size, size_pt,
		        (double)scale, img->xhot, img->yhot);
	} else {
		U_LOG_I("cursor_draw: default look = the cursor theme '%s' left_ptr (%ux%u px, nominal %u, for %d pt at "
		        "scale %.2f)",
		        theme_arg != NULL ? theme_arg : "default", img->width, img->height, img->size, size_pt,
		        (double)scale);
	}
	s_xcursor.image_destroy(img);
	return true;
}

bool
comp_cursor_look_platform_mirror(struct comp_cursor_look *look, float backing_scale)
{
	const float scale = backing_scale > 0.0f ? backing_scale : comp_cursor_look_platform_backing_scale();
	if (look->os_cursor == &s_theme_marker && look->os_scale == scale && look->rgba != NULL) {
		return true; // loaded at this scale: the common case
	}
	pthread_mutex_lock(&s_lock);
	const bool ok = load_theme_locked(look, scale);
	pthread_mutex_unlock(&s_lock);
	return ok;
}

void
comp_cursor_look_platform_release(void *os_cursor)
{
	(void)os_cursor; // the theme image is copied: nothing is retained
}

typedef void *(*pfn_x_open_display)(const char *name);
typedef char *(*pfn_x_resource_manager_string)(void *display);
typedef int (*pfn_x_close_display)(void *display);

//! Xft.dpi / 96, read once (comp_cursor_look_platform_backing_scale()).
static float s_backing_scale = 1.0f;
static pthread_once_t s_backing_scale_once = PTHREAD_ONCE_INIT;

static void
read_backing_scale(void)
{
	float scale = 1.0f;
	void *x11 = getenv("DISPLAY") != NULL ? dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL) : NULL;
	if (x11 != NULL) {
		pfn_x_open_display open_display = NULL;
		pfn_x_resource_manager_string resources = NULL;
		pfn_x_close_display close_display = NULL;
		LOOK_DLSYM(x11, open_display, "XOpenDisplay");
		LOOK_DLSYM(x11, resources, "XResourceManagerString");
		LOOK_DLSYM(x11, close_display, "XCloseDisplay");
		void *dpy = open_display != NULL && resources != NULL && close_display != NULL ? open_display(NULL) : NULL;
		if (dpy != NULL) {
			const float dpi = u_cursor_parse_xft_dpi(resources(dpy));
			if (dpi > 0.0f) {
				scale = dpi / 96.0f;
			}
			close_display(dpy);
		}
		// libX11 stays loaded: other code in the process may hold it too.
	}
	s_backing_scale = scale;
}

float
comp_cursor_look_platform_backing_scale(void)
{
	pthread_once(&s_backing_scale_once, read_backing_scale);
	return s_backing_scale;
}
