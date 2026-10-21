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
 * One file per app id (u_desktop_grant_*: private directory, 0600, atomic):
 *
 *     { "version": 1, "app_id": "org.example.Assistant",
 *       "portal": "remote_desktop" | "screencast",
 *       "restore_token": "…", "devices": true, "screens": true,
 *       "monitors": ["DP-1", "HDMI-1"], "saved": "2026-10-21T05:12:00Z" }
 *
 * The key is the app id the portal accepted through
 * org.freedesktop.host.portal.Registry — the id the portal bound the token to.
 * Without one nothing is saved (see u_desktop_gnome_portal.c).
 *
 * Compiled on every OS so tests_desktop_control pins it; the GNOME backend is its
 * only caller. Never logs a token.
 */

#pragma once

#include <stdbool.h>
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

enum u_desktop_gnome_grant_status
{
	U_DESKTOP_GNOME_GRANT_NONE = 0, //!< nothing saved for this app id
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
 * The file content for @p g (malloc'd, free() it — it holds the token: the
 * caller wipes it), or NULL when @p g is not saveable (no token, a token that is
 * not printable ASCII, a shape that covers nothing).
 */
char *
u_desktop_gnome_grant_encode(const char *app_id, const struct u_desktop_gnome_grant *g);

//! Parse @p json for @p app_id. OK, CORRUPT or NEWER; @p out is zeroed unless OK.
enum u_desktop_gnome_grant_status
u_desktop_gnome_grant_decode(const char *json, const char *app_id, struct u_desktop_gnome_grant *out);

//! Read @p app_id's saved grant (a CORRUPT file is removed).
enum u_desktop_gnome_grant_status
u_desktop_gnome_grant_load(const char *app_id, struct u_desktop_gnome_grant *out);

//! Save @p g for @p app_id, replacing what was there. False when not saveable / not written.
bool
u_desktop_gnome_grant_save(const char *app_id, const struct u_desktop_gnome_grant *g);

/*!
 * Remove @p app_id's saved grant — only while it still holds @p token (another
 * process of the same app may have saved a newer one since); NULL: whatever it
 * holds. A file that is not a readable grant (corrupt, unsafe, newer) is removed
 * only with NULL. True when a file was removed.
 */
bool
u_desktop_gnome_grant_forget(const char *app_id, const char *token);

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
