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
 * Platform-neutral handles + explicit window geometry (SPEC_VERSION 7, #1036).
 * Android has neither an HWND nor an IOSurface, so v7 makes the two things that
 * were previously platform-implied explicit and portable:
 *
 *  - HANDLE KIND. Chain an XrWeaveSubmitHandlesDXR to declare what
 *    inputTexture / overlayTexture actually are (D3D11 NT, legacy DXGI,
 *    IOSurface, AHardwareBuffer). PLATFORM_DEFAULT — and omitting the chain
 *    entirely — keeps every pre-v7 caller byte-for-byte correct.
 *  - WINDOW GEOMETRY. xrWeaveBindWindow2DXR takes an XrWeaveBindWindowInfoDXR
 *    with an optional chained XrWeaveWindowGeometryDXR carrying the client
 *    area's absolute on-screen origin + size + display id. Windows/macOS may
 *    still bind by handle and let the runtime derive it; Android MUST supply it
 *    (a SurfaceView's on-screen position is known only to the app, and a pure
 *    move raises no resize). The runtime forwards it to the DP's per-window
 *    phase slot (set_window_screen_rect, ADR-036 D6 / #1033).
 *
 * Sync on Android is the macOS contract: the caller finishes its GPU writes
 * before submitting and the submit returns only once the woven output is
 * GPU-complete (no fence handle, fenceValue is a monotonic counter). An
 * fd-based (`sync_file`) acquire/release variant is a documented follow-up.
 *
 * Per-region hardware wish (SPEC_VERSION 8, browser#88 Phase 3 item A). Until v8
 * the weave path drove the panel's physical 3D element ALL-OR-NOTHING: a present-
 * owner with a single woven element held the whole panel behind the lens, so the
 * flat 2D around it was viewed through a lenticular it did not want (the shipped
 * ghosting). v8 lets the caller name the regions that are FLAT, and the runtime
 * publishes a per-region hardware WISH derived from them — the same mechanism the
 * XR_DXR_display_zones path already publishes (ADR-027 Decision 5), now reached
 * from the weave path too:
 *
 *   wish = union(submitted weave rects) − union(flat rects)
 *
 * Two ways to say "flat", differing only in lifetime and coordinate space:
 *
 *  - PER-SUBMIT. Chain an XrWeaveSubmitFlatRegionsDXR onto
 *    XrWeaveSubmitInfoDXR::next carrying window-relative rects. Scoped to that
 *    one submit — the natural fit for content that moves every frame (a scrolled
 *    page's flat bands).
 *  - STICKY. Call xrWeaveSetScreenFlatRegionsDXR with ABSOLUTE PHYSICAL SCREEN
 *    rects. Latched until the next call, applied immediately — the fit for
 *    screen-anchored furniture (a browser's toolbar / tab strip) that must stay
 *    flat regardless of what any given frame submits.
 *
 * Both are ADVISORY, and in exactly the sense ADR-027 Decision 6 / ADR-030 mean:
 * they move the physical 3D element only. They never gate, mask, crop or reorder
 * CONTENT — the woven pixels are identical with and without them, and a runtime or
 * vendor that ignores the wish entirely is still conformant (its panel is simply
 * 3D where the caller asked for flat, i.e. the pre-v8 behaviour). Rounding, where
 * a rect edge lands between hardware switch cells, always errs toward 3D:
 * marking a working 3D region flat is a mono regression, marking a flat region 3D
 * is only the pre-v8 ghosting. Omitting both = byte-for-byte pre-v8.
 *
 * Version history: 1 = initial (pre-rename numbering carried over); 2 =
 * inputIsDxgi legacy-DXGI handle tagging; 3 = XrWeaveSubmitRectsDXR batched
 * submit; 4 = XrWeaveSubmitOverlaysDXR DP-composited 2D overlay atlas; 5 =
 * XrWeaveSubmitInfoDXR::firstChunk (coherent whole-window output, browser#22);
 * 6 = XrWeaveSubmitLayoutDXR N-view worst-case atlas (#774); 7 =
 * XrWeaveSubmitHandlesDXR handle kinds + xrWeaveBindWindow2DXR /
 * XrWeaveWindowGeometryDXR explicit window geometry, Android support (#1036);
 * 8 = XrWeaveSubmitFlatRegionsDXR + xrWeaveSetScreenFlatRegionsDXR per-region
 * hardware wish on the weave path (browser#88); 9 =
 * xrWeaveExportIpcConnectionDXR + XrWeaveIpcConnectionDXR, brokering a runtime
 * IPC endpoint to a sandboxed sibling process (browser#103), plus the §4b error
 * table (a dead connection now reports XR_ERROR_INSTANCE_LOST /
 * XR_ERROR_SESSION_LOST instead of a generic XR_ERROR_RUNTIME_FAILURE).
 */
#ifndef XR_DXR_WEAVE_H
#define XR_DXR_WEAVE_H 1

#include <openxr/openxr.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_weave 1
#define XR_DXR_weave_SPEC_VERSION 9
#define XR_DXR_WEAVE_EXTENSION_NAME "XR_DXR_weave"

// Reserved 1004999190..199. Final values reconcile with the Khronos registry
// before spec freeze. Allocation registry: README.md in this directory.
#define XR_TYPE_WEAVE_SUBMIT_INFO_DXR     ((XrStructureType)1004999190)
#define XR_TYPE_WEAVE_OUTPUT_DXR          ((XrStructureType)1004999191)
#define XR_TYPE_WEAVE_SUBMIT_RECTS_DXR    ((XrStructureType)1004999192)
#define XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR ((XrStructureType)1004999193)
#define XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR   ((XrStructureType)1004999194)
#define XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR ((XrStructureType)1004999195)
#define XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR  ((XrStructureType)1004999196)
#define XR_TYPE_WEAVE_SUBMIT_HANDLES_DXR   ((XrStructureType)1004999197)
#define XR_TYPE_WEAVE_SUBMIT_FLAT_REGIONS_DXR ((XrStructureType)1004999198)
// 1004999199 is RESERVED for a per-submit wish MASK (an R8 texture handle
// instead of a rect list, for callers whose flat geometry is not rectangular —
// rounded corners, arbitrary CSS clip paths). Do not assign it to anything else.

// Reserved 1004999240..249 — a fresh decade for spec v9+ additions, because the
// 190..199 block above is exhausted (199 is spoken for). Same registry.
#define XR_TYPE_WEAVE_IPC_CONNECTION_DXR ((XrStructureType)1004999240)

//! Upper bound on eye positions carried by XrWeaveSubmitInfoDXR (mirrors the
//! runtime's XRT_MAX_VIEWS). Phase 1: carried but unused.
#define XR_WEAVE_MAX_EYES_DXR 8

//! Upper bound on rects carried by one XrWeaveSubmitRectsDXR (sized so the
//! runtime's IPC message stays within its fixed buffer). Callers with more
//! visible elements split into multiple batched submits.
#define XR_WEAVE_SUBMIT_MAX_RECTS_DXR 32

//! Upper bound on flat rects carried by one XrWeaveSubmitFlatRegionsDXR (spec
//! v8). Smaller than XR_WEAVE_SUBMIT_MAX_RECTS_DXR on purpose: the flat list
//! describes PANEL FURNITURE (bands, gutters, chrome), not per-element geometry,
//! and it has to fit in the same fixed IPC message as the weave rects. A caller
//! with more flat regions than this DROPS the smallest ones. Do NOT merge into
//! bounding boxes: a merged box that swallows woven geometry marks working 3D as
//! flat, and the two error directions are not symmetric — flat-marked-3D is the
//! pre-v8 behaviour (a ghost the caller's own 2D composite already hides), while
//! 3D-marked-flat displays a live woven tile through a flat lens, which is a
//! regression. Dropping a flat rect degrades to the ghost; growing one breaks 3D.
#define XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR 16

//! Upper bound on rects passed to xrWeaveSetScreenFlatRegionsDXR (spec v8).
#define XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR 8

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
 * @brief Platform kind of the GPU handles carried by a submit (spec v7, #1036).
 *
 * The weave service is platform-neutral, but @c inputTexture / @c overlayTexture
 * are opaque platform handles whose meaning is fixed by the OS. Until spec v7
 * the kind was implied by the platform (plus the @c inputIsDxgi bit on Windows),
 * which stopped working once Android joined: an AHardwareBuffer is neither an NT
 * handle nor an IOSurface, and a caller that is wrong about the kind hands the
 * runtime a pointer it will dereference. v7 lets the caller SAY what it is
 * passing so a mismatch is a clean XR_ERROR_VALIDATION_FAILURE, not a crash.
 *
 * @c XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR keeps every pre-v7 caller correct
 * byte-for-byte: the runtime resolves it to the platform's native kind (Windows:
 * NT handle, or legacy DXGI when @c inputIsDxgi / @c overlayIsDxgi is XR_TRUE;
 * macOS: IOSurface; Android: AHardwareBuffer).
 */
typedef enum XrWeaveHandleKindDXR {
    //! Resolve from the platform (+ the inputIsDxgi / overlayIsDxgi bit on Windows).
    XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR = 0,
    //! Windows: D3D11 NT shared handle (IDXGIKeyedMutex, key 0).
    XR_WEAVE_HANDLE_KIND_D3D11_NT_DXR = 1,
    //! Windows: legacy global DXGI shared handle (Low-integrity callers, #743).
    XR_WEAVE_HANDLE_KIND_D3D11_DXGI_DXR = 2,
    //! macOS: IOSurfaceRef (crosses the runtime IPC as a global IOSurfaceID).
    XR_WEAVE_HANDLE_KIND_IOSURFACE_DXR = 3,
    //! Android: AHardwareBuffer* (the buffer itself travels over the IPC socket).
    XR_WEAVE_HANDLE_KIND_AHARDWAREBUFFER_DXR = 4,
    XR_WEAVE_HANDLE_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrWeaveHandleKindDXR;

/*!
 * @brief Explicit handle kinds for a submit (spec v7, #1036).
 *
 * Chain onto XrWeaveSubmitInfoDXR::next. Purely declarative — it changes no
 * layout contract, it only names what @c inputTexture and
 * XrWeaveSubmitOverlaysDXR::overlayTexture ARE, so the runtime can reject a
 * cross-platform mistake instead of dereferencing it. Omit it (or leave both
 * fields PLATFORM_DEFAULT) for the pre-v7 behaviour.
 *
 * Sync note (v7): Android adopts the macOS contract — the caller completes its
 * GPU writes into @c inputTexture BEFORE calling xrWeaveSubmitDXR, and the call
 * RETURNS only once the woven output is GPU-complete (XrWeaveOutputDXR::fence
 * stays NULL, @c fenceValue is a plain monotonic counter). An fd-based
 * (`sync_file`) acquire/release variant is a documented follow-up; it is a
 * latency optimisation, never a correctness requirement.
 */
typedef struct XrWeaveSubmitHandlesDXR {
    XrStructureType          type;        //!< XR_TYPE_WEAVE_SUBMIT_HANDLES_DXR
    const void* XR_MAY_ALIAS next;
    XrWeaveHandleKindDXR     inputKind;   //!< kind of XrWeaveSubmitInfoDXR::inputTexture
    XrWeaveHandleKindDXR     overlayKind; //!< kind of XrWeaveSubmitOverlaysDXR::overlayTexture
} XrWeaveSubmitHandlesDXR;

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
 * @brief N-view worst-case atlas input layout (spec v6, #774).
 *
 * Chain onto XrWeaveSubmitInfoDXR::next. When present it REPLACES the squeezed
 * per-rect SBS contract of XrWeaveSubmitRectsDXR with the same atlas layout every
 * other DisplayXR app already uses (ADR-010 worst-case swapchain + ADR-030
 * crop-before-DP), so the caller becomes an ordinary N-view client:
 *
 *  - @c inputTexture is a WORST-CASE-sized atlas — max over every rendering mode
 *    of (tileColumns * viewScaleX * displayWidth) x (tileRows * viewScaleY *
 *    displayHeight), spanning both orientations for modes that can rotate. Size it
 *    from the DISPLAY, not the current window, so window resize / fullscreen never
 *    reallocates. NOTE: this max is NOT bounded by the display size — a mode with
 *    viewScaleX 1.0 and tileColumns 2 yields a 2*W x H atlas.
 *  - Tiles are packed CONTIGUOUSLY from the top-left at (@c contentViewWidth,
 *    @c contentViewHeight) — tile v sits at ((v % tileColumns) * contentViewWidth,
 *    (v / tileColumns) * contentViewHeight). The stride is the CONTENT size, not
 *    atlasWidth / tileColumns. Everything right of / below the packed region is
 *    dead space the runtime never reads.
 *  - @c contentViewWidth / @c contentViewHeight are the bound window's client size
 *    scaled by the ACTIVE mode's viewScaleX / viewScaleY. Each visible element is
 *    drawn inside tile v at its own window position scaled by the same factors.
 *
 * With this chained, XrWeaveSubmitRectsDXR::rects become a scope hint (zone /
 * wish-mask publication and the caller's own draw-back region) rather than a
 * layout input, so XR_WEAVE_SUBMIT_MAX_RECTS_DXR no longer bounds how many
 * elements a frame may contain.
 *
 * The runtime hands the atlas to the display processor as-is when it exactly
 * fills the active mode's atlas (zero-copy), otherwise it crops the packed
 * region first. Callers that omit this struct keep the spec v3/v4/v5 behaviour
 * byte-for-byte.
 */
typedef struct XrWeaveSubmitLayoutDXR {
    XrStructureType          type;              //!< XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 viewCount;         //!< active mode view count; must equal tileColumns * tileRows
    uint32_t                 tileColumns;       //!< active mode atlas grid columns
    uint32_t                 tileRows;          //!< active mode atlas grid rows
    uint32_t                 contentViewWidth;  //!< per-tile content width  = windowWidth  * mode viewScaleX
    uint32_t                 contentViewHeight; //!< per-tile content height = windowHeight * mode viewScaleY
} XrWeaveSubmitLayoutDXR;

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
 * @brief Regions of this submit that must be PHYSICALLY FLAT (spec v8, browser#88).
 *
 * Chain onto XrWeaveSubmitInfoDXR::next. The runtime derives this frame's
 * per-region hardware wish as
 *
 *   wish = union(XrWeaveSubmitRectsDXR::rects) − union(@c rects)
 *
 * and publishes it to the display processor, which switches its physical 3D
 * element per region where the hardware can (ADR-027 Decision 5). The classic
 * case is a page with one inline-3D element inside otherwise flat 2D: without
 * this the whole panel sits behind the lens and the flat content ghosts.
 *
 * ADVISORY — it moves the HARDWARE element only, never CONTENT (ADR-027
 * Decision 6 / ADR-030). The woven pixels this call produces are bit-identical
 * whether or not this struct is chained; nothing here crops, masks or reorders
 * anything the caller submitted, and a DP with no per-region capability simply
 * quantizes the wish to its whole-panel any-nonzero default (= pre-v8 behaviour).
 * So a caller may always chain it, and never has to ask whether the panel can
 * honour it.
 *
 * Where a flat rect's edge falls between the hardware's switch cells the runtime
 * rounds TOWARD 3D (the flat rect shrinks to cell boundaries). Marking a working
 * 3D region flat would be a mono regression; leaving a flat region 3D is only the
 * pre-v8 ghosting, so the asymmetry is deliberate.
 *
 * @c rects are window-relative device pixels (y-down), the same space as
 * XrWeaveSubmitRectsDXR::rects. They may overlap each other and may overlap the
 * weave rects — subtraction is by area, not by list position. Scoped to THIS
 * submit; for regions that persist across frames in screen space use
 * xrWeaveSetScreenFlatRegionsDXR instead (the two compose: both are subtracted).
 *
 * @c rectCount 0 (or omitting the struct) = no flat regions, byte-for-byte pre-v8.
 */
typedef struct XrWeaveSubmitFlatRegionsDXR {
    XrStructureType          type;      //!< XR_TYPE_WEAVE_SUBMIT_FLAT_REGIONS_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 rectCount; //!< 0..XR_WEAVE_SUBMIT_MAX_FLAT_RECTS_DXR
    const XrRect2Di*         rects;     //!< window-relative flat regions, device px (y-down)
} XrWeaveSubmitFlatRegionsDXR;

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

/*!
 * @brief Explicit on-screen window geometry (spec v7, #1036).
 *
 * Chain onto XrWeaveBindWindowInfoDXR::next. The weave phase is locked to
 * ABSOLUTE SCREEN position, so the runtime has to know where the present-owner's
 * client area sits on the panel. On Windows it can read that off the HWND
 * (`GetClientRect` + `ClientToScreen`); on Android there is no such handle — a
 * SurfaceView's position is known only to the app (`View.getLocationOnScreen()`),
 * and a pure window MOVE raises no resize, so nothing else in the pipeline can
 * observe it. v7 therefore lets the caller state the geometry itself.
 *
 * The runtime forwards it to the display processor's per-window phase slot
 * (`set_window_screen_rect`, ADR-036 D6 / runtime#1033), which is what makes the
 * interlace phase-correct for a window that is not at the panel origin.
 *
 *  - @c windowOriginOnScreen is the CLIENT AREA's top-left in absolute physical
 *    screen pixels of @c displayId (y-down), NOT the window frame's.
 *  - @c clientSize is the client area in device pixels.
 *  - @c displayId is the platform display the rect is expressed in (Android
 *    `Display.getDisplayId()`, 0 = default panel); -1 = unknown / single display.
 *
 * Usable on every platform. On Windows and macOS it is OPTIONAL — when absent
 * the runtime derives the geometry from @c windowHandle exactly as before. On
 * Android it is REQUIRED (there is nothing to derive it from); a bind with no
 * geometry chain leaves the DP weaving display-scoped, i.e. phase-correct only
 * for a full-screen window at the panel origin.
 *
 * The caller RE-BINDS (calls xrWeaveBindWindow2DXR again) whenever the window
 * moves or resizes. It is a handful of integers and the runtime dedupes, so
 * re-binding every frame from a Choreographer / WM_WINDOWPOSCHANGED callback is
 * a supported (and on Android the expected) usage.
 */
typedef struct XrWeaveWindowGeometryDXR {
    XrStructureType          type;                //!< XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR
    const void* XR_MAY_ALIAS next;
    XrOffset2Di              windowOriginOnScreen; //!< client-area top-left, absolute screen px (y-down)
    XrExtent2Di              clientSize;           //!< client-area size, device px
    int32_t                  displayId;            //!< platform display id; -1 = unknown / single display
} XrWeaveWindowGeometryDXR;

/*!
 * @brief Window-bind parameters (spec v7, #1036).
 *
 * The chainable form of xrWeaveBindWindowDXR, so geometry (and future per-window
 * state) can be attached without another entry point. @c windowHandle carries the
 * platform window handle where one exists (HWND on Windows, an opaque id on
 * macOS) and is NULL on Android, where the app's Surface is its own and the
 * runtime never touches it — there the chained XrWeaveWindowGeometryDXR carries
 * everything the runtime needs.
 */
typedef struct XrWeaveBindWindowInfoDXR {
    XrStructureType          type;         //!< XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR
    const void* XR_MAY_ALIAS next;         //!< chain XrWeaveWindowGeometryDXR here
    void*                    windowHandle; //!< HWND / platform window id; NULL on Android
} XrWeaveBindWindowInfoDXR;

/*!
 * @brief A fresh, connected, UN-HANDSHAKEN runtime IPC endpoint (spec v9, browser#103).
 *
 * Output of xrWeaveExportIpcConnectionDXR. Exactly one of the two value fields
 * is meaningful per platform; both are always present so the struct's layout
 * does not change between platforms.
 *
 *  - @c handle — Windows: the connected named-pipe HANDLE. NULL elsewhere.
 *  - @c fd — POSIX: the connected socket descriptor. -1 on Windows.
 *
 * The caller OWNS what it receives and must `CloseHandle()` / `close()` it once
 * it has been transferred (or if the transfer fails). Transferring it into
 * another process is the caller's business — on Windows, mojo's
 * `PlatformHandle` / `DuplicateHandle` into the target; on POSIX, `SCM_RIGHTS`
 * or the child-launch descriptor table.
 */
typedef struct XrWeaveIpcConnectionDXR {
    XrStructureType    type;   //!< XR_TYPE_WEAVE_IPC_CONNECTION_DXR
    void* XR_MAY_ALIAS next;
    void*              handle; //!< Windows: connected pipe HANDLE; NULL elsewhere
    int32_t            fd;     //!< POSIX: connected socket fd; -1 on Windows
} XrWeaveIpcConnectionDXR;

typedef XrResult (XRAPI_PTR *PFN_xrWeaveBindWindowDXR)(
    XrSession session, void* windowHandle);

typedef XrResult (XRAPI_PTR *PFN_xrWeaveExportIpcConnectionDXR)(
    XrInstance instance, XrWeaveIpcConnectionDXR* connection);

typedef XrResult (XRAPI_PTR *PFN_xrWeaveBindWindow2DXR)(
    XrSession session, const XrWeaveBindWindowInfoDXR* bindInfo);

typedef XrResult (XRAPI_PTR *PFN_xrWeaveSubmitDXR)(
    XrSession session, const XrWeaveSubmitInfoDXR* submitInfo, XrWeaveOutputDXR* output);

typedef XrResult (XRAPI_PTR *PFN_xrWeaveSnapWindowRectDXR)(
    XrSession session, const XrRect2Di* originRect, const XrRect2Di* targetRect, XrRect2Di* snappedRect);

typedef XrResult (XRAPI_PTR *PFN_xrWeaveSetScreenFlatRegionsDXR)(
    XrSession session, uint32_t rectCount, const XrRect2Di* screenRects);

#ifndef XR_NO_PROTOTYPES

//! Bind the present-owner's window (HWND on Windows) so the DP phase-snaps the
//! interlace to the window's panel position on move / resize. Call once before
//! the first xrWeaveSubmitDXR; call again if the window handle changes.
XRAPI_ATTR XrResult XRAPI_CALL xrWeaveBindWindowDXR(
    XrSession session, void* windowHandle);

//! Bind the present-owner's window with explicit, chainable parameters (spec
//! v7). Equivalent to xrWeaveBindWindowDXR(session, bindInfo->windowHandle) plus
//! whatever is chained on @c next — today an XrWeaveWindowGeometryDXR giving the
//! client area's absolute on-screen origin + size, which the runtime forwards to
//! the display processor's per-window phase slot. Call again on every window
//! move / resize. On Android this is the ONLY way to bind (there is no window
//! handle to derive geometry from). Reports XR_ERROR_FEATURE_UNSUPPORTED on an
//! in-process session.
XRAPI_ATTR XrResult XRAPI_CALL xrWeaveBindWindow2DXR(
    XrSession session, const XrWeaveBindWindowInfoDXR* bindInfo);

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

//! Latch the regions of the PANEL that must stay physically flat (spec v8,
//! browser#88). Sticky companion to XrWeaveSubmitFlatRegionsDXR: same advisory
//! hardware-only semantics (see that struct — it moves the 3D element, never
//! content), but expressed in ABSOLUTE PHYSICAL SCREEN pixels and held until the
//! next call rather than scoped to one submit. For screen-anchored furniture — a
//! browser's toolbar and tab strip — that must be flat no matter what any
//! individual frame submits, and that the caller would otherwise have to re-send
//! on every submit forever.
//!
//! Applied IMMEDIATELY: if a wish is currently published the runtime re-rasters
//! and republishes it before returning, so the panel does not wait for the next
//! submit. Screen rects are intersected with the bound window's client area (the
//! wish is published window-anchored), so a rect naming panel area outside the
//! window is simply clipped away, not an error.
//!
//! @c rectCount 0 clears the latch (with @c screenRects NULL or ignored);
//! 1..XR_WEAVE_SET_MAX_SCREEN_FLAT_RECTS_DXR replaces it wholesale — this is a
//! SET, not an add. Reports XR_ERROR_FEATURE_UNSUPPORTED on an in-process
//! session.
XRAPI_ATTR XrResult XRAPI_CALL xrWeaveSetScreenFlatRegionsDXR(
    XrSession session, uint32_t rectCount, const XrRect2Di* screenRects);

//! Export a FRESH, CONNECTED, UN-HANDSHAKEN runtime IPC endpoint so another
//! process can adopt it (spec v9, browser#103).
//!
//! For an embedder whose rendering process cannot reach the runtime's IPC
//! transport itself. Chromium's GPU process is the motivating case: it runs a
//! `USER_LIMITED` restricted token that matches neither ACE on the service
//! pipe's security descriptor, so opening the pipe returns ACCESS_DENIED, while
//! its unsandboxed browser process opens it routinely. The browser process
//! calls this, ships the endpoint to the GPU process over its own transport,
//! and the GPU process adopts it (`ipc_client_connection_adopt_handle`, or
//! `DXR_IPC_HANDLE=<decimal>` / `DXR_IPC_FD=<n>` in the environment) before its
//! own `xrCreateInstance`.
//!
//! INSTANCE-level, not session-level: the endpoint this hands back has no
//! session on it, and the exporting instance may well have no weave session
//! either — a broker is not required to be a present-owner. The extension must
//! be enabled on @c instance.
//!
//! THE EXPORT DOES NOT HANDSHAKE. Connection setup is
//! connect -> shared-memory transfer -> version check -> client description, and
//! only the connect happens here; the other three MUST run in the ADOPTING
//! process, so that process gets its own mapping of the shared memory and is the
//! one identified to the service. An exporter that handshakes hands over a
//! connection attributed to itself, and the service duplicates shared handles
//! into the wrong address space.
//!
//! The caller's own connection is untouched — this opens a new endpoint. The
//! returned handle/fd is owned by the caller.
//!
//! Reports XR_ERROR_FEATURE_UNSUPPORTED when the calling instance is not on the
//! out-of-process (service) path, and XR_ERROR_RUNTIME_FAILURE if no endpoint
//! could be opened.
XRAPI_ATTR XrResult XRAPI_CALL xrWeaveExportIpcConnectionDXR(
    XrInstance instance, XrWeaveIpcConnectionDXR* connection);

#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif /* XR_DXR_WEAVE_H */
