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
 * @brief  Header for XR_DXR_lift extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * 2D→3D CONVERSION ("lift") as a runtime service. A vendor display plug-in may
 * ship a conversion module — monocular depth, stereo (SBS) synthesis, N-view
 * synthesis, or photo → Gaussian splats — and this extension exposes it
 * generically, the same way XR_DXR_weave exposes the vendor's weaver: the
 * caller never learns which model runs, only what it can do (@ref
 * XrLiftPropertiesDXR) and what it produced.
 *
 * Policy (ADR-042): when the runtime reports a READY module, a consumer that
 * also ships an OPEN default converter (the browser's / web SDK's depth
 * estimator + view generator) must prefer the runtime's. The vendor module is
 * calibrated for the panel it is plugged into; the open default is not.
 *
 * Output is NEVER woven here. A lift stream returns pre-weave SBS / N-view
 * pixels (or a depth map, or a splat blob); weaving stays the display
 * processor's job on the ordinary weave path (ADR-007). The one place the two
 * meet is XrWeaveSubmitLiftRectsDXR: a weave rect flagged "this content is
 * 2D — lift it first", which the service routes through a lift stream and then
 * weaves at the rect's CURRENT position with the latest converted result.
 *
 * Asynchronous by construction. Conversion runs on a runtime-owned thread, one
 * frame (or, for photos → splats, seconds) behind the submit:
 *
 *   xrCreateLiftStreamDXR(session, &createInfo, &stream);
 *   // per frame, never blocks on the model:
 *   xrSubmitLiftFrameDXR(stream, &submit, &frameId);          // latest-wins mailbox
 *   XrResult r = xrAcquireLiftResultDXR(stream, &result);     // newest finished frame
 *   if (r == XR_LIFT_NOT_READY_DXR) { keep showing the last result }
 *
 * @c sourceTime is the caller's own timestamp for the submitted frame (a video
 * PTS, an XrTime — the runtime never interprets it) and comes back verbatim on
 * the result it produced, so a caller can pair a late result with its frame.
 *
 * Availability: out-of-process (service / IPC) sessions only, on the Windows
 * D3D11 service, exactly like XR_DXR_weave. An in-process session reports
 * XR_ERROR_FEATURE_UNSUPPORTED from every entry point.
 *
 * Version history: 1 = initial (properties, streams, texture results, the
 * weave-rect lift chain, and the Gaussian-splat blob path reserved + wired).
 */
#ifndef XR_DXR_LIFT_H
#define XR_DXR_LIFT_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_lift 1
#define XR_DXR_lift_SPEC_VERSION 1
#define XR_DXR_LIFT_EXTENSION_NAME "XR_DXR_lift"

// Reserved 1004999270..279. Allocation registry: README.md in this directory.
#define XR_TYPE_LIFT_PROPERTIES_DXR          ((XrStructureType)1004999270)
#define XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR  ((XrStructureType)1004999271)
#define XR_TYPE_LIFT_FRAME_SUBMIT_INFO_DXR   ((XrStructureType)1004999272)
#define XR_TYPE_LIFT_OPTIONS_DXR             ((XrStructureType)1004999273)
#define XR_TYPE_LIFT_RESULT_DXR              ((XrStructureType)1004999274)
#define XR_TYPE_WEAVE_SUBMIT_LIFT_RECTS_DXR  ((XrStructureType)1004999275)
#define XR_TYPE_WEAVE_RECT_LIFT_DXR          ((XrStructureType)1004999276)
#define XR_TYPE_LIFT_BLOB_DXR                ((XrStructureType)1004999277)
//! XrObjectType of an XrLiftStreamDXR (debug-utils naming).
#define XR_OBJECT_TYPE_LIFT_STREAM_DXR       ((XrObjectType)1004999278)
//! SUCCESS-class result: the acquire found no result newer than the last one
//! it handed out. Not an error — keep presenting the previous result.
#define XR_LIFT_NOT_READY_DXR                ((XrResult)1004999279)

//! Size of XrLiftPropertiesDXR::backend, NUL included.
#define XR_LIFT_BACKEND_NAME_MAX_SIZE_DXR 32
//! Upper bound on XrLiftOptionsDXR::viewCount (and explicit viewpoints).
#define XR_LIFT_MAX_VIEWS_DXR 8
//! XrLiftOptionsDXR::convergence value asking the module to pick the
//! zero-disparity depth itself.
#define XR_LIFT_CONVERGENCE_AUTO_DXR (-1.0f)
//! Upper bound on lifted rects carried by one XrWeaveSubmitLiftRectsDXR.
#define XR_WEAVE_SUBMIT_MAX_LIFT_RECTS_DXR 8

XR_DEFINE_HANDLE(XrLiftStreamDXR)

typedef XrFlags64 XrLiftModeFlagsDXR;
//! Monocular depth map (one channel; see XrLiftDepthSemanticsDXR).
static const XrLiftModeFlagsDXR XR_LIFT_MODE_DEPTH_BIT_DXR = 0x00000001;
//! Stereo pair, side by side (left view in the left half), NOT woven.
static const XrLiftModeFlagsDXR XR_LIFT_MODE_SBS_BIT_DXR = 0x00000002;
//! N views side by side in one row (view 0 leftmost), NOT woven.
static const XrLiftModeFlagsDXR XR_LIFT_MODE_NVIEW_BIT_DXR = 0x00000004;
//! Photo → 3D Gaussian splats, returned as a blob (xrAcquireLiftBlobDXR).
//! PHOTO content only; seconds, not frames.
static const XrLiftModeFlagsDXR XR_LIFT_MODE_GAUSSIANS_BIT_DXR = 0x00000008;

//! The ONE mode a stream runs in (a single bit of XrLiftModeFlagsDXR).
typedef enum XrLiftModeDXR {
    XR_LIFT_MODE_DEPTH_DXR = 1,
    XR_LIFT_MODE_SBS_DXR = 2,
    XR_LIFT_MODE_NVIEW_DXR = 4,
    XR_LIFT_MODE_GAUSSIANS_DXR = 8,
    XR_LIFT_MODE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftModeDXR;

typedef enum XrLiftDepthSemanticsDXR {
    //! Relative (affine-invariant) depth: larger = farther, no unit.
    XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR = 0,
    //! Metric depth in metres.
    XR_LIFT_DEPTH_SEMANTICS_METRIC_DXR = 1,
    XR_LIFT_DEPTH_SEMANTICS_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftDepthSemanticsDXR;

typedef enum XrLiftStateDXR {
    //! No module, or the module failed. supportedModes is 0.
    XR_LIFT_STATE_UNAVAILABLE_DXR = 0,
    //! A module exists and is loading (model weights, engine build). Poll.
    XR_LIFT_STATE_ACTIVATING_DXR = 1,
    //! Streams may be created and fed.
    XR_LIFT_STATE_READY_DXR = 2,
    XR_LIFT_STATE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftStateDXR;

typedef enum XrLiftContentHintDXR {
    //! Temporally coherent frames: the module may use temporal state.
    XR_LIFT_CONTENT_HINT_VIDEO_DXR = 0,
    //! Independent stills: favour quality over latency.
    XR_LIFT_CONTENT_HINT_PHOTO_DXR = 1,
    XR_LIFT_CONTENT_HINT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftContentHintDXR;

typedef enum XrLiftViewpointSourceDXR {
    //! Synthesize for the runtime's tracked eyes (the normal display case).
    XR_LIFT_VIEWPOINT_SOURCE_TRACKED_DXR = 0,
    //! Synthesize for XrLiftOptionsDXR::viewpoints (display space, metres).
    XR_LIFT_VIEWPOINT_SOURCE_EXPLICIT_DXR = 1,
    XR_LIFT_VIEWPOINT_SOURCE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftViewpointSourceDXR;

typedef enum XrLiftBlobFormatDXR {
    //! Binary little-endian PLY in the reference 3DGS layout (x y z nx ny nz
    //! f_dc_0..2 [f_rest_*] opacity scale_0..2 rot_0..3).
    XR_LIFT_BLOB_FORMAT_PLY_3DGS_DXR = 1,
    //! PlayCanvas SOG (self-organizing Gaussians) container.
    XR_LIFT_BLOB_FORMAT_SOG_DXR = 2,
    XR_LIFT_BLOB_FORMAT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftBlobFormatDXR;

/*!
 * What the runtime's conversion module can do, and whether it can do it NOW.
 *
 * A consumer with its own open default converter treats
 * @c state == XR_LIFT_STATE_READY_DXR plus the needed mode bit as "use the
 * runtime's" (ADR-042). ACTIVATING is transient — poll (≤ 2 Hz) rather than
 * falling back permanently. UNAVAILABLE with supportedModes 0 is the answer on
 * every display whose plug-in ships no module, and on sim_display.
 */
typedef struct XrLiftPropertiesDXR {
    XrStructureType          type;               //!< XR_TYPE_LIFT_PROPERTIES_DXR
    void* XR_MAY_ALIAS       next;
    XrLiftModeFlagsDXR       supportedModes;     //!< XR_LIFT_MODE_*_BIT_DXR; 0 = none
    uint32_t                 maxStreams;         //!< concurrent streams across the whole runtime
    uint32_t                 maxViews;           //!< upper bound on XrLiftOptionsDXR::viewCount (NVIEW)
    XrLiftDepthSemanticsDXR  depthSemantics;     //!< meaning of a DEPTH result
    XrLiftStateDXR           state;
    char                     backend[XR_LIFT_BACKEND_NAME_MAX_SIZE_DXR]; //!< vendor module name, informational
    XrDuration               typicalLatency;     //!< submit→result, ns, as the module reports it; 0 = unknown
} XrLiftPropertiesDXR;

/*!
 * One conversion stream. @c mode is ONE bit the runtime reported in
 * XrLiftPropertiesDXR::supportedModes. @c inputScale (0, 1] asks the module to
 * convert at a reduced resolution (1.0 = native; 0 is read as 1.0) — a latency
 * lever, advisory.
 */
typedef struct XrLiftStreamCreateInfoDXR {
    XrStructureType          type;        //!< XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS next;
    XrLiftModeDXR            mode;
    XrLiftContentHintDXR     contentHint;
    float                    inputScale;
} XrLiftStreamCreateInfoDXR;

/*!
 * Per-frame conversion parameters. Chain on XrLiftFrameSubmitInfoDXR::next or
 * XrWeaveRectLiftDXR::next; omitted = the module's defaults (auto convergence,
 * strength 1, inpainting on, tracked eyes, 2 views).
 *
 * @c convergence is the RELATIVE depth placed at the display plane, in [0, 1]
 * over the frame's depth range (0 = the nearest content sits on the glass,
 * 1 = the farthest does, 0.5 = mid-range); XR_LIFT_CONVERGENCE_AUTO_DXR (any
 * negative) lets the module choose. Values above 1 are clamped. @c strength scales the disparity (0 = flat, 1 = the module's
 * calibrated budget). @c viewCount is the number of views an NVIEW stream
 * produces (2 for SBS; ignored for DEPTH / GAUSSIANS), ≤ maxViews. With
 * EXPLICIT viewpoints, @c viewpoints holds @c viewCount display-space positions
 * (metres); on the weave path viewpoints must be TRACKED.
 */
typedef struct XrLiftOptionsDXR {
    XrStructureType            type;            //!< XR_TYPE_LIFT_OPTIONS_DXR
    const void* XR_MAY_ALIAS   next;
    float                      convergence;     //!< XR_LIFT_CONVERGENCE_AUTO_DXR = auto
    float                      strength;        //!< disparity scale, >= 0
    XrBool32                   inpaint;         //!< fill disocclusions (XR_TRUE) or leave them
    XrLiftViewpointSourceDXR   viewpointSource;
    uint32_t                   viewCount;       //!< 1..XR_LIFT_MAX_VIEWS_DXR (NVIEW); 2 for SBS
    const XrVector3f*          viewpoints;      //!< viewCount entries when EXPLICIT, else ignored
} XrLiftOptionsDXR;

/*!
 * One frame into a stream. NON-BLOCKING: the runtime snapshots the input's
 * @c extent (top-left sub-rect of @c inputTexture) into its own mailbox before
 * returning — the caller may overwrite the texture immediately after — and
 * the frame then waits for the conversion thread. The mailbox is LATEST-WINS:
 * a frame still waiting when the next one arrives is dropped (never queued),
 * so a slow module lags one frame instead of building a backlog.
 *
 * @c inputTexture has the same handle kinds as XR_DXR_weave v3: a D3D11 NT
 * shared handle, or a legacy global DXGI handle with @c inputIsDxgi = XR_TRUE,
 * carrying an IDXGIKeyedMutex (key 0 = "caller done writing"). RGBA8 or BGRA8.
 */
typedef struct XrLiftFrameSubmitInfoDXR {
    XrStructureType          type;         //!< XR_TYPE_LIFT_FRAME_SUBMIT_INFO_DXR
    const void* XR_MAY_ALIAS next;         //!< chain XrLiftOptionsDXR here
    void*                    inputTexture; //!< shared texture HANDLE (keyed mutex, key 0)
    XrBool32                 inputIsDxgi;  //!< XR_TRUE for a legacy global DXGI handle
    XrExtent2Di              extent;       //!< region of inputTexture to convert, from (0,0)
    XrTime                   sourceTime;   //!< caller's timestamp; echoed on the result
} XrLiftFrameSubmitInfoDXR;

/*!
 * The newest finished conversion (texture modes: DEPTH / SBS / NVIEW).
 *
 * Handles follow the XR_DXR_weave output contract: @c outputTexture and
 * @c fence are shared HANDLEs handed out on the FIRST successful acquire and
 * again whenever the output is reallocated (size or format change — @c extent
 * / @c format tell you); NULL on steady-state acquires. The caller waits
 * @c fence to @c fenceValue before sampling, and finishes sampling before its
 * next acquire on this stream (the runtime copies the next result into the
 * same texture). The texture is runtime-owned; the caller closes its handles.
 *
 * Layout: DEPTH = one channel at the input's aspect; SBS = two views side by
 * side (@c viewCount 2); NVIEW = @c viewCount views side by side, view 0
 * leftmost. @c format is a DXGI_FORMAT value.
 */
typedef struct XrLiftResultDXR {
    XrStructureType    type;          //!< XR_TYPE_LIFT_RESULT_DXR
    void* XR_MAY_ALIAS next;
    uint64_t           frameId;       //!< the xrSubmitLiftFrameDXR frame this converts
    XrTime             sourceTime;    //!< that frame's sourceTime, verbatim
    void*              outputTexture; //!< shared HANDLE on first acquire / realloc, else NULL
    void*              fence;         //!< shared fence HANDLE on first acquire, else NULL
    uint64_t           fenceValue;    //!< wait the fence to this before sampling
    XrExtent2Di        extent;        //!< output texture size (all views)
    int64_t            format;        //!< DXGI_FORMAT
    uint32_t           viewCount;     //!< 1 (DEPTH), 2 (SBS), N (NVIEW)
    XrDuration         latency;       //!< submit → conversion finished, ns (runtime clock)
} XrLiftResultDXR;

/*!
 * The newest finished GAUSSIANS conversion, as bytes (spec v1).
 *
 * Standard two-call idiom on @c byteCapacityInput: 0 writes @c byteCountOutput
 * (and @c frameId / @c sourceTime / @c format) and LATCHES that blob, so the
 * second call — with a buffer at least that large — returns the SAME frame even
 * if a newer one finished in between. XR_ERROR_SIZE_INSUFFICIENT if the buffer
 * is too small (the latch is kept). XR_LIFT_NOT_READY_DXR when nothing newer
 * than the last blob handed out exists.
 */
typedef struct XrLiftBlobDXR {
    XrStructureType     type;              //!< XR_TYPE_LIFT_BLOB_DXR
    void* XR_MAY_ALIAS  next;
    uint64_t            frameId;
    XrTime              sourceTime;
    XrLiftBlobFormatDXR format;
    uint32_t            byteCapacityInput;
    uint32_t            byteCountOutput;
    uint8_t*            bytes;
} XrLiftBlobDXR;

/*!
 * "The content of weave rect @c rectIndex is 2D — lift it before weaving."
 *
 * An element of XrWeaveSubmitLiftRectsDXR (an XrRect2Di has no @c next, so the
 * association is by index into the submit's XrWeaveSubmitRectsDXR::rects).
 * Chain XrLiftOptionsDXR on @c next for per-rect parameters.
 *
 * The caller draws the element's 2D pixels into the rect exactly as it would
 * draw any other element — on the batch (v3) layout the WHOLE rect holds the 2D
 * frame (not squeezed SBS); on the v6 N-view layout, tile 0 at the rect's
 * scaled position does. The service snapshots that region into @c stream's
 * mailbox, and weaves the stream's LATEST converted output at the rect's
 * CURRENT position — so geometry (drag, resize, scroll) is exact and real-time
 * while the conversion itself runs one frame behind. Until the stream's first
 * result exists the rect is woven FLAT (its 2D pixels in every view).
 *
 * @c stream must be an SBS or NVIEW stream of the same session.
 */
typedef struct XrWeaveRectLiftDXR {
    XrStructureType          type;      //!< XR_TYPE_WEAVE_RECT_LIFT_DXR
    const void* XR_MAY_ALIAS next;      //!< chain XrLiftOptionsDXR here
    uint32_t                 rectIndex; //!< index into XrWeaveSubmitRectsDXR::rects
    XrLiftStreamDXR          stream;
} XrWeaveRectLiftDXR;

/*!
 * Chain on XrWeaveSubmitInfoDXR::next (together with XrWeaveSubmitRectsDXR)
 * to flag up to XR_WEAVE_SUBMIT_MAX_LIFT_RECTS_DXR rects as 2D-to-lift.
 * Requires XR_DXR_lift enabled; ignored by a runtime without it (the rects are
 * then woven as whatever the caller drew — flat 2D).
 */
typedef struct XrWeaveSubmitLiftRectsDXR {
    XrStructureType            type;      //!< XR_TYPE_WEAVE_SUBMIT_LIFT_RECTS_DXR
    const void* XR_MAY_ALIAS   next;
    uint32_t                   liftCount; //!< 0..XR_WEAVE_SUBMIT_MAX_LIFT_RECTS_DXR
    const XrWeaveRectLiftDXR*  lifts;
} XrWeaveSubmitLiftRectsDXR;

typedef XrResult (XRAPI_PTR *PFN_xrGetLiftPropertiesDXR)(
    XrSession session, XrLiftPropertiesDXR* properties);
typedef XrResult (XRAPI_PTR *PFN_xrCreateLiftStreamDXR)(
    XrSession session, const XrLiftStreamCreateInfoDXR* createInfo, XrLiftStreamDXR* stream);
typedef XrResult (XRAPI_PTR *PFN_xrDestroyLiftStreamDXR)(XrLiftStreamDXR stream);
typedef XrResult (XRAPI_PTR *PFN_xrSubmitLiftFrameDXR)(
    XrLiftStreamDXR stream, const XrLiftFrameSubmitInfoDXR* submitInfo, uint64_t* frameId);
typedef XrResult (XRAPI_PTR *PFN_xrAcquireLiftResultDXR)(XrLiftStreamDXR stream, XrLiftResultDXR* result);
typedef XrResult (XRAPI_PTR *PFN_xrAcquireLiftBlobDXR)(XrLiftStreamDXR stream, XrLiftBlobDXR* blob);

#ifndef XR_NO_PROTOTYPES

//! Query the conversion module. Cheap and non-blocking: the first call may
//! report ACTIVATING while the runtime brings the module up in the background.
XRAPI_ATTR XrResult XRAPI_CALL xrGetLiftPropertiesDXR(
    XrSession session, XrLiftPropertiesDXR* properties);

//! Create a stream in one mode. XR_ERROR_FEATURE_UNSUPPORTED if the mode is not
//! in supportedModes (or the session is in-process); XR_ERROR_LIMIT_REACHED
//! past maxStreams. May be called while ACTIVATING (frames wait for READY).
XRAPI_ATTR XrResult XRAPI_CALL xrCreateLiftStreamDXR(
    XrSession session, const XrLiftStreamCreateInfoDXR* createInfo, XrLiftStreamDXR* stream);

//! Destroy a stream. Its textures stay valid in the caller until it closes
//! its own handles. Destroying the session destroys its streams.
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyLiftStreamDXR(XrLiftStreamDXR stream);

//! Hand one frame to the stream's latest-wins mailbox; returns its frameId
//! (monotonic per stream, from 1). Never waits for the conversion.
//! XR_ERROR_RUNTIME_FAILURE (non-fatal, retry next frame) when the input could
//! not be acquired within the service's 4 ms keyed-mutex budget.
XRAPI_ATTR XrResult XRAPI_CALL xrSubmitLiftFrameDXR(
    XrLiftStreamDXR stream, const XrLiftFrameSubmitInfoDXR* submitInfo, uint64_t* frameId);

//! The newest finished texture result newer than the last one acquired, or
//! XR_LIFT_NOT_READY_DXR. XR_ERROR_VALIDATION_FAILURE on a GAUSSIANS stream.
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireLiftResultDXR(XrLiftStreamDXR stream, XrLiftResultDXR* result);

//! The newest finished blob (GAUSSIANS streams), two-call idiom — see
//! XrLiftBlobDXR. XR_ERROR_VALIDATION_FAILURE on a texture-mode stream.
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireLiftBlobDXR(XrLiftStreamDXR stream, XrLiftBlobDXR* blob);

#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_LIFT_H
