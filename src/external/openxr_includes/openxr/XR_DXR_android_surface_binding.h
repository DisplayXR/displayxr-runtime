// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the XR_DXR_* extensions in this header are NOT yet registered in the
// Khronos OpenXR registry: extension numbers and XrStructureType values sit
// in a provisional experimental block (1004999xxx) pending official
// assignment. Extension names are expected to be stable; numeric values are
// not.
// See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Header for XR_DXR_android_surface_binding extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension lets an OpenXR application provide its own Android Surface
 * (an `ANativeWindow*`, or the Java `android.view.Surface` it came from) to
 * the runtime. When provided, the runtime composites and weaves into the
 * application's surface instead of spawning a SurfaceView of its own.
 * Sibling of XR_DXR_win32_window_binding (HWND),
 * XR_DXR_cocoa_window_binding (NSView), XR_DXR_xlib_window_binding (X11) and
 * XR_DXR_wayland_surface_binding (wl_surface).
 *
 * Why it is not optional on Android (ADR-036 D2, runtime#1037): the
 * runtime-spawned SurfaceView is added straight to the WindowManager, so it
 * has no `ViewParent` — and `SurfaceView.onAttachedToWindow` dereferences one
 * on the freeform/translucent path, which crashes the app the moment it is
 * placed in a multi-window (freeform / split-screen) task. An app that owns
 * its own SurfaceView has a ViewParent by construction. The runtime keeps its
 * self-spawned SurfaceView only as the `_hosted` (fullscreen) fallback for an
 * app that chains nothing.
 *
 * Two runtime functions accompany the create-info struct, because both facts
 * they carry CHANGE during a session on Android and neither has a callback the
 * runtime can hook from outside the app's own View hierarchy:
 *
 *   - @ref xrSetAndroidSurfaceDXR — the surface is destroyed and recreated on
 *     every background/resume (`surfaceDestroyed` / `surfaceCreated`, or
 *     native_app_glue's `APP_CMD_TERM_WINDOW` / `APP_CMD_INIT_WINDOW`). The
 *     app republishes it; the runtime rebuilds its VkSurfaceKHR + swapchain
 *     and pauses/resumes the display processor across the gap.
 *   - @ref xrSetAndroidWindowGeometryDXR — a pure window MOVE raises no
 *     resize: `WindowFrames.didFrameSizeChange` compares w/h only, so the move
 *     goes out as a `oneway IWindow.moved` with no layout, no invalidate and no
 *     public callback, while SurfaceFlinger has already repositioned the layer
 *     with the OLD buffer. The 3D weave's interlace phase and the per-window
 *     Kooima frustum are both referenced to the window's on-panel origin, so
 *     the app publishes that origin itself (cheaply, once per frame, from a
 *     `Choreographer` callback). ADR-036 D6; ADR-033 is unchanged — this
 *     reports GEOMETRY, the weaver still owns all phase including snapping.
 *
 * Spec v2 adds one EVENT in the other direction — @ref
 * XrEventDataAndroidWindowLayoutHintDXR. An OEM "mini-window" container scales
 * the whole task with a SurfaceFlinger leash, which resamples (and so destroys)
 * the weave; escaping it needs a layout change only the app can make, from a
 * measurement only the runtime does. See that struct.
 *
 * TRAP: an OEM that applies the `OVERRIDE_SANDBOX_VIEW_BOUNDS_APIS` compat
 * change makes `View.getLocationOnScreen()` return WINDOW-relative
 * coordinates, so every window would report (0,0) with no error anywhere. The
 * opt-out is per-APP and therefore belongs in the application's manifest:
 * `<property android:name=
 * "android.window.PROPERTY_COMPAT_ALLOW_SANDBOXING_VIEW_BOUNDS_APIS"
 * android:value="false"/>`.
 */
#ifndef XR_DXR_ANDROID_SURFACE_BINDING_H
#define XR_DXR_ANDROID_SURFACE_BINDING_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_android_surface_binding 1
#define XR_DXR_android_surface_binding_SPEC_VERSION 2
#define XR_DXR_ANDROID_SURFACE_BINDING_EXTENSION_NAME "XR_DXR_android_surface_binding"

// XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR keeps the value published in
// the (previously unimplemented) sketch in docs/specs/extensions/
// XR_DXR_display_info.md §4 — it sits in one of the two unused gaps of the
// XR_DXR_display_info decade rather than in this extension's own decade. The
// geometry struct takes the next free decade, 1004999220–229 (220 = geometry,
// 221 = the spec-v2 layout-hint event). Both are recorded in this directory's
// README.md registry.
#define XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999005)
#define XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR ((XrStructureType)1004999220)
#define XR_TYPE_EVENT_DATA_ANDROID_WINDOW_LAYOUT_HINT_DXR ((XrStructureType)1004999221)

#if defined(__ANDROID__)

// Opaque stand-in so the header stays self-contained when <android/native_window.h>
// was not included first (same trick as the xlib/wayland siblings).
struct ANativeWindow;

/*!
 * @brief Structure passed in XrSessionCreateInfo::next chain to provide an
 *        application-owned Android Surface for session rendering.
 *
 * At least one of @p nativeWindow and @p surface must be non-NULL. The runtime
 * takes its OWN reference on the resulting `ANativeWindow` (`ANativeWindow_acquire`)
 * and releases it when the binding is replaced or the session is destroyed, so
 * the runtime never owns the Surface's lifecycle — the application does, and
 * must keep the Java `Surface` / `SurfaceView` alive for as long as it wants
 * frames on screen.
 *
 * The application MUST NOT draw into the bound surface itself: one BufferQueue
 * has one producer, and a `Surface.lockCanvas()` poisons it for GL/VK for good.
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrAndroidSurfaceBindingCreateInfoDXR {
    XrStructureType             type;           //!< Must be XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS    next;           //!< Pointer to next structure in chain
    //! The application's `ANativeWindow`, e.g. from `ANativeWindow_fromSurface()`,
    //! `ASurfaceHolder_getNativeWindow()`, or `android_app::window` in a
    //! NativeActivity app. NULL ⟹ derive it from @p surface.
    struct ANativeWindow*       nativeWindow;
    //! Optional `jobject` for the Java `android.view.Surface` the window came
    //! from, as an opaque pointer (the header stays JNI-free). When
    //! @p nativeWindow is NULL the runtime resolves it with
    //! `ANativeWindow_fromSurface()`. Should be a global reference if provided.
    void*                       surface;
    //! Initial on-screen origin of the surface, in physical display pixels of
    //! the CURRENT rotation (`View.getLocationOnScreen()`). Seeds the weave
    //! phase and the per-window Kooima before the first
    //! @ref xrSetAndroidWindowGeometryDXR call; 0,0 is a valid fullscreen value.
    int32_t                     screenOffsetX;
    int32_t                     screenOffsetY;
    //! When XR_TRUE the runtime configures the bound surface for translucent
    //! composition, so pixels the app writes with alpha = 0 compose through to
    //! whatever SurfaceFlinger has behind the layer. Sibling of
    //! XrWin32WindowBindingCreateInfoDXR::transparentBackgroundEnabled. The app
    //! is responsible for requesting a translucent `SurfaceHolder` format.
    XrBool32                    transparentBackgroundEnabled;
} XrAndroidSurfaceBindingCreateInfoDXR;

/*!
 * @brief The bound surface's live on-screen rectangle.
 *
 * @p windowRect is in physical screen pixels of the CURRENT rotation, y down,
 * origin inclusive of any caption. @p panelExtent is the full panel extent in
 * that SAME rotation (`Display.getRealSize()`), which the runtime cannot derive
 * on its own: its display info describes the panel in its NATURAL orientation,
 * and a sub-panel window fits inside both orderings, so "which way is the panel
 * held" is genuinely ambiguous from the rect alone.
 */
typedef struct XrAndroidWindowGeometryDXR {
    XrStructureType             type;         //!< Must be XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR
    const void* XR_MAY_ALIAS    next;         //!< Pointer to next structure in chain
    XrRect2Di                   windowRect;   //!< On-screen rect, current rotation, physical px
    XrExtent2Di                 panelExtent;  //!< Panel extent in the same rotation, physical px
    int32_t                     displayId;    //!< `Display.getDisplayId()`; 0 = default display
} XrAndroidWindowGeometryDXR;

/*!
 * @brief The runtime's answer to "this window is being scaled by its container"
 *        (spec v2, runtime#1396).
 *
 * Some OEM multi-window shells ("mini-window", "window reply", freeform-with-
 * scale) do not give the task a smaller window — they give it a FULL-SIZE
 * logical window and shrink the whole task with a SurfaceFlinger leash
 * (measured on one A13 tablet: `SCALE TRANSLATE 0.67 @ (1757,236)`, so a
 * 1080x1685 logical window lands as 723x1129 physical pixels). The vendor
 * interlacer is strict 1:1 buffer→panel, so ANY resample between the woven
 * buffer and the panel destroys the interlace; a runtime that notices this and
 * does nothing must fall back to flat 2D.
 *
 * The fix is to make the COMPOSED transform identity rather than to fight the
 * leash: give the surface a buffer of `round(logical * scale)` pixels, and
 * SurfaceFlinger's buffer→layer scale times the leash multiplies out to 1.0.
 * That needs BOTH halves and only the application owns one of them:
 *
 *   - the BUFFER size — reachable from either side, and
 *   - the LAYOUT size of the view — reachable only by the app. It matters
 *     because `logical * scale` must land on a whole pixel: 1080 x 0.67 =
 *     723.6 composes to 0.9994, ~0.4 px of drift across the window, which
 *     reads as a slight double image in BOTH eyes (the signature of a residual
 *     resample; a phase error blurs one eye only). 1079 x 0.67 = 722.93 → 723,
 *     a 0.07 px residual, is invisible. `ANativeWindow_setBuffersGeometry` on
 *     the bound window cannot reach that: it sets the buffer, not the layout.
 *
 * So the runtime keeps the POLICY (it owns the scale probe and the exact-
 * integer search — see the spec) and the app keeps its WINDOW. The runtime
 * emits this event; the app resizes its content view to @p layoutSize, fixes
 * its buffer to @p bufferSize, and republishes @p physicalRect through
 * @ref xrSetAndroidWindowGeometryDXR. When @p active is XR_FALSE the window
 * fits the panel again and the app restores its ordinary layout.
 *
 * An app that ignores the event is not broken — it keeps the runtime's honest
 * 2D fallback in the scaled container, exactly as before spec v2.
 *
 * Re-emitted at `xrBeginSession` and on the next `xrSetAndroidSurfaceDXR`
 * publish while a hint is active, so an app that starts (or resumes) already
 * inside a scaled container never misses it.
 *
 * @extends XrEventDataBaseHeader
 */
typedef struct XrEventDataAndroidWindowLayoutHintDXR {
    XrStructureType             type;          //!< Must be XR_TYPE_EVENT_DATA_ANDROID_WINDOW_LAYOUT_HINT_DXR
    const void* XR_MAY_ALIAS    next;
    XrSession                   session;       //!< Session whose bound surface this describes
    //! XR_TRUE  = apply the layout below.
    //! XR_FALSE = the container no longer scales this window; restore the
    //! ordinary match-parent layout and drop the fixed buffer size. Every
    //! other field is 0 when this is XR_FALSE.
    XrBool32                    active;
    //! The measured container scale (physical / logical), e.g. 0.67. Informational —
    //! the app must use the integer sizes below, which are the RATIONALISED answer;
    //! recomputing from this float re-introduces the drift the search removed.
    float                       scale;
    //! Logical size the app must lay its content view / window out at. Never
    //! LARGER than the window: overscanning does not work, because a SurfaceView
    //! bigger than its window has its surface sized to the VISIBLE frame and the
    //! resample comes straight back. The remainder (typically ONE logical pixel)
    //! shows as a strip on the right/bottom edge; paint it black.
    XrExtent2Di                 layoutSize;
    //! Fixed buffer size in physical panel pixels — `SurfaceHolder.setFixedSize()`
    //! or `ANativeWindow_setBuffersGeometry()`. This is `round(layoutSize * scale)`.
    XrExtent2Di                 bufferSize;
    //! The on-screen rect to publish through @ref xrSetAndroidWindowGeometryDXR
    //! ONCE the buffer has actually come back at @p bufferSize. Its offset is the
    //! window's on-screen origin as the runtime last saw it (the app should keep
    //! publishing its own live `View.getLocationOnScreen()` as the window moves)
    //! and its extent is @p bufferSize.
    XrRect2Di                   physicalRect;
} XrEventDataAndroidWindowLayoutHintDXR;

/*!
 * @brief Republish (or drop) the application-owned Surface mid-session.
 *
 * @param session The session created with a chained
 *        @ref XrAndroidSurfaceBindingCreateInfoDXR.
 * @param binding The new surface, or NULL (or a binding whose @p nativeWindow
 *        and @p surface are both NULL) to report SURFACE LOST. On loss the
 *        runtime tears its VkSurfaceKHR down and pauses the display processor;
 *        on the next non-NULL call it rebuilds and resumes. Only
 *        @p nativeWindow / @p surface / @p screenOffset* are read — the
 *        transparency opt-in is fixed at session create.
 *
 * Idempotent: republishing the same window is a cheap no-op.
 *
 * @return XR_SUCCESS, XR_ERROR_HANDLE_INVALID, XR_ERROR_VALIDATION_FAILURE or
 *         XR_ERROR_FUNCTION_UNSUPPORTED.
 */
XRAPI_ATTR XrResult XRAPI_CALL
xrSetAndroidSurfaceDXR(XrSession session, const XrAndroidSurfaceBindingCreateInfoDXR *binding);

/*!
 * @brief Publish the bound surface's current on-screen geometry.
 *
 * Cheap enough to call once per frame; the runtime de-duplicates. Calling it
 * from a `Choreographer` callback is the recommended shape — see the file
 * comment for why no Android callback reports a pure window move.
 *
 * @return XR_SUCCESS, XR_ERROR_HANDLE_INVALID, XR_ERROR_VALIDATION_FAILURE or
 *         XR_ERROR_FUNCTION_UNSUPPORTED.
 */
XRAPI_ATTR XrResult XRAPI_CALL
xrSetAndroidWindowGeometryDXR(XrSession session, const XrAndroidWindowGeometryDXR *geometry);

typedef XrResult(XRAPI_PTR *PFN_xrSetAndroidSurfaceDXR)(XrSession session,
                                                        const XrAndroidSurfaceBindingCreateInfoDXR *binding);
typedef XrResult(XRAPI_PTR *PFN_xrSetAndroidWindowGeometryDXR)(XrSession session,
                                                               const XrAndroidWindowGeometryDXR *geometry);

#endif // defined(__ANDROID__)

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_ANDROID_SURFACE_BINDING_H
