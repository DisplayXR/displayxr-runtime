// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the XR_DXR_* extensions in this header are NOT yet registered in the
// Khronos OpenXR registry: extension numbers and XrStructureType values sit
// in a provisional experimental block (1004999xxx) pending official
// assignment. Extension names are expected to be stable; numeric values are
// not.
// See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Header for XR_DXR_stereo_comfort extension
 * @ingroup external_openxr
 *
 * THE COMFORT ZONE, FROM THE RUNTIME. How much disparity a viewer tolerates
 * depends on the viewing distance and on the viewer, not on the app: the eyes
 * converge on a point at distance z while they focus on the glass at distance
 * V, and comfort is a bound on the vergence change
 *
 *     dD = 1/V - 1/z      (diopters; > 0 behind the glass, < 0 in front)
 *
 * which lands on the glass as the disparity p = e * V * dD (e = 0.063 m). The
 * same diopter bound allows a different share of a different canvas at a
 * different distance, so an app that hard-codes a percentage is right on one
 * display only. This extension reports the bound for the CURRENT viewer, so an
 * app that sizes its stereo from it is right on every display.
 *
 * Division of labour: the RUNTIME owns the zone (which published bound, the
 * user's depth preference, the viewer distance it tracks); the APP owns its
 * scene (its nearest and farthest points, its eye separation and convergence)
 * and fits them inside the zone. Nothing here changes the views the runtime
 * locates.
 *
 * OPT-IN, ZERO COST OTHERWISE. The runtime fills @ref XrStereoComfortDXR only
 * when it is chained on XrViewState::next in xrLocateViews; it is filled on
 * every such call, in-process and over IPC.
 *
 * Zones (the same ones the DisplayXR web SDK's comfortZone() states):
 *  - XR_STEREO_COMFORT_ZONE_SHIBATA_DXR: Shibata et al. 2011, "The zone of
 *    comfort" (J. Vision 11(8):11). Near vergence limit Dv/1.035 + 0.626, far
 *    Dv/1.129 - 0.442 (Dv = 1/V). At 0.6 m: 0.57 D in front, 0.63 D behind.
 *    A far limit at or below 0 D leaves behind unbounded (+infinity).
 *  - XR_STEREO_COMFORT_ZONE_PERCIVAL_DXR: Percival's zone, 1/3 D each side.
 *  - XR_STEREO_COMFORT_ZONE_CUSTOM_DXR: a symmetric bound in diopters.
 * The runtime's default zone and the user's depth preference (a factor in
 * (0, 1] that scales both bounds; 1 = the published zone) are runtime
 * settings. An app may request a zone with @ref XrStereoComfortRequestDXR on
 * XrViewLocateInfo::next; the user's preference still applies to it.
 */
#ifndef XR_DXR_STEREO_COMFORT_H
#define XR_DXR_STEREO_COMFORT_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_stereo_comfort 1
#define XR_DXR_stereo_comfort_SPEC_VERSION 1
#define XR_DXR_STEREO_COMFORT_EXTENSION_NAME "XR_DXR_stereo_comfort"

#define XR_TYPE_STEREO_COMFORT_DXR ((XrStructureType)1004999440)
#define XR_TYPE_STEREO_COMFORT_REQUEST_DXR ((XrStructureType)1004999441)

typedef enum XrStereoComfortZoneDXR {
    XR_STEREO_COMFORT_ZONE_DEFAULT_DXR = 0,  //!< request only: the runtime's configured zone
    XR_STEREO_COMFORT_ZONE_SHIBATA_DXR = 1,
    XR_STEREO_COMFORT_ZONE_PERCIVAL_DXR = 2,
    XR_STEREO_COMFORT_ZONE_CUSTOM_DXR = 3,   //!< symmetric bound, customDiopters
    XR_STEREO_COMFORT_ZONE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrStereoComfortZoneDXR;

/*!
 * @brief Optional: the zone the app asks for. Chained on XrViewLocateInfo::next.
 *
 * Without it (or with ZONE_DEFAULT) the runtime's configured zone is used. An
 * invalid zone or a CUSTOM bound <= 0 falls back to the configured zone.
 */
typedef struct XrStereoComfortRequestDXR {
    XrStructureType             type;   //!< Must be XR_TYPE_STEREO_COMFORT_REQUEST_DXR
    const void* XR_MAY_ALIAS    next;
    XrStereoComfortZoneDXR      zone;
    float                       customDiopters;  //!< ZONE_CUSTOM only: |dD| bound each side
} XrStereoComfortRequestDXR;

/*!
 * @brief The comfort zone for the current viewer. Chained on XrViewState::next;
 *        the runtime fills it.
 *
 * Disparities are on the glass, in metres, between the two eyes' images
 * (divide by the canvas width for a fraction of it). Behind values may be
 * +infinity (Shibata, distant viewer). Bounds already include the user's depth
 * preference. Values are per locate and unsmoothed: an app that follows a
 * tracked viewer should smooth what it derives from them.
 */
typedef struct XrStereoComfortDXR {
    XrStructureType             type;   //!< Must be XR_TYPE_STEREO_COMFORT_DXR
    void* XR_MAY_ALIAS          next;
    XrStereoComfortZoneDXR      zone;                    //!< the zone applied (never DEFAULT)
    float                       viewerDistanceMeters;    //!< V: tracked eyes' mean distance, else nominal
    XrBool32                    viewerTracked;           //!< V came from tracked eyes
    float                       frontDiopters;           //!< |dD| allowed in front of the glass
    float                       behindDiopters;          //!< |dD| allowed behind (may be +inf)
    float                       frontDisparityMeters;    //!< e * V * frontDiopters
    float                       behindDisparityMeters;   //!< e * V * behindDiopters (may be +inf)
    float                       userDepthScale;          //!< the user's preference already applied, (0, 1]
} XrStereoComfortDXR;

#ifdef __cplusplus
}
#endif

#endif
