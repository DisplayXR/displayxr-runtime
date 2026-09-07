<!--
Copyright 2026, The DisplayXR Project
SPDX-License-Identifier: Apache-2.0
-->

# XR_DXR_android_surface_binding

**Status:** Implemented (runtime#1037, ADR-036 D2/D6; layout hint runtime#1396) · **Spec version:** 2
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

And one event travels the other way — `XrEventDataAndroidWindowLayoutHintDXR`
(spec v2, §2.5). It exists because escaping an OEM-scaled "mini-window" needs a
measurement only the runtime does and a layout change only the app can make.


The geometry channel is the in-process half of ADR-036 **D6**. ADR-033 is
unchanged by it: the placement authority reports *geometry*; the weaver still
owns all *phase*, snapping included.

## 2. API Reference

### 2.1 Extension name and constants

```c
#define XR_DXR_android_surface_binding 1
#define XR_DXR_android_surface_binding_SPEC_VERSION 2
#define XR_DXR_ANDROID_SURFACE_BINDING_EXTENSION_NAME "XR_DXR_android_surface_binding"

#define XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR   ((XrStructureType)1004999005)
#define XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR               ((XrStructureType)1004999220)
#define XR_TYPE_EVENT_DATA_ANDROID_WINDOW_LAYOUT_HINT_DXR ((XrStructureType)1004999221)
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

### 2.5 XrEventDataAndroidWindowLayoutHintDXR (spec v2, #1396)

```c
#define XR_TYPE_EVENT_DATA_ANDROID_WINDOW_LAYOUT_HINT_DXR ((XrStructureType)1004999221)

typedef struct XrEventDataAndroidWindowLayoutHintDXR {
    XrStructureType             type;
    const void* XR_MAY_ALIAS    next;
    XrSession                   session;
    XrBool32                    active;        // XR_FALSE = restore, every other field 0
    float                       scale;         // measured container scale, informational
    XrExtent2Di                 layoutSize;    // logical size to lay the window out at
    XrExtent2Di                 bufferSize;    // fixed buffer size, physical panel px
    XrRect2Di                   physicalRect;  // republish this through xrSetAndroidWindowGeometryDXR
} XrEventDataAndroidWindowLayoutHintDXR;
```

#### Why it exists

Some OEM multi-window shells ("mini-window", "window reply", freeform-with-scale)
do **not** give the task a smaller window. They give it a full-size *logical*
window and shrink the whole task with a SurfaceFlinger leash. Measured on the
reference A13 tablet: the task layer carries `geomLayerTransform (SCALE
TRANSLATE)` with scale `0.67` at `(1757,236)`, so a `1080x1685` logical window
lands on the panel as `723x1129` physical pixels, with
`forceClientComposition=true`.

A weave is an *interlace*: the vendor display processor is strict 1:1
buffer→panel, and **any** resample between the woven buffer and the panel
destroys it (bilinear filtering is not invertible). The runtime detects this
placement — the `CONTAINER_SCALED` tell, `x < 0 || y < 0 || x+w > panelW ||
y+h > panelH`, which fires because the window reports its *logical* extent at
its *physical* origin — and degrades honestly to flat 2D.

The escape is to make SurfaceFlinger's **composed** transform identity rather
than to fight the leash: give the surface a buffer of `round(logical × scale)`
pixels, and the buffer→layer scale (`1080/723`) times the leash (`0.67`)
multiplies out to `1.0`. That needs two halves, and only the application owns
one of them:

| Half | Who can do it |
|---|---|
| The **buffer** size | either side — `ANativeWindow_setBuffersGeometry()` works on the bound window |
| The **layout** size of the view/window | **only the app** — the runtime does not own its view hierarchy |

The layout half is not optional, and it is not cosmetic. `1080 × 0.67 = 723.6`;
rounding to 724 composes to `0.9994`, ≈0.4 px of drift across the window, which
reads as *a slight double image in both eyes*. (Both eyes equally is the
signature of a residual **resample**; a phase error blurs one eye only.) There
is no integer near `1080 × 0.67`, so the layout itself must move.

Hence: the runtime keeps the policy, emits this event, and the app applies it.

#### The measured rule the runtime applies

1. **Get the scale.** Preferred source is the OEM's own API, read by reflection
   from the Activity the app passed in `XrInstanceCreateInfoAndroidKHR::applicationActivity`
   (on the reference device `ActivityManager.getDefaultWindowParamByTaskForNormalWr(taskId)`,
   a non-blocklisted test-API returning the **post-scale** on-screen `Rect`).
   **The read is gated on the tell** and that gate is load-bearing: the method
   answers the *nominal* mini-window placement even in fullscreen.
   The raw `Rect` is kept, not just the float — it is quantised to whole pixels
   and is the only evidence that actually pins the scale.
2. **Rationalise it to `p/q`, trying `q = 100` FIRST.** An OEM window scale is a
   round percentage (0.67; SurfaceFlinger prints the leash as `0.6700`).
   Smallest-`q`-wins alone picks the *wrong* fraction: inside the `Rect`'s ±3e-4
   band, `63/94 = 0.670213` reproduces the `Rect` exactly and has a smaller `q`
   than `67/100`, while composing to `0.99968` instead of `1`.
3. **Search for the layout**, per axis: the **largest** `L ≤ W` within `q` of the
   window for which `|L·p/q − round(L·p/q)| ≤ 0.1 px`. On the reference device
   that is `1079` wide (`722.93`, e = 0.07 px → buffer `723`) and the **full**
   `1685` high (`1128.95`, e = 0.05 px → buffer `1129`). The border collapses to
   **one logical pixel**.
   *Tolerance calibration:* 0.4 px drift is a visible double image; ≤0.07 px is
   invisible. Both shipping residuals have been on the panel without complaint.
4. **Never overscan.** A layout *larger* than the window does not work: a
   SurfaceView bigger than its window has its surface sized to the **visible
   frame**, so a `737x1139` buffer got mapped into `723.6x1128.95` screen px —
   composed `0.982`, a 2 % resample, and the double image came straight back.
   Round-down to multiples of `q` also fails a different way: it costs a 54 px
   (5 %) black strip.
5. **Never size to SurfaceFlinger's `displayFrame`** (`732x1137` here). That
   includes the task layer's shadow (`shadowRadius` 6 × 0.67 ≈ 4 px a side);
   sizing to it re-introduces an 8 px resample — exactly the thing being removed.

One implementation of all of the above ships in the runtime
(`org.freedesktop.monado.auxiliary.MiniWindowLayout`) and is shared verbatim with
the runtime's own hosted `MonadoView`, so the two paths cannot drift.

#### Application obligations

On `active == XR_TRUE`:

1. Lay the window / content view out at `layoutSize`, anchored top-left. A
   `NativeActivity` has no content view — its Surface *is* the activity window's
   surface — so the layout half is
   `getWindow().setLayout(w, h)` with `gravity = TOP|START`. An app with its own
   `SurfaceView` sets that view's `LayoutParams` instead.
2. Fix the buffer to `bufferSize` — `ANativeWindow_setBuffersGeometry(win, w, h, 0)`
   or `SurfaceHolder.setFixedSize(w, h)`. **Re-assert it after every surface
   recreate**; a window resize destroys and rebuilds the Surface and the override
   goes with it.
3. Publish `physicalRect` through `xrSetAndroidWindowGeometryDXR` — the app's own
   live `View.getLocationOnScreen()` origin (which is already on-screen
   coordinates in a scaled container) with `bufferSize` as the extent. Do it only
   **once the override has taken** (`ANativeWindow_getWidth/Height` report
   `bufferSize`), so the runtime never weaves at a size the buffer does not have.
   Until then keep publishing the logical rect — that keeps the honest 2D
   fallback, which is the direction to fail in.
4. Paint the one-pixel right/bottom remainder black if anything shows through.

On `active == XR_FALSE`: restore the ordinary match-parent layout, drop the
fixed buffer size (`ANativeWindow_setBuffersGeometry(win, 0, 0, 0)`), and go back
to publishing the logical rect.

**Ignoring the event is legal.** An app that does nothing keeps the runtime's 2D
fallback in the scaled container, exactly as before spec v2. There is no
`XR_ERROR_*` for not applying a hint.

#### Runtime behaviour

- The hint is evaluated inside `xrSetAndroidWindowGeometryDXR`, against the rect
  the app just published and the panel extent it carried. No panel extent ⟹ no
  evaluation (never decide on ignorance).
- **One event per distinct answer.** Once a hint is active the app publishes the
  *physical* rect, which fits the panel and therefore no longer trips the tell;
  the runtime recognises "the hint is applied" by the published **extent** equal
  to `bufferSize` (the origin keeps changing as the window is dragged) and
  latches. A published extent that is neither the buffer size nor container-scaled
  ends the hint with `active == XR_FALSE`.
- **Re-emitted** at `xrBeginSession` and on every `xrSetAndroidSurfaceDXR`
  republish while a hint is active, so an app that starts — or resumes — already
  inside a scaled container never misses it.
- If the app keeps publishing a logical rect after a hint was delivered, the
  runtime logs that **once** and stays on the 2D fallback. It does not re-push.
- The compositor is unchanged: its `CONTAINER_SCALED` 2D fallback is exactly the
  behaviour an app that does not act on the hint keeps.

#### Limitation: no touch-ratio fallback on this path

The hosted `MonadoView` has a second, vendor-independent way to measure the
scale: on a real dispatched drag, `|Δraw| / |Δlocal|` at
`dispatchTouchEvent` **is** the container scale (measured `0.670000`, residual
1e-4 px over 100 samples), because the platform inverts the leash on the way in.
That needs the app's `MotionEvent`s, and on the surface-binding path the runtime
sees none of them — the app owns the view.

So this path is **vendor-API only**. On a device with a scaled container and no
readable OEM API, the runtime logs once and leaves the app on the 2D fallback
rather than guessing. Forwarding an app-measured ratio is deliberately **not**
part of spec v2: it would add API surface for a case that does not exist on any
shipping device, and the app can always apply its own layout if it wants to.
(Synthetic `MotionEvent.obtain()` events carry an identity transform and read
`1.0`, and accessibility magnification multiplies in — which is why the hosted
path cross-checks the two sources and refuses to pick one when they disagree.)

#### Debug

`debug.dxr.miniwindow_1to1` (default **on**, read **once per process**) disables
the whole mechanism, on both paths, for an A/B against the 2D fallback. It is
cached deliberately: flipping it live re-sizes a surface underneath an in-flight
weave, and the vendor `leia_cnsdk_weave` then parks forever in `vkWaitForFences`.

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
  aspect, and do not call `SurfaceHolder.setFixedSize` **on your own initiative**
  — server-side compat scaling is invisible to the client and destroys per-pixel
  interlacing. The one legitimate use is applying a
  `XrEventDataAndroidWindowLayoutHintDXR` (§2.5), where the runtime has measured
  the scale and the fixed size is what *removes* the resample.
- Handle `XrEventDataAndroidWindowLayoutHintDXR` if you want to weave inside an
  OEM-scaled multi-window container (§2.5). Optional — ignoring it keeps the
  runtime's 2D fallback there.

## 5. Reference Implementation

| Layer | File |
|---|---|
| Header | `src/external/openxr_includes/openxr/XR_DXR_android_surface_binding.h` |
| Extension gate | `src/xrt/state_trackers/oxr/oxr_extension_support.h` |
| Entry points | `src/xrt/state_trackers/oxr/oxr_android_surface.c`, `oxr_api_negotiate.c` |
| Binding parse | `src/xrt/state_trackers/oxr/oxr_session.c` |
| Compositor bring-up | `src/xrt/state_trackers/oxr/oxr_session_gfx_vk_native.c` |
| Rect → DP + Kooima | `src/xrt/compositor/vk_native/comp_vk_native_compositor.c` |
| Layout-hint policy (shared with `MonadoView`) | `src/xrt/auxiliary/android/src/main/java/org/freedesktop/monado/auxiliary/MiniWindowLayout.java` |
| Layout-hint JNI bridge | `src/xrt/auxiliary/android/android_mini_window.{h,cpp}` |
| Layout-hint state machine + event | `src/xrt/state_trackers/oxr/oxr_android_surface.c`, `oxr_event.c` |
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
| 2 | Adds `XrEventDataAndroidWindowLayoutHintDXR` (#1396): the runtime measures an OEM container scale and hands the app the exact-integer layout + buffer size that makes the composition 1:1, so an app that owns its Surface can weave inside a scaled mini-window instead of falling back to 2D. Purely additive — no existing struct, function or behaviour changes. |
| 1 | Initial implementation (#1037). Replaces the "planned, not implemented" sketch in `XR_DXR_display_info.md` §4; adds `transparentBackgroundEnabled` to that sketch's struct, plus `XrAndroidWindowGeometryDXR`, `xrSetAndroidSurfaceDXR` and `xrSetAndroidWindowGeometryDXR`. |
