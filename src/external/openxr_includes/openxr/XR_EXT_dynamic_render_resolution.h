// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_dynamic_render_resolution extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension allows the runtime to notify the application when the
 * recommended render resolution changes (e.g., due to a window resize on a
 * light-field display). The application adapts by rendering to a smaller
 * viewport within its existing swapchain and setting subImage.imageRect
 * accordingly -- no swapchain recreation needed.
 */
#ifndef XR_EXT_DYNAMIC_RENDER_RESOLUTION_H
#define XR_EXT_DYNAMIC_RENDER_RESOLUTION_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_dynamic_render_resolution 1
#define XR_EXT_dynamic_render_resolution_SPEC_VERSION 1
#define XR_EXT_DYNAMIC_RENDER_RESOLUTION_EXTENSION_NAME "XR_EXT_dynamic_render_resolution"

// Type enum value (next in our vendor range after 1000999002)
#define XR_TYPE_EVENT_DATA_RENDER_RESOLUTION_CHANGED_EXT ((XrStructureType)1000999003)

/*!
 * @brief Event notifying the application that the recommended per-eye render
 *        resolution has changed.
 *
 * The application should adjust its viewport and subImage.imageRect to the new
 * recommended dimensions. The existing swapchain remains valid; there is no
 * need to recreate it.
 *
 * Field naming mirrors XrViewConfigurationView.recommendedImageRect* so that
 * developers immediately know what the values represent.
 */
typedef struct XrEventDataRenderResolutionChangedEXT {
    XrStructureType          type;    //!< Must be XR_TYPE_EVENT_DATA_RENDER_RESOLUTION_CHANGED_EXT
    const void* XR_MAY_ALIAS next;
    uint32_t                 recommendedImageRectWidth;   //!< New recommended width per eye
    uint32_t                 recommendedImageRectHeight;  //!< New recommended height per eye
} XrEventDataRenderResolutionChangedEXT;

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_DYNAMIC_RENDER_RESOLUTION_H
