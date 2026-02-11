// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for XR_EXT_display_info extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * This extension exposes physical display properties and recommended view
 * scaling factors to the application via xrGetSystemProperties. The app
 * multiplies its current window size by the scale factors to compute the
 * per-eye render resolution each frame, eliminating the need for runtime
 * events on window resize.
 */
#ifndef XR_EXT_DISPLAY_INFO_H
#define XR_EXT_DISPLAY_INFO_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_display_info 1
#define XR_EXT_display_info_SPEC_VERSION 2
#define XR_EXT_DISPLAY_INFO_EXTENSION_NAME "XR_EXT_display_info"

// Reuse the type value from the deleted XR_EXT_dynamic_render_resolution
#define XR_TYPE_DISPLAY_INFO_EXT ((XrStructureType)1000999003)
#define XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT ((XrReferenceSpaceType)1000999004)

/*!
 * @brief Display information returned by xrGetSystemProperties.
 *
 * When chained to XrSystemProperties via the next pointer, the runtime fills
 * in the physical display dimensions and recommended view scale factors.
 *
 * The application computes per-eye render resolution as:
 *   renderWidth  = (uint32_t)(windowWidth  * recommendedViewScaleX)
 *   renderHeight = (uint32_t)(windowHeight * recommendedViewScaleY)
 *
 * The scale factors are static display properties (sr_recommended / display_pixels)
 * that do not change with window resize.
 *
 * @extends XrSystemProperties
 */
typedef struct XrDisplayInfoEXT {
    XrStructureType             type;       //!< Must be XR_TYPE_DISPLAY_INFO_EXT
    void* XR_MAY_ALIAS          next;       //!< Pointer to next structure in chain
    XrExtent2Df                 displaySizeMeters;          //!< Physical display size in meters
    XrPosef                     nominalViewerPoseInDisplaySpace; //!< Nominal viewer position relative to display center
    float                       recommendedViewScaleX;      //!< Horizontal scale: sr_recommended_w / display_pixel_w
    float                       recommendedViewScaleY;      //!< Vertical scale: sr_recommended_h / display_pixel_h
} XrDisplayInfoEXT;

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_DISPLAY_INFO_H
