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
 * @brief  Header for XR_DXR_app_share extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * 3D screen share (ADR-061): a running DisplayXR app's PRE-WEAVE views,
 * offered as one more source of XR_DXR_stereo_camera. This extension adds no
 * entry point. Enabling it (XR_DXR_stereo_camera must be enabled too) makes
 * xrEnumerateStereoCamerasDXR also list one entry per shareable running app,
 * flagged XR_STEREO_CAMERA_APP_CONTENT_BIT_DXR, after every camera. Streams,
 * frames, the shared-memory ring, the wake handle, events and stats are the
 * stereo camera's, unchanged:
 *
 *   // instance: enable XR_DXR_stereo_camera + XR_DXR_app_share
 *   xrEnumerateStereoCamerasDXR(...);       // chain XrAppShareSourcePropertiesDXR
 *   if (props.flags & XR_STEREO_CAMERA_APP_CONTENT_BIT_DXR) { ... an app ... }
 *   xrCreateStereoCameraStreamDXR(...);     // output RECTIFIED, format NV12
 *   xrStartStereoCameraStreamDXR(stream);   // the runtime's share prompt (spec §6)
 *   xrAcquireStereoCameraFrameDXR(stream, &frame);   // chain XrAppShareFrameInfoDXR
 *
 * The frame is one side-by-side image, left view in the left half, taken from
 * the app's composed atlas before any weave, cropped to the active mode's
 * content (ADR-030), the centre pair when the app renders more than two views,
 * the same view in both halves while the app renders one (ADR-041). It is
 * DISPLAY-REFERRED, not a camera pair: zero disparity sits at the sender's
 * display plane, not at infinity (XrAppShareFrameInfoDXR::zeroDisparityShiftFrac).
 *
 * An instance that does not enable this extension never sees an app entry, so
 * existing camera consumers are unaffected.
 *
 * Spec: docs/specs/extensions/XR_DXR_app_share.md. Decision: ADR-061.
 *
 * Version history: 1 = initial (design accepted, ADR-061; not implemented).
 */
#ifndef XR_DXR_APP_SHARE_H
#define XR_DXR_APP_SHARE_H 1

#include <openxr/openxr.h>
#include <openxr/XR_DXR_stereo_camera.h> // XrStereoCameraFlagsDXR, XrStereoCameraPropertiesDXR, XrStereoCameraFrameDXR
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_app_share 1
#define XR_DXR_app_share_SPEC_VERSION 1
#define XR_DXR_APP_SHARE_EXTENSION_NAME "XR_DXR_app_share"

// Reserved 1004999430..439. Allocation registry: README.md in this directory.
//! Chain on XrStereoCameraPropertiesDXR::next (xrEnumerateStereoCamerasDXR).
#define XR_TYPE_APP_SHARE_SOURCE_PROPERTIES_DXR ((XrStructureType)1004999430)
//! Chain on XrStereoCameraFrameDXR::next (xrAcquireStereoCameraFrameDXR).
#define XR_TYPE_APP_SHARE_FRAME_INFO_DXR        ((XrStructureType)1004999431)
//! Chain on XrSessionCreateInfo::next: the shared app's own opt-out.
#define XR_TYPE_SESSION_SHAREABILITY_DXR        ((XrStructureType)1004999432)
// 433..439 free for this extension.

/*!
 * An XrStereoCameraFlagsDXR bit, defined here because only an instance that
 * enables this extension can see it: the entry is a running app's pre-weave
 * views, not a camera. App entries also carry NATIVELY_RECTIFIED and
 * RECTIFIABLE (row-aligned views, nothing to rectify), never CALIBRATED,
 * USER_FACING, SHARED_WITH_EYE_TRACKING or MONOCHROME; baselineMm and
 * horizontalFovDeg are 0. XR_DXR_stereo_camera reserves this bit value.
 */
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_APP_CONTENT_BIT_DXR = 0x00000040;

//! XrAppShareSourcePropertiesDXR::executableName size, NUL included.
#define XR_APP_SHARE_EXECUTABLE_NAME_MAX_SIZE_DXR 260

/*!
 * Chain on XrStereoCameraPropertiesDXR::next. Filled by
 * xrEnumerateStereoCamerasDXR for an APP_CONTENT entry; left untouched for a
 * camera (check the flag, not the struct).
 */
typedef struct XrAppShareSourcePropertiesDXR {
    XrStructureType type; //!< XR_TYPE_APP_SHARE_SOURCE_PROPERTIES_DXR
    void* XR_MAY_ALIAS next;
    //! The shared app's process id (OS-derived by the service, never app-asserted).
    uint64_t processId;
    //! File name of the shared app's executable, no directory ("modelviewer.exe").
    char executableName[XR_APP_SHARE_EXECUTABLE_NAME_MAX_SIZE_DXR];
    //! XrApplicationInfo::applicationName of the shared app's instance.
    char applicationName[XR_MAX_APPLICATION_NAME_SIZE];
    //! The app's window in desktop pixels; extent 0 when it has none on the desktop.
    XrRect2Di windowRect;
    //! Per-view content size at the source, before scaling to eyeExtent.
    XrExtent2Di contentEyeExtent;
    //! Views the app submits (the fixed count, ADR-041).
    uint32_t viewCount;
    //! Views active in the app's current mode (1 = mono, both halves carry it).
    uint32_t activeViewCount;
    //! See XrAppShareFrameInfoDXR; the value at enumeration time.
    float zeroDisparityShiftFrac;
    //! XR_TRUE when the source atlas held LINEAR values (ADR-044). Diagnostic:
    //! delivered frames are always gamma-encoded NV12, BT.601 full range.
    XrBool32 encodingIsLinear;
} XrAppShareSourcePropertiesDXR;

/*!
 * Chain on XrStereoCameraFrameDXR::next when acquiring from an APP_CONTENT
 * stream. Per frame, because the source app can change mode at any time.
 */
typedef struct XrAppShareFrameInfoDXR {
    XrStructureType type; //!< XR_TYPE_APP_SHARE_FRAME_INFO_DXR
    void* XR_MAY_ALIAS next;
    //! The content inside ONE eye half (left-half coordinates; the right half
    //! uses the same rect offset by eyeWidth). Outside it is letterbox black.
    XrRect2Di contentRect;
    //! XR_TRUE: one view in both halves (the app is in a mono / 2D mode).
    XrBool32 mono;
    //! Bumps whenever contentRect, mono or the app's view layout changed.
    uint32_t contentGeneration;
    /*!
     * Horizontal disparity, as a fraction of the delivered per-eye width
     * (eyeExtent.width; right-view x minus left-view x), of a point on the
     * SENDER's display plane. A receiver
     * that wants the sender's display plane at its own zero-disparity plane
     * shifts each eye by half of it in opposite directions. 0 = the frame is
     * already display-referred (always 0 in spec v1: the runtime's off-axis
     * projection puts the display plane at zero disparity by construction).
     */
    float zeroDisparityShiftFrac;
    //! When the source app's frame was committed (runtime clock).
    XrTime sourceCommitTime;
} XrAppShareFrameInfoDXR;

/*!
 * Chain on XrSessionCreateInfo::next (the app enables XR_DXR_app_share) to keep
 * this session out of every share: shareable = XR_FALSE removes it from
 * enumeration and ends any stream of it. Absent = shareable (spec §5).
 */
typedef struct XrSessionShareabilityDXR {
    XrStructureType type; //!< XR_TYPE_SESSION_SHAREABILITY_DXR
    const void* XR_MAY_ALIAS next;
    XrBool32 shareable;
} XrSessionShareabilityDXR;

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_APP_SHARE_H
