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

#include "os/os_time.h"

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
#include "android/android_mini_window.h"
#include "android/android_mini_window_tell.h"

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
		/*
		 * android_globals_set_window ADOPTS the reference resolve_window
		 * just took, and drops the one the globals held on whatever it
		 * replaces (#1040). So exactly one acquire per publish, and the
		 * globals are the single owner — a second owner here would double-
		 * release: after a surface-lost the globals still point at the old
		 * window, and the next publish's internal drop would run on memory
		 * this file had already freed (SIGSEGV in RefBase::decStrong,
		 * caught on the NP02J background→resume cycle).
		 */
		android_globals_set_window((struct _ANativeWindow *)win);
		/*
		 * Seed the rect so the very first weave is phase-correct before the
		 * app's first xrSetAndroidWindowGeometryDXR. ONLY when nothing has
		 * been published yet: the seed carries no panel extent (the create-
		 * info has no field for it), and overwriting a good rect with a
		 * partial one on every republish would silently drop the per-window
		 * Kooima back to display-scoped for the rest of the session — the
		 * app de-duplicates its own samples, so it would not re-push an
		 * unchanged rect to repair it.
		 */
		if (binding != NULL && !android_globals_get_window_screen_rect(NULL, NULL, NULL, NULL, NULL,
		                                                               NULL, NULL, NULL)) {
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
		// Marker, NOT an owned reference (see above) — it records that this
		// session put a window into the globals, so fini knows to take it
		// back out. A surface-lost publish leaves it non-NULL on purpose:
		// the globals still hold a reference on the last real window.
		if (win != NULL) {
			sess->android_bound_window = (void *)win;
		}
	}

	return XR_SUCCESS;
}

void
oxr_android_surface_session_fini(struct oxr_session *sess)
{
	if (sess == NULL || sess->android_bound_window == NULL) {
		return;
	}
	/*
	 * Release the globals' reference on our window. Publishing NULL is the
	 * only way to do it: android_globals_clear_window deliberately KEEPS the
	 * reference so the stale pointer stays safe to compare against (#1040),
	 * and set_window(NULL) is what finally drops it.
	 */
	android_globals_set_window(NULL);
	sess->android_bound_window = NULL;

	/*
	 * #1396: drop the layout-hint episode with the session. The Java helper
	 * caches the probed vendor scale and its p/q for the lifetime of an
	 * episode, so a SECOND session in the same process (an end→begin bounce,
	 * a relaunch) would otherwise inherit the previous window's answer and
	 * apply it to a window it was never measured for. No event: the session
	 * that would receive it is going away.
	 */
	sess->android_hint_active = false;
	sess->android_hint_unknown_logged = false;
	sess->android_hint_ignored_logged = false;
	sess->android_hint_layout_w = 0;
	sess->android_hint_layout_h = 0;
	sess->android_hint_buffer_w = 0;
	sess->android_hint_buffer_h = 0;
	sess->android_hint_disp_w = 0;
	sess->android_hint_disp_h = 0;
	sess->android_hint_prev_layout_w = 0;
	sess->android_hint_prev_layout_h = 0;
	sess->android_hint_prev_disp_w = 0;
	sess->android_hint_prev_disp_h = 0;
	sess->android_hint_prev_armed_ns = 0;
	sess->android_hint_prev_refused_logged = false;
	sess->android_hint_scale = 0.0f;
	sess->android_hint_pending = false;
	sess->android_hint_coalesce_logged = false;
	sess->android_hint_last_emit_ns = 0;
	android_mini_window_reset();
}

/*
 *
 * Mini-window layout hint (#1396, spec v2).
 *
 * An OEM "window reply" container does not give the task a smaller window — it
 * gives it a FULL-SIZE logical window and scales the whole task with a
 * SurfaceFlinger leash. The vendor interlacer is strict 1:1 buffer→panel, so
 * that resample destroys the weave and the compositor honestly degrades to 2D
 * (`vk_android_update_container_scaled`).
 *
 * Escaping it needs a LAYOUT change (1080 → 1079 logical, so `logical * scale`
 * lands on a whole pixel) plus a fixed BUFFER. `ANativeWindow_setBuffersGeometry`
 * on the bound window reaches the second but not the first, and the app owns its
 * view — so the runtime measures and the app applies. Exactly the ADR-036 D6
 * split the geometry channel already uses, in the other direction.
 *
 * The hosted `MonadoView` does both halves itself; both paths call the SAME
 * `MiniWindowLayout` for every measured rule, so they cannot drift.
 */

/*!
 * How long the "do not derive from the previous episode's layout" memo lives.
 *
 * It only has to outlive the app's asynchronous restore — measured on the NP02J
 * at 50-160 ms between the `OFF` and the next real rect — so a second is a wide
 * margin, and it is far shorter than any human re-entry (recents + a tap).
 *
 * Wall clock rather than a publish count on purpose: the app publishes only when
 * its rect CHANGES, and the platform freezes a backgrounded app outright
 * (`CpuFreezerManagerServiceV2` on this OEM), so a counter can sit unadvanced for
 * an unbounded time. A clock cannot.
 */
#define OXR_ANDROID_PREV_LAYOUT_GRACE_NS (1000 * 1000 * 1000ULL)

/*!
 * Floor on the interval between two hint events INSIDE one episode (#1401).
 *
 * Only recomputes are throttled — the first answer of an episode is always
 * emitted at once, because that is the container transition and it is what
 * unblocks the weave. 100 ms is chosen to be longer than a drag's frame period
 * (so an intermediate size is dropped rather than resizing the app's buffer)
 * and far shorter than a human's reaction to the size they settled on.
 *
 * NEVER a drop: a throttled answer is stored and delivered by
 * @ref oxr_android_window_hint_flush from the frame loop. A container this is
 * reachable on has not been seen — the NP02J's mini-window is a fixed nominal
 * placement — so this is a bound on a class of behaviour, not a tuned constant.
 */
#define OXR_ANDROID_HINT_MIN_EMIT_INTERVAL_NS (100 * 1000 * 1000ULL)

/*!
 * Latch an answer into the session and push it (#1401).
 *
 * @p repeat is "this is a recompute inside an episode, not the transition into
 * one" — it only picks the log tier; the event is identical either way.
 */
static void
oxr_android_window_hint_commit(struct oxr_logger *log,
                               struct oxr_session *sess,
                               int32_t layout_w,
                               int32_t layout_h,
                               int32_t buffer_w,
                               int32_t buffer_h,
                               int32_t x,
                               int32_t y,
                               uint32_t disp_w,
                               uint32_t disp_h,
                               float scale,
                               bool repeat)
{
	sess->android_hint_active = true;
	sess->android_hint_ignored_logged = false;
	sess->android_hint_layout_w = layout_w;
	sess->android_hint_layout_h = layout_h;
	sess->android_hint_buffer_w = buffer_w;
	sess->android_hint_buffer_h = buffer_h;
	sess->android_hint_x = x;
	sess->android_hint_y = y;
	sess->android_hint_disp_w = (int32_t)disp_w;
	sess->android_hint_disp_h = (int32_t)disp_h;
	sess->android_hint_scale = scale;

	sess->android_hint_pending = false;
	sess->android_hint_last_emit_ns = os_monotonic_get_ns();

	oxr_event_push_XrEventDataAndroidWindowLayoutHint(log, sess, XR_TRUE, scale, layout_w, layout_h, buffer_w,
	                                                  buffer_h, x, y, repeat);
}

void
oxr_android_window_hint_flush(struct oxr_logger *log, struct oxr_session *sess)
{
	if (sess == NULL || !sess->android_hint_pending) {
		return;
	}
	if (os_monotonic_get_ns() - sess->android_hint_last_emit_ns < OXR_ANDROID_HINT_MIN_EMIT_INTERVAL_NS) {
		return;
	}
	oxr_android_window_hint_commit(log, sess, sess->android_hint_pending_layout_w,
	                               sess->android_hint_pending_layout_h, sess->android_hint_pending_buffer_w,
	                               sess->android_hint_pending_buffer_h, sess->android_hint_pending_x,
	                               sess->android_hint_pending_y, (uint32_t)sess->android_hint_pending_disp_w,
	                               (uint32_t)sess->android_hint_pending_disp_h, sess->android_hint_pending_scale,
	                               /* repeat */ true);
}

//! End the episode and tell the app to restore. No-op when no hint is active.
static void
oxr_android_window_hint_clear(struct oxr_logger *log, struct oxr_session *sess, uint32_t disp_w, uint32_t disp_h)
{
	if (!sess->android_hint_active) {
		return;
	}
	/*
	 * Remember the layout we are ending on. The app restores asynchronously, so
	 * its very next publish may still carry it — and deriving a new hint from
	 * THAT is deriving a hint from a hint. MEASURED on the NP02J: rotating out
	 * of and back into the mini-window recomputed against the app's still-1079
	 * layout instead of the container's natural 1080, and settled on a 723x1130
	 * buffer where a clean entry gives 723x1129 — about 1.05 px of drift, above
	 * the 0.4 px that is visibly a double image. The container never hands out a
	 * size we invented, so refusing exactly this extent is safe.
	 */
	sess->android_hint_prev_layout_w = sess->android_hint_layout_w;
	sess->android_hint_prev_layout_h = sess->android_hint_layout_h;
	/*
	 * Armed against the panel of the publish that ENDS the episode, not the one
	 * the hint was computed in. On a rotation those differ, and arming with the
	 * old one expires the memo on the very next publish — which is the 723x1130
	 * bug it exists to prevent, measured again on the NP02J when this was first
	 * written the other way round.
	 */
	sess->android_hint_prev_disp_w = (int32_t)disp_w;
	sess->android_hint_prev_disp_h = (int32_t)disp_h;
	sess->android_hint_prev_armed_ns = os_monotonic_get_ns();
	sess->android_hint_prev_refused_logged = false;

	sess->android_hint_active = false;
	sess->android_hint_unknown_logged = false;
	sess->android_hint_ignored_logged = false;
	sess->android_hint_layout_w = 0;
	sess->android_hint_layout_h = 0;
	sess->android_hint_buffer_w = 0;
	sess->android_hint_buffer_h = 0;
	sess->android_hint_disp_w = 0;
	sess->android_hint_disp_h = 0;
	sess->android_hint_scale = 0.0f;
	// A coalesced answer belongs to the episode that is ending; delivering it
	// after the OFF would tell the app to shrink a window that just restored.
	sess->android_hint_pending = false;
	sess->android_hint_coalesce_logged = false;
	android_mini_window_reset();
	sess->android_hint_last_emit_ns = os_monotonic_get_ns();
	oxr_event_push_XrEventDataAndroidWindowLayoutHint(log, sess, XR_FALSE, 0.0f, 0, 0, 0, 0, 0, 0,
	                                                  /* repeat */ false);
}

void
oxr_android_window_hint_reemit(struct oxr_logger *log, struct oxr_session *sess)
{
	if (sess == NULL || !sess->android_hint_active) {
		return;
	}
	if (!sess->sys->inst->extensions.DXR_android_surface_binding) {
		return;
	}
	// A re-delivery is a lifecycle event (xrBeginSession, a surface republish),
	// not a recompute — it keeps the WARN tier.
	oxr_event_push_XrEventDataAndroidWindowLayoutHint(
	    log, sess, XR_TRUE, sess->android_hint_scale, sess->android_hint_layout_w, sess->android_hint_layout_h,
	    sess->android_hint_buffer_w, sess->android_hint_buffer_h, sess->android_hint_x, sess->android_hint_y,
	    /* repeat */ false);
}

void
oxr_android_window_hint_update(struct oxr_logger *log,
                               struct oxr_session *sess,
                               int32_t x,
                               int32_t y,
                               uint32_t w,
                               uint32_t h,
                               uint32_t disp_w,
                               uint32_t disp_h)
{
	if (sess == NULL || w == 0 || h == 0) {
		return;
	}
	if (disp_w == 0 || disp_h == 0) {
		// No panel extent to compare against. Never decide on ignorance —
		// keep whatever state we are in (same rule as the compositor's tell).
		return;
	}

	if (sess->android_hint_active) {
		/*
		 * Two things can end an episode that the published RECT cannot show,
		 * so they are checked on every publish while a hint is live — not only
		 * on the latched branch below. An app that leaves the container while
		 * publishing a rect that still spills (a resize, a rotation, a
		 * different display) would otherwise keep the hint latched forever.
		 *
		 * 1. The container itself. A physical rect fits the panel BY
		 *    CONSTRUCTION, so "the window left its container" and "the hint is
		 *    working" are indistinguishable from the rect. MEASURED on the
		 *    NP02J: moving the task back to fullscreen left the app's window at
		 *    the hinted layout, so it kept publishing a fitting 723x1129 rect
		 *    and the hint stayed latched — a small window weaving in a
		 *    fullscreen task. `Activity.isInMultiWindowMode()` is the exact
		 *    public answer.
		 * 2. The panel frame. Every number in the hint — the scale, the
		 *    layout, the buffer — was derived against one panel extent. A
		 *    rotation replaces it, so the episode is over; the app restores and
		 *    the next publish re-derives from scratch. Without this the latch
		 *    below keeps weaving at the pre-rotation size.
		 */
		if (!android_mini_window_still_scalable()) {
			oxr_android_window_hint_clear(log, sess, disp_w, disp_h);
			return;
		}
		if ((int32_t)disp_w != sess->android_hint_disp_w || (int32_t)disp_h != sess->android_hint_disp_h) {
			oxr_android_window_hint_clear(log, sess, disp_w, disp_h);
			return;
		}

		/*
		 * Once the app has applied a hint it publishes the PHYSICAL rect, which
		 * fits the panel and so no longer trips the tell. Recognise that by the
		 * EXTENT (the origin keeps changing as the window is dragged) and latch,
		 * or every drag frame would look like "the window left its container".
		 */
		if ((int32_t)w == sess->android_hint_buffer_w && (int32_t)h == sess->android_hint_buffer_h) {
			sess->android_hint_x = x;
			sess->android_hint_y = y;
			return;
		}
	}

	// ONE definition, shared with vk_android_update_container_scaled and pinned
	// against MiniWindowLayout.isTell by tests_aux_mini_window_tell (#1401).
	const bool tell = android_mini_window_is_tell(x, y, w, h, disp_w, disp_h);

	if (!tell) {
		oxr_android_window_hint_clear(log, sess, disp_w, disp_h);
		return;
	}

	/*
	 * The app may not have finished restoring from the previous episode — a rect
	 * that is still THAT episode's layout size is not the container's own, and
	 * deriving from it is deriving a hint from a hint.
	 *
	 * The memo is bounded (see OXR_ANDROID_PREV_LAYOUT_GRACE_NS). Unbounded it
	 * would be a permanent, silent lock-out on any device whose container scale
	 * rationalises to a small q — 0.75, 0.80, 0.50, 0.60 all leave
	 * snapDimension() returning the window unchanged, so the previous layout IS
	 * the container's natural logical size and every re-entry would match it
	 * forever. (The NP02J escapes only because 1080 x 0.67 is not integral.)
	 *
	 * It cannot instead be cleared on any non-spilling publish: a 1079x1685
	 * window FITS a rotated 1600x2560 panel, so such a publish arrives
	 * mid-rotation before the app has restored — which is exactly the case that
	 * measured 723x1130 instead of 723x1129.
	 */
	if (sess->android_hint_prev_layout_w > 0) {
		const uint64_t now = os_monotonic_get_ns();
		const bool expired = (int32_t)disp_w != sess->android_hint_prev_disp_w ||
		                     (int32_t)disp_h != sess->android_hint_prev_disp_h ||
		                     now - sess->android_hint_prev_armed_ns > OXR_ANDROID_PREV_LAYOUT_GRACE_NS;

		if (!expired && (int32_t)w == sess->android_hint_prev_layout_w &&
		    (int32_t)h == sess->android_hint_prev_layout_h) {
			// The only state decision in this file that produces no event.
			// Say it once so it is never silent.
			if (!sess->android_hint_prev_refused_logged) {
				sess->android_hint_prev_refused_logged = true;
				U_LOG_W(
				    "XR_DXR_android_surface_binding: ignoring a %ux%u rect — still the previous "
				    "episode's layout, waiting for the container's own (#1396)",
				    w, h);
			}
			return;
		}
		sess->android_hint_prev_layout_w = 0;
		sess->android_hint_prev_layout_h = 0;
		sess->android_hint_prev_disp_w = 0;
		sess->android_hint_prev_disp_h = 0;
		sess->android_hint_prev_refused_logged = false;
	}

	struct android_mini_window_hint hint = {0};
	if (!android_mini_window_compute_hint(x, y, w, h, disp_w, disp_h, &hint)) {
		/*
		 * No usable answer for this rect. Either the container scale is not
		 * knowable — the app owns the view, so the touch-ratio fallback the
		 * hosted path uses is not available here and there is no readable
		 * vendor API — or the helper rejected the rect as a mid-rotation
		 * sample. Both fail to the 2D fallback, which is the honest direction.
		 */
		if (!sess->android_hint_unknown_logged) {
			sess->android_hint_unknown_logged = true;
			U_LOG_W(
			    "XR_DXR_android_surface_binding: window %d,%d %ux%u spills the panel %ux%u but no "
			    "1:1 layout is derivable for it (scale unknown, or a mid-rotation sample) — "
			    "staying on the 2D fallback (#1396)",
			    x, y, w, h, disp_w, disp_h);
		}
		return;
	}

	if (sess->android_hint_active && hint.layout_w == sess->android_hint_layout_w &&
	    hint.layout_h == sess->android_hint_layout_h && hint.buffer_w == sess->android_hint_buffer_w &&
	    hint.buffer_h == sess->android_hint_buffer_h) {
		// Same answer, and the app is still publishing a LOGICAL rect — it has
		// not applied the hint (or does not implement spec v2). Say so once and
		// do not re-push: the event is already in its queue.
		sess->android_hint_x = x;
		sess->android_hint_y = y;
		if (!sess->android_hint_ignored_logged) {
			sess->android_hint_ignored_logged = true;
			U_LOG_W(
			    "XR_DXR_android_surface_binding: layout hint delivered but the app is still "
			    "publishing a logical %ux%u rect — expecting the physical %dx%d one; the 2D "
			    "fallback stays until it does (#1396)",
			    w, h, hint.buffer_w, hint.buffer_h);
		}
		return;
	}

	/*
	 * #1401: throttle RECOMPUTES, never the transition.
	 *
	 * The first answer of an episode is the container transition — it is what
	 * takes the window off the 2D fallback, and delaying it would be visible.
	 * A later, DIFFERENT answer is a resize of a container that can be resized;
	 * on such a device every intermediate size of a drag would otherwise cost
	 * the app a buffer reallocation and put a WARN in the log. Hold it instead,
	 * and let the frame loop deliver whatever the gesture settled on
	 * (@ref oxr_android_window_hint_flush) — a coalesced answer is delayed,
	 * never dropped.
	 */
	if (sess->android_hint_active) {
		const uint64_t now = os_monotonic_get_ns();
		if (now - sess->android_hint_last_emit_ns < OXR_ANDROID_HINT_MIN_EMIT_INTERVAL_NS) {
			sess->android_hint_pending = true;
			sess->android_hint_pending_layout_w = hint.layout_w;
			sess->android_hint_pending_layout_h = hint.layout_h;
			sess->android_hint_pending_buffer_w = hint.buffer_w;
			sess->android_hint_pending_buffer_h = hint.buffer_h;
			sess->android_hint_pending_x = x;
			sess->android_hint_pending_y = y;
			sess->android_hint_pending_disp_w = (int32_t)disp_w;
			sess->android_hint_pending_disp_h = (int32_t)disp_h;
			sess->android_hint_pending_scale = hint.scale;
			if (!sess->android_hint_coalesce_logged) {
				sess->android_hint_coalesce_logged = true;
				U_LOG_W(
				    "XR_DXR_android_surface_binding: the container is answering a new size "
				    "faster than %u ms — coalescing layout hints for this episode, further "
				    "recomputes log at INFO (#1401)",
				    (unsigned)(OXR_ANDROID_HINT_MIN_EMIT_INTERVAL_NS / (1000 * 1000)));
			}
			return;
		}
	}

	oxr_android_window_hint_commit(log, sess, hint.layout_w, hint.layout_h, hint.buffer_w, hint.buffer_h, x, y,
	                               disp_w, disp_h, hint.scale,
	                               /* repeat */ sess->android_hint_active);
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

	XrResult ret = oxr_android_surface_publish(&log, sess, binding, NULL);

	// A republish is a background→resume (or a container transition): the app's
	// event queue may have been drained across it, and it has just rebuilt its
	// Surface at the layout size. Re-deliver an active hint so it re-applies.
	if (ret == XR_SUCCESS && binding != NULL) {
		oxr_android_window_hint_reemit(&log, sess);
	}

	return ret;
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

	// #1396: the same rect decides whether this window needs a 1:1 layout to
	// weave at all. Cheap — the tell is four compares, and the vendor probe
	// behind it resolves once per scaled-container episode.
	oxr_android_window_hint_update(&log, sess, geometry->windowRect.offset.x, geometry->windowRect.offset.y,
	                               (uint32_t)geometry->windowRect.extent.width,
	                               (uint32_t)geometry->windowRect.extent.height,
	                               (uint32_t)(geometry->panelExtent.width > 0 ? geometry->panelExtent.width : 0),
	                               (uint32_t)(geometry->panelExtent.height > 0 ? geometry->panelExtent.height : 0));

	return XR_SUCCESS;
}

#endif // OXR_HAVE_DXR_android_surface_binding
