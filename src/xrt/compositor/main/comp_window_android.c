// Copyright 2019-2020, Collabora, Ltd.
// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Android present target for the out-of-process service compositor.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_main
 *
 * DisplayXR (#510): ported from the Monado-legacy comp_window_android, trimmed
 * to the out-of-process service case. The app hands its surface across the IPC
 * boundary (Client.java passAppSurface → MonadoImpl → android_globals); this
 * target pulls the ANativeWindow from android_globals and presents to it via a
 * VK_KHR_android_surface swapchain. The legacy in-process branches (creating a
 * MonadoView from an Activity, the SYSTEM_ALERT_WINDOW overlay path) and the
 * comp_target_factory are intentionally omitted: the runtime service has no
 * Activity, and the null/service compositor creates this target directly via
 * its comp_target_service, not through a factory.
 */

#include "xrt/xrt_compiler.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_time.h"

#include "os/os_time.h"

#include "android/android_globals.h"

#include "main/comp_compositor.h"
#include "main/comp_target_swapchain.h"
#include "main/comp_window_android.h"

#include <android/native_window.h>

#include <stdlib.h>


/*
 *
 * Private structs.
 *
 */

/*!
 * An Android present target.
 *
 * @implements comp_target_swapchain
 */
struct comp_window_android
{
	struct comp_target_swapchain base;
};


/*
 *
 * Functions.
 *
 */

static inline struct vk_bundle *
get_vk(struct comp_window_android *cwa)
{
	// cwa->base.base.c is the owning compositor. In the service this is a
	// null_compositor up-cast to comp_compositor; both begin with comp_base,
	// so base.vk is the same bundle the system compositor allocates from.
	return &cwa->base.base.c->base.vk;
}

static bool
comp_window_android_init(struct comp_target *ct)
{
	(void)ct;
	return true;
}

static void
comp_window_android_destroy(struct comp_target *ct)
{
	struct comp_window_android *cwa = (struct comp_window_android *)ct;

	comp_target_swapchain_cleanup(&cwa->base);

	free(ct);
}

static void
comp_window_android_update_window_title(struct comp_target *ct, const char *title)
{
	(void)ct;
	(void)title;
}

static VkResult
comp_window_android_create_surface(struct comp_window_android *cwa,
                                   struct ANativeWindow *window,
                                   VkSurfaceKHR *out_surface)
{
	struct vk_bundle *vk = get_vk(cwa);
	VkResult ret;

	VkAndroidSurfaceCreateInfoKHR surface_info = {
	    .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
	    .flags = 0,
	    .window = window,
	};

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	ret = vk->vkCreateAndroidSurfaceKHR(vk->instance, &surface_info, NULL, &surface);
	if (ret != VK_SUCCESS) {
		U_LOG_E("comp_window_android: vkCreateAndroidSurfaceKHR: %s", vk_result_string(ret));
		return ret;
	}

	VK_NAME_SURFACE(vk, surface, "comp_window_android surface");
	*out_surface = surface;

	return VK_SUCCESS;
}

static bool
comp_window_android_init_swapchain(struct comp_target *ct, uint32_t width, uint32_t height)
{
	struct comp_window_android *cwa = (struct comp_window_android *)ct;
	(void)width;
	(void)height;

	// Out-of-process: poll for the surface the app pushed across IPC
	// (Client.java blockingConnect → passAppSurface → android_globals). It is
	// normally already present by the time the session reaches this point.
	struct ANativeWindow *window = NULL;
	for (int i = 0; i < 100; i++) {
		window = (struct ANativeWindow *)android_globals_get_window();
		if (window != NULL) {
			break;
		}
		os_nanosleep(20 * U_TIME_1MS_IN_NS);
	}

	if (window == NULL) {
		U_LOG_E("comp_window_android: no ANativeWindow available from android_globals");
		return false;
	}

	VkResult ret = comp_window_android_create_surface(cwa, window, &cwa->base.surface.handle);
	if (ret != VK_SUCCESS) {
		U_LOG_E("comp_window_android: failed to create surface: %s", vk_result_string(ret));
		return false;
	}

	U_LOG_W("comp_window_android: VkSurfaceKHR created from ANativeWindow %p", (void *)window);
	return true;
}

static void
comp_window_android_flush(struct comp_target *ct)
{
	(void)ct;
}

struct comp_target *
comp_window_android_create(struct comp_compositor *c)
{
	struct comp_window_android *w = U_TYPED_CALLOC(struct comp_window_android);
	if (w == NULL) {
		return NULL;
	}

	// The display-timing code path hasn't been exercised on Android; force fake timing.
	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);

	w->base.base.name = "Android";
	w->base.base.destroy = comp_window_android_destroy;
	w->base.base.flush = comp_window_android_flush;
	w->base.base.init_pre_vulkan = comp_window_android_init;
	w->base.base.init_post_vulkan = comp_window_android_init_swapchain;
	w->base.base.set_title = comp_window_android_update_window_title;
	w->base.base.c = c;

	return &w->base.base;
}
