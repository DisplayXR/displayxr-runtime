// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_android_surface_binding API entry points.
 * @author David Fattal
 * @ingroup oxr_main
 *
 * Two facts about an app-owned Android Surface change during a session and
 * Android reports neither to anyone but the app itself:
 *
 *   - the Surface is destroyed and recreated on every background/resume, and
 *   - a pure window MOVE raises no resize (`WindowFrames.didFrameSizeChange`
 *     compares w/h only, so it goes out as a `oneway IWindow.moved` with no
 *     layout, no invalidate and no public callback) while SurfaceFlinger has
 *     already repositioned the layer with the OLD buffer.
 *
 * So the app publishes both, through these two calls. Everything downstream
 * already exists: the surface lands in the same `android_globals` window slot
 * the compositor target polls per frame to rebuild its VkSurfaceKHR (#507 /
 * #528), and the rect lands in the same `android_globals` rect slot the
 * out-of-process client feeds over `IMonado.updateWindowRect` (#1033) — which
 * the in-process compositor now reads for both the DP weave phase and the
 * per-window Kooima canvas (#1034).
 *
 * ADR-036 D2/D6, runtime#1037. ADR-033 is unchanged: this reports GEOMETRY;
 * the weaver owns all phase, snapping included.
 */

#include "xrt/xrt_compiler.h"

#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_logging.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_handle.h"
#include "oxr_api_verify.h"

#include <openxr/XR_DXR_android_surface_binding.h>

#ifdef OXR_HAVE_DXR_android_surface_binding

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include "android/android_globals.h"

/*!
 * Resolve a binding struct to an ANativeWindow the caller may keep.
 *
 * Returns a REFERENCED window (exactly one `ANativeWindow_release()` owed) or
 * NULL when the binding reports "no surface". `nativeWindow` wins over
 * `surface`; a `surface` jobject is resolved with `ANativeWindow_fromSurface`,
 * which already returns a referenced window.
 */
static ANativeWindow *
resolve_window(struct oxr_logger *log, const XrAndroidSurfaceBindingCreateInfoDXR *b)
{
	if (b == NULL) {
		return NULL;
	}
	if (b->nativeWindow != NULL) {
		ANativeWindow *win = (ANativeWindow *)b->nativeWindow;
		ANativeWindow_acquire(win);
		return win;
	}
	if (b->surface == NULL) {
		return NULL;
	}

	JavaVM *vm = (JavaVM *)android_globals_get_vm();
	if (vm == NULL) {
		oxr_warn(log, "XR_DXR_android_surface_binding: no JavaVM stored — cannot resolve the "
		              "android.view.Surface jobject; pass nativeWindow instead");
		return NULL;
	}
	JNIEnv *env = NULL;
	bool attached = false;
	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
		if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || env == NULL) {
			oxr_warn(log, "XR_DXR_android_surface_binding: AttachCurrentThread failed");
			return NULL;
		}
		attached = true;
	}
	ANativeWindow *win = ANativeWindow_fromSurface(env, (jobject)b->surface);
	if (attached) {
		(*vm)->DetachCurrentThread(vm);
	}
	if (win == NULL) {
		oxr_warn(log, "XR_DXR_android_surface_binding: ANativeWindow_fromSurface returned NULL");
	}
	return win;
}

/*!
 * Publish (or drop) an app-provided surface. Shared by session-create parsing
 * and the runtime function below, so both take the reference the same way.
 *
 * `sess` may be NULL at session-create time (the session object is not built
 * yet); it is only used to track the reference the session owes a release on.
 */
XrResult
oxr_android_surface_publish(struct oxr_logger *log,
                            struct oxr_session *sess,
                            const XrAndroidSurfaceBindingCreateInfoDXR *binding,
                            void **out_window)
{
	ANativeWindow *win = resolve_window(log, binding);

	if (out_window != NULL) {
		*out_window = (void *)win;
	}

	// Lifecycle event, never per frame.
	U_LOG_W("XR_DXR_android_surface_binding: app surface %s (window=%p)",
	        win != NULL ? "published" : "LOST", (void *)win);

	if (win == NULL) {
		// Surface lost: clear the published window. android_globals keeps
		// its own reference on the old pointer so nothing can dangle
		// between the clear and the next publish (#1040); the compositor
		// target notices the generation bump and tears its VkSurfaceKHR
		// down instead of presenting into a dead window.
		android_globals_clear_window();
	} else {
		android_globals_set_window((struct _ANativeWindow *)win);
		if (binding != NULL && (binding->screenOffsetX != 0 || binding->screenOffsetY != 0)) {
			// Seed the rect so the very first weave is phase-correct even
			// before the app's first geometry call. Extent unknown here —
			// use the window's own buffer size, which is the surface size.
			int32_t w = ANativeWindow_getWidth(win);
			int32_t h = ANativeWindow_getHeight(win);
			if (w > 0 && h > 0) {
				android_globals_set_window_screen_rect(binding->screenOffsetX,
				                                       binding->screenOffsetY, (uint32_t)w,
				                                       (uint32_t)h, /*display_id*/ 0,
				                                       /*disp_w*/ 0, /*disp_h*/ 0);
			}
		}
	}

	if (sess != NULL) {
		// Drop the reference the previous publish left us holding. Done
		// AFTER the new publish so the window can never be freed while the
		// globals still point at it.
		if (sess->android_bound_window != NULL && sess->android_bound_window != (void *)win) {
			ANativeWindow_release((ANativeWindow *)sess->android_bound_window);
		}
		sess->android_bound_window = (void *)win;
	}

	return XR_SUCCESS;
}

void
oxr_android_surface_session_fini(struct oxr_session *sess)
{
	if (sess == NULL || sess->android_bound_window == NULL) {
		return;
	}
	ANativeWindow_release((ANativeWindow *)sess->android_bound_window);
	sess->android_bound_window = NULL;
}

XrResult
oxr_xrSetAndroidSurfaceDXR(XrSession session, const XrAndroidSurfaceBindingCreateInfoDXR *binding)
{
	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetAndroidSurfaceDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_android_surface_binding);

	if (binding != NULL && binding->type != XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrSetAndroidSurfaceDXR: binding->type must be "
		                 "XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR");
	}

	return oxr_android_surface_publish(&log, sess, binding, NULL);
}

XrResult
oxr_xrSetAndroidWindowGeometryDXR(XrSession session, const XrAndroidWindowGeometryDXR *geometry)
{
	struct oxr_session *sess = NULL;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrSetAndroidWindowGeometryDXR");
	OXR_VERIFY_EXTENSION(&log, sess->sys->inst, DXR_android_surface_binding);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, geometry, XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR);

	if (geometry->windowRect.extent.width <= 0 || geometry->windowRect.extent.height <= 0) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrSetAndroidWindowGeometryDXR: windowRect extent must be positive (%dx%d)",
		                 geometry->windowRect.extent.width, geometry->windowRect.extent.height);
	}

	// De-duplicated + generation-bumped inside; the WARN for an actual change
	// is emitted by the consumers (compositor), so this stays silent per frame.
	android_globals_set_window_screen_rect(
	    geometry->windowRect.offset.x, geometry->windowRect.offset.y,
	    (uint32_t)geometry->windowRect.extent.width, (uint32_t)geometry->windowRect.extent.height,
	    geometry->displayId, (uint32_t)(geometry->panelExtent.width > 0 ? geometry->panelExtent.width : 0),
	    (uint32_t)(geometry->panelExtent.height > 0 ? geometry->panelExtent.height : 0));

	return XR_SUCCESS;
}

#endif // OXR_HAVE_DXR_android_surface_binding
