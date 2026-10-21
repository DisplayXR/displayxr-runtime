// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-control OS backend interface (XR_DXR_desktop_control, ADR-056).
 *
 * One backend per OS / desktop: Win32 (u_desktop_win32.cpp), macOS
 * (u_desktop_macos.m), GNOME Wayland (u_desktop_gnome.c). A backend is a thin,
 * mechanical layer over the OS: it lists windows, hit-tests, reads accessible
 * text, acts on windows and injects input. It owns NO policy — consent order,
 * the kill switch, the idle wait, target verification and the audit log all live
 * in the common layer (u_desktop_control.c), identically on every OS.
 *
 * ## The consent rule (ADR-056 D2)
 *
 * The backend runs inside the APP's process and must ask the OS in the app's
 * name: the GNOME RemoteDesktop portal (attributed to the app's app id),
 * macOS TCC (Accessibility + Screen Recording, attributed to the app bundle /
 * responsible process). On Windows, where the OS has no prompt, request_access
 * shows DisplayXR's own prompt naming the app (and `remembers_in_store` is true
 * so the common layer persists the answer). A backend never stores, forwards or
 * shares a grant, and never exposes these operations outside the process.
 *
 * ## Coordinates
 *
 * DESKTOP coordinates everywhere (see the public header): Windows physical
 * virtual-desktop px (the backend must use a per-monitor-v2 DPI-aware thread
 * context for every geometry call — docs/reference/dpi-awareness.md); macOS
 * global points, top-left origin of the primary display, y down; GNOME stage px.
 *
 * ## Threading
 *
 * - Action calls (window_action, type_text, key_chord, pointer_*, scroll) are
 *   made from ONE worker thread per backend, never concurrently with each other.
 *   They may block (portal round trips, waiting for a grab to end) and must be
 *   bounded: a few seconds for everything but text. type_text takes time
 *   proportional to its length (chunk pacing, about 1 s per 2 KiB worst case:
 *   a 64 KiB text runs ~13 s on macOS, ~30 s on Windows); the input guard,
 *   re-asked before every chunk, keeps it interruptible (kill switch, lock,
 *   focus change) throughout.
 * - Observation calls (list_windows, get_pointer, pick, text_at, focused_window,
 *   is_locked, user_idle_ms, access_status) may come from ANY thread, concurrently
 *   with an action in flight, and must NOT block on it: they are called from the
 *   app's main thread (a Unity frame). Keep them fast — pick / focused_window /
 *   get_pointer / is_locked / user_idle_ms target < 5 ms; list_windows and text_at
 *   (window-server / accessibility round trips) target < 50 ms typical, with OS-side
 *   spikes allowed (macOS CGWindowList measured 3 ms mean, ~90 ms max). Callers must
 *   not list windows every frame; the common layer's per-action checks use pick and
 *   focused_window. Use separate connections / locks from the action path where the
 *   OS API needs it.
 * - request_access may complete asynchronously: call @p done exactly once, from
 *   any thread, possibly before request_access returns.
 *
 * ## Errors
 *
 * Functions return enum u_desktop_result. On failure a backend may fill the
 * optional @p msg buffer (UTF-8, NUL-terminated, may be NULL) with a short human
 * reason; it ends up in the action-completed event and the audit log.
 *
 * ## Privacy (contract v2)
 *
 * A message, a log line (U_LOG at any level) or anything else a backend emits must
 * NEVER contain typed text, a character from a chord, clipboard contents or UI text
 * read from another app. Name the failure, not the data ("this layout has no key for
 * that character", not "no key for 'é'").
 *
 * ## Delivery guarantee (contract v2)
 *
 * The common layer verifies the target (spec §5 step 4) and then hands the action to
 * the backend together with a struct u_desktop_input_guard. The verification is only
 * as good as the gap between it and the actual injection, so every injecting backend
 * call MUST, immediately before EACH injection batch (each chunk of text, the paste
 * keystroke, each Return/Tab inside text, each key-down of a chord, each button press,
 * each scroll burst, each Super/Alt gesture of a window op):
 *   1. call guard->still_allowed(guard->ctx) and stop with U_DESKTOP_ERR_REFUSED if it
 *      returns false (the kill switch or the lock screen arrived mid-action), and
 *   2. re-check that the target still holds what the action needs — keyboard focus
 *      (text, chords: the focused window is guard->target_id or a window whose
 *      parent_id is it) or the pick at the point (buttons, scroll, gestures) — and stop
 *      with U_DESKTOP_ERR_REFUSED "focus moved" / "target no longer under the point".
 * Anything already injected stays injected; a stop must release every key and button
 * the backend pressed. A backend must never report OK for input the OS dropped (e.g. a
 * revoked permission): re-check the OS permission before injecting and return
 * U_DESKTOP_ERR_NOT_GRANTED, which the common layer turns into a DENIED state.
 *
 * ## Lifetime (contract v2)
 *
 * destroy() must not return while a request_access answer is still owed: it cancels
 * the prompt / the polling (close the dialog, stop the poll thread), JOINS that
 * thread, and either calls done() first (with U_DESKTOP_ACCESS_UNKNOWN when the user
 * never answered) or guarantees done() is never called afterwards. The common layer
 * frees the context right after destroy() returns.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_DESKTOP_APP_ID_SIZE 128
#define U_DESKTOP_TITLE_SIZE 256
#define U_DESKTOP_MSG_SIZE 256
//! A consent dialog's parent window, terminator included (XR_MAX_DESKTOP_PARENT_WINDOW_SIZE_DXR).
#define U_DESKTOP_PARENT_WINDOW_SIZE 256

enum u_desktop_result
{
	U_DESKTOP_OK = 0,
	U_DESKTOP_ERR_UNSUPPORTED,   //!< this backend cannot do it (capability missing)
	U_DESKTOP_ERR_NOT_GRANTED,   //!< the OS refused: permission missing / revoked
	U_DESKTOP_ERR_WINDOW_LOST,   //!< the window id no longer exists
	U_DESKTOP_ERR_REFUSED,       //!< the OS (or a grab / a modal) refused this one
	U_DESKTOP_ERR_TIMEOUT,       //!< a bounded wait ran out
	U_DESKTOP_ERR_FAILED,        //!< anything else
};

//! Same bit values as XrDesktopControlCapabilityFlagsDXR.
enum u_desktop_caps
{
	U_DESKTOP_CAP_LIST_WINDOWS = 0x01,
	U_DESKTOP_CAP_POINTER_POSITION = 0x02,
	U_DESKTOP_CAP_PICK = 0x04,
	U_DESKTOP_CAP_READ_TEXT = 0x08,
	U_DESKTOP_CAP_WINDOW_ACTIONS = 0x10,
	U_DESKTOP_CAP_INPUT = 0x20,
	U_DESKTOP_CAP_CAPTURE = 0x40, //!< spec v2: screen stills (the `capture` op)
};

//! The consent scopes (spec v2, same bits as XrDesktopControlScopeFlagsDXR).
enum u_desktop_scope
{
	U_DESKTOP_SCOPE_CONTROL = 0x1, //!< everything in spec v1
	U_DESKTOP_SCOPE_CAPTURE = 0x2, //!< screen pixels
	//! Not a scope: ask again even for a GRANTED scope (the user may change what is
	//! shared, or decline). Passed through to request_scopes.
	U_DESKTOP_SCOPE_REASK = 0x100,
};

//! Same bit values as XrDesktopWindowStateFlagsDXR.
enum u_desktop_window_state
{
	U_DESKTOP_WINDOW_FOCUSED = 0x01,
	U_DESKTOP_WINDOW_MINIMIZED = 0x02,
	U_DESKTOP_WINDOW_MAXIMIZED = 0x04,
	U_DESKTOP_WINDOW_FULLSCREEN = 0x08,
	U_DESKTOP_WINDOW_OWN = 0x10, //!< belongs to the calling process (pid == getpid())
};

struct u_desktop_rect
{
	int32_t x, y, w, h;
};

struct u_desktop_window
{
	uint64_t id;        //!< never 0
	uint64_t parent_id; //!< owner window of a dialog / transient, else 0
	uint32_t pid;
	char app_id[U_DESKTOP_APP_ID_SIZE]; //!< exe base name / bundle id / Wayland app id
	char title[U_DESKTOP_TITLE_SIZE];   //!< UTF-8, truncated on a code-point boundary
	struct u_desktop_rect frame;        //!< desktop coordinates, visible frame (no shadow)
	uint32_t state;                     //!< enum u_desktop_window_state bits
	uint32_t z;                         //!< 0 = top-most (list order)
};

//! Same values as XrDesktopPickKindDXR.
enum u_desktop_pick_kind
{
	U_DESKTOP_PICK_WINDOW = 0,
	U_DESKTOP_PICK_DESKTOP = 1,
	U_DESKTOP_PICK_SHELL = 2,
	U_DESKTOP_PICK_OUTSIDE = 3,
};

struct u_desktop_pick
{
	int32_t x, y;
	enum u_desktop_pick_kind kind;
	struct u_desktop_window window; //!< valid when kind == U_DESKTOP_PICK_WINDOW
};

//! Same values as the window subset of XrDesktopActionTypeDXR.
enum u_desktop_window_op
{
	U_DESKTOP_OP_ACTIVATE = 0,
	U_DESKTOP_OP_MOVE_RESIZE = 1,
	U_DESKTOP_OP_MINIMIZE = 2,
	U_DESKTOP_OP_RESTORE = 3,
	U_DESKTOP_OP_MAXIMIZE = 4,
	U_DESKTOP_OP_CLOSE = 5,
};

enum u_desktop_button
{
	U_DESKTOP_BUTTON_LEFT = 0,
	U_DESKTOP_BUTTON_RIGHT = 1,
	U_DESKTOP_BUTTON_MIDDLE = 2,
};

enum u_desktop_mod
{
	U_DESKTOP_MOD_CTRL = 0x1,
	U_DESKTOP_MOD_SHIFT = 0x2,
	U_DESKTOP_MOD_ALT = 0x4,   //!< Option on macOS
	U_DESKTOP_MOD_SUPER = 0x8, //!< Windows key / Command / Super
};

/*!
 * A parsed key chord ("ctrl+shift+v"), produced by u_desktop_chord_parse().
 * `key` is one canonical lowercase name: "a".."z", "0".."9", "f1".."f24",
 * "enter", "tab", "escape", "backspace", "delete", "insert", "home", "end",
 * "pageup", "pagedown", "left", "right", "up", "down", "space", "minus",
 * "equal", "comma", "period", "slash", "backslash", "semicolon", "quote",
 * "bracketleft", "bracketright", "grave", "capslock", "printscreen", "menu",
 * the keypad "numpad0".."numpad9", "numpadmultiply", "numpadadd",
 * "numpadsubtract", "numpaddivide", "numpaddecimal", "numpadenter",
 * "pause", "numlock", "scrolllock", the media keys "mediaplaypause",
 * "mediatracknext", "mediatrackprevious", "mediastop", "audiovolumeup",
 * "audiovolumedown", "audiovolumemute", and the right-hand modifiers as keys
 * "rightctrl", "rightshift", "rightalt", "rightsuper" (u_desktop_chord_named_key
 * lists the named ones; spec §5 has the table and what each OS does with them).
 * An empty key means "modifiers only" (e.g. "super" alone). A backend with no key
 * for a name (macOS has no Pause, lock, media or right-hand modifier key codes
 * here) fails with U_DESKTOP_ERR_UNSUPPORTED.
 *
 * Letters, digits and punctuation name the CHARACTER on the user's ACTIVE keyboard
 * layout, because that is how every OS matches shortcuts: "ctrl+z" must press the
 * key labelled Z, which on a French AZERTY layout is not the US-QWERTY Z position.
 * Backends map character -> key through the active layout (Windows VkKeyScanExW on
 * the foreground thread's HKL; macOS UCKeyTranslate over the current input source;
 * GNOME the session keymap libei hands out, via xkbcommon). If the active layout has
 * no key producing the character without AltGr-style extra modifiers, fail with
 * U_DESKTOP_ERR_UNSUPPORTED and a message that does NOT name the character (see
 * "Privacy": "the active keyboard layout has no key for that character"). Named keys (enter,
 * f5, left, ...) are layout-independent. Text goes through type_text, never chords.
 */
struct u_desktop_chord
{
	uint32_t mods; //!< enum u_desktop_mod bits
	char key[24];
};

enum u_desktop_access
{
	U_DESKTOP_ACCESS_UNKNOWN = 0, //!< not determinable without prompting
	U_DESKTOP_ACCESS_GRANTED,
	U_DESKTOP_ACCESS_DENIED,
	U_DESKTOP_ACCESS_PENDING,
};

//! Who is asking — the app the OS / the prompt must name.
struct u_desktop_app_info
{
	const char *exe_path;     //!< full path of the process executable (identity for the store)
	const char *display_name; //!< human name for a prompt (product / bundle name, else exe base name)
	const char *reason;       //!< app-supplied, may be NULL
	/*!
	 * Spec v2: the app window the consent dialog belongs to, in the xdg-desktop-portal
	 * "parent_window" form ("wayland:<handle>" / "x11:<hex XID>"); NULL or "" = none.
	 * Already validated (u_desktop_parent_window_valid). A placement hint only: a
	 * backend without a use for it ignores it. Valid for the call only — copy to keep.
	 */
	const char *parent_window;
};

typedef void (*u_desktop_access_done_fn)(void *ctx, enum u_desktop_access result);

/*!
 * The answer to request_scopes (spec v2): one result per scope. A scope the prompt
 * said nothing about is U_DESKTOP_ACCESS_UNKNOWN (left as it was). A backend may
 * report a scope it was not asked for when the OS dialog decided it anyway (GNOME:
 * one Start response decides input AND screens).
 */
typedef void (*u_desktop_scopes_done_fn)(void *ctx, enum u_desktop_access control, enum u_desktop_access capture);

/*!
 * A captured image (spec v2 §10.3): RGBA8, sRGB, straight alpha, top row first,
 * stride w * 4. Withheld pixels are (0,0,0,0). Allocate with u_desktop_image_alloc
 * (zeroed: everything withheld until written), free with u_desktop_image_free.
 * NEVER written to disk or a log (privacy, spec §10.4).
 */
struct u_desktop_image
{
	uint8_t *rgba;
	uint32_t w, h;
	uint64_t frame_ns; //!< os_monotonic_get_ns() time at which these pixels were current
};

//! Zeroed w x h image (all withheld). False on a bad size / no memory.
bool
u_desktop_image_alloc(struct u_desktop_image *img, uint32_t w, uint32_t h);

void
u_desktop_image_free(struct u_desktop_image *img);

/*!
 * Handed to every injecting backend call (see "Delivery guarantee").
 */
struct u_desktop_input_guard
{
	//! The window the action was verified against (never 0).
	uint64_t target_id;
	//! True while the action may continue: not suspended (kill switch), not denied,
	//! not locked. Cheap (a stat of the store + the backend's is_locked); call it
	//! before every injection batch. Never NULL.
	bool (*still_allowed)(void *ctx);
	void *ctx;
};

struct u_desktop_backend
{
	const char *name; //!< "win32", "macos", "gnome", "fake"
	uint32_t caps;    //!< enum u_desktop_caps bits this backend implements

	/*!
	 * True when the OS shows NO prompt of its own (Windows): request_access then
	 * draws DisplayXR's prompt, and the common layer persists the answer in the
	 * DisplayXR store. False when the OS owns and remembers consent (portal, TCC).
	 */
	bool remembers_in_store;

	//! Grant state as the OS sees it now, without prompting.
	enum u_desktop_access (*access_status)(struct u_desktop_backend *b);

	//! Ask the OS / show the prompt. Calls @p done exactly once.
	void (*request_access)(struct u_desktop_backend *b,
	                       const struct u_desktop_app_info *app,
	                       u_desktop_access_done_fn done,
	                       void *ctx);

	//! True while the session is locked / a secure desktop is up.
	bool (*is_locked)(struct u_desktop_backend *b);

	//! Milliseconds since the last user input of ANY kind (ours included — the
	//! common layer subtracts its own injections). False when unknown.
	bool (*user_idle_ms)(struct u_desktop_backend *b, uint64_t *out_ms);

	//! Visible top-level windows, top-most first. Fills up to @p cap, sets
	//! @p count to the total available.
	enum u_desktop_result (*list_windows)(struct u_desktop_backend *b,
	                                      struct u_desktop_window *out,
	                                      uint32_t cap,
	                                      uint32_t *count);

	enum u_desktop_result (*get_pointer)(struct u_desktop_backend *b, int32_t *x, int32_t *y);

	//! The OS hit test at (x, y): what a click there would meet.
	enum u_desktop_result (*pick)(struct u_desktop_backend *b, int32_t x, int32_t y, struct u_desktop_pick *out);

	//! The window with keyboard focus (0 if none / unknown).
	enum u_desktop_result (*focused_window)(struct u_desktop_backend *b, uint64_t *out_id);

	//! Accessible name/value of the UI element at (x, y), UTF-8. "" if none.
	enum u_desktop_result (*text_at)(
	    struct u_desktop_backend *b, int32_t x, int32_t y, char *buf, size_t cap, char *msg);

	// ---- actions: worker thread only ----

	/*!
	 * Optional (may be NULL). Called on the worker BEFORE the common layer verifies
	 * the target of an input action or a window op, so slow setup (GNOME: opening /
	 * restoring the portal session) happens before verification, not between
	 * verification and injection. Must not inject anything.
	 */
	enum u_desktop_result (*prepare_input)(struct u_desktop_backend *b, char *msg);

	//! @p rect only for U_DESKTOP_OP_MOVE_RESIZE (w/h 0 = keep size). Backends that
	//! perform a window op through input (GNOME gestures) honour @p guard as for input
	//! (re-pick before every press); others may ignore it.
	enum u_desktop_result (*window_op)(struct u_desktop_backend *b,
	                                   const struct u_desktop_input_guard *guard,
	                                   enum u_desktop_window_op op,
	                                   uint64_t window_id,
	                                   const struct u_desktop_rect *rect,
	                                   char *msg);

	//! Commit @p utf8 as text into guard->target_id (IME-safe: not key codes),
	//! re-checking the guard before every batch (see "Delivery guarantee").
	enum u_desktop_result (*type_text)(struct u_desktop_backend *b,
	                                   const struct u_desktop_input_guard *guard,
	                                   const char *utf8,
	                                   char *msg);

	enum u_desktop_result (*key_chord)(struct u_desktop_backend *b,
	                                   const struct u_desktop_input_guard *guard,
	                                   const struct u_desktop_chord *chord,
	                                   char *msg);

	//! Move the pointer to (x, y). The backend verifies it arrived (within 2 px).
	enum u_desktop_result (*pointer_move)(struct u_desktop_backend *b, int32_t x, int32_t y, char *msg);

	//! Press / release at the VERIFIED point (@p x, @p y): the backend posts the event
	//! at that point (not "wherever the cursor is now") and, for a press, first
	//! confirms the pointer is still there (2 px) and the guard still holds.
	enum u_desktop_result (*pointer_button)(struct u_desktop_backend *b,
	                                        const struct u_desktop_input_guard *guard,
	                                        enum u_desktop_button button,
	                                        bool down,
	                                        int32_t x,
	                                        int32_t y,
	                                        char *msg);

	//! Notches at the verified point (@p x, @p y): +dx right, +dy down.
	enum u_desktop_result (*scroll)(struct u_desktop_backend *b,
	                                const struct u_desktop_input_guard *guard,
	                                int32_t x,
	                                int32_t y,
	                                int32_t dx,
	                                int32_t dy,
	                                char *msg);

	// ---- spec v2: screen capture. All optional (NULL = no capture; then the backend
	// ---- must not set U_DESKTOP_CAP_CAPTURE). See "Capture" below.

	/*!
	 * The CAPTURE scope's grant as the OS sees it now, without prompting (as
	 * access_status is for CONTROL).
	 */
	enum u_desktop_access (*capture_access_status)(struct u_desktop_backend *b);

	/*!
	 * Ask for @p scopes (enum u_desktop_scope bits) in ONE prompt. Calls @p done
	 * exactly once (any thread, possibly before returning) with a result per scope.
	 * When NULL, the common layer uses request_access for CONTROL-only requests.
	 */
	void (*request_scopes)(struct u_desktop_backend *b,
	                       const struct u_desktop_app_info *app,
	                       uint32_t scopes,
	                       u_desktop_scopes_done_fn done,
	                       void *ctx);

	/*!
	 * The monitors, desktop coordinates (every monitor, shared or not). Fills up to
	 * @p cap, sets @p count to the total. Observation-class: any thread, fast.
	 */
	enum u_desktop_result (*monitors)(
	    struct u_desktop_backend *b, struct u_desktop_rect *out, uint32_t cap, uint32_t *count, char *msg);

	/*!
	 * One still of @p src (desktop coordinates, already clipped to the monitors) at
	 * the backend's native density: img->w x img->h device px where the OS has them
	 * (GNOME: the highest monitor scale in the region). Allocates @p img with
	 * u_desktop_image_alloc; pixels it cannot show (off every monitor, an unshared
	 * monitor, a window the OS / the DisplayXR extension excludes from capture) stay
	 * (0,0,0,0) — and when the backend cannot tell which windows are excluded it
	 * REFUSES ("cannot honour capture exclusion") rather than return them unmasked.
	 * The common layer masks the app's OWN windows itself. Called only from the
	 * session's capture worker, never concurrently with itself; bounded (2 s plus any
	 * session restore). Must refuse while locked. Never writes pixels anywhere else.
	 */
	enum u_desktop_result (*capture)(struct u_desktop_backend *b,
	                                 const struct u_desktop_rect *src,
	                                 struct u_desktop_image *img,
	                                 char *msg);

	//! See "Lifetime": never returns while a consent answer is owed.
	void (*destroy)(struct u_desktop_backend *b);
};

/*!
 * The platform backend for this process, or NULL when the OS / desktop has none
 * (e.g. a non-GNOME Wayland session, Android). Implemented once per OS; exactly
 * one implementation is compiled in. Never prompts.
 */
struct u_desktop_backend *
u_desktop_backend_create_platform(void);

#ifdef __cplusplus
}
#endif
