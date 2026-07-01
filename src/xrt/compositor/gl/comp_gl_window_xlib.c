// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Xlib/GLX window helper for the native GL compositor on desktop Linux.
 *
 * @ingroup comp_gl
 */

#include "comp_gl_window_xlib.h"

#include "util/u_misc.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <GL/glx.h>

struct comp_gl_window_xlib
{
	//! Borrowed from the app's XrGraphicsBindingOpenGLXlibKHR.xDisplay — NOT
	//! opened or closed here (GLX context sharing needs one shared Display).
	Display *dpy;
	Window window;
	Window root;
	GLXContext context; //!< Compositor's own context, shares with the app's.
	GLXFBConfig fbconfig;

	Atom atom_wm_delete_window;

	uint32_t width;
	uint32_t height;

	//! Cleared when the user/WM closes the window.
	bool valid;
};

xrt_result_t
comp_gl_window_xlib_create(void *app_display,
                           void *app_glx_context,
                           uint32_t width,
                           uint32_t height,
                           struct comp_gl_window_xlib **out_win)
{
	if (width == 0) {
		width = 1280;
	}
	if (height == 0) {
		height = 720;
	}

	Display *dpy = (Display *)app_display;
	if (dpy == NULL) {
		U_LOG_E("GL/Xlib: no app Display* — the OpenXR Xlib GL binding must carry xDisplay");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	int screen = DefaultScreen(dpy);

	// Double-buffered RGBA8 + depth FBConfig for the compositor's present window.
	const int fb_attrs[] = {
	    GLX_X_RENDERABLE, True,
	    GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
	    GLX_RENDER_TYPE, GLX_RGBA_BIT,
	    GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
	    GLX_RED_SIZE, 8,
	    GLX_GREEN_SIZE, 8,
	    GLX_BLUE_SIZE, 8,
	    GLX_ALPHA_SIZE, 8,
	    GLX_DEPTH_SIZE, 24,
	    GLX_DOUBLEBUFFER, True,
	    None};

	int fbcount = 0;
	GLXFBConfig *fbc = glXChooseFBConfig(dpy, screen, fb_attrs, &fbcount);
	if (fbc == NULL || fbcount == 0) {
		U_LOG_E("GL/Xlib: glXChooseFBConfig found no matching config");
		return XRT_ERROR_OPENGL;
	}
	GLXFBConfig fbconfig = fbc[0];

	XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbconfig);
	if (vi == NULL) {
		U_LOG_E("GL/Xlib: glXGetVisualFromFBConfig failed");
		XFree(fbc);
		return XRT_ERROR_OPENGL;
	}

	Window root = RootWindow(dpy, vi->screen);
	Colormap cmap = XCreateColormap(dpy, root, vi->visual, AllocNone);

	XSetWindowAttributes swa;
	memset(&swa, 0, sizeof(swa));
	swa.colormap = cmap;
	swa.background_pixel = 0;
	swa.border_pixel = 0;
	swa.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask |
	                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

	Window win = XCreateWindow(dpy, root, 0, 0, width, height, 0, vi->depth,
	                           InputOutput, vi->visual,
	                           CWColormap | CWEventMask | CWBackPixel | CWBorderPixel, &swa);
	XFree(vi);
	if (win == 0) {
		U_LOG_E("GL/Xlib: XCreateWindow failed");
		XFree(fbc);
		return XRT_ERROR_OPENGL;
	}

	XStoreName(dpy, win, "DisplayXR");

	// WM_DELETE_WINDOW so a user close is a ClientMessage we can detect rather
	// than a fatal X error.
	Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	if (wm_delete != None) {
		XSetWMProtocols(dpy, win, &wm_delete, 1);
	}

	XMapWindow(dpy, win);

	// Compositor context shares the texture namespace with the app's context so
	// it can sample the app-rendered swapchain textures directly (no external
	// memory). Direct rendering; falls back to indirect if that fails.
	GLXContext share = (GLXContext)app_glx_context;
	GLXContext ctx = glXCreateNewContext(dpy, fbconfig, GLX_RGBA_TYPE, share, True);
	if (ctx == NULL && share != NULL) {
		U_LOG_W("GL/Xlib: shared GLX context creation failed; retrying without sharing");
		ctx = glXCreateNewContext(dpy, fbconfig, GLX_RGBA_TYPE, NULL, True);
	}
	XFree(fbc);
	if (ctx == NULL) {
		U_LOG_E("GL/Xlib: glXCreateNewContext failed");
		XDestroyWindow(dpy, win);
		return XRT_ERROR_OPENGL;
	}

	struct comp_gl_window_xlib *w = U_TYPED_CALLOC(struct comp_gl_window_xlib);
	w->dpy = dpy;
	w->window = win;
	w->root = root;
	w->context = ctx;
	w->fbconfig = fbconfig;
	w->atom_wm_delete_window = wm_delete;
	w->width = width;
	w->height = height;
	w->valid = true;

	XFlush(dpy);

	U_LOG_W("GL/Xlib: created %ux%u window 0x%08lx (shared=%s)", width, height,
	        (unsigned long)win, share != NULL ? "yes" : "no");

	*out_win = w;
	return XRT_SUCCESS;
}

void *
comp_gl_window_xlib_get_glx_context(struct comp_gl_window_xlib *win)
{
	return win != NULL ? (void *)win->context : NULL;
}

bool
comp_gl_window_xlib_make_current(struct comp_gl_window_xlib *win)
{
	if (win == NULL || win->dpy == NULL) {
		return false;
	}
	return glXMakeCurrent(win->dpy, win->window, win->context) == True;
}

void
comp_gl_window_xlib_swap_buffers(struct comp_gl_window_xlib *win)
{
	if (win == NULL || win->dpy == NULL) {
		return;
	}
	glXSwapBuffers(win->dpy, win->window);
}

void
comp_gl_window_xlib_save_current(void **out_dpy, unsigned long *out_drawable, void **out_ctx)
{
	if (out_dpy != NULL) {
		*out_dpy = (void *)glXGetCurrentDisplay();
	}
	if (out_drawable != NULL) {
		*out_drawable = (unsigned long)glXGetCurrentDrawable();
	}
	if (out_ctx != NULL) {
		*out_ctx = (void *)glXGetCurrentContext();
	}
}

void
comp_gl_window_xlib_restore_current(void *dpy, unsigned long drawable, void *ctx)
{
	if (dpy != NULL && ctx != NULL) {
		glXMakeCurrent((Display *)dpy, (GLXDrawable)drawable, (GLXContext)ctx);
	}
}

// Pull ONLY this window's events off the (shared with the app) Display, so we
// track resize/close without consuming the app's events.
static void
pump(struct comp_gl_window_xlib *win)
{
	if (win == NULL || win->dpy == NULL) {
		return;
	}
	XEvent ev;
	while (XCheckWindowEvent(win->dpy, win->window, StructureNotifyMask, &ev)) {
		if (ev.type == ConfigureNotify) {
			if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0) {
				win->width = (uint32_t)ev.xconfigure.width;
				win->height = (uint32_t)ev.xconfigure.height;
			}
		} else if (ev.type == DestroyNotify) {
			win->valid = false;
		}
	}
	while (XCheckTypedWindowEvent(win->dpy, win->window, ClientMessage, &ev)) {
		if (win->atom_wm_delete_window != None &&
		    (Atom)ev.xclient.data.l[0] == win->atom_wm_delete_window) {
			win->valid = false;
		}
	}
}

void
comp_gl_window_xlib_get_dimensions(struct comp_gl_window_xlib *win,
                                   uint32_t *out_width,
                                   uint32_t *out_height)
{
	if (win == NULL) {
		return;
	}
	pump(win);
	if (out_width != NULL) {
		*out_width = win->width;
	}
	if (out_height != NULL) {
		*out_height = win->height;
	}
}

bool
comp_gl_window_xlib_get_screen_position(struct comp_gl_window_xlib *win,
                                        int32_t *out_left_px,
                                        int32_t *out_top_px)
{
	if (win == NULL || win->dpy == NULL) {
		return false;
	}
	int x = 0, y = 0;
	Window child = 0;
	if (XTranslateCoordinates(win->dpy, win->window, win->root, 0, 0, &x, &y, &child) == 0) {
		return false;
	}
	if (out_left_px != NULL) {
		*out_left_px = x;
	}
	if (out_top_px != NULL) {
		*out_top_px = y;
	}
	return true;
}

bool
comp_gl_window_xlib_is_valid(struct comp_gl_window_xlib *win)
{
	if (win == NULL) {
		return false;
	}
	pump(win);
	return win->valid;
}

void
comp_gl_window_xlib_pump_input(struct comp_gl_window_xlib *win,
                               comp_gl_window_xlib_input_cb cb,
                               void *ctx)
{
	if (win == NULL || win->dpy == NULL || cb == NULL) {
		return;
	}
	const long mask = KeyPressMask | KeyReleaseMask | ButtonPressMask |
	                  ButtonReleaseMask | PointerMotionMask;
	XEvent ev;
	// Only this window's input events — leaves the app's (on the shared Display) alone.
	while (XCheckWindowEvent(win->dpy, win->window, mask, &ev)) {
		cb(ctx, &ev);
	}
}

void
comp_gl_window_xlib_destroy(struct comp_gl_window_xlib **win_ptr)
{
	if (win_ptr == NULL || *win_ptr == NULL) {
		return;
	}
	struct comp_gl_window_xlib *win = *win_ptr;
	if (win->dpy != NULL) {
		glXMakeCurrent(win->dpy, None, NULL);
		if (win->context != NULL) {
			glXDestroyContext(win->dpy, win->context);
		}
		if (win->window != 0) {
			XDestroyWindow(win->dpy, win->window);
		}
		XFlush(win->dpy);
		// NOTE: do not XCloseDisplay — the Display is borrowed from the app.
	}
	free(win);
	*win_ptr = NULL;
}
