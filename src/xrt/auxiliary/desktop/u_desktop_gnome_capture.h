// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME screen capture (XR_DXR_desktop_control spec §10.5, ADR-056
 *         Amendment 1 D9): the portal side (ScreenCast sources in the
 *         RemoteDesktop session, or a standalone ScreenCast session for
 *         CAPTURE without CONTROL), the scopes' consent, and the backend's
 *         capture / monitors / capture_access_status / request_scopes ops.
 *
 * ## Consent shapes (lessons from the GNOME 50.1 prototype)
 *
 * xdg-desktop-portal-gnome restores EXACTLY the shape a restore token was issued
 * for: an input-only RemoteDesktop token restores without screens, forever. So:
 *  - a request that adds CAPTURE to a granted CONTROL opens a NEW combined
 *    session interactively (no restore token: a token cannot be widened) — one
 *    dialog, which asks about input again (spec §10.2);
 *  - a session is never downgraded: once the token covers the screens, every
 *    later restore (an input action included) selects the screens again, and once
 *    it covers input, a CAPTURE upgrade asks for input too;
 *  - the Start response decides each scope: `devices` non-zero ⇒ CONTROL,
 *    `streams` non-empty ⇒ CAPTURE (the user can untick remote interaction, or
 *    share only some monitors);
 *  - CAPTURE alone (no CONTROL) is a standalone ScreenCast session, persist_mode
 *    on SelectSources (decision 1); in a combined session persistence stays on
 *    SelectDevices, as the portal spec requires.
 * The token stays in memory only, as in v1.
 *
 * ## Frames
 *
 * OpenPipeWireRemote after Start; per capture, one fresh pw_stream per monitor
 * the region touches (u_desktop_gnome_pipewire.c), the first frame awaited (2 s).
 * The image is the region in stage px × the highest scale among those monitors
 * (as GNOME's own screenshots); streams at a lower density are scaled up to it,
 * monitors the user did not share stay withheld. Every window the DisplayXR GNOME
 * extension lists as `capture_excluded` is withheld over its frame and buffer
 * rects; without the extension's answer the capture is refused.
 */

#pragma once

#include "u_desktop.h"
#include "u_desktop_gnome_pipewire.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GP_SC_IFACE "org.freedesktop.portal.ScreenCast"
#define GP_SC_SOURCE_MONITOR 1u
#define GP_SC_CURSOR_HIDDEN 1u
#define GP_SC_CURSOR_EMBEDDED 2u

struct gnome_portal;
struct gnome_backend;

//! Capture state inside struct gnome_portal (u_desktop_gnome_portal_priv.h).
struct gp_cap
{
	// ---- immutable after gnome_capture_init ----
	bool pipewire; //!< libpipewire-0.3 loads: frames can be grabbed in this process

	// ---- the ScreenCast portal, probed on the portal thread, RETRIED until it
	// ---- answers (a slow portal at login must not be cached as "none") ----
	bool sc_known; //!< a probe answered
	bool sc_ok;    //!< ScreenCast v4+ records monitors
	uint32_t sc_version;
	uint32_t cursor_mode; //!< HIDDEN if offered, else EMBEDDED, else 0 (not sent)
	uint64_t sc_next_probe_ms;

	// ---- monitors the screen grant covers (connectors) and layout changes ----
	char covered[8][32];
	uint32_t ncovered;
	uint32_t dc_serial;     //!< DisplayConfig serial last seen
	uint64_t dc_changed_ms; //!< when it last changed
	bool reask;             //!< the pending request asks again even if granted

	// ---- shared, under gnome_portal::lock ----
	bool token_sc;                //!< the restore token is a standalone ScreenCast session's
	bool token_devices;           //!< the token's grant covers input (RemoteDesktop tokens)
	bool token_screens;           //!< the token's grant covers monitors
	bool open_streams;            //!< the open session has monitor streams
	bool open_devices;            //!< the open session has input devices
	bool pending;                 //!< a CAPTURE request is in flight
	enum u_desktop_access access; //!< CAPTURE's last outcome (when no token / session says more)
	int handoff_fd;               //!< prepare → caller: a dup of the PipeWire remote (-1: none)
	struct gnome_pw_stream handoff[8];
	uint32_t handoff_n;

	// ---- portal thread only ----
	bool want_sources;     //!< the next interactive RemoteDesktop open asks for monitors
	bool sources_selected; //!< SelectSources was sent on the open session
	bool session_sc;       //!< the open session is a standalone ScreenCast one
	bool resp_has_devices;
	uint32_t resp_devices;
	struct gnome_pw_stream resp_streams[8];
	uint32_t resp_nstreams;
	struct gnome_pw_stream streams[8]; //!< the open session's monitor streams
	uint32_t nstreams;
	int pw_fd; //!< OpenPipeWireRemote of the open session (-1: none)
};

/*
 * Hooks called from u_desktop_gnome_portal.c (portal thread unless noted).
 */

//! What gp_cap_begin_upgrade set aside: the grant before an interactive re-ask.
struct gp_cap_saved
{
	bool have_token;
	char token[1024];
	bool token_sc, token_devices, token_screens;
};

/*!
 * Before an interactive RemoteDesktop open that must replace the current grant
 * (a token cannot be widened): set the token aside (it is not passed, so not
 * consumed) and ask for the screens if the grant covered them (never downgrade).
 */
void
gp_cap_begin_upgrade(struct gnome_portal *p, struct gp_cap_saved *saved);

//! After it: on failure the set-aside grant is put back. Wipes @p saved.
void
gp_cap_end_upgrade(struct gnome_portal *p, struct gp_cap_saved *saved, bool ok);

/*!
 * open_session, before a silent restore: when the grant covers screens, wait
 * (bounded) for the monitor layout to be stable 2 s and for every monitor the
 * grant covers to be present — restoring with one missing (an HDMI handshake
 * after boot) gives a session without it and GNOME saves that reduced approval.
 * TIMEOUT when it does not settle (nothing is restored, the grant is kept).
 */
enum u_desktop_result
gp_cap_before_restore(struct gnome_portal *p, char *msg);

//! A restore that is not silent shows GNOME's dialog: close the pending Request
//! object (closing the session alone leaves the dialog up).
void
gp_cap_close_request(struct gnome_portal *p);

//! gnome_portal_create: zero state, no fds (any thread, before the thread starts).
void
gp_cap_construct(struct gnome_portal *p);

//! expect_response: reset the per-response fields.
void
gp_cap_expect(struct gnome_portal *p);

//! parse_results: a Start result key this file owns ("devices", "streams"). True if taken.
bool
gp_cap_parse_result(struct gnome_portal *p, const char *key, void *variant_iter);

/*!
 * open_session, after SelectDevices: add ScreenCast.SelectSources to the
 * RemoteDesktop session when the grant should cover the screens (no persist
 * keys: persistence stays on SelectDevices for a combined session).
 */
enum u_desktop_result
gp_cap_select_sources(struct gnome_portal *p, bool interactive, char *msg);

/*!
 * open_session, after a successful Start, before the token is stored: record the
 * grant's shape (under the lock, with the token) and open the PipeWire remote.
 * Returns false when the user unticked remote interaction (no devices): the
 * caller then must not connect libei.
 */
bool
gp_cap_after_start(struct gnome_portal *p);

//! gp_session_forget: close the PipeWire remote, forget the streams (takes the lock).
void
gp_cap_forget(struct gnome_portal *p);

//! A revocation (the user stopped the session, a restore was not silent). Lock held.
void
gp_cap_revoked_locked(struct gnome_portal *p);

//! True when the CONTROL grant exists: an input-covering token or open session. Lock held.
bool
gp_cap_control_granted_locked(struct gnome_portal *p);

/*
 * The backend side (u_desktop_gnome.c calls this once, in create_platform).
 */

//! Set the capture ops on @p g's vtable when capture is available; returns the CAPTURE cap bit or 0.
uint32_t
gnome_capture_init(struct gnome_backend *g);

#ifdef __cplusplus
}
#endif
