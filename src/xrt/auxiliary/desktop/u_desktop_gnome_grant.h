// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: the portal grant saved across launches of the
 *         same app (XR_DXR_desktop_control spec §6.4, ADR-056 Amendment 2).
 *
 * The RemoteDesktop / ScreenCast portal hands out a restore token for a grant
 * the user approved (`persist_mode` 2). Passing it back on the next session
 * restores the grant without the dialog. The GNOME backend saves the token with
 * the grant's SHAPE — which portal issued it, whether it covers input and the
 * screens, and which monitors (connectors) the screens grant covered — so that a
 * new process of the same app restores exactly what was granted (never a
 * downgrade: xdg-desktop-portal-gnome saves the shape a restore asked for) and
 * can keep the monitor-blink guard (a covered monitor missing at restore time).
 *
 * One file per app (u_desktop_grant_*: private directory, 0600, atomic), keyed by
 * the app's own identity (u_desktop_gnome_grant_key): the app id the portal's host
 * registry accepted, else the app's own id (DXR_DESKTOP_APP_ID / the .desktop
 * basename) even unregistered, else its executable ("exe-<name>-<hash of the
 * absolute path>"). The portal stays the judge: it honours the token only for the
 * app id IT bound it to (the registered id, or one it derived from the launch
 * context — the process's systemd scope). Launched differently, the restore shows
 * the dialog once and the new token replaces the saved one.
 *
 *     { "version": 1, "key": "org.example.Assistant",
 *       "key_source": "registered" | "app_id" | "executable",
 *       "portal": "remote_desktop" | "screencast",
 *       "restore_token": "…", "devices": true, "screens": true,
 *       "monitors": ["DP-1", "HDMI-1"], "saved": "2026-10-21T05:12:00Z",
 *       "portal_app_id": "org.example.Assistant",           (registered only)
 *       "exe": "/opt/example/Assistant", "launch_scope": "app-gnome-org.example.Assistant-4242.scope" }
 *
 * The last three are diagnostics only (which id the portal used, if known; what
 * launched the process): never read back as part of the grant.
 *
 * Compiled on every OS so tests_desktop_control pins it; the GNOME backend is its
 * only caller. Never logs a token.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_DESKTOP_GNOME_GRANT_VERSION 1
//! Restore token buffer (terminator included) — gnome_portal::token's size.
#define U_DESKTOP_GNOME_TOKEN_SIZE 1024
#define U_DESKTOP_GNOME_GRANT_MONITORS 8
#define U_DESKTOP_GNOME_CONNECTOR_SIZE 32

struct u_desktop_gnome_grant
{
	char token[U_DESKTOP_GNOME_TOKEN_SIZE];
	bool screencast; //!< a standalone ScreenCast session's token (else RemoteDesktop)
	bool devices;    //!< the grant covers input (RemoteDesktop only)
	bool screens;    //!< the grant covers monitors
	//! The connectors the screens grant covered (the monitor-blink guard).
	char monitors[U_DESKTOP_GNOME_GRANT_MONITORS][U_DESKTOP_GNOME_CONNECTOR_SIZE];
	uint32_t nmonitors;
};

/*
 * The key: whose grant this is.
 */

enum u_desktop_gnome_key_source
{
	U_DESKTOP_GNOME_KEY_NONE = 0,   //!< nothing to key it by: not saved
	U_DESKTOP_GNOME_KEY_REGISTERED, //!< the app id org.freedesktop.host.portal.Registry accepted
	U_DESKTOP_GNOME_KEY_APP_ID,     //!< the app's own id (env / .desktop), not registered
	U_DESKTOP_GNOME_KEY_EXECUTABLE, //!< no usable app id: the executable's absolute path
};

//! "none", "registered", "app_id", "executable".
const char *
u_desktop_gnome_key_source_str(enum u_desktop_gnome_key_source s);

/*!
 * The saved grant's key for this process, first that applies:
 *  1. @p registered_id, the id the portal's registry accepted (may be NULL / "");
 *  2. @p app_id, the app's own id (DXR_DESKTOP_APP_ID, else the basename of
 *     GIO_LAUNCHED_DESKTOP_FILE) even when registration failed or was not tried;
 *  3. @p exe_path, an absolute path: "exe-<basename, [A-Za-z0-9._-] only, at most
 *     48 bytes>-<16 hex digits of FNV-1a 64 of the whole path>" — two executables
 *     with the same name in different places get different keys.
 * An id that is not a valid key (u_desktop_grant_key_valid) is skipped. Writes the
 * key to @p out (NUL-terminated; "" for NONE).
 */
enum u_desktop_gnome_key_source
u_desktop_gnome_grant_key(const char *registered_id, const char *app_id, const char *exe_path, char *out, size_t cap);

//! Diagnostics written next to a grant (all may be NULL / ""): never read back.
struct u_desktop_gnome_grant_origin
{
	enum u_desktop_gnome_key_source source;
	const char *portal_app_id; //!< the id the portal bound the token to, when known (registered)
	const char *exe;           //!< this process's executable
	const char *launch_scope;  //!< the systemd unit it runs in (how the portal attributes an unregistered app)
};

/*!
 * The systemd unit this process runs in (the last component of its cgroup v2 path,
 * e.g. "app-gnome-org.gnome.Ptyxis-1234.scope"), which is how xdg-desktop-portal
 * attributes an app that did not register. False when unknown (not Linux).
 */
bool
u_desktop_gnome_launch_scope(char *out, size_t cap);

enum u_desktop_gnome_grant_status
{
	U_DESKTOP_GNOME_GRANT_NONE = 0, //!< nothing saved for this key
	U_DESKTOP_GNOME_GRANT_OK,       //!< @p out holds the saved grant
	//! The file is not a grant this format describes (unparseable, another app id,
	//! a bad token or shape): ignored and REMOVED (a later grant writes a new one).
	U_DESKTOP_GNOME_GRANT_CORRUPT,
	//! A later format version (a newer runtime wrote it): ignored, left alone.
	U_DESKTOP_GNOME_GRANT_NEWER,
	//! Not a private file of this user (u_desktop_grant_read): ignored, left alone.
	U_DESKTOP_GNOME_GRANT_UNSAFE,
};

//! "none", "ok", "corrupt", "newer", "unsafe".
const char *
u_desktop_gnome_grant_status_str(enum u_desktop_gnome_grant_status s);

/*!
 * The file content for @p g under @p key (malloc'd, free() it — it holds the
 * token: the caller wipes it), or NULL when @p g is not saveable (no token, a token
 * that is not printable ASCII, a shape that covers nothing). @p origin may be NULL.
 */
char *
u_desktop_gnome_grant_encode(const char *key,
                             const struct u_desktop_gnome_grant *g,
                             const struct u_desktop_gnome_grant_origin *origin);

//! Parse @p json for @p key. OK, CORRUPT or NEWER; @p out is zeroed unless OK.
enum u_desktop_gnome_grant_status
u_desktop_gnome_grant_decode(const char *json, const char *key, struct u_desktop_gnome_grant *out);

//! Read @p key's saved grant (a CORRUPT file is removed).
enum u_desktop_gnome_grant_status
u_desktop_gnome_grant_load(const char *key, struct u_desktop_gnome_grant *out);

//! Save @p g under @p key, replacing what was there. False when not saveable / not written.
bool
u_desktop_gnome_grant_save(const char *key,
                           const struct u_desktop_gnome_grant *g,
                           const struct u_desktop_gnome_grant_origin *origin);

/*!
 * Remove @p key's saved grant — only while it still holds @p token (another
 * process of the same app may have saved a newer one since); NULL: whatever it
 * holds. A file that is not a readable grant (corrupt, unsafe, newer) is removed
 * only with NULL. True when a file was removed.
 */
bool
u_desktop_gnome_grant_forget(const char *key, const char *token);

/*
 * When the saved grant is kept, written or removed.
 */

enum u_desktop_gnome_grant_action
{
	U_DESKTOP_GNOME_GRANT_KEEP = 0, //!< leave the saved grant as it is
	U_DESKTOP_GNOME_GRANT_SAVE,     //!< save the new grant (its token replaces the old one)
	U_DESKTOP_GNOME_GRANT_FORGET,   //!< remove the saved grant (the next request asks again)
};

/*!
 * After an interactive request (the dialog):
 *
 * | Start             | token | ask_again | had a grant | action                              |
 * |-------------------|-------|-----------|-------------|-------------------------------------|
 * | granted           | yes   | any       | any         | SAVE                                |
 * | granted           | no    | any       | any         | FORGET: nothing restorable          |
 * | declined (Cancel) | -     | yes       | any         | FORGET: the answer replaces it (1)  |
 * | declined (Cancel) | -     | no        | no          | FORGET: the user said no            |
 * | declined (Cancel) | -     | no        | yes         | KEEP: an added scope declined (2)   |
 * | anything else     | -     | any       | any         | KEEP: no answer / a portal error (3)|
 *
 * (1) spec §10.2. (2) the old grant stays (gnome test plan 11.18). (3) the old grant,
 * if any, is put back. @p declined: the Start response was 1 (the user cancelled).
 */
enum u_desktop_gnome_grant_action
u_desktop_gnome_grant_after_request(bool granted, bool got_token, bool declined, bool ask_again, bool had_grant);

#ifdef __cplusplus
}
#endif
