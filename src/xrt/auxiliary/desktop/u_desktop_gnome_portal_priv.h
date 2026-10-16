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
#include "u_desktop_gnome_libs.h"

#include <dbus/dbus.h>
#include <pthread.h>

#define PORTAL_DEST "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define RD_IFACE "org.freedesktop.portal.RemoteDesktop"
#define CLIP_IFACE "org.freedesktop.portal.Clipboard"
#define CALL_TIMEOUT_MS 3000
//! A silent restore that has not answered by then is a dialog nobody is answering.
#define RESTORE_START_TIMEOUT_MS 15000
//! A restore (Start answered silently) plus ConnectToEIS fits in this.
#define ACTION_TIMEOUT_MS (RESTORE_START_TIMEOUT_MS + 6000)

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
	u_desktop_access_done_fn done;
	void *done_ctx;

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
	char token[1024];          //!< restore token — memory only, never written anywhere
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
	uint32_t token_counter;
	bool warned_no_token;

	char session[512];
	bool session_open;
	bool clipboard_enabled;

	char wait_path[512];
	bool resp_done;
	uint32_t resp_code;
	char resp_session[512];
	char resp_token[1024];
	bool resp_has_token;
	bool resp_clip;

	char *clip_text; //!< what we serve on SelectionTransfer
	bool clip_owner; //!< SelectionOwnerChanged said session_is_owner
	uint32_t transfers;

	struct ei *ei;
	bool ei_dead;
	struct
	{
		struct ei_device *dev;
		bool resumed;
	} devs[GP_MAX_DEVICES];
	uint32_t seq;
	uint64_t last_use_ms;

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
gp_ensure_session(struct gnome_portal *p, char *msg);

void
gp_session_forget(struct gnome_portal *p);

struct gp_cmd *
gp_cmd_new(gp_cmd_fn fn);

void
gp_cmd_free(struct gp_cmd *c);

//! Run @p c on the thread and wait at most @p timeout_ms. Takes ownership of @p c.
enum u_desktop_result
gp_run_sync(struct gnome_portal *p, struct gp_cmd *c, uint32_t timeout_ms, char *msg);

//! u_desktop_gnome_input.c: answer a SelectionTransfer (the paste asking for our text).
void
gp_serve_transfer(struct gnome_portal *p, const char *mime, uint32_t serial);

//! u_desktop_gnome_input.c: drop the compiled keymap.
void
gp_xkb_forget(struct gnome_portal *p);
