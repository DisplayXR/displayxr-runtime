// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — the XR_EXT_* identifiers in this header are NOT registered
// with the Khronos OpenXR registry. They are provisional placeholders used
// during DisplayXR incubation and will be re-registered — and may be renamed
// (e.g. to a registered XR_<AUTHORID>_ prefix) — through the official Khronos
// process on the EXT -> KHR path. Do not treat these names or numeric values
// as stable. See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Header for XR_EXT_weave extension
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
 *   xrWeaveBindWindowEXT(session, hwnd);
 *
 *   // per frame, per weaved element:
 *   XrWeaveSubmitInfoEXT in  = { ..., inputTexture, rect, eyes };
 *   XrWeaveOutputEXT     out = { XR_TYPE_WEAVE_OUTPUT_EXT };
 *   xrWeaveSubmitEXT(session, &in, &out);  // out.weavedTexture + out.fence
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
 * xrWeaveSubmitEXT RETURNS the runtime's current tracked eye positions in
 * XrWeaveOutputEXT; the caller renders the NEXT frame's pair from them.
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
 *   xrWeaveSnapWindowRectEXT(session, &origin, &target, &snapped);
 *   pos->x = snapped.offset.x; pos->y = snapped.offset.y;  // apply
 *
 * Only the rect's offset (top-left) is snapped; the extent passes through
 * unchanged. This is a synchronous, GPU-free query — no texture, no fence.
 */
#ifndef XR_EXT_WEAVE_H
#define XR_EXT_WEAVE_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_EXT_weave 1
#define XR_EXT_weave_SPEC_VERSION 2
#define XR_EXT_WEAVE_EXTENSION_NAME "XR_EXT_weave"

// Reserved 1000999190..191. Final values reconcile with the Khronos registry
// before spec freeze. Allocation registry: README.md in this directory.
#define XR_TYPE_WEAVE_SUBMIT_INFO_EXT ((XrStructureType)1000999190)
#define XR_TYPE_WEAVE_OUTPUT_EXT      ((XrStructureType)1000999191)

//! Upper bound on eye positions carried by XrWeaveSubmitInfoEXT (mirrors the
//! runtime's XRT_MAX_VIEWS). Phase 1: carried but unused.
#define XR_WEAVE_MAX_EYES_EXT 8

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
 */
typedef struct XrWeaveSubmitInfoEXT {
    XrStructureType          type;         //!< XR_TYPE_WEAVE_SUBMIT_INFO_EXT
    const void* XR_MAY_ALIAS next;
    void*                    inputTexture; //!< pre-weave SBS shared texture HANDLE (keyed-mutex)
    XrBool32                 inputIsDxgi;  //!< XR_TRUE for a legacy global DXGI handle (else NT handle)
    XrRect2Di                rect;         //!< window-relative sub-rect, device px (y-down)
} XrWeaveSubmitInfoEXT;

/*!
 * @brief Weaved output handed back to the present-owner.
 *
 * @c weavedTexture is a runtime-allocated shared GPU texture HANDLE sized to
 * the bound window's client area; the sub-rect named by
 * XrWeaveSubmitInfoEXT::rect is weaved at the correct absolute phase, so
 * compositing/presenting the texture at the window places the weave correctly.
 * @c fence is a shared sync HANDLE the runtime signals to @c fenceValue once
 * the weave for this frame is complete; the caller waits on it before
 * presenting.
 *
 * @c weavedTexture and @c fence are valid (non-NULL) on the FIRST successful
 * xrWeaveSubmitEXT and again whenever the output is re-allocated (window
 * resize → @c width / @c height change). On steady-state frames they are NULL
 * and the caller reuses the handles it already opened. @c width, @c height and
 * @c fenceValue are valid on every call. The caller owns the returned HANDLEs
 * and must CloseHandle() them when done.
 */
typedef struct XrWeaveOutputEXT {
    XrStructureType    type;          //!< XR_TYPE_WEAVE_OUTPUT_EXT
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
    XrVector3f         eyes[XR_WEAVE_MAX_EYES_EXT];
    XrBool32           eyesValid;
    XrBool32           eyesTracking;
} XrWeaveOutputEXT;

typedef XrResult (XRAPI_PTR *PFN_xrWeaveBindWindowEXT)(
    XrSession session, void* windowHandle);

typedef XrResult (XRAPI_PTR *PFN_xrWeaveSubmitEXT)(
    XrSession session, const XrWeaveSubmitInfoEXT* submitInfo, XrWeaveOutputEXT* output);

typedef XrResult (XRAPI_PTR *PFN_xrWeaveSnapWindowRectEXT)(
    XrSession session, const XrRect2Di* originRect, const XrRect2Di* targetRect, XrRect2Di* snappedRect);

#ifndef XR_NO_PROTOTYPES

//! Bind the present-owner's window (HWND on Windows) so the DP phase-snaps the
//! interlace to the window's panel position on move / resize. Call once before
//! the first xrWeaveSubmitEXT; call again if the window handle changes.
XRAPI_ATTR XrResult XRAPI_CALL xrWeaveBindWindowEXT(
    XrSession session, void* windowHandle);

//! Weave one window sub-rect from a pre-weave SBS texture. Synchronous: returns
//! once the runtime has issued the DP weave into the output texture and signaled
//! the fence; the caller waits the fence to XrWeaveOutputEXT::fenceValue before
//! presenting. Reports XR_ERROR_FEATURE_UNSUPPORTED on an in-process session.
XRAPI_ATTR XrResult XRAPI_CALL xrWeaveSubmitEXT(
    XrSession session, const XrWeaveSubmitInfoEXT* submitInfo, XrWeaveOutputEXT* output);

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
XRAPI_ATTR XrResult XRAPI_CALL xrWeaveSnapWindowRectEXT(
    XrSession session, const XrRect2Di* originRect, const XrRect2Di* targetRect, XrRect2Di* snappedRect);

#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif /* XR_EXT_WEAVE_H */
