// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux twin of the Windows workspace-session window hide.
 *
 * ## Problem
 *
 * A `_handle` app launched into the workspace (`DISPLAYXR_WORKSPACE_SESSION=1`)
 * is an IPC client: the service composites its swapchain into the shared
 * spatial surface, and the app's own window is never a render target. If that
 * window is mapped anyway, the user sees the app's bare window — fullscreen on
 * the 3D panel for the cube apps — until the workspace tile appears.
 *
 * Windows solves this in oxr_session.c (`ShowWindow(hwnd, SW_HIDE)` under the
 * same gate) and the D3D11 service re-shows the HWND on hot-switch deactivate
 * (`ShowWindowAsync(SW_SHOWNOACTIVATE)`). This is the desktop-Linux arm.
 *
 * ## X11 — withdrawn here
 *
 * The runtime is handed the app's `Display*` + `Window` through
 * XR_DXR_xlib_window_binding, so it can withdraw the window itself:
 * `XWithdrawWindow` = `XUnmapWindow` + the ICCCM synthetic UnmapNotify to the
 * root, so the window manager moves it to the Withdrawn state rather than
 * treating it as iconified (which would leave a taskbar/dock entry). Then
 * `XFlush`, because the app may not flush its connection for a while.
 *
 * libX11 is resolved with `dlopen(RTLD_NOLOAD)` instead of being linked: the
 * runtime must call into the SAME libX11 instance that owns the app's
 * `Display*`, and an app that chained an Xlib binding necessarily has it loaded
 * already. No new link dependency for the headless / Wayland-only builds.
 *
 * Threading: the call runs on the thread that called xrCreateSession, with the
 * app's Display. That is the same contract the in-process vk_native path
 * already relies on (it borrows the Display for its XCB surface): an app that
 * touches its Display from several threads must have called XInitThreads.
 *
 * Restore: there is no Linux service-side hot-switch that re-shows an app
 * window (the service never learns the app's window, and a Wayland surface
 * cannot be touched cross-process at all). What the runtime CAN do is the
 * client-side half of the Windows contract — "an app that recreates its
 * session after workspace_deactivate comes up standalone with a normal visible
 * window" — so the withdrawn windows are remembered, and a later standalone
 * session on the same Display/Window maps it again. The restore deliberately
 * happens at session CREATE (the window is known-valid: the app just bound it)
 * and never at session destroy, where the app may already have destroyed the
 * window and a BadWindow would reach the app's X error handler (whose default
 * exits the process).
 *
 * ## Wayland — NOT hidden here, by design
 *
 * XR_DXR_wayland_surface_binding hands the runtime a bare `wl_surface` on the
 * app's `wl_display`. Unmapping it means `wl_surface_attach(NULL)` + commit on
 * an object whose role (xdg_toplevel), buffers and configure sequence the app
 * owns. After a NULL-buffer unmap the xdg_surface protocol requires the client
 * to redo the initial commit / configure / ack handshake before attaching a
 * buffer again; an app (or libdecor) that attaches its next buffer without
 * knowing the runtime unmapped it commits a protocol error and the compositor
 * DISCONNECTS the app. Minimizing is not an option either:
 * `xdg_toplevel.set_minimized` needs the xdg_toplevel object, which the
 * binding does not carry (and a minimized toplevel still has a taskbar entry
 * and still showed its first frame). So on Wayland the only robust fix is
 * app-side: do not map the toplevel in a workspace session. The runtime logs
 * one WARN naming that so a third-party app's flash is diagnosable.
 *
 * @ingroup oxr_main
 */

#include "xrt/xrt_config_os.h"

#include "oxr_workspace_window_linux.h"

#include "util/u_logging.h"

#ifdef XRT_OS_LINUX_DESKTOP

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>

// Xlib prototypes, declared locally so this TU needs no <X11/Xlib.h> (the
// symbols are resolved at run time from the app's already-loaded libX11).
typedef int (*pfn_XWithdrawWindow)(void *display, unsigned long w, int screen_number);
typedef int (*pfn_XMapWindow)(void *display, unsigned long w);
typedef int (*pfn_XFlush)(void *display);
typedef int (*pfn_XDefaultScreen)(void *display);

struct x11_fns
{
	pfn_XWithdrawWindow withdraw;
	pfn_XMapWindow map;
	pfn_XFlush flush;
	pfn_XDefaultScreen default_screen;
};

// dlsym returns an object pointer; copying its bytes into the function-pointer
// slot is the ISO-C-clean way to convert (a cast warns under -Wpedantic).
static void
x11_sym(void *lib, const char *name, void *out_fn, size_t fn_size)
{
	void *sym = dlsym(lib, name);
	if (sym != NULL && fn_size == sizeof(sym)) {
		memcpy(out_fn, &sym, sizeof(sym));
	}
}

static bool
x11_fns_load(struct x11_fns *f)
{
	// RTLD_NOLOAD: only succeed if libX11 is already in the process — which it
	// is for any app holding an Xlib Display*. Never load a second copy.
	void *lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
	if (lib == NULL) {
		lib = dlopen("libX11.so", RTLD_LAZY | RTLD_NOLOAD);
	}
	if (lib == NULL) {
		return false;
	}
	x11_sym(lib, "XWithdrawWindow", &f->withdraw, sizeof(f->withdraw));
	x11_sym(lib, "XMapWindow", &f->map, sizeof(f->map));
	x11_sym(lib, "XFlush", &f->flush, sizeof(f->flush));
	x11_sym(lib, "XDefaultScreen", &f->default_screen, sizeof(f->default_screen));
	// The NOLOAD handle only bumps a refcount on the app's copy; dropping it
	// is safe because the app keeps libX11 loaded for as long as it holds
	// the Display — and it does, for at least the duration of this call.
	dlclose(lib);
	return f->withdraw != NULL && f->map != NULL && f->flush != NULL && f->default_screen != NULL;
}

/*
 * Windows this module withdrew, so a later standalone session on the same
 * window can map it again. Workspace mode is one session per app process in
 * practice; a few slots cover apps that bind several windows over their life.
 */
#define OXR_WS_LINUX_MAX_HIDDEN 4

static pthread_mutex_t g_hidden_lock = PTHREAD_MUTEX_INITIALIZER;
static struct
{
	void *display;
	unsigned long window;
} g_hidden[OXR_WS_LINUX_MAX_HIDDEN];

static void
hidden_add_locked(void *display, unsigned long window)
{
	for (size_t i = 0; i < OXR_WS_LINUX_MAX_HIDDEN; i++) {
		if (g_hidden[i].display == display && g_hidden[i].window == window) {
			return;
		}
	}
	for (size_t i = 0; i < OXR_WS_LINUX_MAX_HIDDEN; i++) {
		if (g_hidden[i].display == NULL) {
			g_hidden[i].display = display;
			g_hidden[i].window = window;
			return;
		}
	}
	// Full: overwrite the oldest slot. Losing a record only means a later
	// standalone session will not auto-map that window — never a wrong map.
	for (size_t i = 0; i + 1 < OXR_WS_LINUX_MAX_HIDDEN; i++) {
		g_hidden[i] = g_hidden[i + 1];
	}
	g_hidden[OXR_WS_LINUX_MAX_HIDDEN - 1].display = display;
	g_hidden[OXR_WS_LINUX_MAX_HIDDEN - 1].window = window;
}

static bool
hidden_take_locked(void *display, unsigned long window)
{
	for (size_t i = 0; i < OXR_WS_LINUX_MAX_HIDDEN; i++) {
		if (g_hidden[i].display == display && g_hidden[i].window == window) {
			g_hidden[i].display = NULL;
			g_hidden[i].window = 0;
			return true;
		}
	}
	return false;
}

void
oxr_workspace_linux_app_window_on_session_create(void *x_display,
                                                 unsigned long x_window,
                                                 bool have_wayland,
                                                 bool workspace_session)
{
	if (have_wayland && workspace_session) {
		U_LOG_W(
		    "Workspace session: app-owned Wayland surface is NOT hidden by the runtime (an app-owned "
		    "xdg_toplevel cannot be unmapped safely from outside the app). The app should not map its "
		    "window when DISPLAYXR_WORKSPACE_SESSION=1, or it will show until the workspace tile appears.");
		return;
	}

	if (x_display == NULL || x_window == 0) {
		return;
	}

	pthread_mutex_lock(&g_hidden_lock);
	bool restore = !workspace_session && hidden_take_locked(x_display, x_window);
	pthread_mutex_unlock(&g_hidden_lock);

	if (!workspace_session && !restore) {
		return; // ordinary standalone session — leave the window alone.
	}

	struct x11_fns f = {0};
	if (!x11_fns_load(&f)) {
		U_LOG_W("Workspace session: libX11 not resolvable in-process — X11 window 0x%lx left %s", x_window,
		        workspace_session ? "visible" : "withdrawn");
		return;
	}

	if (workspace_session) {
		f.withdraw(x_display, x_window, f.default_screen(x_display));
		f.flush(x_display);
		pthread_mutex_lock(&g_hidden_lock);
		hidden_add_locked(x_display, x_window);
		pthread_mutex_unlock(&g_hidden_lock);
		U_LOG_W("Workspace session: X11 window 0x%lx withdrawn (the service composites the app)", x_window);
	} else {
		f.map(x_display, x_window);
		f.flush(x_display);
		U_LOG_W("Standalone session after workspace: X11 window 0x%lx mapped again", x_window);
	}
}

#else // !XRT_OS_LINUX_DESKTOP

void
oxr_workspace_linux_app_window_on_session_create(void *x_display,
                                                 unsigned long x_window,
                                                 bool have_wayland,
                                                 bool workspace_session)
{
	(void)x_display;
	(void)x_window;
	(void)have_wayland;
	(void)workspace_session;
}

#endif // XRT_OS_LINUX_DESKTOP
