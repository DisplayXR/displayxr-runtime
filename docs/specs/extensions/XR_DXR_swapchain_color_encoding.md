# XR_DXR_swapchain_color_encoding

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_swapchain_color_encoding` |
| Spec Version | 1 |
| Type Values | `XR_TYPE_SWAPCHAIN_COLOR_ENCODING_CREATE_INFO_DXR` (1004999370) |
| Author | The DisplayXR Project |
| Platform | Windows, the in-process D3D11 compositor. Every other path refuses the declaration (§4). |

---

## 1. Overview

OpenXR reads every non-sRGB colour format as **linear**, and the runtime follows that rule
on every backend ([ADR-044](../../adr/ADR-044-colour-contract-per-backend.md)): the values of a
UNORM swapchain are sRGB-encoded on their way to the panel.

That is right for an app that renders linear light. It is wrong for an app that hands on pixels
which are already **display-referred**:
- an injected layer submitting a game's back buffer;
- a decoded video frame;
- a desktop capture.

With an 8-bit format such an app picks the `_SRGB` sibling and copies its bytes in raw. The
deeper formats have no `_SRGB` sibling, so a 10-bit `R10G10B10A2_UNORM` back buffer could only
be converted down to 8 bits first.

`XR_DXR_swapchain_color_encoding` lets the app say what its values are. A swapchain declared
`XR_SWAPCHAIN_COLOR_ENCODING_SRGB_DXR` is read exactly like an `_SRGB` swapchain.

## 2. API

```c
#define XR_TYPE_SWAPCHAIN_COLOR_ENCODING_CREATE_INFO_DXR ((XrStructureType)1004999370)

typedef enum XrSwapchainColorEncodingDXR {
    XR_SWAPCHAIN_COLOR_ENCODING_LINEAR_DXR = 0,  // OpenXR's reading of non-sRGB formats
    XR_SWAPCHAIN_COLOR_ENCODING_SRGB_DXR = 1,    // display-encoded (IEC 61966-2-1)
} XrSwapchainColorEncodingDXR;

typedef struct XrSwapchainColorEncodingCreateInfoDXR {
    XrStructureType             type;
    const void*                 next;
    XrSwapchainColorEncodingDXR colorEncoding;
} XrSwapchainColorEncodingCreateInfoDXR;   // extends XrSwapchainCreateInfo
```

Chain the struct on `XrSwapchainCreateInfo` when calling `xrCreateSwapchain`. There are no
entry points.

## 3. Semantics

A colour swapchain declared `XR_SWAPCHAIN_COLOR_ENCODING_SRGB_DXR` is treated exactly as an
`_SRGB` swapchain is treated in ADR-044 §1:
- **No blending:** when the frame needs no blending (the single-layer fast path, zero-copy), its
  values reach the display processor's encoded atlas as they are, at the format's full precision.
- **Blending:** where layers blend, the values are decoded to linear first, by the hardware.
  - 8-bit formats are sampled through their `_SRGB` sibling view.
  - Formats without an `_SRGB` sibling (`R10G10B10A2_UNORM`, `R16G16B16A16_UNORM`,
    `R16G16B16A16_FLOAT`) are first copied raw into an 8-bit shadow, which is then sampled
    through its `_SRGB` view.
  - Blended frames of such a swapchain therefore carry 8 bits per channel. The fast path keeps
    the full precision.

Further rules:
- `XR_SWAPCHAIN_COLOR_ENCODING_LINEAR_DXR`, and the absence of the struct, keep OpenXR's
  reading.
- On an `_SRGB` format the declaration changes nothing.
- The struct is ignored on depth swapchains.

## 4. Errors

- `XR_ERROR_VALIDATION_FAILURE`: `colorEncoding` is not a value of `XrSwapchainColorEncodingDXR`.
- `XR_ERROR_FEATURE_UNSUPPORTED`: the session's compositor cannot honour
  `XR_SWAPCHAIN_COLOR_ENCODING_SRGB_DXR`. In spec version 1 that is every path except the
  in-process D3D11 compositor: D3D12, IPC/service sessions, Vulkan, OpenGL and Metal.
  - The runtime refuses rather than read the values as linear.
  - An app can then pick the `_SRGB` sibling, or convert, on its own.

## 5. Formats

The in-process D3D11 compositor adds `DXGI_FORMAT_R10G10B10A2_UNORM` to the formats
`xrEnumerateSwapchainFormats` returns. Without this extension it is read as linear, like every
other UNORM format.

## 6. Example

A layer that injects into a game submits the game's 10-bit back buffer without converting it:

```c
XrSwapchainColorEncodingCreateInfoDXR encoding = {XR_TYPE_SWAPCHAIN_COLOR_ENCODING_CREATE_INFO_DXR};
encoding.colorEncoding = XR_SWAPCHAIN_COLOR_ENCODING_SRGB_DXR;

XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
info.next = &encoding;
info.format = DXGI_FORMAT_R10G10B10A2_UNORM;
// ... usage, size, counts
if (xrCreateSwapchain(session, &info, &swapchain) == XR_ERROR_FEATURE_UNSUPPORTED) {
    // fall back: convert to an 8-bit _SRGB swapchain
}
```

Each frame, `CopyResource` from the back buffer into the acquired image.
