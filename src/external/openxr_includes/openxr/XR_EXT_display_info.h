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
#define XR_EXT_display_info_SPEC_VERSION 5
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
    XrVector3f                  nominalViewerPositionInDisplaySpace; //!< Nominal viewer position in display space (meters)
    float                       recommendedViewScaleX;      //!< Horizontal scale: sr_recommended_w / display_pixel_w
    float                       recommendedViewScaleY;      //!< Vertical scale: sr_recommended_h / display_pixel_h
    XrBool32                    supportsDisplayModeSwitch;  //!< XR_TRUE if display supports 2D/3D mode switching
    uint32_t                    displayPixelWidth;          //!< Native display panel width in pixels (0 if unknown)
    uint32_t                    displayPixelHeight;         //!< Native display panel height in pixels (0 if unknown)
} XrDisplayInfoEXT;

/*!
 * @brief Display mode for XR_EXT_display_info 2D/3D switching.
 */
typedef enum XrDisplayModeEXT {
    XR_DISPLAY_MODE_2D_EXT = 0,
    XR_DISPLAY_MODE_3D_EXT = 1,
    XR_DISPLAY_MODE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrDisplayModeEXT;

/*!
 * @brief Request display mode switch (2D/3D).
 *
 * Switches the display between 2D and 3D modes. In 3D mode, the display's
 * light field hardware is active for stereoscopic viewing. In 2D mode, the
 * display behaves as a conventional 2D panel.
 *
 * The runtime automatically switches to 3D mode on xrBeginSession and back
 * to 2D mode on xrEndSession.
 *
 * @param session A valid XrSession handle.
 * @param displayMode The desired display mode (2D or 3D).
 * @return XR_SUCCESS on success.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestDisplayModeEXT)(XrSession session, XrDisplayModeEXT displayMode);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrRequestDisplayModeEXT(
    XrSession                                   session,
    XrDisplayModeEXT                            displayMode);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_DISPLAY_INFO_H
