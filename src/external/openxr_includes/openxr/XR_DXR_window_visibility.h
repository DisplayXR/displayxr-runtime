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
 * @brief  Header for XR_DXR_window_visibility extension
 * @ingroup external_openxr
 *
 * Tells an app whether anyone can see its window (ADR-062 D4, tier 3).
 *
 * The runtime already judges every 3D window off the hot path: minimized,
 * cloaked (another virtual desktop), hidden by its owner after it was shown,
 * behind a locked session or an off display, or covered by other windows for
 * ~1.5 s except for a sliver. While the window is hidden the runtime drops
 * its lens vote and, when it is hidden on every screen, skips its weave and
 * present (tiers 1-2). Those happen whether or not this extension is enabled.
 *
 * This extension adds two things:
 * - XrEventDataWindowVisibilityChangedDXR, queued on every edge of the
 *   session's verdict "hidden on every screen the window covers" (and when
 *   the reason of a hidden window changes), for sessions whose instance
 *   enabled the extension;
 * - an opt-in, XrSessionWindowVisibilityInfoDXR::demoteWhenHidden, chained on
 *   XrSessionCreateInfo: while the window is hidden the session is moved to
 *   XR_SESSION_STATE_SYNCHRONIZED (shouldRender = XR_FALSE), and back to
 *   VISIBLE then FOCUSED when it is seen again. Two guards: a session is
 *   never demoted while its window is the OS foreground window, nor before
 *   its window has been shown once (NOT_SHOWN).
 *
 * A session that does not chain the struct gets the runtime's default: it is
 * demoted (same guards) only while its window is MINIMIZED. Chaining the
 * struct with demoteWhenHidden = XR_FALSE opts out: never demoted.
 * `_hosted` sessions (no app window) are judged on the window the runtime
 * created for them, and get the event and the opt-in the same way.
 */
#ifndef XR_DXR_WINDOW_VISIBILITY_H
#define XR_DXR_WINDOW_VISIBILITY_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_window_visibility 1
#define XR_DXR_window_visibility_SPEC_VERSION 1
#define XR_DXR_WINDOW_VISIBILITY_EXTENSION_NAME "XR_DXR_window_visibility"

// Reserved 1004999380-389 (allocation registry: README.md in this directory).
#define XR_TYPE_EVENT_DATA_WINDOW_VISIBILITY_CHANGED_DXR ((XrStructureType)1004999380)
#define XR_TYPE_SESSION_WINDOW_VISIBILITY_INFO_DXR       ((XrStructureType)1004999381)

/*!
 * @brief Why the session's window is (or is not) visible.
 *
 * VISIBLE accompanies visible = XR_TRUE; every other value accompanies
 * visible = XR_FALSE.
 */
typedef enum XrWindowVisibilityReasonDXR {
    //! Some meaningful part of the window can be seen.
    XR_WINDOW_VISIBILITY_REASON_VISIBLE_DXR = 0,
    //! The window (its root window) is minimized. Reported at once.
    XR_WINDOW_VISIBILITY_REASON_MINIMIZED_DXR = 1,
    //! The window is cloaked: on another virtual desktop, or hidden by the shell. Reported at once.
    XR_WINDOW_VISIBILITY_REASON_CLOAKED_DXR = 2,
    //! Other windows cover all but a sliver of it, on every screen it covers. Reported after ~1.5 s.
    XR_WINDOW_VISIBILITY_REASON_COVERED_DXR = 3,
    //! The window has not been shown yet. Never demotes the session.
    XR_WINDOW_VISIBILITY_REASON_NOT_SHOWN_DXR = 4,
    //! The user session is locked. Reported at once.
    XR_WINDOW_VISIBILITY_REASON_LOCKED_DXR = 5,
    //! The display is off. Reported at once.
    XR_WINDOW_VISIBILITY_REASON_DISPLAY_OFF_DXR = 6,
    //! The window was hidden by its owner after it had been shown. Reported at once.
    XR_WINDOW_VISIBILITY_REASON_WINDOW_HIDDEN_DXR = 7,
    XR_WINDOW_VISIBILITY_REASON_MAX_ENUM_DXR = 0x7FFFFFFF
} XrWindowVisibilityReasonDXR;

/*!
 * @brief The session's window was hidden on every screen it covers, seen
 * again, or the reason of a hidden window changed.
 *
 * Queued only for sessions whose instance enabled the extension. The first
 * event of a session reports a change from "visible": a session whose window
 * is seen from the start receives none.
 */
typedef struct XrEventDataWindowVisibilityChangedDXR {
    XrStructureType             type;    //!< XR_TYPE_EVENT_DATA_WINDOW_VISIBILITY_CHANGED_DXR
    const void* XR_MAY_ALIAS    next;
    XrSession                   session;
    XrBool32                    visible; //!< XR_FALSE: hidden on every screen, or not shown yet
    XrWindowVisibilityReasonDXR reason;
    XrTime                      time;    //!< when the runtime judged the change
} XrEventDataWindowVisibilityChangedDXR;

/*!
 * @brief Session-create opt-in to session-state demotion while hidden.
 *
 * Chain on XrSessionCreateInfo::next. Ignored unless the extension is
 * enabled. With demoteWhenHidden = XR_TRUE a hidden session goes to
 * SYNCHRONIZED (shouldRender = XR_FALSE: the app may skip rendering, and must
 * keep calling xrWaitFrame / xrBeginFrame / xrEndFrame) and returns to
 * VISIBLE then FOCUSED when the window is seen again — never while the window
 * is the OS foreground window, and never before it has been shown once.
 */
typedef struct XrSessionWindowVisibilityInfoDXR {
    XrStructureType          type;   //!< XR_TYPE_SESSION_WINDOW_VISIBILITY_INFO_DXR
    const void* XR_MAY_ALIAS next;
    XrBool32                 demoteWhenHidden;
} XrSessionWindowVisibilityInfoDXR;

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_WINDOW_VISIBILITY_H
