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
 *   They may block (portal round trips, waiting for a grab to end) — bounded,
 *   at most a few seconds each.
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
 * "bracketleft", "bracketright", "grave", "capslock", "printscreen", "menu".
 * An empty key means "modifiers only" (e.g. "super" alone).
 *
 * Letters, digits and punctuation name the CHARACTER on the user's ACTIVE keyboard
 * layout, because that is how every OS matches shortcuts: "ctrl+z" must press the
 * key labelled Z, which on a French AZERTY layout is not the US-QWERTY Z position.
 * Backends map character -> key through the active layout (Windows VkKeyScanExW on
 * the foreground thread's HKL; macOS UCKeyTranslate over the current input source;
 * GNOME the session keymap libei hands out, via xkbcommon). If the active layout has
 * no key producing the character without AltGr-style extra modifiers, fail with
 * U_DESKTOP_ERR_UNSUPPORTED and a message naming the character. Named keys (enter,
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
};

typedef void (*u_desktop_access_done_fn)(void *ctx, enum u_desktop_access result);

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

	//! @p rect only for U_DESKTOP_OP_MOVE_RESIZE (w/h 0 = keep size).
	enum u_desktop_result (*window_op)(struct u_desktop_backend *b,
	                                   enum u_desktop_window_op op,
	                                   uint64_t window_id,
	                                   const struct u_desktop_rect *rect,
	                                   char *msg);

	//! Commit @p utf8 as text into the focused window (IME-safe: not key codes).
	enum u_desktop_result (*type_text)(struct u_desktop_backend *b, const char *utf8, char *msg);

	enum u_desktop_result (*key_chord)(struct u_desktop_backend *b, const struct u_desktop_chord *chord, char *msg);

	//! Move the pointer to (x, y). The backend verifies it arrived (within 2 px).
	enum u_desktop_result (*pointer_move)(struct u_desktop_backend *b, int32_t x, int32_t y, char *msg);

	enum u_desktop_result (*pointer_button)(struct u_desktop_backend *b,
	                                        enum u_desktop_button button,
	                                        bool down,
	                                        char *msg);

	//! Notches: +x right, +y down.
	enum u_desktop_result (*scroll)(struct u_desktop_backend *b, int32_t dx, int32_t dy, char *msg);

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
