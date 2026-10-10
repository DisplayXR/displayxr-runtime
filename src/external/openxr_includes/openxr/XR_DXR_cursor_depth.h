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
 * @brief  Header for XR_DXR_cursor_depth extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * A DEPTH-AWARE CURSOR: lift the pointer off the display plane so it never
 * sits behind the content it is hovering.
 *
 * The OS cursor is flat — it is drawn at zero disparity, on the physical
 * display plane (the ZDP). Hover it over content that pops out of the glass
 * and the cursor is drawn ON TOP of pixels whose disparity says they are IN
 * FRONT of it: an occlusion-vs-disparity conflict (a "depth violation"), the
 * same class of conflict ADR-040 handles for rear content over the desktop.
 * The fix is to draw the cursor as a stereo sprite at the depth of the nearest
 * content under it.
 *
 * Division of labour (ADR-046): the APP owns the content and so the depth
 * under the cursor (it raycasts, or reads back its depth buffer, or samples a
 * depth map); the RUNTIME owns the placement — where along the viewer's line
 * of sight the sprite goes, how big it must be to keep a constant apparent
 * size, and how it moves over time (rise fast, sink slowly, a small margin in
 * front, a comfort clamp) — so every DisplayXR app's cursor behaves alike; the
 * app draws the sprite at the pose it is handed.
 *
 * OPT-IN, ZERO COST OTHERWISE. Enabling the extension costs nothing. The
 * runtime does cursor work only for an xrLocateViews call that chains an
 * @ref XrCursorDepthHintDXR — no hint, no computation, no state change, and
 * the runtime never touches the OS cursor. That rule is ADR-046's first
 * principle and holds for every later SPEC_VERSION: any runtime-side cost
 * (drawing the cursor, measuring disparity from the atlas) will be a separate,
 * explicitly requested struct.
 *
 * Spec v2 adds the first such struct, @ref XrCursorDepthSourceDXR: an app that
 * already submits depth with its projection layer (XR_KHR_composition_layer_depth)
 * chains it on the hint with XR_CURSOR_DEPTH_SOURCE_SUBMITTED_DEPTH_DXR and
 * sends no nearest point; the runtime reads a cursor-sized patch of that depth
 * itself (ADR-046 Phase 3a).
 *
 * One call, one space. The hint is chained on XrViewLocateInfo::next and the
 * result on XrViewState::next of the SAME xrLocateViews call, and both are in
 * XrViewLocateInfo::space. The hint normally comes from the previous frame's
 * render (a depth readback is a frame late); the runtime's time filter absorbs
 * that.
 *
 * Geometry is solved from the views this very call returns — the canvas point
 * under the cursor is where the outermost pair of view rays through it meet —
 * so the placement is correct for display rigs, camera rigs, any m2v scale,
 * display zones (the canvas is the zone), and any view count >= 2. In 2D (one
 * active view) the result is inactive: show the OS cursor.
 */
#ifndef XR_DXR_CURSOR_DEPTH_H
#define XR_DXR_CURSOR_DEPTH_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_cursor_depth 1
#define XR_DXR_cursor_depth_SPEC_VERSION 3
#define XR_DXR_CURSOR_DEPTH_EXTENSION_NAME "XR_DXR_cursor_depth"

// Reserved 1004999320-329 (next free decade after stereo_camera's 310-319).
// Final values reconcile with the Khronos registry before spec freeze.
#define XR_TYPE_CURSOR_DEPTH_HINT_DXR ((XrStructureType)1004999320)
#define XR_TYPE_CURSOR_DEPTH_PLACEMENT_DXR ((XrStructureType)1004999321)
//! Spec v2 (ADR-046 Phase 3a).
#define XR_TYPE_CURSOR_DEPTH_SOURCE_DXR ((XrStructureType)1004999322)
//! Spec v3 (ADR-046 Amendment 1, Phase 3c: lifted content).
#define XR_TYPE_CURSOR_DEPTH_LIFT_RECT_DXR ((XrStructureType)1004999323)
#define XR_TYPE_CURSOR_DEPTH_LIFT_STATE_DXR ((XrStructureType)1004999324)

// ---- Input: app chains this on XrViewLocateInfo::next; runtime reads it. ----

/*!
 * @brief Where the cursor is, and the nearest content under it.
 *
 * Chaining this IS the per-frame request: a locate without it does no cursor
 * work at all. Chain it on at most one locate per frame — with display zones,
 * the zone-scoped locate of the zone the cursor is over. A malformed hint is
 * never an error; it produces an inactive placement.
 */
typedef struct XrCursorDepthHintDXR {
    XrStructureType          type;   //!< Must be XR_TYPE_CURSOR_DEPTH_HINT_DXR
    const void* XR_MAY_ALIAS next;
    /*!
     * The cursor hotspot, CANVAS-normalised: [0,1]^2, origin top-left, u right,
     * v DOWN. The canvas is the one this locate's views frame — the window
     * client area (or texture sub-rect), or the zone rect for a zone-scoped
     * locate. Outside [0,1] the placement is inactive.
     */
    XrVector2f cursorUV;
    /*!
     * XR_FALSE: nothing of the app's 3D content lies under the cursor
     * footprint — the cursor settles onto the display plane (zero disparity).
     */
    XrBool32 hasContent;
    /*!
     * In XrViewLocateInfo::space: the content point under the cursor
     * FOOTPRINT (not just the hotspot pixel — the whole sprite, plus a little)
     * that is NEAREST the viewer. Using the footprint is what keeps a
     * neighbouring nearer edge from cutting through the sprite. Ignored when
     * hasContent is XR_FALSE.
     */
    XrVector3f nearestPoint;
    /*!
     * Desired sprite height as a fraction of the canvas height, as seen from
     * the viewer. <= 0 (or non-finite) selects the runtime default (0.03).
     */
    float cursorHeight;
} XrCursorDepthHintDXR;

// ---- Spec v2: where the nearest point comes from (ADR-046 Phase 3a). ----

/*!
 * @brief Who supplies the nearest content point under the cursor.
 */
typedef enum XrCursorDepthSourceKindDXR {
    /*!
     * v1 behaviour: the app supplies XrCursorDepthHintDXR::hasContent and
     * ::nearestPoint itself. Same as not chaining XrCursorDepthSourceDXR.
     */
    XR_CURSOR_DEPTH_SOURCE_APP_POINT_DXR = 0,
    /*!
     * The runtime finds the nearest point itself, from the depth the app
     * already submits with its projection layer (XR_KHR_composition_layer_depth,
     * an XrCompositionLayerDepthInfoKHR on every XrCompositionLayerProjectionView).
     * The hint's hasContent and nearestPoint are then IGNORED; cursorUV and
     * cursorHeight still apply (the height sizes the footprint the runtime
     * searches).
     *
     * Requirements: XR_KHR_composition_layer_depth enabled, and the projection
     * layer's space the SAME XrSpace as XrViewLocateInfo::space. When either
     * fails, or the compositor cannot read depth, the placement behaves as if
     * hasContent were XR_FALSE (cursor on the display plane) - never an error.
     *
     * The answer is asynchronous: the depth read at frame N's xrEndFrame
     * (a cursor-sized patch, copied without any GPU wait) reaches the
     * placement one or two frames later.
     */
    XR_CURSOR_DEPTH_SOURCE_SUBMITTED_DEPTH_DXR = 1,
    XR_CURSOR_DEPTH_SOURCE_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrCursorDepthSourceKindDXR;

/*!
 * @brief Optional, chained on XrCursorDepthHintDXR::next (spec v2).
 *
 * Chaining it with XR_CURSOR_DEPTH_SOURCE_SUBMITTED_DEPTH_DXR IS the request
 * for the runtime's depth read, and the request covers THIS frame only: chain
 * it on every frame's hint for as long as you want the service. A frame
 * without it costs the runtime nothing (ADR-046 section 0) - no GPU copy, no
 * readback, no allocation.
 */
typedef struct XrCursorDepthSourceDXR {
    XrStructureType            type;   //!< Must be XR_TYPE_CURSOR_DEPTH_SOURCE_DXR
    const void* XR_MAY_ALIAS   next;
    XrCursorDepthSourceKindDXR source;
} XrCursorDepthSourceDXR;

// ---- Result: app chains this on XrViewState::next; runtime fills it. ----

/*!
 * @brief Where to draw the cursor sprite this frame.
 *
 * Filled on every locate that chains it. Draw the sprite LAST, depth test
 * off, as an ordinary quad in the locate space — every view then renders it
 * at the right disparity. Hide the OS cursor over the canvas while
 * isActive is XR_TRUE, and show it while it is XR_FALSE.
 */
typedef struct XrCursorDepthPlacementDXR {
    XrStructureType    type;   //!< Must be XR_TYPE_CURSOR_DEPTH_PLACEMENT_DXR
    void* XR_MAY_ALIAS next;
    /*!
     * XR_FALSE when there is nothing to draw: no hint chained on this locate,
     * fewer than two active views (2D), a cursor off the canvas, or degenerate
     * view geometry. Every other field is then zero / identity.
     */
    XrBool32 isActive;
    /*!
     * Sprite centre and orientation in XrViewLocateInfo::space. The
     * orientation is the display plane's: draw the sprite in its local XY
     * plane, +X right, +Y up, facing +Z (toward the viewer). The position lies
     * on the line from the viewer through the cursor's point on the canvas,
     * so the cursor never appears to slide sideways as its depth changes.
     */
    XrPosef pose;
    /*!
     * Sprite height in locate-space units — the requested cursorHeight of the
     * canvas, scaled with depth so the apparent size stays constant.
     */
    float height;
    /*!
     * Filtered disparity, in units of the eye baseline: 0 = on the display
     * plane, < 0 = in front of it (crossed), > 0 = behind. Diagnostic; the
     * pose already encodes it.
     */
    float disparity;
    //! Unfiltered target disparity this locate (content + margin, clamped). Diagnostic.
    float targetDisparity;
} XrCursorDepthPlacementDXR;

// ---- Spec v3: lifted content (ADR-046 Amendment 1, Phase 3c). ----
//
// A 2D element lifted on a weave rect (XR_DXR_lift, XrWeaveRectLiftDXR) has no
// app that holds its views: the runtime synthesizes them and weaves them. So
// for such a rect the RUNTIME draws the cursor, into the lifted views, placed
// from the conversion's own depth map. Requires XR_DXR_lift and XR_DXR_weave,
// and a lift stream created with XrLiftDepthRequestDXR (no depth, no cursor).

/*!
 * @brief Request a depth-aware cursor on one lifted rect (spec v3).
 *
 * Chain on XrWeaveRectLiftDXR::next. Chaining it IS the request, and it covers
 * THIS weave submit only: chain it on every submit for as long as you want the
 * cursor. A rect without it costs the runtime nothing (ADR-046 section 0).
 *
 * The runtime reads the OS cursor position itself when it draws, maps it into
 * the bound window, and draws the cursor only on the rect it is over, with the
 * same margin, comfort clamp and smoothing as every DisplayXR depth cursor.
 * It reports what it drew in @ref XrCursorDepthLiftStateDXR; hide the OS
 * cursor over the rect only for a frame it reports drawn, so the failure mode
 * is the flat OS cursor, never no cursor.
 */
typedef struct XrCursorDepthLiftRectDXR {
    XrStructureType          type;  //!< Must be XR_TYPE_CURSOR_DEPTH_LIFT_RECT_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 flags; //!< Reserved, must be 0.
} XrCursorDepthLiftRectDXR;

/*!
 * @brief Which lifted rects got the runtime's cursor this submit (spec v3).
 *
 * Chain on XrWeaveOutputDXR::next of a submit that requested a cursor on any
 * rect; filled on every successful submit (0 when it drew none).
 */
typedef struct XrCursorDepthLiftStateDXR {
    XrStructureType    type; //!< Must be XR_TYPE_CURSOR_DEPTH_LIFT_STATE_DXR
    void* XR_MAY_ALIAS next;
    /*!
     * Bit i set = the runtime drew the cursor into lifts[i] of this submit's
     * XrWeaveSubmitLiftRectsDXR in the output just woven. Clear for every
     * rect it could not place it on: no depth or display mapping for that
     * frame, the cursor not over the rect, the views shown flat (2D), or a
     * workspace cursor already drawn.
     */
    uint32_t drawnLiftMask;
} XrCursorDepthLiftStateDXR;

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_CURSOR_DEPTH_H
