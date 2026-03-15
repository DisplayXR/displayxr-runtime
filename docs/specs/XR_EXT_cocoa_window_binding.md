# XR_EXT_cocoa_window_binding

| Property | Value |
|----------|-------|
| Extension Name | `XR_EXT_cocoa_window_binding` |
| Spec Version | 3 |
| Type Values | `XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT` (1000999004), `XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT` (1000999002) |
| Author | Leia Inc. |
| Platform | macOS (Cocoa / AppKit). |

---

## 1. Overview

`XR_EXT_cocoa_window_binding` is the macOS counterpart to `XR_EXT_win32_window_binding`. It allows an OpenXR application to provide its own `NSView*` (backed by a `CAMetalLayer`) to the runtime when creating a session. When present, the runtime renders into the application's view instead of creating its own window.

The target use case is **desktop 3D light field displays** on macOS, where the application controls the window lifecycle, input handling, and event loop.

**Header:** `src/external/openxr_includes/openxr/XR_EXT_cocoa_window_binding.h`

---

## 2. Motivation

Same as `XR_EXT_win32_window_binding` — desktop 3D displays are monitors, not headsets. The application needs to own its window for input handling, multi-app compositing, and hybrid 2D/3D UI. On macOS, the runtime uses Metal (via MoltenVK for Vulkan apps) and needs a `CAMetalLayer`-backed view to render into.

---

## 3. New Enum Constants

```c
#define XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT ((XrStructureType)1000999004)
```

This extension also uses the shared window-space composition layer type:

```c
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT ((XrStructureType)1000999002)
```

> **Note:** The type value was changed from `1000999003` to `1000999004` in the docs refactor
> to resolve a collision with `XR_TYPE_DISPLAY_INFO_EXT`.

---

## 4. New Structures

### XrCocoaWindowBindingCreateInfoEXT

Chained to `XrSessionCreateInfo` (via the graphics binding's `next` pointer) to provide an external view handle for session rendering on macOS.

| Member | Type | Description |
|---|---|---|
| `type` | `XrStructureType` | Must be `XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT`. |
| `next` | `const void*` | Pointer to next structure in the chain, or `NULL`. |
| `viewHandle` | `void*` | `NSView*` with `CAMetalLayer` backing, or `NULL` for offscreen mode. |
| `readbackCallback` | `PFN_xrReadbackCallback` | Callback receiving composited RGBA pixels (offscreen mode), or `NULL`. |
| `readbackUserdata` | `void*` | Opaque pointer passed to `readbackCallback`. |
| `sharedIOSurface` | `void*` | `IOSurfaceRef` for zero-copy Metal texture sharing, or `NULL`. |

```c
typedef void (*PFN_xrReadbackCallback)(
    const uint8_t *pixels, uint32_t width, uint32_t height, void *userdata);

typedef struct XrCocoaWindowBindingCreateInfoEXT {
    XrStructureType          type;
    const void* XR_MAY_ALIAS next;
    void*                    viewHandle;
    PFN_xrReadbackCallback   readbackCallback;
    void*                    readbackUserdata;
    void*                    sharedIOSurface;
} XrCocoaWindowBindingCreateInfoEXT;
```

**Valid Usage:**
- `type` **must** be `XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT`.
- If `viewHandle` is non-NULL, it **must** point to a valid `NSView` whose `-makeBackingLayer` returns a `CAMetalLayer`.
- The view **must** remain valid for the lifetime of the `XrSession`.
- The application **must** run the `NSApplication` event loop on the main thread.
- The application **must not** remove the view from its superview before calling `xrDestroySession`.

### XrCompositionLayerWindowSpaceEXT

Shared with `XR_EXT_win32_window_binding`. See that extension's spec for the full definition.

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
- Shared-texture apps (`_shared` class) where the app composites the final output
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

XrCocoaWindowBindingCreateInfoEXT cocoaBinding = {
    XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT};
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
| `XR_EXT_win32_window_binding` | Windows counterpart — mutually exclusive (one per platform). Shares `XrCompositionLayerWindowSpaceEXT`. |
| `XR_EXT_android_surface_binding` | Android counterpart (planned, not yet implemented). |
| `XR_EXT_display_info` | Independent. Can be used alone (runtime-managed window) or together (app-owned view + display geometry + mode control). |

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

## 9. Revision History

| Version | Date | Author | Changes |
|---|---|---|---|
| 1 | 2025-06-15 | David Fattal | Initial version with viewHandle |
| 2 | 2025-09-20 | David Fattal | Added readbackCallback for offscreen mode |
| 3 | 2026-01-10 | David Fattal | Added sharedIOSurface for zero-copy Metal texture sharing. Fixed type value collision (1000999003 → 1000999004). |
