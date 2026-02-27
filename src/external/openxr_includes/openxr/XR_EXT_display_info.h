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
#define XR_EXT_display_info_SPEC_VERSION 6
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

// ---- v6: Eye Tracking Mode Control ----

#define XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT ((XrStructureType)1000999006)
#define XR_TYPE_VIEW_EYE_TRACKING_STATE_EXT        ((XrStructureType)1000999007)

/*!
 * @brief Eye tracking mode enum.
 *
 * SMOOTH (0) is the default — apps that never call xrRequestEyeTrackingModeEXT
 * get current behavior (vendor SDK handles grace period + smoothing).
 * RAW (1) provides unfiltered positions + explicit isTracking flag.
 */
typedef enum XrEyeTrackingModeEXT {
    XR_EYE_TRACKING_MODE_SMOOTH_EXT   = 0,
    XR_EYE_TRACKING_MODE_RAW_EXT      = 1,
    XR_EYE_TRACKING_MODE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrEyeTrackingModeEXT;

/*!
 * @brief Capability flags for eye tracking modes (bitmask).
 *
 * A value of 0 means the display has NO eye tracking capability at all.
 */
typedef XrFlags64 XrEyeTrackingModeCapabilityFlagsEXT;
static const XrEyeTrackingModeCapabilityFlagsEXT
    XR_EYE_TRACKING_MODE_CAPABILITY_NONE_EXT       = 0;
static const XrEyeTrackingModeCapabilityFlagsEXT
    XR_EYE_TRACKING_MODE_CAPABILITY_SMOOTH_BIT_EXT = 0x00000001;
static const XrEyeTrackingModeCapabilityFlagsEXT
    XR_EYE_TRACKING_MODE_CAPABILITY_RAW_BIT_EXT    = 0x00000002;

/*!
 * @brief Eye tracking mode capabilities — chained to XrSystemProperties.
 *
 * If supportedModes is 0 (NONE), the display has no eye tracking. In that
 * case defaultMode is undefined, xrRequestEyeTrackingModeEXT returns
 * XR_ERROR_FEATURE_UNSUPPORTED for any mode, and XrViewEyeTrackingStateEXT
 * always reports isTracking=XR_FALSE.
 *
 * xrLocateViews ALWAYS returns fully populated views (count, positions, FOVs)
 * regardless of tracking capability or state. The vendor SDK decides the view
 * positions (e.g., nominal viewer, last known, filtered). isTracking only
 * indicates whether those positions come from live eye tracking or a fallback.
 *
 * @extends XrSystemProperties
 */
typedef struct XrEyeTrackingModeCapabilitiesEXT {
    XrStructureType                        type;           //!< Must be XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT
    void* XR_MAY_ALIAS                     next;
    XrEyeTrackingModeCapabilityFlagsEXT    supportedModes; //!< Bitmask of supported modes (0 = no tracking)
    XrEyeTrackingModeEXT                   defaultMode;    //!< Mode used if app never requests one
} XrEyeTrackingModeCapabilitiesEXT;

/*!
 * @brief Per-frame eye tracking state — chained to XrViewState in xrLocateViews.
 *
 * xrLocateViews ALWAYS returns fully populated views (positions, FOVs)
 * regardless of isTracking. When isTracking is XR_FALSE, positions are
 * still valid — the vendor SDK populates them as it sees fit (e.g., last
 * known, nominal viewer, filtered). isTracking tells the app whether
 * positions come from live eye tracking or vendor-chosen fallback.
 * The app may use isTracking to trigger its own animations or UI.
 *
 * @extends XrViewState
 */
typedef struct XrViewEyeTrackingStateEXT {
    XrStructureType           type;       //!< Must be XR_TYPE_VIEW_EYE_TRACKING_STATE_EXT
    void* XR_MAY_ALIAS        next;
    XrBool32                  isTracking; //!< XR_TRUE if eyes are actively tracked this frame
    XrEyeTrackingModeEXT     activeMode; //!< Currently active mode
} XrViewEyeTrackingStateEXT;

/*!
 * @brief Request eye tracking mode switch.
 *
 * Switches between smooth and raw eye tracking modes. In smooth mode,
 * the vendor SDK handles grace period + smoothing internally. In raw mode,
 * the SDK provides unfiltered positions and the app uses isTracking to
 * handle tracking loss.
 *
 * @param session A valid XrSession handle.
 * @param mode The desired eye tracking mode.
 * @return XR_SUCCESS on success,
 *         XR_ERROR_FEATURE_UNSUPPORTED if the mode is not supported,
 *         XR_ERROR_VALIDATION_FAILURE if mode is invalid.
 */
typedef XrResult (XRAPI_PTR *PFN_xrRequestEyeTrackingModeEXT)(
    XrSession session, XrEyeTrackingModeEXT mode);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrRequestEyeTrackingModeEXT(
    XrSession               session,
    XrEyeTrackingModeEXT    mode);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_EXT_DISPLAY_INFO_H
