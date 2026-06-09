// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_android_surface_binding extension
 * @ingroup external_openxr
 *
 * Android counterpart to XR_EXT_win32_window_binding / XR_EXT_cocoa_window_binding.
 * Lets an OpenXR application on Android hand the runtime resources it owns at
 * xrCreateSession instead of the runtime self-spawning a SurfaceView:
 *
 * - window:      an ANativeWindow* the app owns. When provided, the runtime
 *                presents into it (handle-class with an app surface). When
 *                NULL, behaviour depends on sharedImage (below).
 *
 * - sharedImage: a VkImage the app owns, used as the compositor's weave-output
 *                TARGET (texture-class). On Android the app and runtime share a
 *                single VkDevice (the app passes its device in
 *                XrGraphicsBindingVulkanKHR and the compositor builds its
 *                vk_bundle from it), so the raw VkImage handle is usable
 *                directly — NO external-memory export/import is needed (unlike
 *                the D3D11 NT-handle / IOSurface paths on desktop). The runtime
 *                weaves the stereo content into the canvas sub-rect of this
 *                image (see xrSetSharedTextureOutputRectEXT) and does NOT
 *                present — the app reads the image and composites it (plus any
 *                2D surround) into its own surface.
 *
 * With both NULL the runtime falls back to self-spawning a SurfaceView (the
 * pre-existing single-app POC behaviour).
 *
 * POC note: this is the pragmatic in-process channel. A fully standardized
 * cross-process shared-texture path (AHardwareBuffer + Vulkan external memory)
 * would replace sharedImage with an exported buffer handle.
 */
#ifndef XR_EXT_ANDROID_SURFACE_BINDING_H
#define XR_EXT_ANDROID_SURFACE_BINDING_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_android_surface_binding 1
#define XR_EXT_android_surface_binding_SPEC_VERSION 1
#define XR_EXT_ANDROID_SURFACE_BINDING_EXTENSION_NAME "XR_EXT_android_surface_binding"

// Vendor extension range (1000000000+); next after the cocoa binding's
// 1000999004. Replace with a Khronos-assigned value if ever standardized.
#define XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_EXT ((XrStructureType)1000999005)

/*!
 * @brief Structure passed in XrSessionCreateInfo::next to provide an
 *        app-owned ANativeWindow and/or a shared VkImage weave target on
 *        Android.
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrAndroidSurfaceBindingCreateInfoEXT {
    XrStructureType          type;              //!< XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_EXT
    const void* XR_MAY_ALIAS next;              //!< Pointer to next structure in chain
    void*                    window;            //!< ANativeWindow* the app owns, or NULL
    uint64_t                 sharedImage;       //!< VkImage handle (weave target), or 0 for none
    uint32_t                 sharedImageWidth;  //!< sharedImage width in pixels
    uint32_t                 sharedImageHeight; //!< sharedImage height in pixels
    uint32_t                 sharedImageFormat; //!< VkFormat of sharedImage, as a uint32_t
} XrAndroidSurfaceBindingCreateInfoEXT;

// ---- Canvas sub-rect + 2D surround ----
// Shared with XR_EXT_win32_window_binding / XR_EXT_cocoa_window_binding; the
// PFN typedefs and prototypes are guarded so including more than one binding
// header in a translation unit does not redefine them.

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

#ifndef PFN_xrSetSharedTextureSurround2DEXT_DEFINED
#define PFN_xrSetSharedTextureSurround2DEXT_DEFINED
typedef XrResult (XRAPI_PTR *PFN_xrSetSharedTextureSurround2DEXT)(
    XrSession session,
    void*     sharedTextureHandle,
    uint32_t  width,
    uint32_t  height);
#endif

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrSetSharedTextureSurround2DEXT(
    XrSession                           session,
    void*                               sharedTextureHandle,
    uint32_t                            width,
    uint32_t                            height);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_ANDROID_SURFACE_BINDING_H
