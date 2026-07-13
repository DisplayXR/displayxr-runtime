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
 * @brief  Header for XR_DXR_canvas_rect extension
 * @author David Fattal
 * @ingroup external_openxr
 *
 * Lets a present-owning / windowless producer declare the on-panel rectangle
 * where its content will appear — directly, as a display-relative pixel rect —
 * instead of the runtime deriving it from a bound OS window (HWND / NSView /
 * Xlib Window).
 *
 * ## Why this exists (#697)
 *
 * The lenticular weave phase and the off-axis (Kooima) projection are both
 * functions of WHERE ON THE PANEL the content lands. For a windowed app the
 * runtime learns that rect by reading the bound window's client rect
 * (GetClientRect + ClientToScreen) every locate/weave; the window is a
 * position/phase ANCHOR, never a render target (see
 * docs/getting-started/app-classes.md). A genuinely windowless producer — a
 * WebXR bridge, a browser/CEF embedder, a capture/stream target, an engine
 * offscreen renderer — has no such window, so without this extension it
 * degrades to DISPLAY-scoped framing (the whole panel) and loses phase
 * alignment.
 *
 * This extension supplies the missing input: the app hands the runtime the
 * canvas rect itself. It is ORTHOGONAL to, and usable WITHOUT, any window
 * binding — enabling it does not require XR_DXR_win32_window_binding et al. A
 * texture-mode producer that also happens to own a window may still prefer the
 * window binding (the OS tracks the rect for free); this extension is for
 * producers that would rather declare the rect than bind a window.
 *
 * ## What you give up vs. a bound window
 *
 * Only SELF-TRACKING. A bound window is updated by the OS as it moves/resizes,
 * and the runtime re-reads it live. With this extension the APP owns rect
 * updates: seed the initial rect at session create via XrCanvasRectBindingDXR,
 * then call xrSetCanvasRectDXR whenever the canvas moves or resizes. No phase
 * MATH is lost — drag phase-snap (XR_DXR_weave::xrWeaveSnapWindowRectDXR) is
 * already pure absolute-screen-coordinates and the app can drive it from its
 * own move handler.
 *
 * ## Coordinate contract
 *
 * @c rectPx is in DISPLAY-RELATIVE device pixels: the origin (0,0) is the 3D
 * panel's top-left, +x right, +y down. The panel's desktop origin is available
 * to the app via XR_DXR_display_info's XrDisplayDesktopPositionDXR. The physical
 * size of the canvas is DERIVED by the runtime from the display's pixel pitch
 * (displaySizeMeters / displayPixel*), so the app supplies pixels only.
 *
 *   // once, at xrCreateSession (frame-0 correctness):
 *   XrCanvasRectBindingDXR bind = { XR_TYPE_CANVAS_RECT_BINDING_DXR };
 *   bind.rectPx = (XrRect2Di){ {x, y}, {w, h} };   // display-relative px
 *   bind.next   = &graphicsBinding;                // chain on XrSessionCreateInfo
 *
 *   // whenever the canvas moves/resizes:
 *   XrRect2Di r = { {x, y}, {w, h} };
 *   xrSetCanvasRectDXR(session, &r);
 *
 *   // to relinquish the override (revert to bound-window / display-scoped):
 *   xrSetCanvasRectDXR(session, NULL);
 *
 * Full design: docs/specs/extensions/XR_DXR_canvas_rect.md;
 * background: docs/adr/ADR-027-display-zones.md, app-classes.md (#697).
 */
#ifndef XR_DXR_CANVAS_RECT_H
#define XR_DXR_CANVAS_RECT_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_canvas_rect 1
#define XR_DXR_canvas_rect_SPEC_VERSION 1
#define XR_DXR_CANVAS_RECT_EXTENSION_NAME "XR_DXR_canvas_rect"

// Extension type-value range (1004999xxx); replace with a Khronos-assigned
// value if standardized. Allocation registry: README.md in this directory.
#define XR_TYPE_CANVAS_RECT_BINDING_DXR ((XrStructureType)1004999220)

/*!
 * @brief Initial canvas placement, chained on XrSessionCreateInfo::next.
 *
 * Optional. Seeds the canvas rect so the very first xrLocateViews / weave frame
 * is correctly framed before the app's first xrSetCanvasRectDXR call. A
 * producer whose canvas never moves may seed here and never call the setter.
 *
 * @c rectPx is display-relative device pixels (origin = panel top-left).
 *
 * @extends XrSessionCreateInfo
 */
typedef struct XrCanvasRectBindingDXR {
    XrStructureType          type;   //!< XR_TYPE_CANVAS_RECT_BINDING_DXR
    const void* XR_MAY_ALIAS next;
    XrRect2Di                rectPx; //!< display-relative device px; origin = panel top-left
} XrCanvasRectBindingDXR;

typedef XrResult (XRAPI_PTR *PFN_xrSetCanvasRectDXR)(
    XrSession session, const XrRect2Di* rectPx);

#ifndef XR_NO_PROTOTYPES

//! Set (or update) the session's canvas rect — the on-panel rectangle, in
//! display-relative device pixels, where this producer's content appears. Call
//! whenever the canvas moves or resizes. Pass @p rectPx == NULL to clear the
//! override, reverting to the bound window (if any) else display-scoped
//! framing. Valid any time after xrCreateSession. Returns
//! XR_ERROR_FUNCTION_UNSUPPORTED on a backend that does not yet implement the
//! override.
XRAPI_ATTR XrResult XRAPI_CALL xrSetCanvasRectDXR(
    XrSession session, const XrRect2Di* rectPx);

#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif /* XR_DXR_CANVAS_RECT_H */
