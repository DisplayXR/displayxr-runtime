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
 * so the common layer persists the answer). A backend never forwards or shares a
 * grant, and never exposes these operations outside the process. The one grant a
 * backend stores is GNOME's portal restore token, in a private file of the same app
 * (its app id, or its executable), so the app's next launch restores it without the
 * dialog (spec §6.4, u_desktop_gnome_grant.h).
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
 * - Action calls (window_action, type_text, key_chord, pointer_*, scroll, key_event)
 *   are made from ONE worker thread per backend, never concurrently with each other.
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
 * For text and chords the common layer goes further and never passes a backend's
 * message on at all: the app gets a FIXED string per result. A backend that knows
 * more than its result says (GNOME: the clipboard changed hands, the window was
 * handed the user's clipboard, ...) names it with a verdict (enum
 * u_desktop_input_verdict, through u_desktop_guard_set_verdict), and the app gets
 * that verdict's fixed string instead.
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
 *      Spec v3: guard->target_id may be U_DESKTOP_TARGET_SHELL — then the re-check is
 *      that NO app window is there (u_desktop_pick_matches / u_desktop_focus_matches).
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
	U_DESKTOP_CAP_CLIPBOARD = 0x80, //!< spec v3: clipboard text (clipboard_get / clipboard_set)
};

//! Spec v3: the largest clipboard text read or written, in bytes (terminator excluded).
#define U_DESKTOP_CLIPBOARD_MAX_BYTES (1u << 20)

/*!
 * Zero @p n bytes at @p p with volatile stores, which the compiler may not drop the way
 * it may drop a memset right before free(). For text that must not outlive its use in
 * freed memory: typed or clipboard text (spec v3 §11.6). NULL is a no-op.
 */
static inline void
u_desktop_wipe(void *p, size_t n)
{
	volatile unsigned char *v = (volatile unsigned char *)p;
	while (v != NULL && n-- > 0) {
		*v++ = 0;
	}
}

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

/*!
 * Spec v3: the reserved target id XR_DESKTOP_TARGET_SHELL_DXR — input meant for the
 * desktop shell (top bar / taskbar, dock, desktop background, overview), never for an
 * app window. A backend's delivery checks honour it through u_desktop_pick_matches /
 * u_desktop_focus_matches. Never a real window id (HWNDs, CGWindowIDs and mutter ids
 * are all far below it).
 */
#define U_DESKTOP_TARGET_SHELL UINT64_MAX

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

/*!
 * Spec v3: what a click at a point meets is what an action for @p target may act on —
 * the target window or a dialog of it, or, for U_DESKTOP_TARGET_SHELL, anything that
 * is NOT an app window (the shell, the desktop, outside every monitor: a pointer that
 * cannot get there fails to move, it is never clamped onto a window).
 */
static inline bool
u_desktop_pick_matches(const struct u_desktop_pick *p, uint64_t target)
{
	if (target == U_DESKTOP_TARGET_SHELL) {
		return p->kind != U_DESKTOP_PICK_WINDOW;
	}
	return target != 0 && p->kind == U_DESKTOP_PICK_WINDOW &&
	       (p->window.id == target || p->window.parent_id == target);
}

/*!
 * Spec v3: the keyboard is where an action for @p target may type — @p focused (a
 * focused_window answer) is the target or a dialog of it (@p focused_parent is its
 * parent_id), or, for U_DESKTOP_TARGET_SHELL, no app window holds the keyboard:
 * nothing is focused, the shell holds it (@p focused == U_DESKTOP_TARGET_SHELL, e.g.
 * the GNOME overview), or the focused window is not an app window (@p focused_listed
 * false: not in list_windows, e.g. the Windows taskbar after a click on it).
 */
static inline bool
u_desktop_focus_matches(uint64_t target, uint64_t focused, uint64_t focused_parent, bool focused_listed)
{
	if (target == U_DESKTOP_TARGET_SHELL) {
		return focused == 0 || focused == U_DESKTOP_TARGET_SHELL || !focused_listed;
	}
	return target != 0 && focused != 0 && focused != U_DESKTOP_TARGET_SHELL &&
	       (focused == target || (focused_parent != 0 && focused_parent == target));
}

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
 * Why a keyboard action (type_text, key_chord) ended as it did, when the backend
 * knows more than its result says (see "Privacy"). Each verdict belongs to ONE
 * result: the backend returns that result and sets the verdict with
 * u_desktop_guard_set_verdict; the app then gets the verdict's fixed message
 * (U_DESKTOP_VERDICT_MSG_*) with that result's usual XrResult. A verdict that comes
 * with any other result is ignored (the result's own fixed message stands), and so
 * is NONE. The messages are part of the public contract (XR_DXR_desktop_control.md
 * §9): change them only with it. Today only the GNOME clipboard paste sets one.
 */
enum u_desktop_input_verdict
{
	U_DESKTOP_VERDICT_NONE = 0, //!< nothing more to say: the result's fixed message
	//! U_DESKTOP_OK: the text was read after the paste keystroke, nothing confirmed it arrived.
	U_DESKTOP_VERDICT_UNVERIFIED_OK,
	//! U_DESKTOP_ERR_REFUSED: the user copied before the paste keystroke; nothing was pasted.
	U_DESKTOP_VERDICT_CLIPBOARD_CHANGED_HANDS,
	//! U_DESKTOP_ERR_REFUSED: the user copied just as the agent's text was offered; the
	//! offer replaced the user's copy (gone) and the clipboard was released.
	U_DESKTOP_VERDICT_USER_COPY_LOST_RELEASED,
	//! U_DESKTOP_ERR_FAILED: the user copied right after the keystroke and the agent's
	//! text was never read: whatever the window pasted was the user's clipboard.
	U_DESKTOP_VERDICT_PASTED_USER_CLIPBOARD,
	//! U_DESKTOP_ERR_FAILED: the user copied during the paste; whether the text arrived is unknown.
	U_DESKTOP_VERDICT_DELIVERY_UNKNOWN,
	//! U_DESKTOP_ERR_FAILED: the focused window never asked for the pasted text.
	U_DESKTOP_VERDICT_NOT_ASKED,
	//! U_DESKTOP_ERR_FAILED: the focused text field did not change.
	U_DESKTOP_VERDICT_NOT_DELIVERED,
	U_DESKTOP_VERDICT_COUNT, //!< not a verdict
};

/*
 * The verdicts' fixed messages: plain strings (no format specifiers) that never name
 * any text. UNVERIFIED_OK completes "ok (unverified: <its message>)". The em dash is
 * spelled as UTF-8 bytes so no compiler's source charset can change it.
 */
#define U_DESKTOP_EM_DASH "\xe2\x80\x94"
#define U_DESKTOP_VERDICT_MSG_UNVERIFIED_OK \
	"the window read the text after the paste, but no accessible text field confirmed it was inserted " \
	"(a clipboard manager reads too)"
#define U_DESKTOP_VERDICT_MSG_CLIPBOARD_CHANGED_HANDS \
	"the clipboard changed hands (the user copied something); nothing was pasted"
#define U_DESKTOP_VERDICT_MSG_USER_COPY_LOST_RELEASED \
	"the clipboard changed hands (the user copied something) just as the agent's text was being offered " \
	U_DESKTOP_EM_DASH " nothing was pasted, but the offer had already replaced what the user copied, which " \
	"is gone (copy it again); the clipboard was released and is empty"
#define U_DESKTOP_VERDICT_MSG_PASTED_USER_CLIPBOARD \
	"the clipboard changed hands right after the paste keystroke (the user copied something) and the " \
	"agent's text was never read " U_DESKTOP_EM_DASH " whatever the window pasted was the user's clipboard, " \
	"not the agent's text; the user's clipboard was left as is"
#define U_DESKTOP_VERDICT_MSG_DELIVERY_UNKNOWN \
	"lost the clipboard during the paste (the user copied something) " U_DESKTOP_EM_DASH " whether the text " \
	"arrived is unknown; the user's clipboard was left as is"
#define U_DESKTOP_VERDICT_MSG_NOT_ASKED "the focused window did not ask for the pasted text (does it accept text?)"
#define U_DESKTOP_VERDICT_MSG_NOT_DELIVERED \
	"the focused text field did not change " U_DESKTOP_EM_DASH " the paste was not seen to arrive"

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
	//! Where a keyboard action's verdict goes: set it with u_desktop_guard_set_verdict.
	//! NULL when the caller collects none (the smoke tool).
	enum u_desktop_input_verdict *verdict;
};

//! Report a keyboard action's verdict, with the result it belongs to (see the enum).
static inline void
u_desktop_guard_set_verdict(const struct u_desktop_input_guard *guard, enum u_desktop_input_verdict v)
{
	if (guard != NULL && guard->verdict != NULL) {
		*guard->verdict = v;
	}
}

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

	/*!
	 * Optional (may be NULL). The CONTROL scope became GRANTED (@p granted true) or
	 * stopped being GRANTED (false: denied, revoked by the OS, the kill switch, the
	 * store's per-app switch, the lock screen, a new prompt, the session ending).
	 * The common layer calls it only on a change, in order, from the thread that
	 * noticed it, with its state mutex released (it holds only the lock that keeps
	 * these calls in order: never call back into the control object from here), and
	 * once with false before destroy when the last call said true. Bounded (an
	 * observation-class call).
	 * GNOME: the DisplayXR extension gives other apps' window titles only to a bus
	 * connection that asked for them (extension v18, wayland-window-geometry spec
	 * §12.5), so the backend asks on its observation connection while granted and
	 * takes it back when not.
	 */
	void (*grant_changed)(struct u_desktop_backend *b, bool granted);

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

	//! The window with keyboard focus (0 if none / unknown). Spec v3: or
	//! U_DESKTOP_TARGET_SHELL when the shell holds the keyboard over the windows (the
	//! GNOME overview), so keys go there whatever window mutter calls focused.
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

	// ---- spec v3: held input (worker thread only). Optional: NULL = KEY_DOWN / KEY_UP
	// ---- and modifiers around a pointer action are UNSUPPORTED on this backend.

	/*!
	 * Press (@p down) or release ONE key and leave it so: @p key carries exactly one
	 * modifier bit and no key name ("shift"), or no modifier and one canonical key
	 * name ("tab", "a"; a character key presses the key producing that character on
	 * the active layout, WITHOUT adding Shift — modifiers are always explicit here).
	 *
	 * A press is an injection batch: first guard->still_allowed, then, with
	 * @p check_focus, the keyboard re-check against guard->target_id (as for a chord's
	 * key-down). Without it (a modifier held around a pointer action, whose button
	 * press re-checks the point) only still_allowed. A release is NEVER refused and
	 * never consults the guard: releasing what we hold is always allowed, also while
	 * suspended or locked; releasing a key this backend does not hold is a no-op OK.
	 *
	 * The common layer tracks what it holds and releases it (spec §11.3); a backend
	 * still releases whatever it holds when its OS session ends or on destroy.
	 */
	enum u_desktop_result (*key_event)(struct u_desktop_backend *b,
	                                   const struct u_desktop_input_guard *guard,
	                                   const struct u_desktop_chord *key,
	                                   bool down,
	                                   bool check_focus,
	                                   char *msg);

	/*!
	 * Optional (NULL = never). True while this backend holds a key that the OS
	 * auto-repeats AS INPUT — every repeat resets the idle counter user_idle_ms reads
	 * (GNOME: mutter repeats a held non-modifier libei key every ~30 ms after 500 ms).
	 * The user's own input cannot be told from that repeat then: the common layer stops
	 * reading the idle counter as the user's while it is true (spec §11.3). Worker only.
	 */
	bool (*holds_repeating_key)(struct u_desktop_backend *b);

	// ---- spec v3: clipboard text (worker thread only, in submission order with every
	// ---- other action, so it never runs during a paste). Optional together: a backend
	// ---- with both sets U_DESKTOP_CAP_CLIPBOARD. PRIVACY: the text never reaches a log,
	// ---- a message or the audit (sizes only).

	/*!
	 * The clipboard's text as UTF-8: @p *out malloc'd and NUL-terminated (the caller
	 * wipes and frees it), @p *len its length. At most U_DESKTOP_CLIPBOARD_MAX_BYTES + 1
	 * bytes are returned: a longer text is cut there and the common layer refuses it.
	 * OK with @p *out NULL: the clipboard holds no text (empty, or another type only).
	 * Calls guard->still_allowed before it reads. Bounded (~1 s).
	 */
	enum u_desktop_result (*clipboard_get)(struct u_desktop_backend *b,
	                                       const struct u_desktop_input_guard *guard,
	                                       char **out,
	                                       size_t *len,
	                                       char *msg);

	/*!
	 * Make @p utf8 (valid, non-empty, at most U_DESKTOP_CLIPBOARD_MAX_BYTES) the
	 * clipboard's text, as a copy would — for every app, until something else is
	 * copied. Calls guard->still_allowed right before it writes. Bounded.
	 */
	enum u_desktop_result (*clipboard_set)(struct u_desktop_backend *b,
	                                       const struct u_desktop_input_guard *guard,
	                                       const char *utf8,
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

	/*!
	 * Optional (may be NULL). The user's kill switch ended this app's grant (spec §6.4):
	 * drop the OS grant this process holds — close any OS session, forget any token in
	 * memory (and its saved copy) — so that nothing is restored until the app requests
	 * access again. Called with the common layer's mutex held: must not block (post the
	 * work to the backend's own thread).
	 */
	void (*revoke_grant)(struct u_desktop_backend *b);

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
