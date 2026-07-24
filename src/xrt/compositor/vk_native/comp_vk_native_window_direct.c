// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Direct-scanout window backend for the VK native compositor on Linux.
 *
 * Model: Monado's comp_window_direct_randr — steal a RandR output from the
 * running X server as a VkDisplayKHR, acquire it, and present to it directly,
 * bypassing Xorg + the desktop compositor (ST-5539). See the header for the
 * ownership contract and the fullscreen-only, opt-in constraints.
 *
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_window_direct.h"

// The whole backend is inert unless the bundle was built with the display +
// acquire-xlib platform macros (CMake: XRT_HAVE_XLIB_XRANDR). CMake only adds
// this source under that guard; the #if is belt-and-suspenders so a stray build
// can't break on the missing vk_bundle fields / Vulkan display types.
#if defined(VK_USE_PLATFORM_XLIB_XRANDR_EXT) && defined(VK_USE_PLATFORM_DISPLAY_KHR)

#include "util/u_misc.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

struct comp_vk_native_window_direct
{
	//! Borrowed — the compositor's bundle (instance + physical_device + the
	//! display/acquire function pointers loaded when the exts are enabled).
	struct vk_bundle *vk;

	//! X connection we own (opened here purely to acquire the display; the
	//! surface itself is a Vulkan display-plane surface, not an X window).
	Display *dpy;

	//! The connector we stole from X and now own until destroy.
	VkDisplayKHR display;

	//! Display-plane surface built on @ref display — OWNED here (see header).
	VkSurfaceKHR surface;

	//! Native mode dimensions = the scanout resolution the swapchain matches.
	uint32_t width;
	uint32_t height;

	//! Only turns false on teardown; direct scanout has no user-close event.
	bool valid;
};

/*!
 * Find the RandR output whose CRTC origin matches the plug-in-reported panel
 * position (@p screen_left, @p screen_top). Falls back to the first connected,
 * CRTC-active output when nothing matches exactly — the common single-panel
 * demo box still resolves. Returns None on no candidate.
 */
static RROutput
find_target_randr_output(Display *dpy, int32_t screen_left, int32_t screen_top)
{
	Window root = DefaultRootWindow(dpy);
	XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
	if (res == NULL) {
		U_LOG_E("direct: XRRGetScreenResourcesCurrent failed");
		return None;
	}

	RROutput match = None;
	RROutput first_active = None;

	for (int i = 0; i < res->noutput; i++) {
		XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
		if (oi == NULL) {
			continue;
		}
		if (oi->connection != RR_Connected || oi->crtc == 0) {
			XRRFreeOutputInfo(oi);
			continue;
		}

		XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
		if (ci != NULL) {
			if (first_active == None) {
				first_active = res->outputs[i];
			}
			if ((int32_t)ci->x == screen_left && (int32_t)ci->y == screen_top) {
				match = res->outputs[i];
				U_LOG_I("direct: matched RandR output '%.*s' at (%d,%d)",
				        oi->nameLen, oi->name, (int)ci->x, (int)ci->y);
			}
			XRRFreeCrtcInfo(ci);
		}
		XRRFreeOutputInfo(oi);

		if (match != None) {
			break;
		}
	}

	XRRFreeScreenResources(res);

	if (match == None && first_active != None) {
		U_LOG_W("direct: no RandR output at (%d,%d); using first connected output",
		        (int)screen_left, (int)screen_top);
	}
	return match != None ? match : first_active;
}

/*!
 * Pick a display plane usable with @p display: prefer one already bound to it,
 * else the first unbound plane (currentDisplay == VK_NULL_HANDLE). Avoids the
 * (unloaded) vkGetDisplayPlaneSupportedDisplaysKHR by reading currentDisplay
 * from the plane properties directly.
 */
static bool
pick_display_plane(struct vk_bundle *vk,
                   VkDisplayKHR display,
                   uint32_t *out_plane_index,
                   uint32_t *out_stack_index)
{
	uint32_t count = 0;
	VkDisplayPlanePropertiesKHR *props = NULL;
	VkResult res = vk_enumerate_physical_display_plane_properties(vk, vk->physical_device, &count, &props);
	if (res != VK_SUCCESS || count == 0 || props == NULL) {
		U_LOG_E("direct: no display planes (%d)", res);
		free(props);
		return false;
	}

	bool found = false;
	// First pass: a plane already driving our display.
	for (uint32_t i = 0; i < count && !found; i++) {
		if (props[i].currentDisplay == display) {
			*out_plane_index = i;
			*out_stack_index = props[i].currentStackIndex;
			found = true;
		}
	}
	// Second pass: any unbound plane.
	for (uint32_t i = 0; i < count && !found; i++) {
		if (props[i].currentDisplay == VK_NULL_HANDLE) {
			*out_plane_index = i;
			*out_stack_index = props[i].currentStackIndex;
			found = true;
		}
	}

	free(props);
	if (!found) {
		U_LOG_E("direct: no display plane free for the acquired display");
	}
	return found;
}

xrt_result_t
comp_vk_native_window_direct_create(struct vk_bundle *vk,
                                    int32_t screen_left,
                                    int32_t screen_top,
                                    struct comp_vk_native_window_direct **out_win)
{
	*out_win = NULL;

	// Every function pointer below is only non-NULL when its instance
	// extension was enabled at bundle build (all optional). A NULL here means
	// this box isn't direct-scanout capable — the caller falls back to XCB.
	if (vk->vkGetRandROutputDisplayEXT == NULL || vk->vkAcquireXlibDisplayEXT == NULL ||
	    vk->vkCreateDisplayPlaneSurfaceKHR == NULL || vk->vkGetPhysicalDeviceDisplayPlanePropertiesKHR == NULL) {
		U_LOG_I("direct: display/acquire-xlib extensions not enabled; XCB fallback");
		return XRT_ERROR_VULKAN;
	}

	Display *dpy = XOpenDisplay(NULL);
	if (dpy == NULL) {
		U_LOG_E("direct: XOpenDisplay(NULL) failed");
		return XRT_ERROR_VULKAN;
	}

	RROutput output = find_target_randr_output(dpy, screen_left, screen_top);
	if (output == None) {
		U_LOG_E("direct: no connected RandR output to acquire");
		XCloseDisplay(dpy);
		return XRT_ERROR_VULKAN;
	}

	// RandR output -> VkDisplayKHR. Null means the ICD does not expose this
	// output as a Vulkan display (e.g. already leased) — bail to XCB.
	VkDisplayKHR vkdisplay = VK_NULL_HANDLE;
	VkResult res = vk->vkGetRandROutputDisplayEXT(vk->physical_device, dpy, output, &vkdisplay);
	if (res != VK_SUCCESS || vkdisplay == VK_NULL_HANDLE) {
		U_LOG_E("direct: vkGetRandROutputDisplayEXT failed (%d)", res);
		XCloseDisplay(dpy);
		return XRT_ERROR_VULKAN;
	}

	// Evict the output from the X server; we scan out to it exclusively now.
	res = vk->vkAcquireXlibDisplayEXT(vk->physical_device, dpy, vkdisplay);
	if (res != VK_SUCCESS) {
		U_LOG_E("direct: vkAcquireXlibDisplayEXT failed (%d)", res);
		XCloseDisplay(dpy);
		return XRT_ERROR_VULKAN;
	}

	// Native mode (index 0 is the panel's preferred/native timing) gives both
	// the VkDisplayModeKHR and the scanout extent.
	uint32_t mode_count = 0;
	VkDisplayModePropertiesKHR *modes = NULL;
	res = vk_enumerate_display_mode_properties(vk, vk->physical_device, vkdisplay, &mode_count, &modes);
	if (res != VK_SUCCESS || mode_count == 0 || modes == NULL) {
		U_LOG_E("direct: no display modes (%d)", res);
		free(modes);
		vk->vkReleaseDisplayEXT(vk->physical_device, vkdisplay);
		XCloseDisplay(dpy);
		return XRT_ERROR_VULKAN;
	}
	VkDisplayModeKHR mode = modes[0].displayMode;
	uint32_t width = modes[0].parameters.visibleRegion.width;
	uint32_t height = modes[0].parameters.visibleRegion.height;
	free(modes);

	uint32_t plane_index = 0;
	uint32_t stack_index = 0;
	if (!pick_display_plane(vk, vkdisplay, &plane_index, &stack_index)) {
		vk->vkReleaseDisplayEXT(vk->physical_device, vkdisplay);
		XCloseDisplay(dpy);
		return XRT_ERROR_VULKAN;
	}

	VkDisplaySurfaceCreateInfoKHR surface_ci = {
	    .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
	    .displayMode = mode,
	    .planeIndex = plane_index,
	    .planeStackIndex = stack_index,
	    .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
	    .globalAlpha = 1.0f,
	    .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
	    .imageExtent = {width, height},
	};

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	res = vk->vkCreateDisplayPlaneSurfaceKHR(vk->instance, &surface_ci, NULL, &surface);
	if (res != VK_SUCCESS) {
		U_LOG_E("direct: vkCreateDisplayPlaneSurfaceKHR failed (%d)", res);
		vk->vkReleaseDisplayEXT(vk->physical_device, vkdisplay);
		XCloseDisplay(dpy);
		return XRT_ERROR_VULKAN;
	}

	struct comp_vk_native_window_direct *win = U_TYPED_CALLOC(struct comp_vk_native_window_direct);
	if (win == NULL) {
		vk->vkDestroySurfaceKHR(vk->instance, surface, NULL);
		vk->vkReleaseDisplayEXT(vk->physical_device, vkdisplay);
		XCloseDisplay(dpy);
		return XRT_ERROR_ALLOCATION;
	}

	win->vk = vk;
	win->dpy = dpy;
	win->display = vkdisplay;
	win->surface = surface;
	win->width = width;
	win->height = height;
	win->valid = true;

	U_LOG_W("direct: scanning out directly to %ux%u connector (bypassing Xorg/compositor)", width, height);

	*out_win = win;
	return XRT_SUCCESS;
}

VkSurfaceKHR
comp_vk_native_window_direct_get_surface(struct comp_vk_native_window_direct *win)
{
	return win != NULL ? win->surface : VK_NULL_HANDLE;
}

void
comp_vk_native_window_direct_get_dimensions(struct comp_vk_native_window_direct *win,
                                            uint32_t *out_width,
                                            uint32_t *out_height)
{
	if (win == NULL) {
		return;
	}
	*out_width = win->width;
	*out_height = win->height;
}

bool
comp_vk_native_window_direct_is_valid(struct comp_vk_native_window_direct *win)
{
	return win != NULL && win->valid;
}

void
comp_vk_native_window_direct_destroy(struct comp_vk_native_window_direct **win_ptr)
{
	struct comp_vk_native_window_direct *win = *win_ptr;
	if (win == NULL) {
		return;
	}

	struct vk_bundle *vk = win->vk;
	if (win->surface != VK_NULL_HANDLE) {
		vk->vkDestroySurfaceKHR(vk->instance, win->surface, NULL);
	}
	if (win->display != VK_NULL_HANDLE && vk->vkReleaseDisplayEXT != NULL) {
		// Hand the connector back to the X server.
		vk->vkReleaseDisplayEXT(vk->physical_device, win->display);
	}
	if (win->dpy != NULL) {
		XCloseDisplay(win->dpy);
	}

	free(win);
	*win_ptr = NULL;
}

#endif // VK_USE_PLATFORM_XLIB_XRANDR_EXT && VK_USE_PLATFORM_DISPLAY_KHR
