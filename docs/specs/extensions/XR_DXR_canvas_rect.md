# XR_DXR_canvas_rect

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_canvas_rect` |
| Spec Version | 1 |
| Type Values | `XR_TYPE_CANVAS_RECT_BINDING_DXR` (1004999220) |
| Author | DisplayXR |
| Platform | Platform-neutral surface; honored on the D3D11 compositors (in-process + service) today. Other backends report `XR_ERROR_FUNCTION_UNSUPPORTED` from the setter and keep the window-binding path. |
| Issue | [#697](https://github.com/DisplayXR/displayxr-runtime/issues/697) |

---

## 1. Overview

`XR_DXR_canvas_rect` lets a present-owning / windowless producer **declare the on-panel rectangle where its content appears** — directly, as a display-relative pixel rect — instead of the runtime deriving it from a bound OS window.

The lenticular weave **phase** and the off-axis (**Kooima**) projection are both functions of *where on the panel the content lands*. For a windowed app the runtime learns that rect by reading the bound window's client rect (`GetClientRect` + `ClientToScreen`) each locate/weave; the window is a **position/phase anchor, never a render target** (see [`docs/getting-started/app-classes.md`](../../getting-started/app-classes.md) and [`XR_DXR_win32_window_binding` §2.4](XR_DXR_win32_window_binding.md#24-the-phase-alignment-problem)). A genuinely windowless producer — a WebXR bridge, a browser/CEF embedder, a capture/stream target, an engine offscreen renderer — has no such window, so before this extension it degraded to **display-scoped** framing (the whole panel) and lost phase alignment.

This extension supplies the missing input: the app hands the runtime the canvas rect itself. It is **orthogonal to, and usable without, any window binding** — enabling it does not require `XR_DXR_win32_window_binding` et al.

---

## 2. Motivation

The HWND does two position-only jobs for a texture producer (neither of which is a render target):

1. **Canvas-scoped Kooima** — `get_window_metrics` turns the client rect into a panel-relative canvas (`canvasRectPx`, `canvasSizeMeters`, per-eye off-axis offsets). No HWND → metrics unavailable → Kooima falls back **display-scoped**, mis-framing the projection.
2. **Weave phase** — phase is a function of the canvas's absolute screen position; the DP tracks the HWND to keep the interlace phase-locked, and (as an optional vendor optimization) snaps window drags to phase-aligned coordinates.

**Neither job needs an HWND for its math.** The Kooima inputs derive entirely from the canvas rect plus what the runtime already knows from [`XR_DXR_display_info`](XR_DXR_display_info.md) (panel origin, physical size, pixel pitch); drag phase-snap ([`XR_DXR_weave`](../../../src/external/openxr_includes/openxr/XR_DXR_weave.h)'s `xrWeaveSnapWindowRectDXR`) is already pure absolute-screen-coordinates. The one thing an HWND uniquely provides is **self-tracking** — the OS updates it as it moves/resizes and the runtime re-reads it live.

`XR_DXR_canvas_rect` trades that for **app-owned rect updates**: the app seeds the rect at session create and calls `xrSetCanvasRectDXR` whenever its canvas moves or resizes. No phase math is lost; the app simply owns the tracking the OS used to do.

---

## 3. API Reference

### 3.1 Defines

```c
#define XR_DXR_canvas_rect                          1
#define XR_DXR_canvas_rect_SPEC_VERSION             1
#define XR_DXR_CANVAS_RECT_EXTENSION_NAME           "XR_DXR_canvas_rect"
#define XR_TYPE_CANVAS_RECT_BINDING_DXR             ((XrStructureType)1004999220)
```

### 3.2 Coordinate contract

`rectPx` is in **display-relative device pixels**: the origin `(0,0)` is the 3D panel's top-left, `+x` right, `+y` down. This is the runtime's canonical internal form (`canvasRectPx = window_screen − display_screen`), so the runtime stores it with zero conversion. The panel's desktop origin is available to the app via [`XR_DXR_display_info`](XR_DXR_display_info.md)'s `XrDisplayDesktopPositionDXR`, should the app need to convert between desktop and panel-relative coordinates.

The physical size of the canvas is **derived** by the runtime from the display's pixel pitch (`displaySizeMeters / displayPixel*`); the app supplies pixels only — no meters to compute.

### 3.3 XrCanvasRectBindingDXR

```c
typedef struct XrCanvasRectBindingDXR {
    XrStructureType          type;   // Must be XR_TYPE_CANVAS_RECT_BINDING_DXR
    const void* XR_MAY_ALIAS next;
    XrRect2Di                rectPx; // display-relative device px; origin = panel top-left
} XrCanvasRectBindingDXR;
```

**Chaining:** Optional. Placed in the `next` chain of `XrSessionCreateInfo` (e.g. via the graphics binding's `next`). Seeds the canvas rect so the very first `xrLocateViews` / weave frame is correctly framed before the app's first `xrSetCanvasRectDXR` call. A producer whose canvas never moves may seed here and never call the setter.

| Field | Description |
|-------|-------------|
| `type` | Must be `XR_TYPE_CANVAS_RECT_BINDING_DXR` |
| `next` | `NULL` or pointer to next structure in chain |
| `rectPx` | Initial canvas rect (display-relative device px). A non-positive extent is ignored (no seed). |

### 3.4 xrSetCanvasRectDXR

```c
XrResult xrSetCanvasRectDXR(XrSession session, const XrRect2Di* rectPx);
```

Set (or update) the session's canvas rect. Call whenever the canvas moves or resizes. Pass `rectPx == NULL` to **clear** the override, reverting to the bound window (if any) else display-scoped framing. Valid any time after `xrCreateSession`.

| Result | Meaning |
|--------|---------|
| `XR_SUCCESS` | Override set/cleared. |
| `XR_ERROR_VALIDATION_FAILURE` | `rectPx` non-NULL with a non-positive extent. |
| `XR_ERROR_FUNCTION_UNSUPPORTED` | The compositor backend does not implement the override (currently anything but D3D11 in-process / service). |

---

## 4. Usage

```c
// Enable the extension (+ a graphics binding, + XR_DXR_display_info for the panel geometry).
// A windowless producer passes NO XR_DXR_win32_window_binding windowHandle.

// 1. Seed the canvas rect at session create (frame-0 correctness).
XrCanvasRectBindingDXR canvas = { XR_TYPE_CANVAS_RECT_BINDING_DXR };
canvas.rectPx = (XrRect2Di){ {x, y}, {w, h} };   // display-relative px
canvas.next   = &graphicsBinding;                // chain onto XrSessionCreateInfo

XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
sci.next = &canvas;
sci.systemId = systemId;
xrCreateSession(instance, &sci, &session);

// 2. On every canvas move/resize, push the new rect.
XrRect2Di r = { {x, y}, {w, h} };
xrSetCanvasRectDXR(session, &r);

// 3. To relinquish the override (revert to bound-window / display-scoped):
xrSetCanvasRectDXR(session, NULL);
```

The rect is consumed at `xrLocateViews` (for the canvas-scoped Kooima projection returned in `XrView` / `XrViewDisplayRawDXR::canvasRectPx`) and at weave time. Set or update it **before** the locate whose framing it should affect.

### 4.1 Drag phase-snap (optional)

A windowless producer that moves its own canvas keeps the woven phase locked by driving [`XR_DXR_weave`](../../../src/external/openxr_includes/openxr/XR_DXR_weave.h)'s `xrWeaveSnapWindowRectDXR` from its own move handler (the snap is pure absolute-screen-coordinates, computed by a service-owned probe window — it never references the app's HWND) and calling `xrSetCanvasRectDXR` with the snapped rect. This is the app-driven equivalent of the automatic HWND-subclass snap a windowed app gets for free.

---

## 5. Implementation

| Layer | File(s) |
|---|---|
| Extension header | `src/external/openxr_includes/openxr/XR_DXR_canvas_rect.h`; included by `src/xrt/include/xrt/xrt_openxr_includes.h`; registry `.../openxr/README.md` |
| OpenXR API | `oxr_canvas_rect.c` (setter dispatch → in-proc D3D11 or IPC bridge), `oxr_session.c` (create-time seed parse into `xrt_session_info`), `oxr_api_negotiate.c` / `oxr_api_funcs.h` / `oxr_extension_support.h` |
| Session carry | `xrt_session_info` canvas-rect fields (`xrt_compositor.h`); crosses IPC verbatim in the `session_create` payload |
| In-process D3D11 | `comp_d3d11_compositor.{h,cpp}`: `comp_d3d11_compositor_set_canvas_rect`, override branch in `comp_d3d11_compositor_get_window_metrics` |
| Service / IPC | `comp_d3d11_service.{h,cpp}`: `comp_d3d11_service_set_canvas_rect`, override branch in `comp_d3d11_service_get_client_app_window_metrics`; IPC `set_canvas_rect` (`proto.json`, `ipc_client_compositor.c`, `ipc_server_handler.c`) |

The override is injected at the single `get_window_metrics` producer in each of the in-process and service paths: when a canvas rect is present, the producer synthesizes the `xrt_window_metrics` window group from the rect (`window_screen = display_screen + rect.offset`, `window_pixel = rect.extent`) and skips the `GetClientRect` / `ClientToScreen` read. Every downstream `canvasRectPx` / `canvasSizeMeters` / Kooima expression is unchanged — feeding the window group correctly at the producer makes the in-process, service, and IPC rig-reply routes all emit the right framing.

---

## 6. Relationship to other extensions

- **`XR_DXR_win32_window_binding`** — the two are alternatives for supplying the position/phase anchor. A texture producer that owns a window may still prefer the window binding (the OS tracks the rect for free). This extension is for producers that would rather declare the rect than bind a window. Both can be enabled; a valid canvas-rect override takes precedence over the HWND in `get_window_metrics`.
- **`XR_DXR_display_zones`** — declares 2D/3D regions *within* the canvas (client-window/canvas pixels). Orthogonal: `XR_DXR_canvas_rect` declares where the canvas itself sits on the panel; display-zones subdivides it.
- **`XR_DXR_weave`** — the present-owner weave service. Its `xrWeaveSnapWindowRectDXR` (absolute-screen-coords) is how a windowless producer drives phase-snap. A symmetric rect-based `xrWeaveBindRectDXR` is a natural future addition.

---

## 7. Future directions

- **Symmetric weave-service rect bind** — `xrWeaveBindRectDXR` alongside `xrWeaveBindWindowDXR`, so a windowless present-owner drives the synchronous weave service with the same rect concept.
- **Additional backends** — D3D12, GL, Vulkan, Metal `get_window_metrics` producers gain the same override branch; the header is already platform-neutral.
- **Explicit physical size override** — a `next`-chain struct carrying `XrExtent2Df sizeMeters`, for a producer targeting a non-native pixel pitch. Today the runtime derives meters from the panel pitch.
