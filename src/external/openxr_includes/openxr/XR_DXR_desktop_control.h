// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the XR_DXR_* extensions in this header are NOT yet registered in the
// Khronos OpenXR registry: extension numbers and XrStructureType values sit
// in a provisional experimental block (1004999xxx) pending official
// assignment. Extension names are expected to be stable; numeric values are
// not. See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Header for XR_DXR_desktop_control (an app's agent operates the desktop).
 * @author DisplayXR
 * @ingroup external_openxr
 *
 * Lets an app — typically an AI agent living on a spatial display — observe and
 * operate the user's desktop: list windows, find what is under the pointer, read
 * UI text, activate / move / minimize / close windows, and type, press keys and
 * click into other apps. One API on every OS (ADR-056).
 *
 * THE CONSENT BELONGS TO THE APP. The runtime executes every operation INSIDE the
 * calling app's process, through the OS's own permission path, so the OS asks the
 * user about *this app*, by name:
 *   - GNOME / KDE Wayland: the RemoteDesktop portal (approved once per app id);
 *   - macOS: Accessibility + Screen Recording (System Settings, per app bundle);
 *   - Windows (no OS prompt exists): a DisplayXR first-run prompt naming the app.
 * The runtime never holds a grant on behalf of anyone else, and never exposes
 * these operations to other processes (not over MCP, not over IPC).
 *
 * Safety layer, identical on every OS (spec §5): operations run only while the
 * state is XR_DESKTOP_CONTROL_STATE_GRANTED_DXR; they are refused while the screen
 * is locked, while the user's kill switch is on (`displayxr-cli desktop stop`), or
 * when the user disabled this app; input waits until the user has been idle for a
 * moment (never fights live typing); and before every click or keystroke the
 * runtime verifies that the action's target window is the one actually there
 * (pick at the point / keyboard focus). A failed check fails the action — input is
 * never delivered to a different window. Every mutating action is written to the
 * user's local audit log.
 *
 * Coordinates are DESKTOP coordinates: the OS's global input space — Windows:
 * physical virtual-desktop pixels; macOS: global points (top-left origin of the
 * primary display, y down); GNOME: stage (logical) pixels. Window frames and
 * action points use the same space, so a frame read from one call can be fed to
 * another unchanged.
 *
 * Actions are asynchronous: xrSubmitDesktopActionDXR queues and returns an id;
 * the result arrives as XrEventDataDesktopActionCompletedDXR through xrPollEvent.
 * Observation calls are synchronous and cheap.
 *
 * Spec version 2 adds screen capture as a second consent SCOPE (CAPTURE, next to
 * CONTROL = everything in version 1): stills on request, never a stream, never
 * written to disk by the runtime, behind the same kill switch, lock check and audit log.
 *
 * Spec: displayxr-runtime docs/specs/extensions/XR_DXR_desktop_control.md
 * Decision: docs/adr/ADR-056-desktop-control-runs-in-the-app.md
 */
#ifndef XR_DXR_DESKTOP_CONTROL_H
#define XR_DXR_DESKTOP_CONTROL_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_desktop_control 1
#define XR_DXR_desktop_control_SPEC_VERSION 2
#define XR_DXR_DESKTOP_CONTROL_EXTENSION_NAME "XR_DXR_desktop_control"

#define XR_TYPE_DESKTOP_WINDOW_DXR                          ((XrStructureType)1004999400)
#define XR_TYPE_DESKTOP_PICK_DXR                            ((XrStructureType)1004999401)
#define XR_TYPE_DESKTOP_ACTION_DXR                          ((XrStructureType)1004999402)
#define XR_TYPE_EVENT_DATA_DESKTOP_ACTION_COMPLETED_DXR     ((XrStructureType)1004999403)
#define XR_TYPE_EVENT_DATA_DESKTOP_CONTROL_STATE_CHANGED_DXR ((XrStructureType)1004999404)
#define XR_TYPE_DESKTOP_CONTROL_REQUEST_INFO_DXR            ((XrStructureType)1004999405)
// Spec version 2 (screen capture, spec §10, ADR-056 Amendment 1).
#define XR_TYPE_DESKTOP_CAPTURE_INFO_DXR                     ((XrStructureType)1004999406)
#define XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_COMPLETED_DXR     ((XrStructureType)1004999407)
#define XR_TYPE_DESKTOP_CONTROL_REQUEST_SCOPE_DXR            ((XrStructureType)1004999408)
#define XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_STATE_CHANGED_DXR ((XrStructureType)1004999409)

//! The operation is refused by the safety layer (lock screen, kill switch, the
//! user is typing and the wait timed out, target verification failed). The
//! completion event's `message` says which.
#define XR_ERROR_DESKTOP_CONTROL_REFUSED_DXR                ((XrResult)-1004999400)
//! Desktop control is not granted for this app (see XrDesktopControlStateDXR).
#define XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR            ((XrResult)-1004999401)
//! The target window no longer exists.
#define XR_ERROR_DESKTOP_WINDOW_LOST_DXR                    ((XrResult)-1004999402)

#define XR_MAX_DESKTOP_APP_ID_SIZE_DXR 128
#define XR_MAX_DESKTOP_TITLE_SIZE_DXR 256
#define XR_MAX_DESKTOP_MESSAGE_SIZE_DXR 256

/*!
 * Where the app stands with desktop control. Only GRANTED allows operations.
 */
typedef enum XrDesktopControlStateDXR {
    //! No backend on this OS / desktop (e.g. a Wayland desktop without the portal).
    XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR      = 0,
    //! Never requested in this session; call xrRequestDesktopControlDXR.
    XR_DESKTOP_CONTROL_STATE_NOT_REQUESTED_DXR    = 1,
    //! A consent prompt (OS or DisplayXR) is up; wait for the state-changed event.
    XR_DESKTOP_CONTROL_STATE_PENDING_DXR          = 2,
    XR_DESKTOP_CONTROL_STATE_GRANTED_DXR          = 3,
    //! The user (or the OS) said no. Request again only on a new user gesture.
    XR_DESKTOP_CONTROL_STATE_DENIED_DXR           = 4,
    //! The user turned desktop control off for this app (`displayxr-cli desktop deny`).
    XR_DESKTOP_CONTROL_STATE_DISABLED_BY_USER_DXR = 5,
    //! Temporarily unavailable: the user's kill switch is on, or the screen is locked.
    XR_DESKTOP_CONTROL_STATE_SUSPENDED_DXR        = 6,
    XR_DESKTOP_CONTROL_STATE_MAX_ENUM_DXR         = 0x7FFFFFFF
} XrDesktopControlStateDXR;

typedef XrFlags64 XrDesktopControlCapabilityFlagsDXR;
static const XrDesktopControlCapabilityFlagsDXR XR_DESKTOP_CONTROL_CAPABILITY_LIST_WINDOWS_BIT_DXR = 0x00000001;
static const XrDesktopControlCapabilityFlagsDXR XR_DESKTOP_CONTROL_CAPABILITY_POINTER_POSITION_BIT_DXR = 0x00000002;
static const XrDesktopControlCapabilityFlagsDXR XR_DESKTOP_CONTROL_CAPABILITY_PICK_BIT_DXR = 0x00000004;
static const XrDesktopControlCapabilityFlagsDXR XR_DESKTOP_CONTROL_CAPABILITY_READ_TEXT_BIT_DXR = 0x00000008;
static const XrDesktopControlCapabilityFlagsDXR XR_DESKTOP_CONTROL_CAPABILITY_WINDOW_ACTIONS_BIT_DXR = 0x00000010;
static const XrDesktopControlCapabilityFlagsDXR XR_DESKTOP_CONTROL_CAPABILITY_INPUT_BIT_DXR = 0x00000020;
//! Spec version 2: the backend can capture screen pixels (and this system has the
//! libraries for it). Without it the CAPTURE scope is UNSUPPORTED.
static const XrDesktopControlCapabilityFlagsDXR XR_DESKTOP_CONTROL_CAPABILITY_CAPTURE_BIT_DXR = 0x00000040;

/*!
 * Spec version 2: the consent scopes. Each has its own state (XrDesktopControlStateDXR
 * values), its own state-changed event and its own entry in the user's store.
 */
typedef XrFlags64 XrDesktopControlScopeFlagsDXR;
//! Everything in spec version 1: observation, window actions, input.
static const XrDesktopControlScopeFlagsDXR XR_DESKTOP_CONTROL_SCOPE_CONTROL_BIT_DXR = 0x00000001;
//! Screen pixels (xrCaptureDesktopDXR).
static const XrDesktopControlScopeFlagsDXR XR_DESKTOP_CONTROL_SCOPE_CAPTURE_BIT_DXR = 0x00000002;

typedef XrFlags64 XrDesktopWindowStateFlagsDXR;
static const XrDesktopWindowStateFlagsDXR XR_DESKTOP_WINDOW_STATE_FOCUSED_BIT_DXR    = 0x00000001;
static const XrDesktopWindowStateFlagsDXR XR_DESKTOP_WINDOW_STATE_MINIMIZED_BIT_DXR  = 0x00000002;
static const XrDesktopWindowStateFlagsDXR XR_DESKTOP_WINDOW_STATE_MAXIMIZED_BIT_DXR  = 0x00000004;
static const XrDesktopWindowStateFlagsDXR XR_DESKTOP_WINDOW_STATE_FULLSCREEN_BIT_DXR = 0x00000008;
//! The window belongs to the calling app's own process.
static const XrDesktopWindowStateFlagsDXR XR_DESKTOP_WINDOW_STATE_OWN_BIT_DXR        = 0x00000010;

/*!
 * One top-level window. `windowId` is stable for the window's lifetime and never 0
 * (HWND on Windows, CGWindowID on macOS, mutter's window id on GNOME).
 */
typedef struct XrDesktopWindowDXR {
    XrStructureType              type;   //!< XR_TYPE_DESKTOP_WINDOW_DXR
    void* XR_MAY_ALIAS           next;
    uint64_t                     windowId;
    uint64_t                     parentWindowId; //!< owner of a dialog / transient; 0 if none
    uint32_t                     pid;
    char                         appId[XR_MAX_DESKTOP_APP_ID_SIZE_DXR];  //!< exe base name / bundle id / app id (UTF-8)
    char                         title[XR_MAX_DESKTOP_TITLE_SIZE_DXR];   //!< UTF-8, truncated
    XrRect2Di                    frame;  //!< desktop coordinates
    XrDesktopWindowStateFlagsDXR state;
    uint32_t                     zOrder; //!< 0 = top-most of the listed windows
} XrDesktopWindowDXR;

typedef enum XrDesktopPickKindDXR {
    XR_DESKTOP_PICK_KIND_WINDOW_DXR  = 0, //!< a top-level window (see `window`)
    XR_DESKTOP_PICK_KIND_DESKTOP_DXR = 1, //!< the desktop background / icons
    XR_DESKTOP_PICK_KIND_SHELL_DXR   = 2, //!< taskbar, dock, panel, system UI
    XR_DESKTOP_PICK_KIND_OUTSIDE_DXR = 3, //!< not on any monitor
    XR_DESKTOP_PICK_KIND_MAX_ENUM_DXR = 0x7FFFFFFF
} XrDesktopPickKindDXR;

/*!
 * What a click at `point` would meet — the OS's own hit test, so stacking,
 * click-through regions and system UI on top count as they would for a click.
 */
typedef struct XrDesktopPickDXR {
    XrStructureType       type;   //!< XR_TYPE_DESKTOP_PICK_DXR
    void* XR_MAY_ALIAS    next;
    XrOffset2Di           point;  //!< desktop coordinates
    XrDesktopPickKindDXR  kind;
    XrDesktopWindowDXR    window; //!< output; filled by the runtime (incl. window.type) when kind == WINDOW, zeroed otherwise
} XrDesktopPickDXR;

typedef enum XrDesktopActionTypeDXR {
    // --- windows (targetWindowId required) ---
    XR_DESKTOP_ACTION_TYPE_ACTIVATE_DXR      = 0,  //!< raise + focus (restores if minimized where the OS allows)
    XR_DESKTOP_ACTION_TYPE_MOVE_RESIZE_DXR   = 1,  //!< to `rect` (width/height 0 = keep size)
    XR_DESKTOP_ACTION_TYPE_MINIMIZE_DXR      = 2,
    XR_DESKTOP_ACTION_TYPE_RESTORE_DXR       = 3,  //!< un-minimize / un-maximize
    XR_DESKTOP_ACTION_TYPE_MAXIMIZE_DXR      = 4,
    XR_DESKTOP_ACTION_TYPE_CLOSE_DXR         = 5,  //!< a polite close request (the app may ask to save)
    // --- input (targetWindowId required: verified before delivery) ---
    XR_DESKTOP_ACTION_TYPE_TYPE_TEXT_DXR     = 10, //!< `text` (UTF-8), committed as text — IME-safe, any language
    XR_DESKTOP_ACTION_TYPE_KEY_CHORD_DXR     = 11, //!< `keys`, e.g. "ctrl+s", "alt+tab", "enter", "shift+f10"
    XR_DESKTOP_ACTION_TYPE_POINTER_MOVE_DXR  = 12, //!< to `point`
    XR_DESKTOP_ACTION_TYPE_CLICK_DXR         = 13, //!< `button` × `clickCount` at `point`
    XR_DESKTOP_ACTION_TYPE_DRAG_DXR          = 14, //!< `button` held from `point` to `point2`
    XR_DESKTOP_ACTION_TYPE_SCROLL_DXR        = 15, //!< `scrollX`/`scrollY` notches at `point`
    XR_DESKTOP_ACTION_TYPE_MAX_ENUM_DXR      = 0x7FFFFFFF
} XrDesktopActionTypeDXR;

typedef enum XrDesktopPointerButtonDXR {
    XR_DESKTOP_POINTER_BUTTON_LEFT_DXR   = 0,
    XR_DESKTOP_POINTER_BUTTON_RIGHT_DXR  = 1,
    XR_DESKTOP_POINTER_BUTTON_MIDDLE_DXR = 2,
    XR_DESKTOP_POINTER_BUTTON_MAX_ENUM_DXR = 0x7FFFFFFF
} XrDesktopPointerButtonDXR;

/*!
 * One action. Fields not used by `actionType` are ignored. Strings are copied at
 * submit time.
 */
typedef struct XrDesktopActionDXR {
    XrStructureType            type;   //!< XR_TYPE_DESKTOP_ACTION_DXR
    const void* XR_MAY_ALIAS   next;
    XrDesktopActionTypeDXR     actionType;
    //! The window this action is for. Window actions act on it; input actions are
    //! delivered only if it is the window under `point` (pointer actions) or has
    //! keyboard focus (text / keys) at delivery time.
    uint64_t                   targetWindowId;
    XrRect2Di                  rect;       //!< MOVE_RESIZE
    XrOffset2Di                point;      //!< pointer actions
    XrOffset2Di                point2;     //!< DRAG end
    XrDesktopPointerButtonDXR  button;     //!< CLICK, DRAG
    uint32_t                   clickCount; //!< CLICK (0 is treated as 1)
    int32_t                    scrollX;    //!< SCROLL, notches, +right
    int32_t                    scrollY;    //!< SCROLL, notches, +down
    const char*                text;       //!< TYPE_TEXT
    const char*                keys;       //!< KEY_CHORD
} XrDesktopActionDXR;

/*!
 * Optional argument of xrRequestDesktopControlDXR (pass NULL for none). `reason` is
 * shown to the user in the DisplayXR prompt where DisplayXR draws one (Windows) and
 * written to the audit log. Spec version 2: `next` may carry an
 * XrDesktopControlRequestScopeDXR (other structures in the chain are ignored).
 */
typedef struct XrDesktopControlRequestInfoDXR {
    XrStructureType            type;   //!< XR_TYPE_DESKTOP_CONTROL_REQUEST_INFO_DXR
    const void* XR_MAY_ALIAS   next;
    const char*                reason; //!< UTF-8, e.g. "so Lumo can open apps and type for you"
} XrDesktopControlRequestInfoDXR;

/*!
 * Spec version 2. Chained on XrDesktopControlRequestInfoDXR::next: the scopes asked for,
 * in ONE prompt. Absent = CONTROL only (the version 1 behaviour). A scope already
 * GRANTED is not asked again. On GNOME one portal dialog covers both scopes, but a
 * restore token cannot be widened: asking for CAPTURE after CONTROL shows a second,
 * combined dialog that asks about input again — apps that will capture should ask for
 * both scopes in their first request.
 */
typedef struct XrDesktopControlRequestScopeDXR {
    XrStructureType                type;   //!< XR_TYPE_DESKTOP_CONTROL_REQUEST_SCOPE_DXR
    const void* XR_MAY_ALIAS       next;
    XrDesktopControlScopeFlagsDXR  scopes; //!< non-zero, known bits only
} XrDesktopControlRequestScopeDXR;

//! Spec version 2: what xrCaptureDesktopDXR captures.
typedef enum XrDesktopCaptureTargetDXR {
    XR_DESKTOP_CAPTURE_TARGET_DESKTOP_DXR = 0, //!< the bounding box of every monitor
    XR_DESKTOP_CAPTURE_TARGET_REGION_DXR  = 1, //!< `region`, desktop coordinates (clipped to the monitors)
    XR_DESKTOP_CAPTURE_TARGET_WINDOW_DXR  = 2, //!< `windowId`'s frame as it is on screen (overlapping windows included)
    XR_DESKTOP_CAPTURE_TARGET_MAX_ENUM_DXR = 0x7FFFFFFF
} XrDesktopCaptureTargetDXR;

/*!
 * Spec version 2: one still. The image is RGBA8, sRGB-encoded, straight alpha, top row
 * first, stride width * 4, scaled down (area filter) to fit maxWidth x maxHeight keeping
 * the aspect ratio, never up. Withheld pixels — off every monitor, on a monitor the user
 * did not share, inside a capture-excluded window or one of the app's own windows — are
 * (0,0,0,0); every other pixel has alpha 255. Map an image pixel back to desktop
 * coordinates with x = sourceRect.x + (px + 0.5) * sourceRect.width / width (same for y).
 */
typedef struct XrDesktopCaptureInfoDXR {
    XrStructureType            type;      //!< XR_TYPE_DESKTOP_CAPTURE_INFO_DXR
    const void* XR_MAY_ALIAS   next;
    XrDesktopCaptureTargetDXR  target;
    uint64_t                   windowId;  //!< WINDOW
    XrRect2Di                  region;    //!< REGION
    uint32_t                   maxWidth;  //!< 1..8192: the image is scaled down to fit, never up
    uint32_t                   maxHeight; //!< 1..8192
} XrDesktopCaptureInfoDXR;

typedef struct XrEventDataDesktopCaptureCompletedDXR {
    XrStructureType            type;       //!< XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_COMPLETED_DXR
    const void* XR_MAY_ALIAS   next;
    XrSession                  session;
    uint64_t                   captureId;
    XrResult                   result;     //!< XR_SUCCESS or an XR_ERROR_* (as for actions)
    uint32_t                   width;      //!< image size (0 on failure)
    uint32_t                   height;
    XrRect2Di                  sourceRect; //!< what the image covers, desktop coordinates
    XrTime                     frameTime;  //!< when these pixels were current
    char                       message[XR_MAX_DESKTOP_MESSAGE_SIZE_DXR];
} XrEventDataDesktopCaptureCompletedDXR;

typedef struct XrEventDataDesktopCaptureStateChangedDXR {
    XrStructureType            type;  //!< XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_STATE_CHANGED_DXR
    const void* XR_MAY_ALIAS   next;
    XrSession                  session;
    XrDesktopControlStateDXR   state; //!< the CAPTURE scope's state
} XrEventDataDesktopCaptureStateChangedDXR;

typedef struct XrEventDataDesktopActionCompletedDXR {
    XrStructureType            type;   //!< XR_TYPE_EVENT_DATA_DESKTOP_ACTION_COMPLETED_DXR
    const void* XR_MAY_ALIAS   next;
    XrSession                  session;
    uint64_t                   actionId;
    XrResult                   result; //!< XR_SUCCESS or an XR_ERROR_* (REFUSED / NOT_GRANTED / WINDOW_LOST / ...)
    char                       message[XR_MAX_DESKTOP_MESSAGE_SIZE_DXR]; //!< human-readable detail
} XrEventDataDesktopActionCompletedDXR;

typedef struct XrEventDataDesktopControlStateChangedDXR {
    XrStructureType            type;   //!< XR_TYPE_EVENT_DATA_DESKTOP_CONTROL_STATE_CHANGED_DXR
    const void* XR_MAY_ALIAS   next;
    XrSession                  session;
    XrDesktopControlStateDXR   state;
} XrEventDataDesktopControlStateChangedDXR;

//! Current state and what this OS backend can do (capabilities may be a subset).
//! Never gated: succeeds for any valid session, whatever the state (it is how an
//! app learns that it is not granted).
typedef XrResult (XRAPI_PTR *PFN_xrGetDesktopControlStateDXR)(
    XrSession                           session,
    XrDesktopControlStateDXR*           state,
    XrDesktopControlCapabilityFlagsDXR* capabilities);

//! Ask for consent. Returns at once; the state goes PENDING and the outcome arrives
//! as XrEventDataDesktopControlStateChangedDXR. A remembered grant resolves without
//! a prompt. @p info may be NULL.
typedef XrResult (XRAPI_PTR *PFN_xrRequestDesktopControlDXR)(
    XrSession                              session,
    const XrDesktopControlRequestInfoDXR*  info);

//! Top-level, visible windows, top-most first (two-call idiom).
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateDesktopWindowsDXR)(
    XrSession             session,
    uint32_t              windowCapacityInput,
    uint32_t*             windowCountOutput,
    XrDesktopWindowDXR*   windows);

//! The pointer position, and what is under it.
typedef XrResult (XRAPI_PTR *PFN_xrGetDesktopPointerDXR)(
    XrSession             session,
    XrDesktopPickDXR*     pick);

//! What a click at @p point would meet.
typedef XrResult (XRAPI_PTR *PFN_xrPickDesktopDXR)(
    XrSession             session,
    XrOffset2Di           point,
    XrDesktopPickDXR*     pick);

//! The accessible text of the UI element at @p point (UTF-8, two-call idiom).
//! Empty (count 1) when the element exposes none. Apps whose accessibility is off
//! (some browsers / Electron) expose nothing; that is not an error. Each call reads
//! the live UI: the text may change between the size query and the fill call, in
//! which case the fill returns XR_ERROR_SIZE_INSUFFICIENT with the new size — retry.
typedef XrResult (XRAPI_PTR *PFN_xrGetDesktopTextAtDXR)(
    XrSession             session,
    XrOffset2Di           point,
    uint32_t              capacityInput,
    uint32_t*             countOutput,
    char*                 buffer);

//! Queue an action. Returns at once with @p actionId (never 0); the result
//! arrives as XrEventDataDesktopActionCompletedDXR — exactly one per accepted action
//! while the session exists (the event carries only the id: keep your own id → action
//! map). Actions of one session run in submission order. At most 64 are queued per
//! session (XR_ERROR_LIMIT_REACHED beyond); destroying the session drops queued
//! actions without events.
typedef XrResult (XRAPI_PTR *PFN_xrSubmitDesktopActionDXR)(
    XrSession                  session,
    const XrDesktopActionDXR*  action,
    uint64_t*                  actionId);

//! Spec version 2: the CAPTURE scope's state. Never gated, like
//! xrGetDesktopControlStateDXR (which keeps reporting the CONTROL scope, plus the
//! capability bits, CAPTURE_BIT included).
typedef XrResult (XRAPI_PTR *PFN_xrGetDesktopCaptureStateDXR)(
    XrSession                  session,
    XrDesktopControlStateDXR*  state);

//! Spec version 2: start one capture. Returns at once with @p captureId (never 0);
//! exactly one XrEventDataDesktopCaptureCompletedDXR follows while the session exists.
//! Needs the CAPTURE scope GRANTED (CONTROL is not required) and never prompts. One
//! capture in flight per session (XR_ERROR_LIMIT_REACHED otherwise); starts are at
//! least 250 ms apart (an earlier call is accepted and waits).
typedef XrResult (XRAPI_PTR *PFN_xrCaptureDesktopDXR)(
    XrSession                       session,
    const XrDesktopCaptureInfoDXR*  info,
    uint64_t*                       captureId);

//! Spec version 2: the image of the most recent successful capture (two-call idiom, in
//! BYTES: countOutput = width * height * 4). Kept until a call copies it out, the next
//! capture completes, the session ends or 10 s pass; any other id fails validation.
typedef XrResult (XRAPI_PTR *PFN_xrGetDesktopCaptureImageDXR)(
    XrSession  session,
    uint64_t   captureId,
    uint32_t   capacityInput,
    uint32_t*  countOutput,
    uint8_t*   rgba);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrGetDesktopControlStateDXR(XrSession session, XrDesktopControlStateDXR* state, XrDesktopControlCapabilityFlagsDXR* capabilities);
XRAPI_ATTR XrResult XRAPI_CALL xrRequestDesktopControlDXR(XrSession session, const XrDesktopControlRequestInfoDXR* info);
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateDesktopWindowsDXR(XrSession session, uint32_t windowCapacityInput, uint32_t* windowCountOutput, XrDesktopWindowDXR* windows);
XRAPI_ATTR XrResult XRAPI_CALL xrGetDesktopPointerDXR(XrSession session, XrDesktopPickDXR* pick);
XRAPI_ATTR XrResult XRAPI_CALL xrPickDesktopDXR(XrSession session, XrOffset2Di point, XrDesktopPickDXR* pick);
XRAPI_ATTR XrResult XRAPI_CALL xrGetDesktopTextAtDXR(XrSession session, XrOffset2Di point, uint32_t capacityInput, uint32_t* countOutput, char* buffer);
XRAPI_ATTR XrResult XRAPI_CALL xrSubmitDesktopActionDXR(XrSession session, const XrDesktopActionDXR* action, uint64_t* actionId);
XRAPI_ATTR XrResult XRAPI_CALL xrGetDesktopCaptureStateDXR(XrSession session, XrDesktopControlStateDXR* state);
XRAPI_ATTR XrResult XRAPI_CALL xrCaptureDesktopDXR(XrSession session, const XrDesktopCaptureInfoDXR* info, uint64_t* captureId);
XRAPI_ATTR XrResult XRAPI_CALL xrGetDesktopCaptureImageDXR(XrSession session, uint64_t captureId, uint32_t capacityInput, uint32_t* countOutput, uint8_t* rgba);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_DESKTOP_CONTROL_H
