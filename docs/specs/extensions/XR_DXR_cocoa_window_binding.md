# XR_DXR_cocoa_window_binding

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_cocoa_window_binding` |
| Spec Version | 7 |
| Type Values | `XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR` (1004999004), `XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR` (1004999002) |
| Author | Leia Inc. |
| Platform | macOS (Cocoa / AppKit). |

---

## 1. Overview

`XR_DXR_cocoa_window_binding` is the macOS counterpart to `XR_DXR_win32_window_binding`. It allows an OpenXR application to provide its own `NSView*` (backed by a `CAMetalLayer`) to the runtime when creating a session. When present, the runtime renders into the application's view instead of creating its own window.

The target use case is **desktop 3D light field displays** on macOS, where the application controls the window lifecycle, input handling, and event loop.

**Header:** `src/external/openxr_includes/openxr/XR_DXR_cocoa_window_binding.h`

---

## 2. Motivation

Same as `XR_DXR_win32_window_binding` — desktop 3D displays are monitors, not headsets. The application needs to own its window for input handling, multi-app compositing, and hybrid 2D/3D UI. On macOS, the runtime uses Metal (via MoltenVK for Vulkan apps) and needs a `CAMetalLayer`-backed view to render into.

**Phase alignment note:** Lenticular displays compute interlacing patterns from the content's screen-space position. The display processor needs the NSView to track its position on the physical display each frame. On Windows, advanced vendors hook `WM_WINDOWPOSCHANGING` to snap window drag to phase-aligned coordinates (see [XR_DXR_win32_window_binding §2.4](XR_DXR_win32_window_binding.md)). macOS equivalents (e.g., `NSWindowDelegate` position tracking) are a future item for vendor SDKs that support macOS. For IOSurface-based `_texture` apps, the canvas sub-rect from `xrSetSharedTextureOutputRectDXR` flows through the compositor to the display processor's `process_atlas()` call as `canvas_offset_x/y` and `canvas_width/height`, enabling correct phase correction. The app's real NSView is passed directly to the display processor -- no hidden windows are involved.

---

## 3. New Enum Constants

```c
#define XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR ((XrStructureType)1004999004)
```

This extension also uses the shared window-space composition layer type:

```c
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR ((XrStructureType)1004999002)
```

> **Note:** The type value was changed from `1004999003` to `1004999004` in the docs refactor
> to resolve a collision with `XR_TYPE_DISPLAY_INFO_DXR`.

---

## 4. New Structures

### XrCocoaWindowBindingCreateInfoDXR

Chained to `XrSessionCreateInfo` (via the graphics binding's `next` pointer) to provide an external view handle for session rendering on macOS.

| Member | Type | Description |
|---|---|---|
| `type` | `XrStructureType` | Must be `XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR`. |
| `next` | `const void*` | Pointer to next structure in the chain, or `NULL`. |
| `viewHandle` | `void*` | `NSView*` with `CAMetalLayer` backing, or `NULL` for offscreen mode. |
| `readbackCallback` | `PFN_xrReadbackCallback` | Callback receiving composited RGBA pixels (offscreen mode), or `NULL`. |
| `readbackUserdata` | `void*` | Opaque pointer passed to `readbackCallback`. |
| `sharedIOSurface` | `void*` | `IOSurfaceRef` for zero-copy Metal texture sharing, or `NULL`. |
| `transparentBackgroundEnabled` | `XrBool32` | When `XR_TRUE`, the runtime configures the (runtime-owned) `NSWindow` and the `CAMetalLayer` with `isOpaque=NO` so the desktop shows through alpha < 1 regions of the composited output. App-owned windows must set `setOpaque:NO` themselves; the runtime only configures the layer it presents into. *(Spec v5.)* |

```c
typedef void (*PFN_xrReadbackCallback)(
    const uint8_t *pixels, uint32_t width, uint32_t height, void *userdata);

typedef struct XrCocoaWindowBindingCreateInfoDXR {
    XrStructureType          type;
    const void* XR_MAY_ALIAS next;
    void*                    viewHandle;
    PFN_xrReadbackCallback   readbackCallback;
    void*                    readbackUserdata;
    void*                    sharedIOSurface;
    XrBool32                 transparentBackgroundEnabled; // v5
} XrCocoaWindowBindingCreateInfoDXR;
```

### Transparent Background (v5)

`transparentBackgroundEnabled = XR_TRUE` is the macOS analog of the Win32 binding's same-named field, but the implementation is much simpler:

- **Mac is alpha-native.** sim_display preserves per-pixel alpha through its output stage to the `CAMetalLayer` drawable. There is no chroma-key trick (no need for the `chromaKeyColor` field on the Win32 binding).
- App-supplied content with `alpha < 1` shows the desktop through. Antialiased edges look correct (true soft alpha — unlike typical lenticular hardware on Windows, which interlaces opaque RGB).
- Per-layer `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` controls whether the per-tile pass blends or stamps; that's a separate per-frame decision.
- For app-provided `viewHandle`, the app's `NSWindow` setup is the app's responsibility — the runtime only configures the `CAMetalLayer` it presents into. Apps embedding their own NSView should set `setOpaque:NO` and `setBackgroundColor:[NSColor clearColor]` on their NSWindow.

| Layer of the system | Does what |
|---|---|
| App | Submits RGBA texture with alpha < 1 in transparent regions. Sets the per-layer alpha-blend bit on its projection layer. |
| Runtime per-tile pass | Preserves alpha into the atlas (uses the alpha image view when the bit is set). |
| sim_display DP | Passes alpha through to the Metal drawable (no fill, no strip). |
| Cocoa | Composites the drawable against the desktop using the layer/window alpha settings. |

**Three Modes:**

| Mode | `viewHandle` | `sharedIOSurface` | App class | Behavior |
|------|:-:|:-:|---|---|
| **Handle** | NSView* | NULL | `_handle` | Runtime renders directly into the app's view |
| **Texture** | NSView* | IOSurfaceRef | `_texture` | Runtime composites the multi-zone result (3D zones + Local2D zones, declared via [`XR_DXR_display_zones`](XR_DXR_display_zones.md)) into the shared IOSurface. The NSView is still required for screen-space position tracking and phase alignment. The app blits the IOSurface into its view. |
| **Offscreen** | NULL | NULL | — | `readbackCallback` receives composited pixels. No view, no phase alignment. |

> **Important for `_texture` apps:** You **must** provide a valid `viewHandle` even though the runtime renders into the shared IOSurface, not the view. Without it, the display processor cannot compute correct interlacing alignment (see [XR_DXR_win32_window_binding §2.4](XR_DXR_win32_window_binding.md#24-the-phase-alignment-problem)). Where the 3D canvas appears within the view — and any 2D regions around it — is declared via [`XR_DXR_display_zones`](XR_DXR_display_zones.md) (one `XrDisplayZoneDXR` per 3D region, one `XrCompositionLayerLocal2DDXR` per 2D region).

**Valid Usage:**
- `type` **must** be `XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR`.
- If `viewHandle` is non-NULL, it **must** point to a valid `NSView` whose `-makeBackingLayer` returns a `CAMetalLayer`.
- The view **must** remain valid for the lifetime of the `XrSession`.
- The application **must** run the `NSApplication` event loop on the main thread.
- The application **must not** remove the view from its superview before calling `xrDestroySession`.

### XrCompositionLayerWindowSpaceDXR

Shared with `XR_DXR_win32_window_binding`. See that extension's spec for the full definition.

### Region expression — REMOVED in v7 (use `XR_DXR_display_zones`)

> **Removed in spec v7 (ADR-031).** The `xrSetSharedTextureOutputRectDXR` and
> `xrSetSharedTextureSurround2DEXT` entry points (Cocoa never had the D3D12 `…FenceEXT`
> variant) and the bespoke 2D-surround / output-rect mechanism are **deleted**.
> [`XR_DXR_display_zones`](XR_DXR_display_zones.md) is now the sole paradigm for expressing
> 2D/3D regions, including the degenerate single-region cases these calls used to cover (the
> former output rect = one `XrDisplayZoneDXR` 3D zone; the former 2D surround = one
> `XrCompositionLayerLocal2DDXR` zone covering the complement). See the degenerate single-zone
> mapping in [`XR_DXR_display_zones` §6](XR_DXR_display_zones.md) and
> [`docs/roadmap/surround-zones-deprecation.md`](../../roadmap/surround-zones-deprecation.md)
> for the deprecation history. Parity verified by `cube_zones_texture_metal_macos`.

---

## 5. Rendering Modes

### 5.1 Windowed Mode (viewHandle != NULL)

The application creates an `NSWindow` with an `NSView` subclass that returns a `CAMetalLayer` from `-makeBackingLayer`. The runtime (via MoltenVK or native Metal compositor) creates a `VkSurfaceKHR` or MTLDrawable from the layer and renders into it.

```objc
@implementation MetalView
- (CALayer *)makeBackingLayer {
    return [CAMetalLayer layer];
}
- (BOOL)wantsUpdateLayer { return YES; }
@end
```

### 5.2 Offscreen Readback Mode (viewHandle == NULL, readbackCallback != NULL)

The application provides no view. The runtime renders to an offscreen surface and delivers composited RGBA pixels to the callback each frame. Useful for:
- Headless rendering / testing
- Streaming composited output
- Integration with non-Cocoa rendering pipelines

### 5.3 Zero-Copy Shared Texture Mode (viewHandle == NULL, sharedIOSurface != NULL)

The application provides an `IOSurfaceRef`. The runtime creates a Metal texture backed by the IOSurface and renders into it. The application can then use the same IOSurface to create its own Metal texture — zero-copy GPU texture sharing.

Useful for:
- Texture apps (`_texture` class) where the app composites the final output
- Multi-process compositing via IOSurface

---

## 6. Session Creation Example

```objc
// 1. Create window with Metal-backed view
NSWindow *window = [[NSWindow alloc]
    initWithContentRect:NSMakeRect(100, 100, 1920, 1080)
    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
    backing:NSBackingStoreBuffered
    defer:NO];
MetalView *view = [[MetalView alloc] initWithFrame:window.contentView.bounds];
view.wantsLayer = YES;
window.contentView = view;
[window makeKeyAndOrderFront:nil];
```

```c
// 2. Create session with Cocoa window binding
XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
vkBinding.instance = vkInstance;
vkBinding.physicalDevice = vkPhysicalDevice;
vkBinding.device = vkDevice;
vkBinding.queueFamilyIndex = graphicsQueueFamily;
vkBinding.queueIndex = 0;

XrCocoaWindowBindingCreateInfoDXR cocoaBinding = {
    XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_DXR};
cocoaBinding.viewHandle = (__bridge void *)view;
cocoaBinding.readbackCallback = NULL;
cocoaBinding.readbackUserdata = NULL;
cocoaBinding.sharedIOSurface = NULL;

// Chain: sessionInfo -> vkBinding -> cocoaBinding
vkBinding.next = &cocoaBinding;

XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
sessionInfo.next = &vkBinding;
sessionInfo.systemId = systemId;

xrCreateSession(instance, &sessionInfo, &session);
```

---

## 7. Relationship to Other Extensions

| Extension | Relationship |
|---|---|
| `XR_DXR_win32_window_binding` | Windows counterpart — mutually exclusive (one per platform). Shares `XrCompositionLayerWindowSpaceDXR`. |
| `XR_EXT_android_surface_binding` | Android counterpart (planned, not yet implemented). |
| `XR_DXR_display_info` | Independent. Can be used alone (runtime-managed window) or together (app-owned view + display geometry + mode control). |

---

## 8. Compositor Integration

When this extension is used, the runtime routes the session through the appropriate native compositor:

| Graphics Binding | Compositor |
|---|---|
| `XrGraphicsBindingVulkanKHR` | Vulkan native compositor (`compositor/vk_native/`) via MoltenVK |
| `XrGraphicsBindingMetalKHR` (future) | Metal native compositor (`compositor/metal/`) |
| `XrGraphicsBindingOpenGLKHR` | OpenGL native compositor (`compositor/gl/`) |

The Metal compositor is the primary path on macOS. The Vulkan path works via MoltenVK but has known limitations (see `MoltenVK` notes in build docs).

---

## 9. Forward Compatibility

The Cocoa binding tracks `XR_DXR_win32_window_binding` for forward-compatibility intent. See [`XR_DXR_win32_window_binding §9.2`](XR_DXR_win32_window_binding.md#92-forward-compatibility-roadmap-additive-only) for the additive-only roadmap (DPI hints, async readiness, multi-canvas, vendor-private next chains) and [§9.3](XR_DXR_win32_window_binding.md#93-architectural-invariants-wont-change) for the architectural invariants apps can rely on. Any extension that lands on Win32 lands on Cocoa with parity unless explicitly noted.

## 10. Revision History

| Version | Date | Author | Changes |
|---|---|---|---|
| 1 | 2025-06-15 | David Fattal | Initial version with viewHandle |
| 2 | 2025-09-20 | David Fattal | Added readbackCallback for offscreen mode |
| 3 | 2026-01-10 | David Fattal | Added sharedIOSurface for zero-copy Metal texture sharing. Fixed type value collision (1004999003 → 1004999004). |
| 4 | 2026-04-24 | David Fattal | Read-back contract clarified: runtime writes the canvas region at `(x, y)` (not origin) in the shared IOSurface, matching `xrSetSharedTextureOutputRectDXR` args. Apps must sample at `uvOffset + uv * uvScale`. See ADR-010. |
| 6 | 2026-05-28 | David Fattal | Added `xrSetSharedTextureSurround2DEXT` (parity with `XR_DXR_win32_window_binding` v6). Lets `_texture` apps register a full-view 2D IOSurface whose pixels outside the canvas sub-rect are blitted into the target swapchain each frame. Enables apps on fixed-3D-zone displays to fill the surround region with full-resolution 2D content. *(Removed in v7.)* |
| 7 | 2026-06-21 | David Fattal | Removed `xrSetSharedTextureOutputRectDXR` + `xrSetSharedTextureSurround2DEXT` — superseded by [`XR_DXR_display_zones`](XR_DXR_display_zones.md) (ADR-031). See [`docs/roadmap/surround-zones-deprecation.md`](../../roadmap/surround-zones-deprecation.md). |
| 6 (erratum) | 2026-06-06 | David Fattal | Surround deltas corrected to match the shipped Metal implementation (#406): pixel format must match the multiview shared IOSurface (not "must be RGBA8Unorm"); dims are confirmed window-sized with re-register-on-resize; fill is window-clamped per #464 (window rect minus canvas, never the worst-case extent); sync is CFRetain + coherent IOSurface (no fence/use-count in-process). No version bump — text aligned to behavior, no ABI change. |
