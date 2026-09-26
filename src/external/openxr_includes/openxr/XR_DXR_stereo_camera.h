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
 * @brief  Header for XR_DXR_stereo_camera extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * A display's STEREO CAMERA (usually the one its eye tracker looks through) as
 * a runtime-provided source (ADR-043). The vendor display plug-in produces
 * frames from its own stack without taking the device from the tracker; the
 * runtime service owns the camera, opens it once and fans every frame out to
 * each authorised stream; clients consume the newest frame over shared memory:
 *
 *   xrEnumerateStereoCamerasDXR(instance, systemId, 0, &n, NULL);   // + properties
 *   xrCreateStereoCameraStreamDXR(instance, &createInfo, &stream);
 *   xrStartStereoCameraStreamDXR(stream);                            // authorisation point
 *   xrGetStereoCameraStreamInfoDXR(stream, &info);   // chain XrStereoCameraStreamTransportDXR
 *   // map info's section once; per frame (wake on the per-stream wake handle):
 *   if (xrAcquireStereoCameraFrameDXR(stream, &frame) == XR_SUCCESS) { read slot frame.slot }
 *
 * The frame is always ONE side-by-side image, 2*eyeWidth x eyeHeight, left eye
 * in the left half, never mirrored, never woven (showing it in 3D is the
 * weave path's job, ADR-007).
 *
 * Instance-level (a capture component has no session) and SERVICE-ONLY: an
 * in-process instance enumerates zero cameras. The service is the single owner
 * of each camera; an in-process runtime opening it too would recreate the
 * exclusive-device fight this extension exists to end.
 *
 * Scope: stereo pairs. Names and struct shapes are kept open for other camera
 * kinds (XrStereoCameraPropertiesDXR::viewCount, next chains), but v1 sources
 * are two-view side-by-side only.
 *
 * Privacy: frames bypass the OS camera stack, so the runtime gates them itself
 * (consent, foreground rule, in-use indicator, kill switch — spec §7).
 * xrStartStereoCameraStreamDXR and xrGetStereoCameraCalibrationDXR return
 * XR_ERROR_PERMISSION_INSUFFICIENT when refused.
 *
 * Spec: docs/specs/extensions/XR_DXR_stereo_camera.md.
 *
 * Version history: 1 = initial (enumerate + state, calibration raw/rectified,
 * streams with start/stop, latest-wins acquire over a pinned 3-slot shared-
 * memory ring with per-stream wake handles, stream stats).
 */
#ifndef XR_DXR_STEREO_CAMERA_H
#define XR_DXR_STEREO_CAMERA_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_stereo_camera 1
#define XR_DXR_stereo_camera_SPEC_VERSION 1
#define XR_DXR_STEREO_CAMERA_EXTENSION_NAME "XR_DXR_stereo_camera"

// Reserved 1004999290..309. Allocation registry: README.md in this directory.
#define XR_TYPE_STEREO_CAMERA_PROPERTIES_DXR                ((XrStructureType)1004999290)
#define XR_TYPE_STEREO_CAMERA_CALIBRATION_DXR               ((XrStructureType)1004999291)
#define XR_TYPE_STEREO_CAMERA_STREAM_CREATE_INFO_DXR        ((XrStructureType)1004999292)
#define XR_TYPE_STEREO_CAMERA_FRAME_DXR                     ((XrStructureType)1004999293)
#define XR_TYPE_STEREO_CAMERA_STREAM_INFO_DXR               ((XrStructureType)1004999294)
#define XR_TYPE_STEREO_CAMERA_STREAM_TRANSPORT_DXR          ((XrStructureType)1004999295)
#define XR_TYPE_STEREO_CAMERA_STREAM_STATS_DXR              ((XrStructureType)1004999296)
#define XR_TYPE_EVENT_DATA_STEREO_CAMERA_STATE_CHANGED_DXR  ((XrStructureType)1004999297)
#define XR_TYPE_EVENT_DATA_STEREO_CAMERAS_CHANGED_DXR       ((XrStructureType)1004999298)
//! XrObjectType of an XrStereoCameraStreamDXR (debug-utils naming).
#define XR_OBJECT_TYPE_STEREO_CAMERA_STREAM_DXR             ((XrObjectType)1004999299)
//! SUCCESS-class result: no frame newer than the last one this stream acquired.
//! Not an error — keep showing the previous frame.
#define XR_STEREO_CAMERA_FRAME_NOT_READY_DXR                ((XrResult)1004999300)

//! Size of XrStereoCameraPropertiesDXR::persistentId, NUL included.
#define XR_STEREO_CAMERA_PERSISTENT_ID_MAX_SIZE_DXR 64
//! Size of XrStereoCameraPropertiesDXR::displayName, NUL included.
#define XR_STEREO_CAMERA_DISPLAY_NAME_MAX_SIZE_DXR 128
//! Size of XrStereoCameraPropertiesDXR::platformDeviceHint, NUL included.
#define XR_STEREO_CAMERA_PLATFORM_HINT_MAX_SIZE_DXR 256
//! Slots in a stream's shared-memory ring (latest + pinned + one to write).
#define XR_STEREO_CAMERA_RING_SLOTS_DXR 3

XR_DEFINE_HANDLE(XrStereoCameraStreamDXR)

typedef XrFlags64 XrStereoCameraFlagsDXR;
//! The camera is the eye tracker's; frames exist only while tracking runs.
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING_BIT_DXR = 0x00000001;
//! The camera faces the user (a 3D display's front camera).
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_USER_FACING_BIT_DXR = 0x00000002;
//! Calibration is available for the ACTIVE device (RECTIFIED supported).
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_CALIBRATED_BIT_DXR = 0x00000004;
//! The source already delivers a rectified pair.
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_NATIVELY_RECTIFIED_BIT_DXR = 0x00000008;
//! Luma only (an IR / grey tracking sensor).
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_MONOCHROME_BIT_DXR = 0x00000010;

typedef enum XrStereoCameraStateDXR {
    //! Frames flowing, or will flow on start.
    XR_STEREO_CAMERA_STATE_AVAILABLE_DXR = 1,
    //! Source up, no frame yet (tracker warming up).
    XR_STEREO_CAMERA_STATE_WAITING_DXR = 2,
    //! Temporarily no frames: tracker stopped, privacy lock, session locked.
    XR_STEREO_CAMERA_STATE_SUSPENDED_DXR = 3,
    //! Failed / removed.
    XR_STEREO_CAMERA_STATE_UNAVAILABLE_DXR = 4,
    XR_STEREO_CAMERA_STATE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrStereoCameraStateDXR;

typedef enum XrStereoCameraOutputDXR {
    //! Parallel pair, zero disparity at infinity, no convergence shear. Needs
    //! CALIBRATED or NATIVELY_RECTIFIED.
    XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR = 1,
    //! The source's pixels as delivered (native clients only; never web pages).
    XR_STEREO_CAMERA_OUTPUT_RAW_DXR = 2,
    XR_STEREO_CAMERA_OUTPUT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrStereoCameraOutputDXR;

typedef enum XrStereoCameraFormatDXR {
    XR_STEREO_CAMERA_FORMAT_GRAY8_DXR = 1,
    //! A MONOCHROME source in NV12 is luma + neutral (128) chroma.
    XR_STEREO_CAMERA_FORMAT_NV12_DXR = 2,
    XR_STEREO_CAMERA_FORMAT_BGRA8_DXR = 3,
    XR_STEREO_CAMERA_FORMAT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrStereoCameraFormatDXR;

typedef enum XrStereoCameraTransportDXR {
    //! Required; every platform.
    XR_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY_DXR = 1,
    //! Optional (Windows); not implemented by spec v1 runtimes.
    XR_STEREO_CAMERA_TRANSPORT_D3D11_TEXTURE_DXR = 2,
    //! Optional (Android); not implemented by spec v1 runtimes.
    XR_STEREO_CAMERA_TRANSPORT_AHARDWAREBUFFER_DXR = 3,
    XR_STEREO_CAMERA_TRANSPORT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrStereoCameraTransportDXR;

typedef enum XrStereoCameraDistortionModelDXR {
    XR_STEREO_CAMERA_DISTORTION_NONE_DXR = 0,
    //! OpenCV k1 k2 p1 p2 k3.
    XR_STEREO_CAMERA_DISTORTION_RADTAN5_DXR = 1,
    //! + k4 k5 k6 (rational).
    XR_STEREO_CAMERA_DISTORTION_RADTAN8_DXR = 2,
    //! Fisheye (Kannala-Brandt k1..k4).
    XR_STEREO_CAMERA_DISTORTION_KB4_DXR = 3,
    XR_STEREO_CAMERA_DISTORTION_MAX_ENUM_DXR = 0x7FFFFFFF
} XrStereoCameraDistortionModelDXR;

//! One bit per XrStereoCameraFormatDXR value: bit (1 << value).
typedef XrFlags64 XrStereoCameraFormatFlagsDXR;
static const XrStereoCameraFormatFlagsDXR XR_STEREO_CAMERA_FORMAT_GRAY8_BIT_DXR = 0x00000002;
static const XrStereoCameraFormatFlagsDXR XR_STEREO_CAMERA_FORMAT_NV12_BIT_DXR = 0x00000004;
static const XrStereoCameraFormatFlagsDXR XR_STEREO_CAMERA_FORMAT_BGRA8_BIT_DXR = 0x00000008;

//! One bit per XrStereoCameraTransportDXR value: bit (1 << value).
typedef XrFlags64 XrStereoCameraTransportFlagsDXR;
static const XrStereoCameraTransportFlagsDXR XR_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY_BIT_DXR = 0x00000002;
static const XrStereoCameraTransportFlagsDXR XR_STEREO_CAMERA_TRANSPORT_D3D11_TEXTURE_BIT_DXR = 0x00000004;
static const XrStereoCameraTransportFlagsDXR XR_STEREO_CAMERA_TRANSPORT_AHARDWAREBUFFER_BIT_DXR = 0x00000008;

/*!
 * One camera, as enumerated. @c cameraId is valid for the service's lifetime
 * and never reused for another camera within it.
 */
typedef struct XrStereoCameraPropertiesDXR {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    uint64_t cameraId;
    //! Stable per (device, consumer executable); never the raw serial (spec §7.5).
    char persistentId[XR_STEREO_CAMERA_PERSISTENT_ID_MAX_SIZE_DXR];
    //! Plug-in supplied, human-readable ("Built-in 3D camera").
    char displayName[XR_STEREO_CAMERA_DISPLAY_NAME_MAX_SIZE_DXR];
    //! The OS's id of the physical device this source reads, "" if none — so a
    //! capture stack can hide the raw (busy) duplicate.
    char platformDeviceHint[XR_STEREO_CAMERA_PLATFORM_HINT_MAX_SIZE_DXR];
    XrStereoCameraFlagsDXR flags;
    XrStereoCameraStateDXR state;
    //! Views in the source. Always 2 in spec v1 (reserved for other camera kinds).
    uint32_t viewCount;
    //! Native per-eye size (e.g. 640x480).
    XrExtent2Di eyeExtent;
    //! What the source can deliver, Hz.
    float maxFrameRate;
    //! |T| of the pair, millimetres; 0 if uncalibrated.
    float baselineMm;
    //! Per eye, rectified, degrees; 0 if uncalibrated.
    float horizontalFovDeg;
    XrStereoCameraFormatFlagsDXR supportedFormats;
    XrStereoCameraTransportFlagsDXR supportedTransports;
} XrStereoCameraPropertiesDXR;

typedef struct XrStereoCameraIntrinsicsDXR {
    //! The image these numbers describe (per eye).
    XrExtent2Di imageExtent;
    float fx, fy, cx, cy;
    XrStereoCameraDistortionModelDXR model;
    float coefficients[8];
} XrStereoCameraIntrinsicsDXR;

typedef struct XrStereoCameraCalibrationDXR {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    //! IN: which output the numbers should describe.
    XrStereoCameraOutputDXR output;
    //! RAW: native + distortion. RECTIFIED: post-rectification, model NONE, equal fy/cy.
    XrStereoCameraIntrinsicsDXR eye[2];
    //! RAW: the pair's extrinsics. RECTIFIED: pure +x translation of baselineMm/1000.
    XrPosef rightFromLeft;
    float baselineMm;
} XrStereoCameraCalibrationDXR;

typedef struct XrStereoCameraStreamCreateInfoDXR {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    uint64_t cameraId;
    XrStereoCameraOutputDXR output;
    XrStereoCameraFormatDXR format;
    XrStereoCameraTransportDXR transport;
    //! <= 0 = the source's rate; the service decimates, never interpolates.
    float maxFrameRate;
} XrStereoCameraStreamCreateInfoDXR;

/*!
 * The shared-memory ring of a started stream (chained on
 * XrStereoCameraStreamInfoDXR). Handles are NEW handles duplicated into the
 * caller (Windows HANDLE / POSIX fd): the caller closes them. The section is
 * read-only for the caller.
 */
typedef struct XrStereoCameraStreamTransportDXR {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    uint64_t sectionHandle;
    uint64_t sectionSize;
    uint32_t slotCount;
    uint64_t slotStride;
    //! Per-stream wake handle, signalled once per published frame. Windows: an
    //! auto-reset event (SYNCHRONIZE access). POSIX: the read end of a
    //! non-blocking pipe — one byte per frame; drain it before acquiring.
    uint64_t wakeHandle;
} XrStereoCameraStreamTransportDXR;

typedef struct XrStereoCameraStreamInfoDXR {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    //! Full SBS extent (2*eyeWidth x eyeHeight).
    XrExtent2Di extent;
    XrStereoCameraFormatDXR format;
    XrStereoCameraOutputDXR output;
    XrStereoCameraTransportDXR transport;
    //! The effective delivery cap, Hz.
    float maxFrameRate;
} XrStereoCameraStreamInfoDXR;

typedef struct XrStereoCameraFrameDXR {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    //! Source frame counter, monotonic from 1; gaps = frames this stream missed.
    uint64_t frameIndex;
    //! Ring slot holding it; PINNED (byte-stable) until this stream's next acquire.
    uint32_t slot;
    XrExtent2Di extent;
    XrStereoCameraFormatDXR format;
    //! Plane pitches in bytes (NV12: Y, UV; single-plane formats: [0]).
    uint32_t rowPitch[2];
    //! Byte offsets of the planes inside the slot.
    uint64_t planeOffset[2];
    XrTime captureTime;
    //! TRUE = sensor/exposure time; FALSE = arrival time at the plug-in.
    XrBool32 captureTimeIsExposure;
    //! What this frame is (RAW when rectification is unavailable).
    XrStereoCameraOutputDXR output;
    //! Bumps when calibration/rectification changed; re-query calibration.
    uint32_t calibrationGeneration;
} XrStereoCameraFrameDXR;

typedef struct XrStereoCameraStreamStatsDXR {
    XrStructureType type;
    void* XR_MAY_ALIAS next;
    //! Rate the source delivered to the service, Hz (exponential average).
    float sourceFrameRate;
    //! Rate published to THIS stream after decimation, Hz.
    float deliveredFrameRate;
    uint64_t framesPublished;
    //! Frames published but superseded before this stream acquired them.
    uint64_t framesSkipped;
    uint64_t framesAcquired;
    //! Mean plug-in-timestamp -> publish latency, ns.
    uint64_t meanLatencyNs;
} XrStereoCameraStreamStatsDXR;

typedef struct XrEventDataStereoCameraStateChangedDXR {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    uint64_t cameraId;
    XrStereoCameraStateDXR state;
} XrEventDataStereoCameraStateChangedDXR;

typedef struct XrEventDataStereoCamerasChangedDXR {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
} XrEventDataStereoCamerasChangedDXR;

typedef XrResult (XRAPI_PTR *PFN_xrEnumerateStereoCamerasDXR)(
    XrInstance instance, XrSystemId systemId, uint32_t capacityInput, uint32_t* countOutput,
    XrStereoCameraPropertiesDXR* cameras);
typedef XrResult (XRAPI_PTR *PFN_xrGetStereoCameraCalibrationDXR)(
    XrInstance instance, uint64_t cameraId, XrStereoCameraCalibrationDXR* calibration);
typedef XrResult (XRAPI_PTR *PFN_xrCreateStereoCameraStreamDXR)(
    XrInstance instance, const XrStereoCameraStreamCreateInfoDXR* createInfo, XrStereoCameraStreamDXR* stream);
typedef XrResult (XRAPI_PTR *PFN_xrDestroyStereoCameraStreamDXR)(XrStereoCameraStreamDXR stream);
typedef XrResult (XRAPI_PTR *PFN_xrStartStereoCameraStreamDXR)(XrStereoCameraStreamDXR stream);
typedef XrResult (XRAPI_PTR *PFN_xrStopStereoCameraStreamDXR)(XrStereoCameraStreamDXR stream);
typedef XrResult (XRAPI_PTR *PFN_xrGetStereoCameraStreamInfoDXR)(
    XrStereoCameraStreamDXR stream, XrStereoCameraStreamInfoDXR* info);
typedef XrResult (XRAPI_PTR *PFN_xrAcquireStereoCameraFrameDXR)(
    XrStereoCameraStreamDXR stream, XrStereoCameraFrameDXR* frame);
typedef XrResult (XRAPI_PTR *PFN_xrGetStereoCameraStreamStatsDXR)(
    XrStereoCameraStreamDXR stream, XrStereoCameraStreamStatsDXR* stats);

#ifndef XR_NO_PROTOTYPES
#ifdef XR_EXTENSION_PROTOTYPES

//! Two-call idiom. An in-process instance, a plug-in with no camera, and the
//! kill switch all report count 0 — indistinguishable on purpose.
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateStereoCamerasDXR(
    XrInstance instance, XrSystemId systemId, uint32_t capacityInput, uint32_t* countOutput,
    XrStereoCameraPropertiesDXR* cameras);

//! Calibration of the ACTIVE device. Consent-gated (spec §7.1, §7.5).
XRAPI_ATTR XrResult XRAPI_CALL xrGetStereoCameraCalibrationDXR(
    XrInstance instance, uint64_t cameraId, XrStereoCameraCalibrationDXR* calibration);

XRAPI_ATTR XrResult XRAPI_CALL xrCreateStereoCameraStreamDXR(
    XrInstance instance, const XrStereoCameraStreamCreateInfoDXR* createInfo, XrStereoCameraStreamDXR* stream);

XRAPI_ATTR XrResult XRAPI_CALL xrDestroyStereoCameraStreamDXR(XrStereoCameraStreamDXR stream);

//! The authorisation point, and where the service opens the source.
XRAPI_ATTR XrResult XRAPI_CALL xrStartStereoCameraStreamDXR(XrStereoCameraStreamDXR stream);

XRAPI_ATTR XrResult XRAPI_CALL xrStopStereoCameraStreamDXR(XrStereoCameraStreamDXR stream);

//! Valid after start. Chain XrStereoCameraStreamTransportDXR to receive the
//! ring section + wake handle (new handles each call; caller closes).
XRAPI_ATTR XrResult XRAPI_CALL xrGetStereoCameraStreamInfoDXR(
    XrStereoCameraStreamDXR stream, XrStereoCameraStreamInfoDXR* info);

//! Newest frame newer than the last one acquired, or XR_STEREO_CAMERA_FRAME_NOT_READY_DXR.
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireStereoCameraFrameDXR(
    XrStereoCameraStreamDXR stream, XrStereoCameraFrameDXR* frame);

XRAPI_ATTR XrResult XRAPI_CALL xrGetStereoCameraStreamStatsDXR(
    XrStereoCameraStreamDXR stream, XrStereoCameraStreamStatsDXR* stats);

#endif /* XR_EXTENSION_PROTOTYPES */
#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_STEREO_CAMERA_H
