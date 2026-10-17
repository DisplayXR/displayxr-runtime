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
 * weave-rect lift chain, the Gaussian-splat blob path, per-stream priority
 * scheduling and stream stats). 2 = viewpoint policy (ADR-048): TRACKED
 * viewpoints are relative to the lifted rect's / submitting window's centre
 * (was: the panel centre), XrLiftViewControlDXR (display-rig ipd / parallax
 * factors, axis mode, offset clamp, recentering) and the per-result echo
 * XrLiftResultViewpointsDXR. 3 = app camera + metric depth (ADR-048
 * Addendum A): an XrDisplayRigDXR / XrCameraRigDXR (XR_DXR_view_rig) chained
 * on XrLiftOptionsDXR or XrWeaveRectLiftDXR drives the TRACKED viewpoints with
 * the xrLocateViews rig math; EXPLICIT viewpoints are accepted on lifted weave
 * rects; SBS / NVIEW streams may return an auxiliary depth map from the same
 * conversion (XrLiftDepthRequestDXR at stream creation, XrLiftDepthResultDXR
 * on each result, XrLiftDepthPropertiesDXR on the properties). 4 = conversion
 * providers (ADR-053): a task vocabulary (XrLiftTaskDXR) with per-task
 * readiness (xrEnumerateLiftTasksDXR, xrRequestLiftTaskWarmDXR), opt-in task /
 * provider events, task-typed stream creation (XrLiftTaskStreamCreateInfoDXR),
 * platform-neutral input images (XrLiftFrameImagesDXR), frame-discontinuity
 * control, and the result contracts XrLiftResultExportDXR, XrLiftDepthInfoDXR
 * and XrLiftViewsInfoDXR. Four changes are observable by a v3 caller:
 * maxStreams is the selected provider's per-task limit, not runtime-wide; a
 * GAUSSIANS blob may be SOG where it was PLY; an affine-disparity aux depth
 * reads RELATIVE + INVERSE, so auxDepthSupported stays true; and the mode
 * check at create follows the declared capabilities, so a mode whose task is
 * still warming is accepted (its frames wait). 5 = provider candidates
 * (#2178, ADR-054 decision 8): xrEnumerateLiftTaskCandidatesDXR lists, per
 * task, the providers that could serve it -- READ-ONLY: id, the provider's own
 * display name, readiness, licence class, hosting, whether it is the user's
 * default and whether a create would pick it -- and XrLiftProviderPreferenceDXR
 * chained on XrLiftTaskStreamCreateInfoDXR is a per-create HINT that reorders
 * the non-terminal candidates for that create only. The runtime still
 * arbitrates; an app never sets the user's default.
 */
#ifndef XR_DXR_LIFT_H
#define XR_DXR_LIFT_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_lift 1
#define XR_DXR_lift_SPEC_VERSION 5
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
// Second decade 1004999280..289 (the first is full).
#define XR_TYPE_LIFT_STREAM_STATS_DXR        ((XrStructureType)1004999280)
// Spec v2 (ADR-048).
#define XR_TYPE_LIFT_VIEW_CONTROL_DXR        ((XrStructureType)1004999281)
#define XR_TYPE_LIFT_RESULT_VIEWPOINTS_DXR   ((XrStructureType)1004999282)
// Spec v3 (ADR-048 Addendum A).
#define XR_TYPE_LIFT_DEPTH_REQUEST_DXR       ((XrStructureType)1004999283)
#define XR_TYPE_LIFT_DEPTH_RESULT_DXR        ((XrStructureType)1004999284)
#define XR_TYPE_LIFT_DEPTH_PROPERTIES_DXR    ((XrStructureType)1004999285)
// ---- Spec v4 (ADR-053: tasks, readiness, neutral transport) ----
// Reserved 1004999340..369 (three decades, spec 4 + spec 5). Registry:
// README.md in this directory. 1004999340 (held back when provider
// enumeration was cut from spec 4) is spec 5's candidate struct; 353..359
// are free.
#define XR_TYPE_LIFT_TASK_PROPERTIES_DXR               ((XrStructureType)1004999341)
#define XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR       ((XrStructureType)1004999342)
#define XR_TYPE_LIFT_FRAME_IMAGES_DXR                  ((XrStructureType)1004999343)
#define XR_TYPE_LIFT_IMAGE_DXR                         ((XrStructureType)1004999344)
#define XR_TYPE_LIFT_IMAGE_DMABUF_DESC_DXR             ((XrStructureType)1004999345)
#define XR_TYPE_LIFT_RESULT_EXPORT_DXR                 ((XrStructureType)1004999346)
#define XR_TYPE_LIFT_DEPTH_INFO_DXR                    ((XrStructureType)1004999347)
#define XR_TYPE_LIFT_VIEWS_INFO_DXR                    ((XrStructureType)1004999348)
#define XR_TYPE_LIFT_FRAME_CONTROL_DXR                 ((XrStructureType)1004999349)
#define XR_TYPE_EVENT_DATA_LIFT_TASK_STATE_CHANGED_DXR ((XrStructureType)1004999350)
#define XR_TYPE_EVENT_DATA_LIFT_PROVIDERS_CHANGED_DXR  ((XrStructureType)1004999351)
// ---- Spec v5 (#2178: read-only provider candidates + a per-create hint) ----
#define XR_TYPE_LIFT_TASK_CANDIDATE_DXR                ((XrStructureType)1004999340)
#define XR_TYPE_LIFT_PROVIDER_PREFERENCE_DXR           ((XrStructureType)1004999352)

//! Size of XrLiftPropertiesDXR::backend, NUL included.
#define XR_LIFT_BACKEND_NAME_MAX_SIZE_DXR 32
//! Upper bound on XrLiftOptionsDXR::viewCount (and explicit viewpoints).
#define XR_LIFT_MAX_VIEWS_DXR 8
//! XrLiftOptionsDXR::convergence value asking the module to pick the
//! zero-disparity depth itself.
#define XR_LIFT_CONVERGENCE_AUTO_DXR (-1.0f)
//! Upper bound on lifted rects carried by one XrWeaveSubmitLiftRectsDXR.
#define XR_WEAVE_SUBMIT_MAX_LIFT_RECTS_DXR 8
//! Size of XrLiftTaskCandidateDXR::providerId, NUL included (spec 5).
#define XR_LIFT_PROVIDER_ID_MAX_SIZE_DXR 64
//! Size of XrLiftTaskCandidateDXR::displayName, NUL included (spec 5).
#define XR_LIFT_PROVIDER_NAME_MAX_SIZE_DXR 64

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

/*!
 * How the samples of an auxiliary depth map encode depth (spec v3). Decode a
 * sample @c s as d = valueScale * s + valueOffset (XrLiftDepthResultDXR);
 * with INVERSE, depth = 1 / d. The decoded depth is in the result's units.
 */
typedef enum XrLiftDepthEncodingDXR {
    //! The decoded value IS the depth (larger = farther).
    XR_LIFT_DEPTH_ENCODING_LINEAR_DXR = 0,
    //! The decoded value is INVERSE depth (disparity-like; larger = nearer).
    XR_LIFT_DEPTH_ENCODING_INVERSE_DXR = 1,
    XR_LIFT_DEPTH_ENCODING_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftDepthEncodingDXR;

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

/*!
 * How the service's lift thread schedules a stream against the others (the
 * module converts one frame at a time — one GPU, often one serialised
 * inference queue — so N concurrent streams share its throughput). Each
 * scheduling ROUND converts, in order:
 *
 *  - every HIGH stream that has a new frame;
 *  - ONE NORMAL stream with a new frame, round-robin across NORMAL streams;
 *  - every LOW stream with a new frame, but only on every 4th round;
 *  - PAUSED streams never — they keep serving their last result (the weave
 *    keeps weaving it; an acquire keeps reporting NOT READY).
 *
 * Default NORMAL. A per-STREAM setting, changed rarely (e.g. when the active
 * speaker of a call changes), so it is its own call rather than a per-frame
 * field. Scheduling is runtime policy: the vendor module never sees it.
 */
typedef enum XrLiftPriorityDXR {
    XR_LIFT_PRIORITY_PAUSED_DXR = 0,
    XR_LIFT_PRIORITY_LOW_DXR = 1,
    XR_LIFT_PRIORITY_NORMAL_DXR = 2,
    XR_LIFT_PRIORITY_HIGH_DXR = 3,
    XR_LIFT_PRIORITY_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftPriorityDXR;

/*!
 * Which viewpoint components a TRACKED lift follows (spec v2, ADR-048). The
 * others are pinned to the rect's centre line (x, y = 0) or to the reference
 * viewing distance (z). X is the default: horizontal look-around only.
 */
typedef enum XrLiftAxisModeDXR {
    XR_LIFT_AXIS_MODE_X_DXR = 1,
    XR_LIFT_AXIS_MODE_XY_DXR = 2,
    XR_LIFT_AXIS_MODE_XYZ_DXR = 3,
    XR_LIFT_AXIS_MODE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftAxisModeDXR;

/*!
 * How a TRACKED lift's look-around returns to centre (spec v2, ADR-048).
 *
 *  - OFF: the rendered offset is the viewer's offset from the rect centre,
 *    always (v1 behaviour, but rect-relative).
 *  - EASE_BACK (default): the camera eases back to the scene camera origin.
 *    Moving the head gives look-around temporarily; once the viewer's offset
 *    has stayed beyond a small threshold for recenterHoldSeconds, the camera
 *    returns to centre (no look-around) with time constant
 *    recenterTimeConstantSeconds. Any new head motion gives immediate,
 *    temporary look-around again. (The reference viewer position follows
 *    the head, so the rendered offset decays to 0 wherever the viewer
 *    settles.)
 *
 * Frame-rate independent; one filter per stream, reset when the stream is
 * created and whenever tracking is lost.
 */
typedef enum XrLiftRecenterModeDXR {
    XR_LIFT_RECENTER_MODE_OFF_DXR = 0,
    XR_LIFT_RECENTER_MODE_EASE_BACK_DXR = 1,
    XR_LIFT_RECENTER_MODE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftRecenterModeDXR;

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
 * Auxiliary-depth capability (spec v3). Chain on XrLiftPropertiesDXR::next;
 * filled by xrGetLiftPropertiesDXR. @c auxDepthSupported XR_TRUE = an SBS /
 * NVIEW stream created with XrLiftDepthRequestDXR returns the depth map of the
 * same conversion with each result; @c auxDepthSemantics is what that depth
 * means (METRIC = metres). A runtime or module without it leaves XR_FALSE.
 */
typedef struct XrLiftDepthPropertiesDXR {
    XrStructureType          type;               //!< XR_TYPE_LIFT_DEPTH_PROPERTIES_DXR
    void* XR_MAY_ALIAS       next;
    XrBool32                 auxDepthSupported;
    XrLiftDepthSemanticsDXR  auxDepthSemantics;
} XrLiftDepthPropertiesDXR;

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
 * Ask an SBS / NVIEW stream for its depth map as an auxiliary output (spec
 * v3). Chain on XrLiftStreamCreateInfoDXR::next. XR_ERROR_VALIDATION_FAILURE
 * on a DEPTH or GAUSSIANS stream (a DEPTH stream's result already is depth).
 * Advisory: if the module cannot produce it (XrLiftDepthPropertiesDXR::
 * auxDepthSupported XR_FALSE) the stream is still created and every
 * XrLiftDepthResultDXR reports @c depthValid XR_FALSE.
 *
 * @c preferredFormat is a DXGI_FORMAT hint (DXGI_FORMAT_R32_FLOAT or
 * DXGI_FORMAT_R16_FLOAT; 0 = the module's choice). The result's @c format is
 * authoritative.
 */
typedef struct XrLiftDepthRequestDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_DEPTH_REQUEST_DXR
    const void* XR_MAY_ALIAS next;
    int64_t                  preferredFormat; //!< DXGI_FORMAT hint; 0 = module's choice
} XrLiftDepthRequestDXR;

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
 * (metres; the runtime rebases them to the rect centre before the module sees
 * them, spec v2) — on the plain submit path and, since spec v3, on lifted
 * weave rects too. Chain XrLiftViewControlDXR on @c next to shape TRACKED
 * viewpoints (spec v2), and an XrDisplayRigDXR or XrCameraRigDXR
 * (XR_DXR_view_rig) to derive them with the app's own rig (spec v3, see
 * XrLiftResultViewpointsDXR). @c focalPx is the
 * submitted image's focal length in pixels of @c extent (for a photo, e.g. the
 * fx of an estimated intrinsics); <= 0 = unknown, the module assumes its
 * default field of view. Photo → Gaussians modules take it as input; DEPTH /
 * SBS / NVIEW modules ignore it.
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
    float                      focalPx;         //!< input focal length in input pixels; <= 0 = unknown (GAUSSIANS)
} XrLiftOptionsDXR;

/*!
 * Viewpoint policy for TRACKED lifts (spec v2, ADR-048). Chain on
 * XrLiftOptionsDXR::next, or directly on XrWeaveRectLiftDXR::next (the
 * direct chain wins if both are present). Ignored for EXPLICIT viewpoints and
 * for DEPTH / GAUSSIANS streams. Sticky per stream, like XrLiftOptionsDXR:
 * the last one sent applies until another is sent.
 *
 * Omitted entirely, the runtime applies the defaults: ipdFactor 1,
 * parallaxFactor 1, axisMode X, maxOffsetMeters 0 (no clamp), recenterMode
 * EASE_BACK, hold 1 s, time constant 2 s.
 *
 * Frame: every viewpoint is relative to the CENTRE of the lifted region — the
 * lifted weave rect, or for xrSubmitLiftFrameDXR the session's window (the
 * panel centre when the session has none) — in display axes (+x right, +y
 * up, +z toward the viewer), metres.
 *
 * Order of operations, per conversion: rebase the eyes to the rect centre;
 * scale their separation about their midpoint by @c ipdFactor (display-rig
 * step 1a); lerp their midpoint toward the nominal viewer (0, 0, nominal
 * distance) by 1 - @c parallaxFactor (step 1b); mask the midpoint's axes not
 * in @c axisMode; recenter (see XrLiftRecenterModeDXR); clamp the midpoint's
 * x / y offset to @c maxOffsetMeters.
 *
 * @c recenterHoldSeconds < 0 and @c recenterTimeConstantSeconds <= 0 select
 * the defaults (1 s, 2 s).
 */
typedef struct XrLiftViewControlDXR {
    XrStructureType            type;                        //!< XR_TYPE_LIFT_VIEW_CONTROL_DXR
    const void* XR_MAY_ALIAS   next;
    float                      ipdFactor;                   //!< [0, 1]: eye-separation scale (0 = mono)
    float                      parallaxFactor;              //!< [0, 1]: 0 = always the nominal viewer, 1 = follow
    XrLiftAxisModeDXR          axisMode;
    float                      maxOffsetMeters;             //!< x / y offset clamp, metres; <= 0 = none
    XrLiftRecenterModeDXR      recenterMode;
    float                      recenterHoldSeconds;         //!< < 0 = default (1 s)
    float                      recenterTimeConstantSeconds; //!< <= 0 = default (2 s)
} XrLiftViewControlDXR;

/*!
 * The viewpoints a texture result was actually synthesized for (spec v2,
 * ADR-048), after the viewpoint policy — so an app can render matching 3D
 * content for the same eyes. Chain on XrLiftResultDXR::next; filled on every
 * successful acquire. Same frame as XrLiftViewControlDXR (rect-centre
 * relative, display axes, metres); @c rectCenter is that origin in display
 * space (panel centre = 0) and @c rectSize the region's physical size.
 * @c viewpointCountOutput 0 = no viewpoints were known (the module used its
 * defaults), or a DEPTH stream.
 *
 * App rig (spec v3, ADR-048 Addendum A). An XrDisplayRigDXR or XrCameraRigDXR
 * chained on XrLiftOptionsDXR (or directly on XrWeaveRectLiftDXR; direct wins;
 * a camera rig wins over a display rig) makes the runtime derive the TRACKED
 * viewpoints with the SAME math xrLocateViews runs for that rig, with the
 * lifted rect as the screen: display rig = the processed eyes (ipdFactor,
 * parallaxFactor) scaled by perspectiveFactor; camera rig = the physical
 * viewer whose off-axis frustum onto the rect equals the camera rig's view
 * frustum (convergenceDiopters, verticalFov, metersToVirtual;
 * convergenceDiopters 0 = parallel cameras = one viewpoint for every view).
 * The rig's @c pose and virtualDisplayHeight only place the app's virtual
 * world and do not change the physical viewpoints. The rig replaces
 * XrLiftViewControlDXR's ipdFactor / parallaxFactor; its axisMode, clamp and
 * recentering still apply, to the viewer BEFORE the rig maps it — chain
 * XrLiftViewControlDXR with XR_LIFT_AXIS_MODE_XYZ_DXR and recentering OFF for
 * viewpoints identical to xrLocateViews'. The rig rides with the options,
 * like EXPLICIT viewpoints: a submit carrying options without a rig clears
 * it; a submit with no options keeps the stream's last one. Ignored for
 * EXPLICIT viewpoints and for DEPTH / GAUSSIANS streams.
 */
typedef struct XrLiftResultViewpointsDXR {
    XrStructureType    type;                 //!< XR_TYPE_LIFT_RESULT_VIEWPOINTS_DXR
    void* XR_MAY_ALIAS next;
    uint32_t           viewpointCountOutput; //!< 0..XR_LIFT_MAX_VIEWS_DXR
    XrVector3f         viewpoints[XR_LIFT_MAX_VIEWS_DXR];
    XrVector3f         rectCenter;           //!< display space, metres
    XrExtent2Df        rectSize;             //!< metres
} XrLiftResultViewpointsDXR;

/*!
 * The auxiliary depth map of a result (spec v3, ADR-048 Addendum A). Chain on
 * XrLiftResultDXR::next of an SBS / NVIEW stream created with
 * XrLiftDepthRequestDXR; filled on every successful acquire. @c depthValid
 * XR_FALSE = no depth for this result (a module without aux depth, or it
 * declined this frame); every other field is then zero.
 *
 * Handles follow the main result's export rule: @c depthTexture (a D3D11 NT
 * shared handle, runtime-owned, @c extent / @c format) and @c fence are
 * handed out on the FIRST acquire that carries depth and again whenever the
 * depth texture is reallocated; NULL otherwise. @c fence is the SAME fence
 * object as XrLiftResultDXR::fence (a second handle to it; close both) and
 * @c fenceValue equals XrLiftResultDXR::fenceValue: one wait covers the views
 * and the depth. The caller finishes sampling before its next acquire.
 *
 * Samples: decode @c s as d = valueScale * s + valueOffset, then
 * depth = d (LINEAR) or 1 / d (INVERSE), in @c units (METRIC = metres along
 * the source camera's optical axis; RELATIVE = unitless, larger = farther).
 * @c nearDepth / @c farDepth bound the decoded depth (0 = unknown).
 * @c sameInference XR_TRUE = the depth comes from the very inference that
 * produced this result's views; @c frameId always equals
 * XrLiftResultDXR::frameId.
 *
 * Intrinsics (@c intrinsicsValid): the source camera the module assumed, in
 * pixels of the depth map (u right, v down, origin at the top-left corner):
 * @c focalLengthPx and @c principalPointPx. With metric depth they place every
 * texel in the CAMERA's metric space:
 * X = (u - cx) * depth / fx, Y = -(v - cy) * depth / fy, Z = -depth.
 *
 * Display alignment (@c transformValid): @c depthToDisplay maps a texel into
 * DISPLAY space (metres, panel centre = 0, +x right, +y up, +z toward the
 * viewer — the frame of XrLiftResultViewpointsDXR::rectCenter) the way the
 * woven result presents it: the source image fills the lifted rect, seen from
 * @c viewpoint (the midpoint of the viewpoints the views were synthesized
 * for, rect-relative, as in XrLiftResultViewpointsDXR), and the depth
 * @c convergenceDepth (decoded units) lies ON the screen plane. Column-major
 * 4x4 (m[col * 4 + row]) applied to the homogeneous column
 * (un * depth, vn * depth, depth, 1), where un = (u + 0.5) / extent.width and
 * vn = (v + 0.5) / extent.height are the texel centre's normalised
 * coordinates; the output's w is 1. depth = convergenceDepth lands on the
 * rect, depth -> 0 converges on the viewpoint, and every texel stays on the
 * ray from the viewpoint through its rect position — so content the app
 * renders from XrLiftResultViewpointsDXR's eyes lines up with the lifted
 * picture. The spec (§4.3) derives it. Valid only with METRIC units, a known
 * @c convergenceDepth and a known rect; RELATIVE depth reports
 * transformValid / intrinsicsValid XR_FALSE rather than inventing a scale.
 */
typedef struct XrLiftDepthResultDXR {
    XrStructureType         type;             //!< XR_TYPE_LIFT_DEPTH_RESULT_DXR
    void* XR_MAY_ALIAS      next;
    XrBool32                depthValid;
    void*                   depthTexture;     //!< shared HANDLE on first depth acquire / realloc, else NULL
    void*                   fence;            //!< shared fence HANDLE with depthTexture, else NULL
    uint64_t                fenceValue;       //!< == XrLiftResultDXR::fenceValue
    XrExtent2Di             extent;           //!< depth map size, texels
    int64_t                 format;           //!< DXGI_FORMAT (R32_FLOAT / R16_FLOAT)
    XrLiftDepthSemanticsDXR units;
    XrLiftDepthEncodingDXR  encoding;
    float                   valueScale;
    float                   valueOffset;
    float                   nearDepth;        //!< decoded units; 0 = unknown
    float                   farDepth;         //!< decoded units; 0 = unknown
    XrBool32                sameInference;
    uint64_t                frameId;          //!< == XrLiftResultDXR::frameId
    XrBool32                intrinsicsValid;
    XrVector2f              focalLengthPx;    //!< fx, fy in depth-map pixels
    XrVector2f              principalPointPx; //!< cx, cy in depth-map pixels
    float                   convergenceDepth; //!< decoded depth on the screen plane; <= 0 = unknown
    XrVector3f              viewpoint;        //!< rect-relative metres (XrLiftResultViewpointsDXR frame)
    XrBool32                transformValid;
    float                   depthToDisplay[16]; //!< column-major; see above
} XrLiftDepthResultDXR;

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
    void* XR_MAY_ALIAS next;          //!< chain XrLiftResultViewpointsDXR (v2) / XrLiftDepthResultDXR (v3)
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
 * frame (not squeezed SBS); on the v6 N-view layout the 2D frame goes into
 * EVERY tile at the rect's scaled position (tile 0's copy is what gets lifted).
 * The service snapshots that region into @c stream's mailbox, and weaves the
 * stream's LATEST converted output at the rect's CURRENT position — so geometry
 * (drag, resize, scroll) is exact and real-time while the conversion itself
 * runs one frame behind. Until the stream's first result exists the rect is
 * woven FLAT: the service writes the 2D frame into both views (v3), or leaves
 * the caller's identical tiles as drawn (v6).
 *
 * @c stream must be an SBS or NVIEW stream of the same session.
 */
typedef struct XrWeaveRectLiftDXR {
    XrStructureType          type;      //!< XR_TYPE_WEAVE_RECT_LIFT_DXR
    const void* XR_MAY_ALIAS next;      //!< chain XrLiftOptionsDXR / XrLiftViewControlDXR / a view rig here
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

/*!
 * One stream's counters and EFFECTIVE conversion rate — what its priority
 * actually buys it under the current load (xrGetLiftStreamStatsDXR).
 */
typedef struct XrLiftStreamStatsDXR {
    XrStructureType     type;             //!< XR_TYPE_LIFT_STREAM_STATS_DXR
    void* XR_MAY_ALIAS  next;
    XrLiftPriorityDXR   priority;
    uint64_t            framesSubmitted;  //!< frames the mailbox accepted
    uint64_t            framesConverted;  //!< results published
    uint64_t            framesDropped;    //!< superseded before conversion (latest wins)
    uint64_t            framesFailed;     //!< conversions the module declined
    XrDuration          latencyLast;      //!< submit → result, ns
    XrDuration          latencyAverage;   //!< moving average, ns
    XrDuration          latencyMin;
    XrDuration          latencyMax;
    float               conversionRate;   //!< results per second, moving average (0 = none yet)
} XrLiftStreamStatsDXR;

typedef XrResult (XRAPI_PTR *PFN_xrGetLiftPropertiesDXR)(
    XrSession session, XrLiftPropertiesDXR* properties);
typedef XrResult (XRAPI_PTR *PFN_xrCreateLiftStreamDXR)(
    XrSession session, const XrLiftStreamCreateInfoDXR* createInfo, XrLiftStreamDXR* stream);
typedef XrResult (XRAPI_PTR *PFN_xrDestroyLiftStreamDXR)(XrLiftStreamDXR stream);
typedef XrResult (XRAPI_PTR *PFN_xrSubmitLiftFrameDXR)(
    XrLiftStreamDXR stream, const XrLiftFrameSubmitInfoDXR* submitInfo, uint64_t* frameId);
typedef XrResult (XRAPI_PTR *PFN_xrAcquireLiftResultDXR)(XrLiftStreamDXR stream, XrLiftResultDXR* result);
typedef XrResult (XRAPI_PTR *PFN_xrAcquireLiftBlobDXR)(XrLiftStreamDXR stream, XrLiftBlobDXR* blob);
typedef XrResult (XRAPI_PTR *PFN_xrSetLiftStreamPriorityDXR)(XrLiftStreamDXR stream, XrLiftPriorityDXR priority);
typedef XrResult (XRAPI_PTR *PFN_xrGetLiftStreamStatsDXR)(XrLiftStreamDXR stream, XrLiftStreamStatsDXR* stats);

/*
 * ===========================================================================
 * Spec v4 (ADR-053): tasks, per-task readiness and events, task-typed streams,
 * platform-neutral input images, result export and result contracts.
 *
 * Everything below is APPENDED: every v3 struct, enum value and entry point
 * above is unchanged. Detect spec 4 with extensionVersion >= 4. Enums with a
 * conversion-plug-in ABI counterpart use that ABI's values. In spec 4 an app
 * sees TASKS, never providers; spec 5 (below) adds a READ-ONLY candidate list
 * and a per-create HINT, never a way to choose for the user.
 * ===========================================================================
 */

/*!
 * WHAT to compute. A fixed vocabulary; a task is served by at most one
 * provider at a time, chosen by the runtime (an app may HINT a provider per
 * create, spec 5; the runtime still decides). RESERVED values
 * are refused (XR_ERROR_VALIDATION_FAILURE). Tasks marked "spec 5" are not
 * creatable on a spec-4 runtime.
 */
typedef enum XrLiftTaskDXR {
    XR_LIFT_TASK_DEPTH_MONO_DXR        = 1,  //!< one image -> depth (stills; no temporal state)
    XR_LIFT_TASK_DEPTH_VIDEO_DXR       = 2,  //!< frames -> depth, temporally stable; reset on discontinuity
    XR_LIFT_TASK_DISPARITY_STEREO_DXR  = 3,  //!< rectified pair + calibration -> disparity / depth (spec 5)
    XR_LIFT_TASK_VIEWS_MONO_DXR        = 4,  //!< image/frames -> N >= 2 views (+ same-inference aux depth)
    XR_LIFT_TASK_VIEWS_FROM_DEPTH_DXR  = 5,  //!< image + depth -> N views (spec 5)
    XR_LIFT_TASK_SPLATS_MONO_DXR       = 6,  //!< photo -> 3D Gaussian splats (job lane)
    XR_LIFT_TASK_SPLATS_STEREO_DXR     = 7,  //!< pair + calibration -> metric splats (spec 5, job lane)
    XR_LIFT_TASK_SPLATS_VIDEO_DXR      = 8,  //!< RESERVED
    XR_LIFT_TASK_INPAINT_DXR           = 9,  //!< RESERVED (a provider sub-stage, never app-creatable)
    XR_LIFT_TASK_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftTaskDXR;

/*! The shape of the input a task stream consumes. */
typedef enum XrLiftInputKindDXR {
    XR_LIFT_INPUT_KIND_MONO_DXR            = 1, //!< one colour image
    XR_LIFT_INPUT_KIND_STEREO_PAIR_DXR     = 2, //!< rectified L/R pair (spec 5)
    XR_LIFT_INPUT_KIND_IMAGE_AND_DEPTH_DXR = 3, //!< colour + depth image (spec 5)
    XR_LIFT_INPUT_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftInputKindDXR;
typedef XrFlags64 XrLiftInputKindFlagsDXR; //!< bit (1 << XrLiftInputKindDXR)

/*!
 * Readiness of a task's selected provider.
 *
 * Waited for (a create succeeds and its frames wait, latest-wins): UNKNOWN,
 * INSTALLED, DOWNLOADING, WARMING, EVICTED. Terminal for NEW creates (the
 * runtime falls through to the next provider, else the create is refused):
 * ABSENT, INCOMPATIBLE, UNLICENSED, FAILED (any reason), DISABLED. UNKNOWN is
 * reported only before the runtime has described any provider for the task.
 */
typedef enum XrLiftTaskStateDXR {
    XR_LIFT_TASK_STATE_UNKNOWN_DXR      = 0,  //!< discovery not settled; treat as waiting
    XR_LIFT_TASK_STATE_READY_DXR        = 1,  //!< conversions run now
    XR_LIFT_TASK_STATE_WARMING_DXR      = 2,  //!< loading / compiling / first inference; progress may be valid
    XR_LIFT_TASK_STATE_ABSENT_DXR       = 3,  //!< no provider can serve this task
    XR_LIFT_TASK_STATE_INCOMPATIBLE_DXR = 4,  //!< installed but unusable here (version / OS / GPU / driver)
    XR_LIFT_TASK_STATE_UNLICENSED_DXR   = 5,  //!< its licence check refused it
    XR_LIFT_TASK_STATE_FAILED_DXR       = 6,  //!< see XrLiftFailureReasonDXR; may recover later (event)
    XR_LIFT_TASK_STATE_DISABLED_DXR     = 7,  //!< switched off by user / policy
    XR_LIFT_TASK_STATE_INSTALLED_DXR    = 8,  //!< provider present, model cold; a stream / warm request starts warming
    XR_LIFT_TASK_STATE_DOWNLOADING_DXR  = 9,  //!< model being fetched; progress valid
    XR_LIFT_TASK_STATE_EVICTED_DXR      = 10, //!< was READY, unloaded after long idle; a reload reports WARMING
    XR_LIFT_TASK_STATE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftTaskStateDXR;

typedef enum XrLiftFailureReasonDXR {
    XR_LIFT_FAILURE_REASON_NONE_DXR        = 0,
    XR_LIFT_FAILURE_REASON_LICENCE_DXR     = 1, //!< licence missing / expired / not opted in
    XR_LIFT_FAILURE_REASON_LOAD_DXR        = 2, //!< provider or model failed to load
    XR_LIFT_FAILURE_REASON_PLATFORM_DXR    = 3, //!< GPU / OS / backend not supported
    XR_LIFT_FAILURE_REASON_NETWORK_DXR     = 4, //!< download failed
    XR_LIFT_FAILURE_REASON_VERSION_DXR     = 5, //!< provider or its engine outside the supported range
    XR_LIFT_FAILURE_REASON_RESOURCE_DXR    = 6, //!< not enough memory / VRAM
    XR_LIFT_FAILURE_REASON_MODEL_DXR       = 7, //!< model missing, bad hash or unsupported
    XR_LIFT_FAILURE_REASON_DEVICE_LOST_DXR = 8, //!< GPU device lost; the runtime re-warms (events follow)
    XR_LIFT_FAILURE_REASON_CRASHED_DXR     = 9, //!< provider host crash-loop cutoff; a single crash reads as WARMING
    XR_LIFT_FAILURE_REASON_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftFailureReasonDXR;

typedef enum XrLiftLicenceClassDXR {
    XR_LIFT_LICENCE_CLASS_UNKNOWN_DXR       = 0, //!< treated as RESEARCH_ONLY
    XR_LIFT_LICENCE_CLASS_PERMISSIVE_DXR    = 1, //!< open licence, commercial use permitted
    XR_LIFT_LICENCE_CLASS_COMMERCIAL_DXR    = 2, //!< proprietary, licensed for commercial use
    XR_LIFT_LICENCE_CLASS_RESEARCH_ONLY_DXR = 3, //!< research / evaluation only; present only when the USER opted in
    XR_LIFT_LICENCE_CLASS_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftLicenceClassDXR;

/*! Generic quality / latency preference: the ONLY lever an app has on which variant runs. */
typedef enum XrLiftQualityHintDXR {
    XR_LIFT_QUALITY_HINT_DEFAULT_DXR  = 0,
    XR_LIFT_QUALITY_HINT_LATENCY_DXR  = 1,
    XR_LIFT_QUALITY_HINT_BALANCED_DXR = 2,
    XR_LIFT_QUALITY_HINT_QUALITY_DXR  = 3,
    XR_LIFT_QUALITY_HINT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftQualityHintDXR;

typedef XrFlags64 XrLiftLaneFlagsDXR;
static const XrLiftLaneFlagsDXR XR_LIFT_LANE_STREAM_BIT_DXR = 0x00000001; //!< latest-wins streams
static const XrLiftLaneFlagsDXR XR_LIFT_LANE_JOB_BIT_DXR    = 0x00000002; //!< one-shot jobs (spec 5)

typedef XrFlags64 XrLiftOutputFlagsDXR;
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_AUX_DEPTH_BIT_DXR           = 0x00000001; //!< views + same-inference depth
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_METRIC_BIT_DXR              = 0x00000002; //!< depth in metres
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_INTRINSICS_BIT_DXR          = 0x00000004; //!< reports camera intrinsics
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_TEMPORAL_BIT_DXR            = 0x00000008; //!< temporally consistent output
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_EXPLICIT_VIEWPOINTS_BIT_DXR = 0x00000010; //!< honours explicit viewpoints exactly
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_CONVERGENCE_HINT_BIT_DXR    = 0x00000020; //!< emits a suggested convergence (the runtime decides)
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_CONFIDENCE_BIT_DXR          = 0x00000040; //!< RESERVED (confidence plane)
static const XrLiftOutputFlagsDXR XR_LIFT_OUTPUT_COMPOSED_BIT_DXR            = 0x00000080; //!< served by a runtime-composed pipeline (a depth task + the runtime's view synthesis)

/*!
 * Image handle kinds, for inputs (XrLiftImageDXR) and result exports. Values
 * 1..6 equal XrWeaveHandleKindDXR on purpose (same meaning, no dependency on
 * XR_DXR_weave).
 */
typedef enum XrLiftImageKindDXR {
    XR_LIFT_IMAGE_KIND_PLATFORM_DEFAULT_DXR = 0,
    XR_LIFT_IMAGE_KIND_D3D11_NT_DXR         = 1,  //!< NT handle, keyed mutex key 0 (= v3 inputTexture)
    XR_LIFT_IMAGE_KIND_D3D11_DXGI_DXR       = 2,  //!< legacy global DXGI handle (= v3 inputIsDxgi)
    XR_LIFT_IMAGE_KIND_IOSURFACE_DXR        = 3,
    XR_LIFT_IMAGE_KIND_AHARDWAREBUFFER_DXR  = 4,
    XR_LIFT_IMAGE_KIND_DMABUF_DXR           = 5,  //!< + XrLiftImageDmabufDescDXR on the image's next
    XR_LIFT_IMAGE_KIND_OPAQUE_FD_DXR        = 6,  //!< Vulkan OPAQUE_FD, same driver + device only
    XR_LIFT_IMAGE_KIND_D3D12_NT_DXR         = 7,  //!< ID3D12Device::CreateSharedHandle, no keyed mutex
    XR_LIFT_IMAGE_KIND_VK_OPAQUE_WIN32_DXR  = 8,  //!< Vulkan OPAQUE_WIN32 NT handle
    XR_LIFT_IMAGE_KIND_CPU_DXR              = 16, //!< bytes in caller memory; copied before return
    XR_LIFT_IMAGE_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftImageKindDXR;

typedef enum XrLiftSyncKindDXR {
    XR_LIFT_SYNC_KIND_NONE_DXR         = 0, //!< the kind's default (KEYED_MUTEX for D3D11_*, CPU_COMPLETE otherwise)
    XR_LIFT_SYNC_KIND_KEYED_MUTEX_DXR  = 1, //!< key 0 = caller done writing (v3)
    XR_LIFT_SYNC_KIND_D3D_FENCE_DXR    = 2, //!< shared ID3D11Fence / ID3D12Fence NT handle + value
    XR_LIFT_SYNC_KIND_SYNC_FILE_DXR    = 3, //!< sync_file fd (Linux / Android)
    XR_LIFT_SYNC_KIND_VK_TIMELINE_DXR  = 4, //!< exported timeline semaphore (fd or NT handle) + value
    XR_LIFT_SYNC_KIND_SHARED_EVENT_DXR = 5, //!< macOS shared event + value
    XR_LIFT_SYNC_KIND_CPU_COMPLETE_DXR = 6, //!< writes complete before the call (the v3 D3D12 rule)
    XR_LIFT_SYNC_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftSyncKindDXR;

typedef enum XrLiftPixelFormatDXR {
    XR_LIFT_PIXEL_FORMAT_UNKNOWN_DXR      = 0,
    XR_LIFT_PIXEL_FORMAT_RGBA8_UNORM_DXR  = 1,
    XR_LIFT_PIXEL_FORMAT_RGBA8_SRGB_DXR   = 2,
    XR_LIFT_PIXEL_FORMAT_BGRA8_UNORM_DXR  = 3,
    XR_LIFT_PIXEL_FORMAT_BGRA8_SRGB_DXR   = 4,
    XR_LIFT_PIXEL_FORMAT_NV12_DXR         = 5,
    XR_LIFT_PIXEL_FORMAT_R8_UNORM_DXR     = 6,
    XR_LIFT_PIXEL_FORMAT_R16_FLOAT_DXR    = 7,
    XR_LIFT_PIXEL_FORMAT_R32_FLOAT_DXR    = 8,
    XR_LIFT_PIXEL_FORMAT_RGBA16_FLOAT_DXR = 9,
    XR_LIFT_PIXEL_FORMAT_R16_UNORM_DXR    = 10,
    XR_LIFT_PIXEL_FORMAT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftPixelFormatDXR;

typedef enum XrLiftColorSpaceDXR {
    XR_LIFT_COLOR_SPACE_UNKNOWN_DXR       = 0, //!< treated as sRGB-encoded
    XR_LIFT_COLOR_SPACE_SRGB_DXR          = 1,
    XR_LIFT_COLOR_SPACE_LINEAR_SRGB_DXR   = 2,
    XR_LIFT_COLOR_SPACE_BT709_LIMITED_DXR = 3, //!< video-range YUV (the NV12 default)
    XR_LIFT_COLOR_SPACE_BT709_FULL_DXR    = 4,
    XR_LIFT_COLOR_SPACE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftColorSpaceDXR;

typedef enum XrLiftImageRoleDXR {
    XR_LIFT_IMAGE_ROLE_COLOR_DXR       = 1, //!< mono input, or the first/left eye, or a whole SBS pair
    XR_LIFT_IMAGE_ROLE_COLOR_RIGHT_DXR = 2, //!< second eye when the pair is two images (spec 5)
    XR_LIFT_IMAGE_ROLE_DEPTH_DXR       = 3, //!< depth input (+ XrLiftDepthInfoDXR on next), spec 5
    XR_LIFT_IMAGE_ROLE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftImageRoleDXR;

/*! What a decoded depth sample means. */
typedef enum XrLiftDepthKindDXR {
    XR_LIFT_DEPTH_KIND_METRIC_DEPTH_DXR         = 1, //!< metres (v3: METRIC + LINEAR)
    XR_LIFT_DEPTH_KIND_METRIC_INVERSE_DXR       = 2, //!< 1/metres (v3: METRIC + INVERSE)
    XR_LIFT_DEPTH_KIND_AFFINE_INVERSE_DEPTH_DXR = 3, //!< unknown scale+shift of 1/depth, larger = nearer (v3: RELATIVE + INVERSE)
    XR_LIFT_DEPTH_KIND_AFFINE_DISPARITY_DXR     = 4, //!< disparity up to scale+shift (v3: RELATIVE + INVERSE)
    XR_LIFT_DEPTH_KIND_AFFINE_DEPTH_DXR         = 5, //!< unknown scale+shift, larger = farther (v3: RELATIVE + LINEAR)
    XR_LIFT_DEPTH_KIND_PIXEL_DISPARITY_DXR      = 6, //!< exact disparity in pixels of a rectified pair (spec 5)
    XR_LIFT_DEPTH_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftDepthKindDXR;

typedef enum XrLiftDepthMeasureDXR {
    XR_LIFT_DEPTH_MEASURE_PLANAR_DXR = 0, //!< distance to the camera plane (optical-axis z)
    XR_LIFT_DEPTH_MEASURE_RAY_DXR    = 1, //!< distance along the pixel ray
    XR_LIFT_DEPTH_MEASURE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftDepthMeasureDXR;

typedef enum XrLiftIntrinsicsSourceDXR {
    XR_LIFT_INTRINSICS_SOURCE_UNKNOWN_DXR   = 0, //!< no intrinsics; fields are 0
    XR_LIFT_INTRINSICS_SOURCE_GIVEN_DXR     = 1, //!< from the caller (focalPx)
    XR_LIFT_INTRINSICS_SOURCE_ESTIMATED_DXR = 2, //!< estimated from the pixels
    XR_LIFT_INTRINSICS_SOURCE_ASSUMED_DXR   = 3, //!< a provider default (e.g. a fixed FOV); not evidence
    XR_LIFT_INTRINSICS_SOURCE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftIntrinsicsSourceDXR;

typedef enum XrLiftCoordinateSystemDXR {
    XR_LIFT_COORDINATE_SYSTEM_UNKNOWN_DXR    = 0,
    XR_LIFT_COORDINATE_SYSTEM_OPENCV_RDF_DXR = 1, //!< +x right, +y down, +z forward, metres
    XR_LIFT_COORDINATE_SYSTEM_RUB_DXR        = 2, //!< +x right, +y up, +z back (OpenXR / glTF)
    XR_LIFT_COORDINATE_SYSTEM_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftCoordinateSystemDXR;

/*!
 * View layout of a views result. ATLAS stays reserved while the view ceiling
 * is XR_LIFT_MAX_VIEWS_DXR (8): at 8 views ROW always fits the texture limit.
 */
typedef enum XrLiftViewsLayoutDXR {
    XR_LIFT_VIEWS_LAYOUT_ROW_DXR          = 0, //!< v3: side by side, view 0 leftmost
    XR_LIFT_VIEWS_LAYOUT_ARRAY_LAYERS_DXR = 1, //!< one array layer per view
    XR_LIFT_VIEWS_LAYOUT_ATLAS_DXR        = 2, //!< RESERVED: columns x rows, row-major
    XR_LIFT_VIEWS_LAYOUT_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftViewsLayoutDXR;

typedef enum XrLiftCameraModelDXR {
    XR_LIFT_CAMERA_MODEL_UNKNOWN_DXR        = 0,
    XR_LIFT_CAMERA_MODEL_OFF_AXIS_DXR       = 1, //!< asymmetric frusta onto the lifted rect
    XR_LIFT_CAMERA_MODEL_PARALLEL_SHIFT_DXR = 2, //!< parallel cameras + horizontal image shift
    XR_LIFT_CAMERA_MODEL_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftCameraModelDXR;

typedef enum XrLiftConvergenceSourceDXR {
    XR_LIFT_CONVERGENCE_SOURCE_UNKNOWN_DXR      = 0,
    XR_LIFT_CONVERGENCE_SOURCE_APP_DXR          = 1, //!< XrLiftOptionsDXR::convergence
    XR_LIFT_CONVERGENCE_SOURCE_RUNTIME_AUTO_DXR = 2, //!< runtime policy (AUTO)
    XR_LIFT_CONVERGENCE_SOURCE_PROVIDER_DXR     = 3, //!< the provider's own
    XR_LIFT_CONVERGENCE_SOURCE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftConvergenceSourceDXR;

/*!
 * The EFFECTIVE capability of one task on this session: the provider the
 * runtime selected for it, its readiness and its output contract.
 * xrEnumerateLiftTasksDXR returns one element per task that is not ABSENT, in
 * ascending task order. Array element: never grows (extend via @c next).
 *
 * @c imageKinds / @c exportKinds are what THIS session can hand in / take out
 * (bits (1 << XrLiftImageKindDXR); they depend on the session's graphics API
 * and platform; 0 = the task is not reachable from this session yet).
 *
 * Consumer contract: anything but READY means WAIT (poll at <= 2 Hz, or take
 * XrEventDataLiftTaskStateChangedDXR), never a session-long fallback to an
 * in-app converter.
 *
 * Selection: the provider is chosen when a stream is CREATED and never
 * reorders on readiness (a waiting state is waited for, a terminal one falls
 * through). A live stream moves only on a terminal failure of its provider or
 * a drain of the provider's host (update / uninstall), signalled by
 * XrLiftDepthInfoDXR::discontinuity on its next result plus a
 * XrEventDataLiftTaskStateChangedDXR.
 */
typedef struct XrLiftTaskPropertiesDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_TASK_PROPERTIES_DXR
    void* XR_MAY_ALIAS       next;
    XrLiftTaskDXR            task;
    XrLiftTaskStateDXR       state;
    XrLiftFailureReasonDXR   failureReason;   //!< NONE unless FAILED / DISABLED
    float                    progress;        //!< [0,1] while DOWNLOADING / WARMING; < 0 = unknown
    XrLiftLaneFlagsDXR       lanes;
    XrLiftInputKindFlagsDXR  inputKinds;
    XrLiftOutputFlagsDXR     outputs;
    XrFlags64                depthKinds;      //!< bit (1 << XrLiftDepthKindDXR) the task can return
    XrFlags64                blobFormats;     //!< bit (1 << XrLiftBlobFormatDXR) (splat / job outputs)
    XrFlags64                imageKinds;      //!< bit (1 << XrLiftImageKindDXR) accepted as input here
    XrFlags64                exportKinds;     //!< bit (1 << XrLiftImageKindDXR) offered as result export here
    uint32_t                 maxStreams;      //!< for this task's provider, runtime-wide; 0 = no stream lane
    uint32_t                 maxViews;        //!< VIEWS_* tasks (enforced at create); else 0
    XrExtent2Di              maxInputExtent;  //!< larger inputs are downscaled by the runtime; 0 = none
    XrDuration               typicalLatency;  //!< per conversion, ns; 0 = unknown
    XrDuration               warmEstimate;    //!< INSTALLED / EVICTED -> READY, ns; 0 = unknown
    XrLiftLicenceClassDXR    licenceClass;
} XrLiftTaskPropertiesDXR;

/*!
 * The provider set behind the tasks changed (install / uninstall / user
 * preference). Re-enumerate the tasks: their state, outputs and limits may
 * have changed. Live streams do not move. Delivered only to sessions that
 * made a spec-4 call (opt-in, see xrEnumerateLiftTasksDXR). Spec 5: also
 * delivered when the candidate list a session enumerated
 * (xrEnumerateLiftTaskCandidatesDXR) changed, e.g. a candidate behind the
 * selection became READY or FAILED; re-enumerate the candidates too.
 */
typedef struct XrEventDataLiftProvidersChangedDXR {
    XrStructureType          type;            //!< XR_TYPE_EVENT_DATA_LIFT_PROVIDERS_CHANGED_DXR
    const void* XR_MAY_ALIAS next;
    XrSession                session;
} XrEventDataLiftProvidersChangedDXR;

/*!
 * A task's EFFECTIVE state changed, or its progress moved by >= 5 % (at most
 * 2 Hz). Per task, events are ordered and deduplicated: @c previousState is
 * always the @c state of the previous event delivered for that task (or of
 * the session's first spec-4 query), and an event never repeats an unchanged
 * state. Delivered only to sessions that made a spec-4 call.
 */
typedef struct XrEventDataLiftTaskStateChangedDXR {
    XrStructureType          type;            //!< XR_TYPE_EVENT_DATA_LIFT_TASK_STATE_CHANGED_DXR
    const void* XR_MAY_ALIAS next;
    XrSession                session;
    XrLiftTaskDXR            task;
    XrLiftTaskStateDXR       state;
    XrLiftTaskStateDXR       previousState;
    XrLiftFailureReasonDXR   failureReason;
    float                    progress;        //!< < 0 = unknown
} XrEventDataLiftTaskStateChangedDXR;

/*!
 * Chain on XrLiftStreamCreateInfoDXR::next to create a stream for a TASK.
 * XrLiftStreamCreateInfoDXR::mode is then either 0 or the legacy mode matching
 * the task (DEPTH for DEPTH_*, SBS / NVIEW for VIEWS_MONO); anything else is
 * XR_ERROR_VALIDATION_FAILURE. Setting the matching legacy mode makes the
 * create degrade gracefully on a spec-3 runtime, which ignores this struct.
 * contentHint and inputScale keep their v3 meaning.
 *
 * @c viewCount (VIEWS_* tasks): 2..maxViews, or 0 = "match the weave" (on a
 * lifted weave rect, the active mode's view count; on a plain stream, 2).
 *
 * @c exportKind: how results leave the runtime (0 = the platform default,
 * D3D11_NT on Windows, exactly v3). Must be a bit of the task's exportKinds.
 *
 * Creation rules, after selection has fallen through every terminal
 * candidate:
 *  - XR_ERROR_FEATURE_UNSUPPORTED only when the task has no non-terminal
 *    candidate (ABSENT, INCOMPATIBLE, UNLICENSED, FAILED, DISABLED), has no
 *    stream lane, or when a @c requiredOutputs bit is missing from a provider
 *    that is READY. Before READY, @c requiredOutputs is not grounds for a
 *    refusal (a provider may report its outputs only once loaded).
 *  - XR_ERROR_VALIDATION_FAILURE for a reserved or spec-5 task, a @c viewCount
 *    above the task's maxViews, an input kind or export kind no candidate
 *    offers, or a mode that does not match the task.
 *  - XR_ERROR_LIMIT_REACHED past the task's maxStreams.
 * In every waiting state the stream is created and its frames wait,
 * latest-wins, until READY.
 */
typedef struct XrLiftTaskStreamCreateInfoDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS next;
    XrLiftTaskDXR            task;
    XrLiftInputKindDXR       inputKind;       //!< must be a bit of the task's inputKinds
    XrLiftQualityHintDXR     qualityHint;
    uint32_t                 viewCount;       //!< VIEWS_*: 0 = match the weave / 2
    XrLiftOutputFlagsDXR     requiredOutputs; //!< e.g. METRIC | AUX_DEPTH: refuse rather than degrade
    XrLiftImageKindDXR       exportKind;      //!< 0 = platform default
} XrLiftTaskStreamCreateInfoDXR;

/*!
 * One input image, any platform. Handles are BORROWED: the runtime duplicates
 * whatever it keeps. CPU bytes are copied before the call returns. A GPU image
 * is SNAPSHOTTED before return (one copy on the runtime's GPU), honouring
 * @c syncKind, so the caller may rewrite it afterwards. Exception:
 * CPU_COMPLETE on a kind without a keyed mutex, where the v3 D3D12 rule holds
 * (do not rewrite until a later frame; double-buffer).
 */
typedef struct XrLiftImageDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_IMAGE_DXR
    const void* XR_MAY_ALIAS next;            //!< XrLiftImageDmabufDescDXR / XrLiftDepthInfoDXR (DEPTH role)
    XrLiftImageRoleDXR       role;
    XrLiftImageKindDXR       kind;
    void*                    handle;          //!< HANDLE / IOSurfaceRef / AHardwareBuffer* / (void*)(intptr_t)fd; NULL for CPU and DMABUF
    const void*              cpuBytes;        //!< CPU only
    uint32_t                 cpuRowPitch;     //!< CPU only, bytes
    XrLiftPixelFormatDXR     format;
    XrLiftColorSpaceDXR      colorSpace;
    XrRect2Di                region;          //!< sub-rect to convert (v3 = {0,0, extent}); CPU: the image is region.extent
    XrLiftSyncKindDXR        syncKind;
    void*                    syncHandle;      //!< per syncKind; NULL for NONE / KEYED_MUTEX / CPU_COMPLETE
    uint64_t                 syncValue;       //!< fence / timeline value to wait on
} XrLiftImageDXR;

/*! Field-for-field XrWeaveDmabufDescDXR (separate type: no XR_DXR_weave dependency). fd BORROWED. */
typedef struct XrLiftImageDmabufDescDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_IMAGE_DMABUF_DESC_DXR
    const void* XR_MAY_ALIAS next;
    int32_t                  fd;
    uint32_t                 width;
    uint32_t                 height;
    uint32_t                 drmFourcc;
    uint64_t                 drmModifier;
    uint32_t                 planeCount;
    uint32_t                 offsets[4];
    uint32_t                 strides[4];
    uint64_t                 bufferId;        //!< stable caller identity for the import cache; 0 = none
} XrLiftImageDmabufDescDXR;

/*!
 * Chain on XrLiftFrameSubmitInfoDXR::next. When present it REPLACES the base
 * struct's inputTexture / inputIsDxgi / extent; inputTexture may then be NULL
 * (a spec-4 runtime relaxes v3's non-NULL check in this case only). 1..3
 * images, at most one per role; a spec-4 runtime accepts the COLOR role only.
 * The image's kind must be one of the task's imageKinds for the session (on
 * Windows: D3D11_NT, D3D11_DXGI, D3D12_NT, CPU; the Vulkan, dma-buf,
 * IOSurface and AHardwareBuffer kinds are listed only once their transports
 * exist), its region any sub-rect of the image.
 */
typedef struct XrLiftFrameImagesDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_FRAME_IMAGES_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 imageCount;
    const XrLiftImageDXR*    images;
} XrLiftFrameImagesDXR;

typedef XrFlags64 XrLiftFrameControlFlagsDXR;
//! The source jumped (seek, cut, source switch): drop temporal state before this frame.
static const XrLiftFrameControlFlagsDXR XR_LIFT_FRAME_CONTROL_DISCONTINUITY_BIT_DXR = 0x00000001;

/*! Chain on XrLiftFrameSubmitInfoDXR::next or XrWeaveRectLiftDXR::next. Per frame, not sticky. */
typedef struct XrLiftFrameControlDXR {
    XrStructureType            type;          //!< XR_TYPE_LIFT_FRAME_CONTROL_DXR
    const void* XR_MAY_ALIAS   next;
    XrLiftFrameControlFlagsDXR flags;
} XrLiftFrameControlDXR;

/*!
 * Chain on XrLiftResultDXR::next. Says how outputTexture / fence are to be
 * read when the stream's exportKind is not D3D11_NT; XrLiftResultDXR::format
 * is then 0 and these fields are authoritative. On a D3D11_NT stream the
 * struct is left untouched. Handles arrive under the v3 rule: only on the
 * first acquire and on reallocation.
 *
 * A CPU export: XrLiftResultDXR::outputTexture points at the result's bytes,
 * tightly packed (extent.width * bytes-per-pixel per row), runtime-owned and
 * valid until the next acquire on the stream; fence is NULL; @c kind is CPU
 * and @c syncKind CPU_COMPLETE. An auxiliary depth map keeps its D3D11 NT
 * export.
 */
typedef struct XrLiftResultExportDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_RESULT_EXPORT_DXR
    void* XR_MAY_ALIAS       next;
    XrLiftImageKindDXR       kind;            //!< of XrLiftResultDXR::outputTexture (and of the aux depth texture)
    XrLiftPixelFormatDXR     format;
    XrLiftColorSpaceDXR      colorSpace;
    XrLiftSyncKindDXR        syncKind;        //!< of XrLiftResultDXR::fence; value = fenceValue
} XrLiftResultExportDXR;

/*!
 * Everything needed to interpret a depth image. Chain on XrLiftResultDXR::next
 * (a DEPTH_* stream's result, or a VIEWS_* stream's aux depth) or on
 * XrLiftDepthResultDXR::next.
 *
 * Decoding (normative). A sample s DECODES to v = rawScale * s + rawShift.
 * The range fields and convergenceValue are in these decoded units v, never
 * inverted (invalidValue is a raw sample). nearZ / farZ are DEPTH READINGS d
 * (the quantity of the table's middle column, and of spec 3's
 * convergenceDepth), so nearZ < farZ for every kind and farZ < nearZ keeps
 * meaning an infinite far plane. From v, per @c kind:
 *
 *   kind                  depth reading d        inverse-depth coordinate q
 *   METRIC_DEPTH          d = v (metres)         q = 1 / v  (1/metres)
 *   METRIC_INVERSE        d = 1 / v (metres)     q = v      (1/metres)
 *   AFFINE_INVERSE_DEPTH  d = 1 / v (relative)   q = v
 *   AFFINE_DISPARITY      d = 1 / v (relative)   q = v
 *   AFFINE_DEPTH          d = v (relative)       q = 1 / v
 *
 * A metric distance exists only for the METRIC_* kinds; an AFFINE_* d or q is
 * known only up to an unknown scale and shift (an order, never a distance).
 * The display relief (XrLiftViewsInfoDXR::reliefScale) is affine in q; the
 * spec-3 XrLiftDepthResultDXR::convergenceDepth is d of convergenceValue.
 *
 * Units are never invented: with an AFFINE_* kind, near / far and the
 * intrinsics are as reported, or zero. farZ < nearZ means an infinite far
 * plane; both 0 = unknown. The range fields are THIS frame's statistics in
 * decoded units; for an inverse kind their v of the nearest content is the
 * LARGEST (rangeMax), while nearZ is the smallest d.
 */
typedef struct XrLiftDepthInfoDXR {
    XrStructureType           type;               //!< XR_TYPE_LIFT_DEPTH_INFO_DXR
    void* XR_MAY_ALIAS        next;
    XrLiftDepthKindDXR        kind;
    XrLiftDepthMeasureDXR     measure;
    float                     rawScale;
    float                     rawShift;
    XrBool32                  invalidValueValid;
    float                     invalidValue;       //!< raw sample meaning "no depth"
    float                     nearZ;              //!< depth reading d of the nearest content; 0 = unknown
    float                     farZ;               //!< depth reading d; < nearZ = infinite; 0 = unknown
    XrBool32                  rangeValid;
    float                     rangeMin;           //!< decoded, this frame
    float                     rangeMax;
    float                     rangeP05;           //!< decoded, this frame (robust)
    float                     rangeP95;
    XrLiftIntrinsicsSourceDXR intrinsicsSource;
    XrVector2f                focalLengthPx;      //!< fx, fy in pixels of THIS image
    XrVector2f                principalPointPx;   //!< cx, cy
    XrLiftCoordinateSystemDXR convention;         //!< of back-projection with these intrinsics
    XrBool32                  temporallyConsistent; //!< produced by a temporal (video) model
    XrBool32                  discontinuity;      //!< temporal state was reset before this result
    float                     baselineMeters;     //!< PIXEL_DISPARITY: the pair's baseline; else 0
    XrBool32                  convergenceValid;
    float                     convergenceValue;   //!< decoded v placed at zero disparity (on the screen plane)
} XrLiftDepthInfoDXR;

/*!
 * How a views result was made. Chain on XrLiftResultDXR::next of a VIEWS_*
 * (SBS / NVIEW) stream. Distances are in metres in the SOURCE camera's frame.
 * Disparities are fractions of one view's width (positive = uncrossed).
 */
typedef struct XrLiftViewsInfoDXR {
    XrStructureType            type;                 //!< XR_TYPE_LIFT_VIEWS_INFO_DXR
    void* XR_MAY_ALIAS         next;
    XrLiftViewsLayoutDXR       layout;
    uint32_t                   viewCount;
    uint32_t                   columns;              //!< ROW: viewCount
    uint32_t                   rows;                 //!< ROW: 1
    XrExtent2Di                viewExtent;           //!< one tile
    XrBool32                   eyeOffsetsValid;
    float                      eyeOffsetMeters[XR_LIFT_MAX_VIEWS_DXR]; //!< x of each view from the source camera
    float                      baselineMeters;       //!< first-to-last view distance rendered; 0 = unknown
    float                      horizontalFovRadians; //!< of each view; 0 = unknown
    float                      disparityAdjust;      //!< shift that placed the zero-parallax plane, fraction of width
    XrBool32                   disparityRangeValid;
    float                      disparityMin;         //!< rendered disparity range, fraction of width
    float                      disparityMax;
    XrLiftCameraModelDXR       cameraModel;
    XrLiftConvergenceSourceDXR convergenceSource;
    float                      convergenceApplied;   //!< in v3 convergence units [0,1]; < 0 = unknown
    float                      suggestedConvergence; //!< provider hint, same units; < 0 = none (diagnostics)
    float                      suggestedStrength;    //!< provider hint for the disparity budget; < 0 = none
    XrBool32                   reliefValid;          //!< display z (m, toward the viewer) of a texel: z = reliefScale * q + reliefOffset, q per XrLiftDepthInfoDXR
    float                      reliefScale;
    float                      reliefOffset;
} XrLiftViewsInfoDXR;

typedef XrResult (XRAPI_PTR *PFN_xrEnumerateLiftTasksDXR)(
    XrSession session, uint32_t capacityInput, uint32_t* countOutput, XrLiftTaskPropertiesDXR* tasks);
typedef XrResult (XRAPI_PTR *PFN_xrRequestLiftTaskWarmDXR)(XrSession session, XrLiftTaskDXR task);

/*
 * ===========================================================================
 * Spec v5 (#2178, ADR-054 decision 8): provider candidates and the per-create
 * preference hint.
 *
 * APPENDED: every spec-4 struct, enum value and entry point is unchanged.
 * Detect spec 5 with extensionVersion >= 5. A spec-4 runtime ignores
 * XrLiftProviderPreferenceDXR on a create (the runtime's default applies) and
 * does not expose xrEnumerateLiftTaskCandidatesDXR (xrGetInstanceProcAddr
 * fails).
 *
 * Apps express preferences, never policy: the user's default per task is set
 * in the Dashboard / CLI (`displayxr-cli conv use <task> <id>`), and nothing
 * here writes it. Show the candidates' strings verbatim; never hardcode or
 * recognise a provider id or name. A browser must not expose provider identity
 * to web content (it shows at most waiting / ready / unavailable).
 * ===========================================================================
 */

/*! Where a candidate runs (spec 5). */
typedef enum XrLiftProviderHostingDXR {
    //! Loaded in the runtime's service process, on this machine.
    XR_LIFT_PROVIDER_HOSTING_LOCAL_IN_SERVICE_DXR = 0,
    //! In its own supervised host process, on this machine.
    XR_LIFT_PROVIDER_HOSTING_LOCAL_SATELLITE_DXR  = 1,
    //! Pictures leave this machine (a remote endpoint; per-user consent applies).
    XR_LIFT_PROVIDER_HOSTING_REMOTE_DXR           = 2,
    XR_LIFT_PROVIDER_HOSTING_MAX_ENUM_DXR = 0x7FFFFFFF
} XrLiftProviderHostingDXR;

/*!
 * One provider that could serve a task (spec 5), READ-ONLY. One element per
 * provider, in the runtime's selection order: the providers the runtime would
 * consider for a create of the task (enabled, not superseded, licence-eligible
 * -- a RESEARCH_ONLY / UNKNOWN-licence provider appears only when the USER
 * opted in -- and able to serve the task's lane / view count), whatever their
 * readiness. Array element: never grows (extend via @c next).
 *
 * @c providerId is the provider's registration id: the string to put in
 * XrLiftProviderPreferenceDXR. @c displayName is the provider's own name for
 * the capability that serves the task, VERBATIM (the runtime never names a
 * provider). @c state / @c failureReason are that capability's readiness (the
 * same vocabulary as XrLiftTaskPropertiesDXR; a terminal state means a hint to
 * it falls through). @c isUserDefault: the user's per-task default
 * (PreferredProvider) names this provider. @c isSelected: a create of the task
 * WITHOUT a hint, made now, would bind this provider (at most one element;
 * none when every candidate is terminal).
 *
 * The list changes with installs, uninstalls, the user's default and any
 * candidate's readiness; each such change is announced by
 * XrEventDataLiftProvidersChangedDXR to a session that has enumerated.
 */
typedef struct XrLiftTaskCandidateDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_TASK_CANDIDATE_DXR
    void* XR_MAY_ALIAS       next;
    char                     providerId[XR_LIFT_PROVIDER_ID_MAX_SIZE_DXR];
    char                     displayName[XR_LIFT_PROVIDER_NAME_MAX_SIZE_DXR];
    XrLiftTaskStateDXR       state;
    XrLiftFailureReasonDXR   failureReason;   //!< NONE unless FAILED / DISABLED
    XrLiftLicenceClassDXR    licenceClass;
    XrLiftProviderHostingDXR hosting;
    XrBool32                 isUserDefault;
    XrBool32                 isSelected;
} XrLiftTaskCandidateDXR;

/*!
 * Chain on XrLiftTaskStreamCreateInfoDXR::next (spec 5): a per-create HINT
 * naming the provider this app would like for this stream. It applies to job
 * creation too once the job API lands (it will chain on the job create info
 * with the same meaning).
 *
 * @c providerId: a candidate's providerId, optionally "/<model id>" to name
 * one of its capabilities. The hint moves the FIRST non-terminal candidate
 * row it matches to the front of the selection for THIS create only (and for
 * this stream's own rebinds after a terminal failure). The runtime still
 * arbitrates: the user's and the machine's policy decide what is a candidate
 * at all, and readiness falls through as usual. An unknown, filtered or
 * terminal provider id is not an error: the create proceeds with the normal
 * selection (the runtime logs one INFO line). NULL or "" = no hint. It never
 * changes the user's / machine's default, and it does not move a live stream.
 * The create's validation (view count, required outputs, limits) is checked
 * against the provider the create binds.
 */
typedef struct XrLiftProviderPreferenceDXR {
    XrStructureType          type;            //!< XR_TYPE_LIFT_PROVIDER_PREFERENCE_DXR
    const void* XR_MAY_ALIAS next;
    const char*              providerId;
} XrLiftProviderPreferenceDXR;

typedef XrResult (XRAPI_PTR *PFN_xrEnumerateLiftTaskCandidatesDXR)(
    XrSession session, XrLiftTaskDXR task, uint32_t capacityInput, uint32_t* countOutput,
    XrLiftTaskCandidateDXR* candidates);

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

//! Set the stream's scheduling priority (see XrLiftPriorityDXR). Cheap; takes
//! effect at the lift thread's next round. Default NORMAL.
XRAPI_ATTR XrResult XRAPI_CALL xrSetLiftStreamPriorityDXR(XrLiftStreamDXR stream, XrLiftPriorityDXR priority);

//! The stream's counters and effective conversion rate. Cheap (one IPC round
//! trip, no GPU work) — poll it at UI rates, not per frame.
XRAPI_ATTR XrResult XRAPI_CALL xrGetLiftStreamStatsDXR(XrLiftStreamDXR stream, XrLiftStreamStatsDXR* stats);

//! Spec 4: the effective capability and readiness of every task that is not
//! ABSENT, in ascending task order (two-call idiom). Cheap: no provider is
//! loaded by it. The first spec-4 call of a session opts it into the
//! task / provider events.
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateLiftTasksDXR(
    XrSession session, uint32_t capacityInput, uint32_t* countOutput, XrLiftTaskPropertiesDXR* tasks);

//! Spec 4: ask the runtime to warm @p task's provider now (INSTALLED / EVICTED
//! -> WARMING -> READY). Never blocks; progress arrives as events or on the
//! next enumeration. XR_ERROR_VALIDATION_FAILURE for a reserved task.
XRAPI_ATTR XrResult XRAPI_CALL xrRequestLiftTaskWarmDXR(XrSession session, XrLiftTaskDXR task);

//! Spec 5: the read-only candidate providers of @p task, in selection order
//! (two-call idiom). Cheap: nothing is loaded or warmed by it. 0 candidates
//! is XR_SUCCESS (no provider, or no service yet), never an error.
//! XR_ERROR_VALIDATION_FAILURE for a reserved task. Like every spec-4 call it
//! opts the session into the task / provider events.
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateLiftTaskCandidatesDXR(
    XrSession session, XrLiftTaskDXR task, uint32_t capacityInput, uint32_t* countOutput,
    XrLiftTaskCandidateDXR* candidates);

#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_LIFT_H
