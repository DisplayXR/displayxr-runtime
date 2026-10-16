// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME Wayland desktop-control backend — private interfaces between
 *         its files (XR_DXR_desktop_control, ADR-056).
 *
 *   u_desktop_gnome.c          the backend vtable, observation through the
 *                              DisplayXR GNOME extension (v16), consent, input
 *   u_desktop_gnome_windows.c  window ops: other apps' windows as a user would
 *                              (Super+click, mutter keybindings, Super+drag with
 *                              read-back); the app's own via WindowPlacement1
 *   u_desktop_gnome_portal.c   the RemoteDesktop portal session (consent, restore
 *                              token in memory only) on ONE private thread with
 *                              its own private D-Bus connection
 *   u_desktop_gnome_input.c    libei input, layout-aware chords (xkbcommon), and
 *                              text through the portal clipboard — on that thread
 *   u_desktop_gnome_libs.c     libei.so.1 + libxkbcommon.so.0, dlopen'd
 *   u_desktop_gnome_atspi.c    text_at through AT-SPI
 *
 * Only built on desktop Linux with libdbus (src/xrt/auxiliary/desktop/CMakeLists.txt).
 */

#pragma once

#include "u_desktop.h"

#include <dbus/dbus.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * D-Bus helpers (u_desktop_gnome.c)
 */

//! A D-Bus error, copied out of a DBusError (empty name: no error / no reply).
struct gdbus_err
{
	char name[128];
	char message[192];
};

/*!
 * Call @p method and block for the reply (at most @p timeout_ms). The variadic
 * arguments are dbus_message_append_args() pairs ending in DBUS_TYPE_INVALID.
 * Returns the reply (caller unrefs) or NULL, with @p err (may be NULL) filled.
 */
DBusMessage *
gdbus_call(DBusConnection *c,
           const char *dest,
           const char *path,
           const char *iface,
           const char *method,
           int timeout_ms,
           struct gdbus_err *err,
           int first_arg_type,
           ...);

//! org.freedesktop.DBus.Properties.Get of a uint32 property.
bool
gdbus_get_prop_u32(DBusConnection *c,
                   const char *dest,
                   const char *path,
                   const char *iface,
                   const char *prop,
                   int timeout_ms,
                   uint32_t *out);

//! org.freedesktop.DBus.Properties.Get of a boolean property.
bool
gdbus_get_prop_bool(DBusConnection *c,
                    const char *dest,
                    const char *path,
                    const char *iface,
                    const char *prop,
                    int timeout_ms,
                    bool *out);

//! Drop every queued incoming message (signals nobody subscribed to, NameAcquired).
void
gdbus_drain(DBusConnection *c);

//! Copy @p src into @p dst (cap bytes), truncating on a UTF-8 code-point boundary.
void
gdbus_copy_utf8(char *dst, size_t cap, const char *src);

//! Monotonic milliseconds.
uint64_t
gdbus_now_ms(void);

void
gdbus_sleep_ms(uint32_t ms);

/*
 * The portal + libei thread (u_desktop_gnome_portal.c)
 */

struct gnome_portal;

/*!
 * True when libei.so.1 can be dlopen'd with every symbol this backend uses.
 * Absent on Ubuntu 22.04: the backend then has no INPUT / WINDOW_ACTIONS.
 */
bool
gnome_portal_libei_available(void);

/*!
 * @p rd_version: the RemoteDesktop portal's `version` property (0 = no portal:
 * returns NULL). Input needs >= 2 (ConnectToEIS, persist_mode) AND libei; without
 * them the portal is CONSENT-ONLY — the dialog grants observation, the session is
 * closed at once, and every input call returns UNSUPPORTED.
 * @p have_clipboard: the Clipboard portal exists.
 * The thread and its connection are started on first use, never here.
 */
struct gnome_portal *
gnome_portal_create(uint32_t rd_version, bool have_clipboard);

//! True when this portal drives input (libei + RemoteDesktop >= 2).
bool
gnome_portal_has_input(struct gnome_portal *p);

void
gnome_portal_destroy(struct gnome_portal *p);

enum u_desktop_access
gnome_portal_access(struct gnome_portal *p);

//! Open a session interactively (the portal's dialog). Calls @p done once.
void
gnome_portal_request(struct gnome_portal *p, u_desktop_access_done_fn done, void *ctx);

//! libei absolute motion to stage point (x, y). REFUSED when no input region holds it.
enum u_desktop_result
gnome_portal_motion(struct gnome_portal *p, int32_t x, int32_t y, char *msg);

//! @p evdev_button: BTN_LEFT / BTN_RIGHT / BTN_MIDDLE.
enum u_desktop_result
gnome_portal_button(struct gnome_portal *p, uint32_t evdev_button, bool down, char *msg);

//! Notches, +x right, +y down.
enum u_desktop_result
gnome_portal_scroll(struct gnome_portal *p, int32_t dx, int32_t dy, char *msg);

#define GNOME_PORTAL_MAX_KEYS 16

//! A key press/release sequence of evdev codes, sent in order with a short gap.
enum u_desktop_result
gnome_portal_keys(struct gnome_portal *p, const uint32_t *codes, const bool *press, uint32_t n, char *msg);

/*!
 * Press @p mods (enum u_desktop_mod bits) plus one key, release in reverse.
 * @p ch != 0: the key producing that character on the session keymap's ACTIVE
 * layout (libei's keymap through xkbcommon; Shift added when the character is on
 * the Shift level; UNSUPPORTED when no key produces it without AltGr). @p ch == 0:
 * the fixed evdev @p evdev_code (named keys), 0 = modifiers only.
 */
enum u_desktop_result
gnome_portal_chord(struct gnome_portal *p, uint32_t mods, uint32_t evdev_code, uint32_t ch, char *msg);

/*!
 * Put @p utf8 on the clipboard through the portal session, paste it with
 * Ctrl+V (Ctrl+Shift+V when @p terminal), serve the paste, then offer the
 * previous clipboard text again (best effort — see the file's header).
 */
enum u_desktop_result
gnome_portal_paste_text(struct gnome_portal *p, const char *utf8, bool terminal, char *msg);

/*
 * AT-SPI (u_desktop_gnome_atspi.c)
 */

struct gnome_atspi;

//! NULL when the session has no accessibility bus (org.a11y.Bus).
struct gnome_atspi *
gnome_atspi_create(DBusConnection *session_bus);

void
gnome_atspi_destroy(struct gnome_atspi *a);

/*!
 * Text of the deepest accessible at window coordinates (@p wx, @p wy) — relative
 * to the window's frame — of the window of process @p pid titled @p title.
 * Bounded by @p budget_ms. "" (and OK) when nothing is there.
 */
enum u_desktop_result
gnome_atspi_text_at(struct gnome_atspi *a,
                    uint32_t pid,
                    const char *title,
                    int32_t wx,
                    int32_t wy,
                    uint32_t budget_ms,
                    char *buf,
                    size_t cap,
                    char *msg);

/*
 * The backend object and what its files share (u_desktop_gnome.c,
 * u_desktop_gnome_windows.c)
 */

#define WLG_BUS "org.displayxr.WindowGeometry"
#define WLP_PATH "/org/displayxr/WindowPlacement"
#define WLP_IFACE "org.displayxr.WindowPlacement1"
#define GNOME_OBS_TIMEOUT_MS 250
//! How close the pointer must land.
#define POINTER_TOLERANCE_PX 2
//! mutter's Super+click grab: keys sent before it ends are lost.
#define GRAB_SETTLE_MS 150

#include <pthread.h>

struct gnome_backend
{
	struct u_desktop_backend base;

	pthread_mutex_t obs_lock;
	DBusConnection *obs; //!< private; observation + own-window placement only

	bool have_ext16;
	struct gnome_portal *portal; //!< NULL: no RemoteDesktop portal (no consent path)
	struct gnome_atspi *atspi;   //!< NULL: no a11y bus
	bool warned_schema;
};

static inline struct gnome_backend *
gnome(struct u_desktop_backend *b)
{
	return (struct gnome_backend *)b;
}

void
gnome_set_msg(char *msg, const char *fmt, ...);

//! Every window, top-most first (malloc'd, caller frees).
enum u_desktop_result
gnome_snapshot(struct gnome_backend *g, struct u_desktop_window **out, uint32_t *n, char *msg);

const struct u_desktop_window *
gnome_find_window(const struct u_desktop_window *ws, uint32_t n, uint64_t id);

enum u_desktop_result
gnome_pick_msg(struct gnome_backend *g, int32_t x, int32_t y, struct u_desktop_pick *out, char *msg);

enum u_desktop_result
gnome_get_pointer_msg(struct gnome_backend *g, int32_t *x, int32_t *y, char *msg);

enum u_desktop_result
gnome_focused_window(struct u_desktop_backend *b, uint64_t *out_id);

//! Move, then confirm with the extension that the pointer arrived (within 2 px).
enum u_desktop_result
gnome_move_verified(struct gnome_backend *g, int32_t x, int32_t y, char *msg);

bool
gnome_can_input(struct gnome_backend *g, char *msg);

//! u_desktop_gnome_windows.c — the vtable's window_op.
enum u_desktop_result
gnome_window_op(struct u_desktop_backend *b,
                enum u_desktop_window_op op,
                uint64_t window_id,
                const struct u_desktop_rect *rect,
                char *msg);

#ifdef __cplusplus
}
#endif
