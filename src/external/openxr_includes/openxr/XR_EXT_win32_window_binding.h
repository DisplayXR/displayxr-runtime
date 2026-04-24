// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_win32_window_binding extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension allows an OpenXR application to provide its own window handle
 * (HWND on Windows) to the runtime. When provided, the runtime will render
 * into the application's window instead of creating its own window.
 *
 * This enables:
 * - Windowed mode rendering (vs fullscreen)
 * - Application control over window input (keyboard, mouse)
 * - Multiple OpenXR applications on the same display
 * - Offscreen readback: windowHandle=NULL + readbackCallback → composited
 *   pixels delivered to callback instead of presented to a window
 * - Zero-copy shared texture: windowHandle=NULL + sharedTextureHandle →
 *   runtime composites into a shared D3D11/D3D12 texture (HANDLE)
 */
#ifndef XR_EXT_WIN32_WINDOW_BINDING_H
#define XR_EXT_WIN32_WINDOW_BINDING_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_win32_window_binding 1
#define XR_EXT_win32_window_binding_SPEC_VERSION 3
#define XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME "XR_EXT_win32_window_binding"

// Use a value in the vendor extension range (1000000000+)
// This should be replaced with an official Khronos-assigned value if the extension is standardized
#define XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT ((XrStructureType)1000999001)
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT ((XrStructureType)1000999002)

/*!
 * @brief Callback for offscreen readback mode (CPU fallback).
 *
 * When set in XrWin32WindowBindingCreateInfoEXT with windowHandle=NULL,
 * the runtime delivers composited RGBA pixels via this callback each frame.
 *
 * @param pixels   Pointer to RGBA pixel data (width * height * 4 bytes)
 * @param width    Image width in pixels
 * @param height   Image height in pixels
 * @param userdata Opaque pointer from readbackUserdata
 */
typedef void (*PFN_xrReadbackCallback)(
    const uint8_t *pixels, uint32_t width, uint32_t height, void *userdata);

/*!
 * @brief Structure passed in XrSessionCreateInfo::next chain to provide
 *        an external window handle for session rendering.
 *
 * When this structure is provided in the next chain of XrSessionCreateInfo,
 * the runtime will render into the specified window instead of creating
 * its own window. The application is responsible for:
 * - Creating and managing the window lifecycle
 * - Handling the window message pump
 * - Processing input events
 *
 * Alternatively, set windowHandle to NULL and provide either:
 * - readbackCallback for CPU-side offscreen readback (GPU→CPU round-trip), or
 * - sharedTextureHandle for zero-copy GPU texture sharing (D3D11/D3D12 HANDLE)
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrWin32WindowBindingCreateInfoEXT {
    XrStructureType             type;                  //!< Must be XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT
    const void* XR_MAY_ALIAS    next;                  //!< Pointer to next structure in chain
#ifdef _WIN32
    void*                       windowHandle;          //!< HWND of the target window (Windows only)
#else
    void*                       windowHandle;          //!< Platform-specific window handle (reserved)
#endif
    PFN_xrReadbackCallback      readbackCallback;      //!< Offscreen readback callback (CPU fallback), or NULL
    void*                       readbackUserdata;      //!< Passed to readbackCallback
    void*                       sharedTextureHandle;   //!< Shared D3D11/D3D12 texture HANDLE for zero-copy, or NULL
} XrWin32WindowBindingCreateInfoEXT;

/*!
 * @brief Composition layer positioned in fractional window coordinates.
 *
 * This layer type renders a textured quad at a position specified as fractions
 * of the target window dimensions. The coordinates automatically scale when the
 * window is resized.
 *
 * The same texture is composited into both eye views with a per-eye horizontal
 * shift controlled by the disparity parameter. The layer is rendered pre-interlace
 * (passes through the weaver like any other layer).
 *
 * This layer type is only valid when the session was created with
 * XrWin32WindowBindingCreateInfoEXT (i.e., rendering to an application-provided window).
 *
 * @extends XrFrameEndInfo (submitted as a composition layer)
 */
typedef struct XrCompositionLayerWindowSpaceEXT {
    XrStructureType             type;       //!< Must be XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT
    const void* XR_MAY_ALIAS    next;       //!< Pointer to next structure in chain
    XrCompositionLayerFlags     layerFlags; //!< e.g. XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    XrSwapchainSubImage         subImage;   //!< Source swapchain + rect
    float                       x;          //!< Left edge, fraction of window width  [0..1]
    float                       y;          //!< Top edge, fraction of window height   [0..1]
    float                       width;      //!< Fraction of window width  [0..1]
    float                       height;     //!< Fraction of window height [0..1]
    float                       disparity;  //!< Horizontal shift, fraction of window width.
                                            //!< 0 = screen depth, negative = toward viewer
} XrCompositionLayerWindowSpaceEXT;

// ---- Canvas Sub-Rect (Shared Texture Output Rect) ----

/*!
 * @brief Set the canvas sub-rect within the app's window where 3D content appears.
 *
 * For _texture apps (shared texture mode), the 3D canvas may be a sub-rect of
 * the app's window — e.g., a 3D viewport surrounded by 2D toolbars. The runtime
 * needs this rect to:
 * - Compute correct interlacing alignment (screen-space position matters for
 *   lenticular displays — see spec §2.4 "The Phase Alignment Problem")
 * - Size views and Kooima projection based on canvas dimensions, not window size
 *
 * Call this whenever the canvas position or size changes (e.g., on window resize
 * or layout change). For static layouts, call once after session creation.
 *
 * Coordinates are relative to the HWND client area (not screen-space).
 * When this function is never called, the runtime assumes the full window
 * client area is the canvas.
 *
 * @param session The session (must have been created with a window binding).
 * @param x       Left edge of the canvas in client-area pixels.
 * @param y       Top edge of the canvas in client-area pixels.
 * @param width   Canvas width in pixels.
 * @param height  Canvas height in pixels.
 *
 * @return XR_SUCCESS on success.
 */
#ifndef PFN_xrSetSharedTextureOutputRectEXT_DEFINED
#define PFN_xrSetSharedTextureOutputRectEXT_DEFINED
typedef XrResult (XRAPI_PTR *PFN_xrSetSharedTextureOutputRectEXT)(
    XrSession session, int32_t x, int32_t y, uint32_t width, uint32_t height);
#endif

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrSetSharedTextureOutputRectEXT(
    XrSession                           session,
    int32_t                             x,
    int32_t                             y,
    uint32_t                            width,
    uint32_t                            height);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_WIN32_WINDOW_BINDING_H
