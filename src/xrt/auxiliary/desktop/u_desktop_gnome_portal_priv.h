// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME desktop control: the portal thread's state, shared between the
 *         session file (u_desktop_gnome_portal.c) and the input file
 *         (u_desktop_gnome_input.c). Everything not under gnome_portal::lock is
 *         touched by the portal thread only.
 */

#pragma once

#include "u_desktop_gnome.h"
#include "u_desktop_gnome_capture.h"
#include "u_desktop_gnome_grant.h"
#include "u_desktop_gnome_libs.h"
#include "u_desktop_policy.h"

#include <dbus/dbus.h>
#include <pthread.h>

#define PORTAL_DEST "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define RD_IFACE "org.freedesktop.portal.RemoteDesktop"
#define CLIP_IFACE "org.freedesktop.portal.Clipboard"
//! Bounded so destroy() can tear the thread down within ~2 s.
#define CALL_TIMEOUT_MS 1500
//! A silent restore that has not answered by then is a dialog nobody is answering.
#define RESTORE_START_TIMEOUT_MS 5000
//! A restore (Start answered silently) plus ConnectToEIS fits in this.
#define ACTION_TIMEOUT_MS (RESTORE_START_TIMEOUT_MS + 6000)
//! After prepare_input, keep the session open this long (verification + the
//! common layer's idle wait, at most 10 s, run before the injection). The first
//! injection ends the hold: from then on the session closes after
//! SESSION_IDLE_CLOSE_MS (2 s) without input, taking the indicator with it.
#define GNOME_PREPARE_HOLD_MS 12000

struct gp_cmd;
typedef enum u_desktop_result (*gp_cmd_fn)(struct gnome_portal *p, struct gp_cmd *c);

struct gp_cmd
{
	gp_cmd_fn fn;
	struct gp_cmd *next;

	// Arguments.
	int32_t x, y;
	uint32_t code;
	bool down;
	uint32_t keys[GNOME_PORTAL_MAX_KEYS];
	bool press[GNOME_PORTAL_MAX_KEYS];
	uint32_t nkeys;
	uint32_t mods; //!< chord: enum u_desktop_mod bits
	uint32_t ch;   //!< chord: a character to find on the active layout (0: use code)
	char *text;    // owned
	bool terminal;
	//! May (re)open the session (silent restore). Only prepare_input and pointer
	//! motion do: an injecting command after verification must not spend seconds
	//! restoring between the check and the injection — it refuses instead.
	bool restore;
	uint32_t out_mods, out_code; //!< resolved chord
	struct u_desktop_rect mon[GNOME_MAX_MONITORS]; //!< monitors: the pointer's regions
	uint32_t nmon;
	struct u_desktop_paste_obs obs; //!< clip_take / clip_paste: what the paste saw
	u_desktop_access_done_fn done;
	void *done_ctx;
	//! request / request_scopes: the consent dialog's parent window ("" = none).
	char parent_window[U_DESKTOP_PARENT_WINDOW_SIZE];

	// Result.
	enum u_desktop_result result;
	char msg[U_DESKTOP_MSG_SIZE];

	// Lifetime (under gnome_portal::lock).
	bool async;     //!< nobody waits: the thread frees it
	bool started;   //!< the thread took it
	bool finished;  //!< result is valid
	bool abandoned; //!< the waiter timed out: the thread frees it
};

#define GP_MAX_DEVICES 8

struct gnome_portal
{
	// ---- shared, under lock ----
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct gp_cmd *head, *tail;
	bool thread_started;
	bool quit;
	enum u_desktop_access access; //!< last outcome (GRANTED / DENIED / UNKNOWN)
	bool pending;                 //!< an interactive request is in flight
	bool session_open_shared;
	bool have_token;
	//! The restore token. Saved for the next launch only through gp_grant_* (spec
	//! §6.4: per app, a private file); never logged.
	char token[U_DESKTOP_GNOME_TOKEN_SIZE];
	bool consent_only_granted; //!< !input: the user approved the dialog in this process

	// ---- immutable ----
	uint32_t rd_version;
	bool have_clipboard_portal;
	//! libei + RemoteDesktop >= 2. Without it the portal is consent-only: the
	//! dialog is the OS consent for observation, then the session is closed.
	bool input;
	pthread_t thread;
	int wake[2];

	// ---- thread only ----
	DBusConnection *c;
	char sender[128]; //!< unique name, ':' dropped and '.' → '_' (request paths)
	bool registered_tried;
	//! The app id org.freedesktop.host.portal.Registry accepted ("" = none: no app
	//! id, or a portal < 1.19). The portal binds restore tokens to it.
	char registered_id[256];
	uint32_t token_counter;
	bool warned_no_token;

	//! The saved grant (spec §6.4, u_desktop_gnome_grant.h): what this process
	//! knows of the file. Its own token is p->token / p->cap.token_* as always.
	struct
	{
		bool inited; //!< the first request / restore looked for it
		//! The app's key (u_desktop_gnome_grant_key): its registered app id, else its
		//! own app id, else its executable. "" = nothing is saved.
		char key[U_DESKTOP_GRANT_KEY_MAX + 1];
		enum u_desktop_gnome_key_source source;
		char exe[1024];  //!< this process's executable (diagnostics in the file)
		char scope[256]; //!< its systemd unit (diagnostics: how an unregistered app is attributed)
		bool on_disk;    //!< the file holds a grant this process loaded or wrote
		//! The grant in memory came from the file and no Start has used it in this
		//! process yet: a request restores it with a dialog allowed (GP_OPEN_RESTORE_OR_ASK),
		//! since the portal honours it only in the launch context it was issued for.
		bool unverified;
		//! That file was removed by someone else (the kill switch, the user) while
		//! this process held its grant: the grant lives on in memory until the
		//! process exits, but this process never writes the file again.
		bool disowned;
		char token[U_DESKTOP_GNOME_TOKEN_SIZE]; //!< the token in that file
	} saved;
	//! The last Start (RemoteDesktop or ScreenCast) was answered "Cancel" (response 1).
	bool start_declined;

	char session[512];
	bool session_open;
	//! The last request's consent-dialog parent window (xdg-desktop-portal form, ""
	//! = none): the parent_window argument of every Start — RemoteDesktop and
	//! ScreenCast, a silent restore included (it may still turn into a dialog).
	char parent_window[U_DESKTOP_PARENT_WINDOW_SIZE];
	bool clipboard_enabled;

	char wait_path[512];
	bool resp_done;
	uint32_t resp_code;
	char resp_session[512];
	char resp_token[1024];
	bool resp_has_token;
	bool resp_clip;

	char *clip_text;       //!< what we serve on SelectionTransfer
	bool clip_owner;       //!< SelectionOwnerChanged (from the portal) said session_is_owner
	bool clip_is_paste;    //!< clip_text is the agent's text (not the restored previous one)
	//! While serving the agent's text, the portal said another client took the
	//! selection (the user copied): the paste must not happen / its outcome is
	//! unknown, and the previous text must NOT be offered again over the user's.
	bool clip_lost;
	char *clip_prev;       //!< the clipboard text before the paste, to offer again
	//! SelectionOwnerChanged signals saying another client owns the selection
	//! (the user copied), counted whatever we held — so a copy between reading
	//! the previous text and our SetSelection is seen too (clip_lost needs us to
	//! have been the owner already).
	uint32_t clip_foreign_gen;
	//! clip_foreign_gen when the type started (before SelectionRead): any change
	//! since = the user copied; never paste over it / offer the old text over it.
	uint32_t clip_take_gen;
	bool paste_armed;      //!< the paste keystroke was sent: transfers now count
	uint32_t paste_writes; //!< successful writes of the agent's text since armed
	bool paste_lost_before; //!< the last paste: the selection changed hands before its keystroke
	bool paste_lost_after;  //!< ... after it
	uint32_t paste_last_serial;
	bool paste_have_serial;

	//! Unique bus name of the portal, from its first reply: Response / Closed /
	//! Clipboard signals from anyone else are ignored (no forged grant).
	char portal_owner[128];
	//! prepare_input keeps the session open at least until then (the common
	//! layer's verification and idle wait run between prepare and injection).
	uint64_t hold_until_ms;
	//! When a keyboard device was last resumed (gdbus_now_ms); 0 = none yet. See GNOME_KEYBOARD_SETTLE_MS.
	uint64_t kbd_resumed_ms;

	struct ei *ei;
	bool ei_dead;
	struct
	{
		struct ei_device *dev;
		bool resumed;
	} devs[GP_MAX_DEVICES];

	//! Screen capture (u_desktop_gnome_capture.c): the session's ScreenCast side.
	struct gp_cap cap;
	uint32_t seq;
	uint64_t last_use_ms;

	//! Spec v3: keys and buttons libei has pressed and not released (evdev codes):
	//! a held key keeps the session open, and is released before the session closes.
	uint32_t pressed_keys[GNOME_PORTAL_MAX_KEYS];
	uint32_t npressed_keys;
	uint32_t pressed_buttons; //!< bit (code - BTN_LEFT) per held BTN_LEFT / BTN_RIGHT / BTN_MIDDLE

	//! The keyboard device's keymap, compiled (lazily) for char → key lookups.
	struct ei_device *xkb_dev;
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_km;
	bool xkb_tried;
	uint32_t xkb_group; //!< active layout, from EI_EVENT_KEYBOARD_MODIFIERS
};


//! A method call to the portal: (o session)? (s str)? a{sv} options filled by the caller.
struct pmsg
{
	DBusMessage *m;
	DBusMessageIter it;
	DBusMessageIter dict;
};

void
gp_set_msg(char *msg, const char *fmt, ...);

void
gp_pmsg_begin(struct pmsg *pm, const char *iface, const char *method, const char *session, const char *str);

//! Send; returns the reply (caller unrefs) or NULL with @p err filled.
DBusMessage *
gp_pmsg_send(struct gnome_portal *p, struct pmsg *pm, int timeout_ms, struct gdbus_err *err);

void
gp_dict_add_strv(DBusMessageIter *dict, const char *key, const char *const *vals, int n);

//! A resumed device with @p cap; with @p prefer_abs, one that is also absolute first.
struct ei_device *
gp_find_dev(struct gnome_portal *p, int cap, bool prefer_abs);

//! Wait up to @p timeout_ms for anything (a command, a D-Bus message, libei) and handle it.
void
gp_pump(struct gnome_portal *p, int timeout_ms);

//! The session is open with libei connected — restoring it silently if needed.
enum u_desktop_result
gp_ensure_session(struct gnome_portal *p, bool restore, char *msg);

void
gp_session_forget(struct gnome_portal *p);

struct gp_cmd *
gp_cmd_new(gp_cmd_fn fn);

void
gp_cmd_free(struct gp_cmd *c);

/*!
 * Run @p c on the thread. Waits at most @p timeout_ms for it to START (a queued
 * command is dropped with TIMEOUT); once running it is always waited for (every
 * command is bounded), so no input is in flight after the call returns. Takes
 * ownership of @p c.
 */
enum u_desktop_result
gp_run_sync(struct gnome_portal *p, struct gp_cmd *c, uint32_t timeout_ms, char *msg);

//! gp_run_sync, copying the finished command into @p out (may be NULL) first.
enum u_desktop_result
gp_run_sync_keep(struct gnome_portal *p, struct gp_cmd *c, uint32_t timeout_ms, char *msg, struct gp_cmd *out);

//! destroy() has begun: loops on the thread stop waiting.
bool
gp_quitting(struct gnome_portal *p);


//! u_desktop_gnome_input.c: answer a SelectionTransfer (the paste asking for our text).
void
gp_serve_transfer(struct gnome_portal *p, const char *mime, uint32_t serial);

/*
 * Session plumbing shared with u_desktop_gnome_capture.c (portal thread only).
 */

//! How a session is opened.
enum gp_open
{
	//! A request's dialog. No token is passed (a token cannot be widened); a timeout
	//! or a Cancel leaves the grant set aside by gp_cap_begin_upgrade to the caller.
	GP_OPEN_ASK = 0,
	//! A silent restore with the token. A dialog (the portal no longer honours it) is
	//! closed at once and the grant revoked; a refusal revokes it.
	GP_OPEN_RESTORE,
	//! A request's restore of a SAVED grant: the token is passed, and when the portal
	//! does not honour it in this launch context its dialog is shown and is the answer
	//! (the new token replaces the saved one; a Cancel revokes and removes it).
	GP_OPEN_RESTORE_OR_ASK,
};

//! open_session with @p interactive ? GP_OPEN_ASK : GP_OPEN_RESTORE.
enum u_desktop_result
gp_open_session(struct gnome_portal *p, bool interactive, char *msg);

enum u_desktop_result
gp_open_session_mode(struct gnome_portal *p, enum gp_open mode, char *msg);

void
gp_session_close(struct gnome_portal *p);

void
gp_register_app(struct gnome_portal *p);

void
gp_next_token(struct gnome_portal *p, char *out, size_t cap);

void
gp_expect_response(struct gnome_portal *p, const char *token);

bool
gp_wait_response(struct gnome_portal *p, uint32_t timeout_ms);

void
gp_take_request_handle(struct gnome_portal *p, DBusMessage *reply);

void
gp_dict_add_str(DBusMessageIter *dict, const char *key, const char *val);

void
gp_dict_add_u32(DBusMessageIter *dict, const char *key, uint32_t val);

void
gp_dict_add_bool(DBusMessageIter *dict, const char *key, bool val);

//! Queue @p c (async: the thread frees it) and wake the thread. False when shutting down.
bool
gp_enqueue_async(struct gnome_portal *p, struct gp_cmd *c);

/*
 * The saved grant (spec §6.4). Portal thread only; file I/O never under p->lock.
 */

/*!
 * Before a request or a restore: the first call registers the app id and loads
 * the app's saved grant; later calls adopt one that another process of the same
 * app saved since (the portal may issue a new token per Start, which spends the
 * old one), and notice a file
 * removed under us (the kill switch).
 */
void
gp_grant_sync(struct gnome_portal *p);

//! After a successful Start: save the grant now held (or remove the file when the portal returned no token).
void
gp_grant_save(struct gnome_portal *p);

/*!
 * Remove the saved grant if it still holds @p token (NULL: whatever it holds);
 * @p why goes to the log. Call with the token the failed restore used.
 */
void
gp_grant_forget(struct gnome_portal *p, const char *token, const char *why);

/*!
 * A request found a saved grant this process has not used yet: restore it now
 * (GP_OPEN_RESTORE_OR_ASK, the RemoteDesktop or the ScreenCast portal as the token
 * says), so the answer is the portal's. Silent in the launch context the token was
 * issued for; elsewhere the dialog once. u_desktop_gnome_capture.c.
 */
enum u_desktop_result
gp_grant_verify(struct gnome_portal *p, char *msg);

//! u_desktop_gnome_input.c: drop the compiled keymap.
void
gp_xkb_forget(struct gnome_portal *p);

//! u_desktop_gnome_input.c: anything libei holds pressed (spec v3)?
bool
gp_anything_pressed(const struct gnome_portal *p);

/*!
 * u_desktop_gnome_input.c: release every key and button libei holds (spec v3), while
 * the EIS connection lives — before a session closes. Forgets them either way.
 */
void
gp_release_pressed(struct gnome_portal *p);
