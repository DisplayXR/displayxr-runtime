// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the XR_DXR_* extensions in this header are NOT yet registered in the
// Khronos OpenXR registry: extension numbers and XrStructureType values sit
// in a provisional experimental block (1004999xxx) pending official
// assignment. Extension names are expected to be stable; numeric values are
// not. SPEC_VERSION continues the pre-rename XR_EXT_* numbering (the
// interface history did not restart with the name).
// See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Header for XR_DXR_weave extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * A window-bound, synchronous weave service (issue #625). It exists for
 * present-owners: callers that own their OS window and present themselves, but
 * want the runtime's display processor (DP) to weave a sub-rect of their window
 * for them. The caller NEVER weaves (ADR-007 / ADR-019 — weaving is the
 * vendor DP's calibrated, per-display shader, behind the plug-in); it only
 * hands the runtime a pre-weave stereo (side-by-side) texture + a
 * window-relative rect, and gets back a weaved shared texture + a fence it
 * composites at that rect and presents.
 *
 * This is the runtime half of the inline-3D-in-the-browser roadmap
 * (docs/roadmap/webxr-support.md §2.4 "Step 0"): the same window-bound
 * bindWindow + per-element weave() a browser present-owner will drive. It is a
 * synchronous weave *service*, distinct from the steady swapchain frame loop —
 * the caller calls weave once per element per frame.
 *
 *   // once: bind the present-owner's window so the DP can phase-snap the
 *   // interlace as the window moves / resizes
 *   xrWeaveBindWindowDXR(session, hwnd);
 *
 *   // per frame, per weaved element:
 *   XrWeaveSubmitInfoDXR in  = { ..., inputTexture, rect, eyes };
 *   XrWeaveOutputDXR     out = { XR_TYPE_WEAVE_OUTPUT_DXR };
 *   xrWeaveSubmitDXR(session, &in, &out);  // out.weavedTexture + out.fence
 *
 * Weave phase is locked to absolute screen position, so the DP combines the
 * window-relative @c rect with the bound window's tracked position to weave at
 * the correct phase; dragging / resizing the window re-snaps phase
 * automatically (that is why the window must be bound).
 *
 * Eyes flow runtime -> caller, not caller -> weave. The interlace itself is the
 * DP's (vendor's) job and reads the vendor's own eye tracker internally — the
 * caller feeds it nothing for that. What the caller needs eyes FOR is its own
 * off-axis (asymmetric-frustum / Kooima) projection: as the viewer's head
 * moves, the present-owner must re-render its pre-weave stereo pair with frusta
 * skewed to the new eye positions (virtual-camera motion / look-around). So
 * xrWeaveSubmitDXR RETURNS the runtime's current tracked eye positions in
 * XrWeaveOutputDXR; the caller renders the NEXT frame's pair from them.
 *
 * Availability: this runtime implements the weave service only on the
 * out-of-process (service / IPC) path. An in-process session reports
 * XR_ERROR_FEATURE_UNSUPPORTED.
 *
 * Window-drag phase lock (issue #625). A present-owner that moves its own
 * window must keep the woven interlace phase-locked to the panel as the window
 * travels, or the lenticular subpixels shift under the lenses and the 3D
 * collapses into crosstalk jitter. The DP snaps the window to the nearest
 * phase-aligned screen position; the snap math + lens parameters are the
 * vendor's (ADR-019), so they never leave the DP. The present-owner intercepts
 * WM_WINDOWPOSCHANGING, hands the runtime the drag-start origin rect + the
 * proposed target rect, and applies the returned phase-snapped rect before the
 * OS commits the move:
 *
 *   // on WM_ENTERSIZEMOVE: remember the drag-start window rect (origin).
 *   // on WM_WINDOWPOSCHANGING (proposed pos = target):
 *   XrRect2Di snapped = {};
 *   xrWeaveSnapWindowRectDXR(session, &origin, &target, &snapped);
 *   pos->x = snapped.offset.x; pos->y = snapped.offset.y;  // apply
 *
 * Only the rect's offset (top-left) is snapped; the extent passes through
 * unchanged. This is a synchronous, GPU-free query — no texture, no fence.
 *
 * Batched submit (SPEC_VERSION 3, issue #625). Per-element submits serialize
 * the per-call fixed cost (IPC round-trip + shared-texture open + keyed-mutex
 * + fence) N times per frame, which caps a page at ~8-12 visible weaved
 * elements. Version 3 amortizes that to ONE submit per frame: chain an
 * XrWeaveSubmitRectsDXR onto XrWeaveSubmitInfoDXR::next carrying up to
 * XR_WEAVE_SUBMIT_MAX_RECTS_DXR rects. The chained struct SWITCHES the input
 * layout contract:
 *
 *  - Chain absent (v2 behavior, unchanged): @c inputTexture is an
 *    element-sized 2x1 SBS atlas for the single @c rect; the whole texture is
 *    the atlas (left view = left half).
 *  - Chain present (batch): @c inputTexture is sized to the bound window's
 *    client area, and each rect's pre-weave SBS content sits in it AT THAT
 *    RECT'S OWN WINDOW POSITION (identity mapping: sample position == weave
 *    position; each rect region itself holds squeezed SBS, left view in the
 *    left half of the rect). The base @c rect field is ignored.
 *
 * All rects weave into the same window-sized output with ONE fence signal at
 * the end; eyes are returned once per call. A batch is NOT equivalent to N
 * single-rect submits of the same texture — the layouts differ (see above).
 *
 * 2D overlays composited by the DP (SPEC_VERSION 4, browser#18). A present-owner
 * that paints crisp 2D content OVER the woven 3D (hover plates, badges, chrome)
 * must NOT composite it itself: at the caller's composite time the woven pixels
 * do not yet exist, so a 2D layer that samples its backdrop (frosted glass) or
 * that must track the panel at the interlace phase cannot be done correctly
 * caller-side. Instead chain an XrWeaveSubmitOverlaysDXR onto
 * XrWeaveSubmitInfoDXR::next carrying a single window-sized premul-RGBA overlay
 * atlas; the DP composites it OVER the woven output in the same pass
 * (final = woven·(1 − overlay.a) + overlay, premul "over" — the overlay's alpha
 * IS the 2D-vs-3D mask). This reuses the runtime's existing Local2D /
 * masked-composite leg (XR_DXR_local_3d_zone, ADR-027), which the steady
 * xrEndFrame path already runs; the weave service previously bypassed it.
 *
 * The caller flattens all its 2D overlays into that one atlas in stacking (z)
 * order before submit, so z-order is resolved caller-side and the wire carries
 * one texture, not N layers — mirroring set_background_2d and keeping the batch
 * to two handles (content atlas + overlay atlas) and one submit regardless of
 * overlay count. Per-overlay depth (2D-under, mid-depth layers) is reserved for
 * a later version; v4 overlays are all at screen depth, "over" the weave.
 *
 * Version history: 1 = initial (pre-rename numbering carried over); 2 =
 * inputIsDxgi legacy-DXGI handle tagging; 3 = XrWeaveSubmitRectsDXR batched
 * submit; 4 = XrWeaveSubmitOverlaysDXR DP-composited 2D overlay atlas; 5 =
 * XrWeaveSubmitInfoDXR::firstChunk (coherent whole-window output, browser#22).
 */
#ifndef XR_DXR_WEAVE_H
#define XR_DXR_WEAVE_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_weave 1
#define XR_DXR_weave_SPEC_VERSION 5
#define XR_DXR_WEAVE_EXTENSION_NAME "XR_DXR_weave"

// Reserved 1004999190..193. Final values reconcile with the Khronos registry
// before spec freeze. Allocation registry: README.md in this directory.
#define XR_TYPE_WEAVE_SUBMIT_INFO_DXR     ((XrStructureType)1004999190)
#define XR_TYPE_WEAVE_OUTPUT_DXR          ((XrStructureType)1004999191)
#define XR_TYPE_WEAVE_SUBMIT_RECTS_DXR    ((XrStructureType)1004999192)
#define XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR ((XrStructureType)1004999193)

//! Upper bound on eye positions carried by XrWeaveSubmitInfoDXR (mirrors the
//! runtime's XRT_MAX_VIEWS). Phase 1: carried but unused.
#define XR_WEAVE_MAX_EYES_DXR 8

//! Upper bound on rects carried by one XrWeaveSubmitRectsDXR (sized so the
//! runtime's IPC message stays within its fixed buffer). Callers with more
//! visible elements split into multiple batched submits.
#define XR_WEAVE_SUBMIT_MAX_RECTS_DXR 32

/*!
 * @brief Per-frame weave submission for one window sub-rect.
 *
 * @c inputTexture is a shared GPU texture HANDLE to the caller's pre-weave
 * side-by-side stereo content (left view in the left half, right in the right
 * half). On Windows it is a D3D11 NT shared handle carrying an
 * IDXGIKeyedMutex (key 0 = "caller done writing, runtime may read") — that
 * keyed mutex is how the runtime knows the frame is ready (the service does not
 * import caller fences).
 *
 * @c rect is the element's device-pixel rect WITHIN the bound window's client
 * area (y-down). The runtime combines it with the tracked window position to
 * derive the absolute-screen weave phase.
 *
 * @c firstChunk (spec v5, browser#22) marks the FIRST submit of a frame. When
 * XR_TRUE the runtime clears its window-sized woven output to premultiplied
 * transparent (0,0,0,0) before weaving this submit's rects, so regions between
 * the woven tiles become transparent instead of stale — the caller can then
 * present the woven output WHOLE-WINDOW (one "over" composite: opaque tiles
 * replace the page, transparent gaps show it through) instead of per-tile,
 * single-sourcing any 2D chrome that spans tile gaps. A caller that splits a
 * frame across multiple submits (> XR_WEAVE_SUBMIT_MAX_RECTS_DXR elements) sets
 * it XR_TRUE only on the first; later submits accumulate into the same output.
 * Default XR_FALSE preserves the legacy behavior (no clear) for present-owners
 * that draw back only their own tiles.
 */
typedef struct XrWeaveSubmitInfoDXR {
    XrStructureType          type;         //!< XR_TYPE_WEAVE_SUBMIT_INFO_DXR
    const void* XR_MAY_ALIAS next;
    void*                    inputTexture; //!< pre-weave SBS shared texture HANDLE (keyed-mutex)
    XrBool32                 inputIsDxgi;  //!< XR_TRUE for a legacy global DXGI handle (else NT handle)
    XrRect2Di                rect;         //!< window-relative sub-rect, device px (y-down)
    XrBool32                 firstChunk;   //!< XR_TRUE = first submit of the frame; clears the woven output to transparent (v5)
} XrWeaveSubmitInfoDXR;

/*!
 * @brief Batched weave submission — N window sub-rects in ONE call (spec v3).
 *
 * Chain onto XrWeaveSubmitInfoDXR::next. When present it switches the input
 * layout contract (see the file header): @c inputTexture must be sized to the
 * bound window's client area with each rect's pre-weave SBS content placed at
 * that rect's own window position; the base struct's @c rect is ignored. Each
 * rect region holds squeezed SBS (left view in the rect's left half). The
 * runtime weaves every rect into the shared window-sized output and signals
 * the fence ONCE after the last rect.
 *
 * @c rectCount must be 1..XR_WEAVE_SUBMIT_MAX_RECTS_DXR; callers with more
 * visible elements split into multiple batched submits (each with its own
 * fence value; waiting the last covers all).
 */
typedef struct XrWeaveSubmitRectsDXR {
    XrStructureType          type;      //!< XR_TYPE_WEAVE_SUBMIT_RECTS_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 rectCount; //!< 1..XR_WEAVE_SUBMIT_MAX_RECTS_DXR
    const XrRect2Di*         rects;     //!< window-relative sub-rects, device px (y-down)
} XrWeaveSubmitRectsDXR;

/*!
 * @brief DP-composited 2D overlay atlas — crisp 2D painted OVER the weave (spec v4).
 *
 * Chain onto XrWeaveSubmitInfoDXR::next (alongside XrWeaveSubmitRectsDXR). The
 * DP composites @c overlayTexture over the woven output in the same pass, so the
 * 2D content lands on top of the interlaced 3D as crisp screen-depth pixels
 * (final = woven·(1 − overlay.a) + overlay).
 *
 * @c overlayTexture is a shared GPU texture HANDLE to a window-sized,
 * PREMULTIPLIED-alpha RGBA atlas: every 2D overlay is already rastered at its
 * own window position, flattened in stacking (z) order, on transparency
 * elsewhere. The premultiplied alpha channel is the 2D-vs-3D mask — alpha 1 =
 * opaque 2D, alpha 0 = show the weave through. Like @c inputTexture it is a
 * keyed-mutex shared texture (key 0 = "caller done writing"); on Windows an NT
 * shared handle unless @c overlayIsDxgi is XR_TRUE.
 *
 * @c rects (optional) name the window-relative regions the atlas actually
 * touches, letting the DP scope the composite to those areas; @c rectCount 0
 * means "composite the whole atlas". They are a performance hint, not a
 * correctness input — the premultiplied alpha alone defines the result.
 */
typedef struct XrWeaveSubmitOverlaysDXR {
    XrStructureType          type;           //!< XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR
    const void* XR_MAY_ALIAS next;
    void*                    overlayTexture; //!< window-sized premul-RGBA overlay atlas HANDLE (keyed-mutex)
    XrBool32                 overlayIsDxgi;  //!< XR_TRUE for a legacy global DXGI handle (else NT handle)
    uint32_t                 rectCount;      //!< 0..XR_WEAVE_SUBMIT_MAX_RECTS_DXR (0 = whole atlas)
    const XrRect2Di*         rects;          //!< window-relative overlay regions, device px (y-down) — scope hint
} XrWeaveSubmitOverlaysDXR;

/*!
 * @brief Weaved output handed back to the present-owner.
 *
 * @c weavedTexture is a runtime-allocated shared GPU texture HANDLE sized to
 * the bound window's client area; the sub-rect named by
 * XrWeaveSubmitInfoDXR::rect is weaved at the correct absolute phase, so
 * compositing/presenting the texture at the window places the weave correctly.
 * @c fence is a shared sync HANDLE the runtime signals to @c fenceValue once
 * the weave for this frame is complete; the caller waits on it before
 * presenting.
 *
 * @c weavedTexture and @c fence are valid (non-NULL) on the FIRST successful
 * xrWeaveSubmitDXR and again whenever the output is re-allocated (window
 * resize → @c width / @c height change). On steady-state frames they are NULL
 * and the caller reuses the handles it already opened. @c width, @c height and
 * @c fenceValue are valid on every call. The caller owns the returned HANDLEs
 * and must CloseHandle() them when done.
 */
typedef struct XrWeaveOutputDXR {
    XrStructureType    type;          //!< XR_TYPE_WEAVE_OUTPUT_DXR
    void* XR_MAY_ALIAS next;
    void*              weavedTexture; //!< weaved shared texture HANDLE, or NULL on steady-state frames
    uint32_t           width;         //!< weaved texture width (bound-window client width)
    uint32_t           height;        //!< weaved texture height (bound-window client height)
    void*              fence;         //!< shared sync HANDLE (runtime→caller), or NULL on steady-state frames
    uint64_t           fenceValue;    //!< value the caller waits the fence to before presenting this frame
    //! Current tracked eye positions (display-space, metres) the caller uses to
    //! drive its off-axis projection for the NEXT pre-weave frame (look-around).
    //! eyesValid is XR_FALSE until the tracker has a sample; eyesTracking is
    //! XR_FALSE when the position is a fallback (no live lock) but still usable.
    uint32_t           eyeCount;
    XrVector3f         eyes[XR_WEAVE_MAX_EYES_DXR];
    XrBool32           eyesValid;
    XrBool32           eyesTracking;
} XrWeaveOutputDXR;

typedef XrResult (XRAPI_PTR *PFN_xrWeaveBindWindowDXR)(
    XrSession session, void* windowHandle);

typedef XrResult (XRAPI_PTR *PFN_xrWeaveSubmitDXR)(
    XrSession session, const XrWeaveSubmitInfoDXR* submitInfo, XrWeaveOutputDXR* output);

typedef XrResult (XRAPI_PTR *PFN_xrWeaveSnapWindowRectDXR)(
    XrSession session, const XrRect2Di* originRect, const XrRect2Di* targetRect, XrRect2Di* snappedRect);

#ifndef XR_NO_PROTOTYPES

//! Bind the present-owner's window (HWND on Windows) so the DP phase-snaps the
//! interlace to the window's panel position on move / resize. Call once before
//! the first xrWeaveSubmitDXR; call again if the window handle changes.
XRAPI_ATTR XrResult XRAPI_CALL xrWeaveBindWindowDXR(
    XrSession session, void* windowHandle);

//! Weave one window sub-rect from a pre-weave SBS texture. Synchronous: returns
//! once the runtime has issued the DP weave into the output texture and signaled
//! the fence; the caller waits the fence to XrWeaveOutputDXR::fenceValue before
//! presenting. Reports XR_ERROR_FEATURE_UNSUPPORTED on an in-process session.
XRAPI_ATTR XrResult XRAPI_CALL xrWeaveSubmitDXR(
    XrSession session, const XrWeaveSubmitInfoDXR* submitInfo, XrWeaveOutputDXR* output);

//! Snap a proposed window rect to the nearest interlace-phase-aligned screen
//! position so the woven 3D stays locked while the present-owner drags its
//! window. @c originRect is the drag-start window rect; @c targetRect is the
//! proposed (OS-requested) rect; both in absolute screen pixels (the phase is
//! absolute). On return @c snappedRect->offset is the phase-snapped top-left and
//! @c snappedRect->extent equals @c targetRect->extent (size is not snapped).
//! GPU-free and synchronous. If the DP can't snap (no vendor support, panel in
//! 2D), @c snappedRect is copied from @c targetRect (a no-op snap) and the call
//! still returns XR_SUCCESS. Reports XR_ERROR_FEATURE_UNSUPPORTED on an
//! in-process session.
XRAPI_ATTR XrResult XRAPI_CALL xrWeaveSnapWindowRectDXR(
    XrSession session, const XrRect2Di* originRect, const XrRect2Di* targetRect, XrRect2Di* snappedRect);

#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif /* XR_DXR_WEAVE_H */
