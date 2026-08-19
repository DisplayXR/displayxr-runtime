<!--
Copyright 2026, The DisplayXR Project
SPDX-License-Identifier: Apache-2.0
-->

# XR_DXR_android_surface_binding

**Status:** Implemented (runtime#1037, ADR-036 D2/D6) · **Spec version:** 1
**Platforms:** Android (`__ANDROID__`) · **Graphics APIs:** Vulkan (`comp_vk_native`)
**Header:** [`src/external/openxr_includes/openxr/XR_DXR_android_surface_binding.h`](../../../src/external/openxr_includes/openxr/XR_DXR_android_surface_binding.h)

Sibling window-binding extensions:
[`XR_DXR_win32_window_binding`](XR_DXR_win32_window_binding.md) (HWND) ·
[`XR_DXR_cocoa_window_binding`](XR_DXR_cocoa_window_binding.md) (NSView) ·
[`XR_DXR_xlib_window_binding`](XR_DXR_xlib_window_binding.md) (X11) ·
`XR_DXR_wayland_surface_binding` (wl_surface).

## 1. Overview

The application gives the runtime **its own Android Surface**. The runtime
composites the multi-view atlas, hands it to the vendor display processor, and
the DP weaves into that surface — all inside the app's process
(ADR-036 D2, "Architecture A"). Nothing about the surface's lifecycle becomes
the runtime's: the app creates it, keeps it alive, and republishes it across
background/resume.

Without a chained binding the runtime falls back to spawning a `SurfaceView` of
its own (`MonadoView`, added straight to the `WindowManager`). That fallback is
**fullscreen-only, `_hosted`-class**: the view has no `ViewParent`, and
`SurfaceView.onAttachedToWindow` dereferences one on the freeform/translucent
path, so the app crashes with

```
java.lang.NullPointerException: Attempt to invoke interface method
  'void android.view.ViewParent.requestTransparentRegion(android.view.View)'
  at android.view.SurfaceView.onAttachedToWindow(SurfaceView.java:294)
  at org.freedesktop.monado.auxiliary.MonadoView.onAttachedToWindow(MonadoView.java:159)
```

the moment the task is placed in a multi-window (freeform / split-screen)
container. **Multi-window on Android therefore requires this extension.** An
app that owns its SurfaceView has a `ViewParent` by construction.

Two accompanying functions carry the facts that *change* during a session and
that Android reports to nobody but the app itself:

| Function | Why the app, not the runtime |
|---|---|
| `xrSetAndroidSurfaceDXR` | The Surface is destroyed and recreated on every background/resume. Only the app's `SurfaceHolder.Callback` (or `APP_CMD_TERM_WINDOW` / `APP_CMD_INIT_WINDOW`) sees it. |
| `xrSetAndroidWindowGeometryDXR` | A pure window **move** raises no resize: `WindowFrames.didFrameSizeChange` compares w/h only, so the move goes out as a `oneway IWindow.moved` — no layout, no invalidate, no public callback — while SurfaceFlinger has already repositioned the layer with the OLD buffer. |

The geometry channel is the in-process half of ADR-036 **D6**. ADR-033 is
unchanged by it: the placement authority reports *geometry*; the weaver still
owns all *phase*, snapping included.

## 2. API Reference

### 2.1 Extension name and constants

```c
#define XR_DXR_android_surface_binding 1
#define XR_DXR_android_surface_binding_SPEC_VERSION 1
#define XR_DXR_ANDROID_SURFACE_BINDING_EXTENSION_NAME "XR_DXR_android_surface_binding"

#define XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999005)
#define XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR             ((XrStructureType)1004999220)
```

`1004999005` is inherited from the (previously unimplemented) sketch this
extension replaces — it sits in an unused gap of the `XR_DXR_display_info`
decade rather than in this extension's own `1004999220–229`. Both rows are
recorded in the header directory's allocation registry.

### 2.2 XrAndroidSurfaceBindingCreateInfoDXR

Chained into `XrSessionCreateInfo::next` (alongside, or through, the Vulkan
graphics binding).

```c
typedef struct XrAndroidSurfaceBindingCreateInfoDXR {
    XrStructureType             type;
    const void* XR_MAY_ALIAS    next;
    struct ANativeWindow*       nativeWindow;
    void*                       surface;                      // jobject android.view.Surface
    int32_t                     screenOffsetX;
    int32_t                     screenOffsetY;
    XrBool32                    transparentBackgroundEnabled;
} XrAndroidSurfaceBindingCreateInfoDXR;
```

- **`nativeWindow`** — canonical. From `ANativeWindow_fromSurface()`,
  `ASurfaceHolder_getNativeWindow()`, or `android_app::window` in a
  NativeActivity app.
- **`surface`** — optional `jobject` for the Java `android.view.Surface`, as an
  opaque `void*` so the header stays JNI-free. Used only when `nativeWindow` is
  `NULL`, in which case the runtime resolves it with `ANativeWindow_fromSurface()`
  on a JVM-attached thread. Should be a global reference.
  **At least one of the two must be non-NULL.**
- **`screenOffsetX` / `screenOffsetY`** — initial on-screen origin in physical
  pixels of the current rotation, i.e. `View.getLocationOnScreen()`. Seeds the
  weave phase and the per-window Kooima until the first geometry call. `0,0` is
  the ordinary fullscreen value.
- **`transparentBackgroundEnabled`** — as in the win32/xlib siblings; the app is
  responsible for asking for a translucent `SurfaceHolder` format.

**Ownership.** The runtime takes its *own* `ANativeWindow_acquire()` reference
and releases it when the binding is replaced or the session is destroyed
(runtime#1040's refcount invariant — a consumer that intends to *keep* a window
acquires it; it never adopts somebody else's reference). The app still owns the
Surface and must outlive the session.

**The app must never draw into the bound surface.** One `BufferQueue` has one
producer, and a single `Surface.lockCanvas()` poisons it for GL/VK permanently.

### 2.3 XrAndroidWindowGeometryDXR

```c
typedef struct XrAndroidWindowGeometryDXR {
    XrStructureType             type;
    const void* XR_MAY_ALIAS    next;
    XrRect2Di                   windowRect;   // current rotation, physical px, y down
    XrExtent2Di                 panelExtent;  // panel extent in the SAME rotation
    int32_t                     displayId;
} XrAndroidWindowGeometryDXR;
```

`panelExtent` is not redundant with the runtime's own display info: that
describes the panel in its **natural** orientation (this device class is
natively portrait and runs landscape), and a sub-panel window fits inside both
orderings — so the held orientation is genuinely unrecoverable from the rect
alone. `Display.getRealSize()` is the call that yields it.

### 2.4 Functions

```c
XrResult xrSetAndroidSurfaceDXR(XrSession session,
                                const XrAndroidSurfaceBindingCreateInfoDXR *binding);
XrResult xrSetAndroidWindowGeometryDXR(XrSession session,
                                       const XrAndroidWindowGeometryDXR *geometry);
```

`xrSetAndroidSurfaceDXR` with `binding == NULL`, or with both `nativeWindow` and
`surface` NULL, reports **surface lost**: the runtime drops its VkSurfaceKHR and
pauses the display processor (the DP's `on_pause`, which under ADR-036 D7
releases the 3D-lens preference rather than forcing 2D). The next non-NULL call
rebuilds and resumes. Both calls are idempotent and de-duplicated; the geometry
call is cheap enough for once per frame.

Errors: `XR_ERROR_HANDLE_INVALID`, `XR_ERROR_VALIDATION_FAILURE`,
`XR_ERROR_FUNCTION_UNSUPPORTED` (extension not enabled at instance create).

## 3. Runtime Behavior

1. **Session create.** `oxr_session.c` reads the chained binding, resolves
   `surface` → `ANativeWindow` if needed, acquires a reference, seeds the window
   rect from `screenOffset*`, and puts the window in
   `xrt_session_info::external_window_handle`.
2. **Compositor.** `oxr_session_gfx_vk_native.c` hands that window straight to
   `comp_vk_native_compositor_create`. Only when it is `NULL` does the legacy
   runtime-spawned `MonadoView` path run (the `_hosted` fallback).
3. **Publish/loss.** Both create and `xrSetAndroidSurfaceDXR` publish through
   `android_globals_set_window` / `android_globals_clear_window`, which is the
   same channel `comp_vk_native_target` already polls per frame to rebuild its
   `VkSurfaceKHR` + swapchain (#507/#528). No new re-sync machinery.
4. **Geometry.** `xrSetAndroidWindowGeometryDXR` lands in
   `android_globals_set_window_screen_rect` — the same sink the out-of-process
   client feeds over `IMonado.updateWindowRect` (#1033). The in-process
   compositor then does, per frame:
   - `xrt_display_processor_vk_set_window_screen_rect(...)` before
     `process_atlas` — the DP's interlace phase (mirrors
     `comp_multi_system.c::update_window_screen_rect`);
   - fills `xrt_window_metrics` from the same rect in
     `comp_vk_native_compositor_get_window_metrics`, which is what
     `oxr_session.c`'s Kooima block already consumes — so the canvas becomes the
     window's metres and the render eyes are rebased to the window centre
     (the in-process twin of `ipc_try_get_oop_view_poses`, #1034).
5. **No rect ever published** ⟹ every step above degrades to the display-scoped
   behaviour that shipped before, unchanged.

**Threading.** The vendor display processor is created on the thread that calls
`xrCreateSession`, and some vendor SDKs require that thread to have a prepared
`ALooper` that keeps being pumped afterwards. `native_app_glue`'s `android_main`
thread and a Java UI thread both satisfy this; a bare pthread render thread (the
engine shape) does not. The runtime WARNs with that guidance rather than hanging.

## 4. Application Responsibilities

- Own a `SurfaceView` (**never** a `TextureView` — it composites through the
  view hierarchy and loses the direct SurfaceFlinger layer the weave needs), or
  use a `NativeActivity`, whose `android_app::window` is the activity window.
- Chain the binding at `xrCreateSession`; republish on
  `surfaceCreated`/`surfaceDestroyed` (`APP_CMD_INIT_WINDOW` /
  `APP_CMD_TERM_WINDOW`).
- Publish geometry from a `Choreographer` callback — `View.getLocationOnScreen()`
  + `getWidth()/getHeight()` + `Display.getRealSize()` + `getDisplayId()`.
- Declare in the app manifest:

  ```xml
  <property android:name="android.window.PROPERTY_COMPAT_ALLOW_SANDBOXING_VIEW_BOUNDS_APIS"
            android:value="false" />
  ```

  Without it an OEM applying `OVERRIDE_SANDBOX_VIEW_BOUNDS_APIS` makes
  `getLocationOnScreen()` window-relative, so **every** window reports (0,0) —
  silently, with no error anywhere, and a side-by-side pair weaves at the same
  wrong phase.
- Declare `android:resizeableActivity="true"`, no fixed orientation and no fixed
  aspect, and never call `SurfaceHolder.setFixedSize` — server-side compat
  scaling is invisible to the client and destroys per-pixel interlacing.

## 5. Reference Implementation

| Layer | File |
|---|---|
| Header | `src/external/openxr_includes/openxr/XR_DXR_android_surface_binding.h` |
| Extension gate | `src/xrt/state_trackers/oxr/oxr_extension_support.h` |
| Entry points | `src/xrt/state_trackers/oxr/oxr_android_surface.c`, `oxr_api_negotiate.c` |
| Binding parse | `src/xrt/state_trackers/oxr/oxr_session.c` |
| Compositor bring-up | `src/xrt/state_trackers/oxr/oxr_session_gfx_vk_native.c` |
| Rect → DP + Kooima | `src/xrt/compositor/vk_native/comp_vk_native_compositor.c` |
| Window/rect globals | `src/xrt/auxiliary/android/android_globals.{h,cpp}` |
| Example app | `test_apps/handle/cube_handle_vk_android` |

## 6. Out of Scope / Future

- **Offscreen readback / shared-texture (`_texture` class) handoff** — the
  win32/cocoa siblings carry those fields; Android does not yet.
- **`AttachedSurfaceControl.getBufferTransformHint()`** (API 31) — honouring the
  hint would let the app pre-rotate; today the runtime uses an identity
  pretransform.
- **Runtime-side geometry polling** — the runtime could offer its `MonadoView`
  `Choreographer` logic as a reusable Java utility so apps do not reimplement it.

## 7. Revision History

| Version | Change |
|---|---|
| 1 | Initial implementation (#1037). Replaces the "planned, not implemented" sketch in `XR_DXR_display_info.md` §4; adds `transparentBackgroundEnabled` to that sketch's struct, plus `XrAndroidWindowGeometryDXR`, `xrSetAndroidSurfaceDXR` and `xrSetAndroidWindowGeometryDXR`. |
