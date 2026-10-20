// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GNOME screen capture: ScreenCast in the portal session, consent per
 *         scope, frames through PipeWire. See u_desktop_gnome_capture.h.
 */

#include "u_desktop_gnome_capture.h"
#include "u_desktop_capture.h"
#include "u_desktop_gnome_portal_priv.h"

#include "os/os_time.h"
#include "util/u_json.h"
#include "util/u_logging.h"

#include <dbus/dbus.h>

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SC_PROPS_TIMEOUT_MS 3000
#define SC_PROBE_RETRY_MS 10000
#define INTERACTIVE_TIMEOUT_MS (10 * 60 * 1000)
#define RESTORE_TIMEOUT_MS 5000
//! A layout change this recent: do not restore yet (spec §10.5, the DS1's HDMI blink).
#define LAYOUT_SETTLE_MS 2000
//! How long a restore waits for the layout to settle / a covered monitor to return.
#define LAYOUT_WAIT_MAX_MS 6000
//! The first frame of a fresh stream (spec §10.3).
#define FRAME_TIMEOUT_MS 2000
//! prepare → grab: keep the session open across the grab.
#define CAPTURE_HOLD_MS 4000

#define WLG_PATH_ "/org/displayxr/WindowGeometry"
#define WLG_IFACE_ "org.displayxr.WindowGeometry1"
#define DC_DEST "org.gnome.Mutter.DisplayConfig"
#define DC_PATH "/org/gnome/Mutter/DisplayConfig"

#define MAX_STREAMS 8

/*
 *
 * mutter's DisplayConfig: the monitors, in stage coordinates.
 *
 */

struct dc_monitor
{
	char conn[32];
	struct u_desktop_rect rect;
};

struct dc_state
{
	uint32_t serial;
	uint32_t n;
	struct dc_monitor m[8];
};

struct dc_mode
{
	char conn[32];
	int32_t w, h;
};

static bool
iter_is(DBusMessageIter *it, int type)
{
	return dbus_message_iter_get_arg_type(it) == type;
}

//! a{sv}: the bool / uint32 under @p key. Returns true if found.
static bool
dict_find(DBusMessageIter *arr, const char *key, int type, void *out)
{
	DBusMessageIter d;
	dbus_message_iter_recurse(arr, &d);
	while (iter_is(&d, DBUS_TYPE_DICT_ENTRY)) {
		DBusMessageIter e, v;
		const char *k = NULL;
		dbus_message_iter_recurse(&d, &e);
		dbus_message_iter_get_basic(&e, &k);
		if (k != NULL && strcmp(k, key) == 0 && dbus_message_iter_next(&e) && iter_is(&e, DBUS_TYPE_VARIANT)) {
			dbus_message_iter_recurse(&e, &v);
			if (iter_is(&v, type)) {
				dbus_message_iter_get_basic(&v, out);
				return true;
			}
		}
		dbus_message_iter_next(&d);
	}
	return false;
}

/*!
 * GetCurrentState → the logical monitors (stage px: logical px in the logical
 * layout mode, device px in the physical one). @p c must be locked by the caller
 * if shared.
 */
static bool
dc_read(DBusConnection *c, struct dc_state *out)
{
	memset(out, 0, sizeof(*out));
	DBusMessage *reply =
	    gdbus_call(c, DC_DEST, DC_PATH, DC_DEST, "GetCurrentState", 1500, NULL, DBUS_TYPE_INVALID);
	if (reply == NULL) {
		return false;
	}
	bool ok = false;
	struct dc_mode modes[16];
	uint32_t nmodes = 0;
	struct
	{
		int32_t x, y;
		double scale;
		uint32_t transform;
		char conn[32];
	} logical[8];
	uint32_t nlog = 0;
	uint32_t layout_mode = 1;

	DBusMessageIter it;
	if (!dbus_message_iter_init(reply, &it) || !iter_is(&it, DBUS_TYPE_UINT32)) {
		goto done;
	}
	dbus_message_iter_get_basic(&it, &out->serial);

	// a((ssss)a(siiddada{sv})a{sv}) — the physical monitors and their current mode.
	if (!dbus_message_iter_next(&it) || !iter_is(&it, DBUS_TYPE_ARRAY)) {
		goto done;
	}
	DBusMessageIter mons;
	dbus_message_iter_recurse(&it, &mons);
	while (iter_is(&mons, DBUS_TYPE_STRUCT) && nmodes < 16) {
		DBusMessageIter mon, spec;
		dbus_message_iter_recurse(&mons, &mon);
		const char *conn = NULL;
		if (iter_is(&mon, DBUS_TYPE_STRUCT)) {
			dbus_message_iter_recurse(&mon, &spec);
			if (iter_is(&spec, DBUS_TYPE_STRING)) {
				dbus_message_iter_get_basic(&spec, &conn);
			}
		}
		if (conn != NULL && dbus_message_iter_next(&mon) && iter_is(&mon, DBUS_TYPE_ARRAY)) {
			DBusMessageIter ml;
			dbus_message_iter_recurse(&mon, &ml);
			while (iter_is(&ml, DBUS_TYPE_STRUCT)) {
				DBusMessageIter mode;
				dbus_message_iter_recurse(&ml, &mode);
				dbus_int32_t w = 0, h = 0;
				// s id, i w, i h, d refresh, d preferred scale, ad scales, a{sv} props
				dbus_message_iter_next(&mode);
				if (iter_is(&mode, DBUS_TYPE_INT32)) {
					dbus_message_iter_get_basic(&mode, &w);
				}
				dbus_message_iter_next(&mode);
				if (iter_is(&mode, DBUS_TYPE_INT32)) {
					dbus_message_iter_get_basic(&mode, &h);
				}
				for (int k = 0; k < 4; k++) {
					dbus_message_iter_next(&mode);
				}
				dbus_bool_t current = FALSE;
				if (iter_is(&mode, DBUS_TYPE_ARRAY)) {
					dict_find(&mode, "is-current", DBUS_TYPE_BOOLEAN, &current);
				}
				if (current) {
					snprintf(modes[nmodes].conn, sizeof(modes[nmodes].conn), "%s", conn);
					modes[nmodes].w = w;
					modes[nmodes].h = h;
					nmodes++;
					break;
				}
				dbus_message_iter_next(&ml);
			}
		}
		dbus_message_iter_next(&mons);
	}

	// a(iiduba(ssss)a{sv}) — the logical monitors.
	if (!dbus_message_iter_next(&it) || !iter_is(&it, DBUS_TYPE_ARRAY)) {
		goto done;
	}
	DBusMessageIter lms;
	dbus_message_iter_recurse(&it, &lms);
	while (iter_is(&lms, DBUS_TYPE_STRUCT) && nlog < 8) {
		DBusMessageIter lm;
		dbus_message_iter_recurse(&lms, &lm);
		dbus_int32_t x = 0, y = 0;
		double scale = 1.0;
		dbus_uint32_t transform = 0;
		dbus_message_iter_get_basic(&lm, &x);
		dbus_message_iter_next(&lm);
		dbus_message_iter_get_basic(&lm, &y);
		dbus_message_iter_next(&lm);
		if (iter_is(&lm, DBUS_TYPE_DOUBLE)) {
			dbus_message_iter_get_basic(&lm, &scale);
		}
		dbus_message_iter_next(&lm);
		if (iter_is(&lm, DBUS_TYPE_UINT32)) {
			dbus_message_iter_get_basic(&lm, &transform);
		}
		dbus_message_iter_next(&lm); // b primary
		dbus_message_iter_next(&lm); // a(ssss)
		const char *conn = NULL;
		if (iter_is(&lm, DBUS_TYPE_ARRAY)) {
			DBusMessageIter specs, spec;
			dbus_message_iter_recurse(&lm, &specs);
			if (iter_is(&specs, DBUS_TYPE_STRUCT)) {
				dbus_message_iter_recurse(&specs, &spec);
				if (iter_is(&spec, DBUS_TYPE_STRING)) {
					dbus_message_iter_get_basic(&spec, &conn);
				}
			}
		}
		logical[nlog].x = x;
		logical[nlog].y = y;
		logical[nlog].scale = scale > 0 ? scale : 1.0;
		logical[nlog].transform = transform;
		snprintf(logical[nlog].conn, sizeof(logical[nlog].conn), "%s", conn != NULL ? conn : "");
		nlog++;
		dbus_message_iter_next(&lms);
	}

	// a{sv} properties: "layout-mode" 1 logical, 2 physical.
	if (dbus_message_iter_next(&it) && iter_is(&it, DBUS_TYPE_ARRAY)) {
		dict_find(&it, "layout-mode", DBUS_TYPE_UINT32, &layout_mode);
	}

	for (uint32_t i = 0; i < nlog; i++) {
		int32_t w = 0, h = 0;
		for (uint32_t k = 0; k < nmodes; k++) {
			if (strcmp(modes[k].conn, logical[i].conn) == 0) {
				w = modes[k].w;
				h = modes[k].h;
			}
		}
		if (w <= 0 || h <= 0) {
			continue;
		}
		if (logical[i].transform % 2 == 1) { // 90 / 270 (and their flipped forms)
			int32_t t = w;
			w = h;
			h = t;
		}
		if (layout_mode != 2) {
			w = (int32_t)lround(w / logical[i].scale);
			h = (int32_t)lround(h / logical[i].scale);
		}
		struct dc_monitor *m = &out->m[out->n++];
		snprintf(m->conn, sizeof(m->conn), "%s", logical[i].conn);
		m->rect = (struct u_desktop_rect){logical[i].x, logical[i].y, w, h};
	}
	ok = out->n > 0;
done:
	dbus_message_unref(reply);
	return ok;
}

static bool
rect_close(const struct u_desktop_rect *a, const struct u_desktop_rect *b)
{
	return abs(a->x - b->x) <= 2 && abs(a->y - b->y) <= 2 && abs(a->w - b->w) <= 2 && abs(a->h - b->h) <= 2;
}


/*
 *
 * The ScreenCast portal probe — retried until it answers.
 *
 */

static bool
sc_probe(struct gnome_portal *p)
{
	struct gp_cap *cp = &p->cap;
	if (cp->sc_known) {
		return cp->sc_ok;
	}
	if (p->c == NULL || gdbus_now_ms() < cp->sc_next_probe_ms) {
		return false;
	}
	const char *iface = GP_SC_IFACE;
	struct gdbus_err err;
	DBusMessage *reply = gdbus_call(p->c, PORTAL_DEST, PORTAL_PATH, "org.freedesktop.DBus.Properties", "GetAll",
	                                SC_PROPS_TIMEOUT_MS, &err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
	if (reply == NULL) {
		// Not cached: a portal still starting at login answers later.
		cp->sc_next_probe_ms = gdbus_now_ms() + SC_PROBE_RETRY_MS;
		U_LOG_W("desktop/gnome: the ScreenCast portal did not answer (%s) — retrying in %u s", err.name,
		        SC_PROBE_RETRY_MS / 1000);
		return false;
	}
	DBusMessageIter it;
	uint32_t version = 0, types = 0, cursors = 0;
	if (dbus_message_iter_init(reply, &it) && iter_is(&it, DBUS_TYPE_ARRAY)) {
		dict_find(&it, "version", DBUS_TYPE_UINT32, &version);
		dict_find(&it, "AvailableSourceTypes", DBUS_TYPE_UINT32, &types);
		dict_find(&it, "AvailableCursorModes", DBUS_TYPE_UINT32, &cursors);
	}
	dbus_message_unref(reply);
	cp->sc_known = true;
	cp->sc_version = version;
	// persist_mode on a standalone ScreenCast session needs v4.
	cp->sc_ok = version >= 4 && (types & GP_SC_SOURCE_MONITOR) != 0;
	// Hidden, as the Screenshot portal's pictures; a mode the portal does not offer
	// would fail the session.
	cp->cursor_mode = (cursors & GP_SC_CURSOR_HIDDEN) != 0     ? GP_SC_CURSOR_HIDDEN
	                  : (cursors & GP_SC_CURSOR_EMBEDDED) != 0 ? GP_SC_CURSOR_EMBEDDED
	                                                           : 0;
	U_LOG_I("desktop/gnome: ScreenCast portal v%u, sources 0x%x, cursor modes 0x%x%s", version, types, cursors,
	        cp->sc_ok ? "" : " — cannot record monitors");
	return cp->sc_ok;
}


/*
 *
 * Hooks from u_desktop_gnome_portal.c.
 *
 */

void
gp_cap_construct(struct gnome_portal *p)
{
	memset(&p->cap, 0, sizeof(p->cap));
	p->cap.pw_fd = -1;
	p->cap.handoff_fd = -1;
	p->cap.access = U_DESKTOP_ACCESS_UNKNOWN;
}

void
gp_cap_expect(struct gnome_portal *p)
{
	p->cap.resp_has_devices = false;
	p->cap.resp_devices = 0;
	p->cap.resp_nstreams = 0;
}

bool
gp_cap_parse_result(struct gnome_portal *p, const char *key, void *variant_iter)
{
	DBusMessageIter *var = (DBusMessageIter *)variant_iter;
	if (strcmp(key, "devices") == 0 && iter_is(var, DBUS_TYPE_UINT32)) {
		dbus_uint32_t d = 0;
		dbus_message_iter_get_basic(var, &d);
		p->cap.resp_devices = d;
		p->cap.resp_has_devices = true;
		return true;
	}
	if (strcmp(key, "streams") != 0 || !iter_is(var, DBUS_TYPE_ARRAY)) {
		return false;
	}
	// a(ua{sv}): node id, {position (ii), size (ii), source_type u, ...}
	DBusMessageIter arr;
	dbus_message_iter_recurse(var, &arr);
	while (iter_is(&arr, DBUS_TYPE_STRUCT) && p->cap.resp_nstreams < MAX_STREAMS) {
		DBusMessageIter st;
		dbus_message_iter_recurse(&arr, &st);
		dbus_uint32_t node = 0;
		if (iter_is(&st, DBUS_TYPE_UINT32)) {
			dbus_message_iter_get_basic(&st, &node);
		}
		bool have_pos = false, have_size = false;
		int32_t pos[2] = {0, 0}, size[2] = {0, 0};
		if (dbus_message_iter_next(&st) && iter_is(&st, DBUS_TYPE_ARRAY)) {
			DBusMessageIter d;
			dbus_message_iter_recurse(&st, &d);
			while (iter_is(&d, DBUS_TYPE_DICT_ENTRY)) {
				DBusMessageIter e, v, s;
				const char *k = NULL;
				dbus_message_iter_recurse(&d, &e);
				dbus_message_iter_get_basic(&e, &k);
				if (k != NULL && dbus_message_iter_next(&e) && iter_is(&e, DBUS_TYPE_VARIANT)) {
					dbus_message_iter_recurse(&e, &v);
					bool is_pos = strcmp(k, "position") == 0, is_size = strcmp(k, "size") == 0;
					if ((is_pos || is_size) && iter_is(&v, DBUS_TYPE_STRUCT)) {
						dbus_message_iter_recurse(&v, &s);
						int32_t *dst = is_pos ? pos : size;
						if (iter_is(&s, DBUS_TYPE_INT32)) {
							dbus_message_iter_get_basic(&s, &dst[0]);
							dbus_message_iter_next(&s);
							if (iter_is(&s, DBUS_TYPE_INT32)) {
								dbus_message_iter_get_basic(&s, &dst[1]);
								if (is_pos) {
									have_pos = true;
								} else {
									have_size = true;
								}
							}
						}
					}
				}
				dbus_message_iter_next(&d);
			}
		}
		// A stream that cannot be placed (no position / size) cannot be composed.
		if (have_pos && have_size && size[0] > 0 && size[1] > 0) {
			struct gnome_pw_stream *o = &p->cap.resp_streams[p->cap.resp_nstreams++];
			o->node = node;
			o->rect = (struct u_desktop_rect){pos[0], pos[1], size[0], size[1]};
		}
		dbus_message_iter_next(&arr);
	}
	return true;
}

enum u_desktop_result
gp_cap_select_sources(struct gnome_portal *p, bool interactive, char *msg)
{
	pthread_mutex_lock(&p->lock);
	// Never downgrade: a grant that covers the screens is restored WITH them, even
	// when this process cannot grab frames (GNOME saves exactly the restored shape).
	bool want = interactive ? p->cap.want_sources : (p->have_token && p->cap.token_screens);
	pthread_mutex_unlock(&p->lock);
	p->cap.sources_selected = false;
	if (!want) {
		return U_DESKTOP_OK;
	}
	if (!sc_probe(p)) {
		if (interactive) {
			U_LOG_W("desktop/gnome: no usable ScreenCast portal — the dialog asks for input only");
			return U_DESKTOP_OK;
		}
		// Restoring without the screens would save an input-only approval.
		gp_set_msg(msg, "the ScreenCast portal is not answering yet — not restoring without the screens; retry");
		return U_DESKTOP_ERR_TIMEOUT;
	}
	char token[64];
	gp_next_token(p, token, sizeof(token));
	gp_expect_response(p, token);
	struct pmsg pm;
	gp_pmsg_begin(&pm, GP_SC_IFACE, "SelectSources", p->session, NULL);
	gp_dict_add_str(&pm.dict, "handle_token", token);
	gp_dict_add_u32(&pm.dict, "types", GP_SC_SOURCE_MONITOR);
	gp_dict_add_bool(&pm.dict, "multiple", true);
	if (p->cap.cursor_mode != 0) {
		gp_dict_add_u32(&pm.dict, "cursor_mode", p->cap.cursor_mode);
	}
	// No persist keys: for a remote-desktop session they belong to SelectDevices.
	struct gdbus_err err;
	DBusMessage *reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply == NULL) {
		gp_set_msg(msg, "ScreenCast.SelectSources failed: %s %s", err.name, err.message);
		return U_DESKTOP_ERR_FAILED;
	}
	gp_take_request_handle(p, reply);
	dbus_message_unref(reply);
	if (!gp_wait_response(p, CALL_TIMEOUT_MS) || p->resp_code != 0) {
		gp_set_msg(msg, "ScreenCast.SelectSources refused (response %u)", p->resp_code);
		return U_DESKTOP_ERR_FAILED;
	}
	p->cap.sources_selected = true;
	return U_DESKTOP_OK;
}

static bool
open_pw_remote(struct gnome_portal *p)
{
	if (p->cap.pw_fd >= 0) {
		return true;
	}
	struct pmsg pm;
	gp_pmsg_begin(&pm, GP_SC_IFACE, "OpenPipeWireRemote", p->session, NULL);
	struct gdbus_err err;
	DBusMessage *reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	int fd = -1;
	if (reply == NULL || !dbus_message_get_args(reply, NULL, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID) || fd < 0) {
		U_LOG_W("desktop/gnome: ScreenCast.OpenPipeWireRemote failed: %s %s", err.name, err.message);
		if (reply != NULL) {
			dbus_message_unref(reply);
		}
		return false;
	}
	dbus_message_unref(reply);
	p->cap.pw_fd = fd;
	return true;
}

//! The connectors of the monitors the open session's streams show.
static void
record_covered(struct gnome_portal *p)
{
	struct dc_state dc;
	if (!dc_read(p->c, &dc)) {
		return;
	}
	uint32_t n = 0;
	for (uint32_t i = 0; i < p->cap.nstreams && n < 8; i++) {
		for (uint32_t k = 0; k < dc.n; k++) {
			if (rect_close(&p->cap.streams[i].rect, &dc.m[k].rect)) {
				snprintf(p->cap.covered[n++], sizeof(p->cap.covered[0]), "%s", dc.m[k].conn);
				break;
			}
		}
	}
	p->cap.ncovered = n;
	p->cap.dc_serial = dc.serial;
}

bool
gp_cap_after_start(struct gnome_portal *p)
{
	struct gp_cap *cp = &p->cap;
	bool sc = cp->session_sc;
	bool devices = !sc && (!cp->resp_has_devices || cp->resp_devices != 0);
	bool screens = cp->sources_selected && cp->resp_nstreams > 0;
	cp->nstreams = screens ? cp->resp_nstreams : 0;
	memcpy(cp->streams, cp->resp_streams, sizeof(cp->streams));

	pthread_mutex_lock(&p->lock);
	bool had_screens = p->cap.token_screens;
	cp->token_sc = sc;
	cp->token_devices = devices;
	cp->token_screens = screens;
	cp->open_streams = screens;
	cp->open_devices = devices;
	pthread_mutex_unlock(&p->lock);

	if (had_screens && cp->sources_selected && !screens) {
		U_LOG_W("desktop/gnome: the portal restored the session WITHOUT any screen (monitors changed?)");
	}
	if (screens) {
		record_covered(p);
		if (!open_pw_remote(p)) {
			U_LOG_W("desktop/gnome: no PipeWire remote for the screens — capture retries it");
		}
		U_LOG_I("desktop/gnome: %u screen stream(s) in the %s session", cp->nstreams,
		        sc ? "ScreenCast" : "RemoteDesktop");
	}
	return devices;
}

void
gp_cap_forget(struct gnome_portal *p)
{
	struct gp_cap *cp = &p->cap;
	if (cp->pw_fd >= 0) {
		close(cp->pw_fd);
		cp->pw_fd = -1;
	}
	cp->nstreams = 0;
	cp->sources_selected = false;
	cp->session_sc = false;
	pthread_mutex_lock(&p->lock);
	cp->open_streams = false;
	cp->open_devices = false;
	if (!(p->have_token && cp->token_screens) && cp->access == U_DESKTOP_ACCESS_GRANTED) {
		cp->access = U_DESKTOP_ACCESS_UNKNOWN; // nothing to restore from
	}
	pthread_mutex_unlock(&p->lock);
}

void
gp_cap_revoked_locked(struct gnome_portal *p)
{
	p->cap.token_sc = false;
	p->cap.token_devices = false;
	p->cap.token_screens = false;
	p->cap.open_streams = false;
	p->cap.access = U_DESKTOP_ACCESS_DENIED;
}

bool
gp_cap_control_granted_locked(struct gnome_portal *p)
{
	return (p->have_token && !p->cap.token_sc && p->cap.token_devices) ||
	       (p->session_open_shared && p->cap.open_devices);
}

void
gp_cap_begin_upgrade(struct gnome_portal *p, struct gp_cap_saved *saved)
{
	memset(saved, 0, sizeof(*saved));
	pthread_mutex_lock(&p->lock);
	saved->have_token = p->have_token;
	memcpy(saved->token, p->token, sizeof(saved->token));
	saved->token_sc = p->cap.token_sc;
	saved->token_devices = p->cap.token_devices;
	saved->token_screens = p->cap.token_screens;
	// Not passed to the portal, so not consumed: put back if the dialog fails.
	p->have_token = false;
	memset(p->token, 0, sizeof(p->token));
	// Never downgrade: a grant that covered the screens asks for them again.
	p->cap.want_sources = p->cap.want_sources || saved->token_screens;
	pthread_mutex_unlock(&p->lock);
}

void
gp_cap_end_upgrade(struct gnome_portal *p, struct gp_cap_saved *saved, bool ok)
{
	pthread_mutex_lock(&p->lock);
	if (!ok && saved->have_token && !p->have_token) {
		p->have_token = true;
		memcpy(p->token, saved->token, sizeof(p->token));
		p->cap.token_sc = saved->token_sc;
		p->cap.token_devices = saved->token_devices;
		p->cap.token_screens = saved->token_screens;
	}
	p->cap.want_sources = false;
	pthread_mutex_unlock(&p->lock);
	memset(saved, 0, sizeof(*saved));
}

enum u_desktop_result
gp_cap_before_restore(struct gnome_portal *p, char *msg)
{
	pthread_mutex_lock(&p->lock);
	bool screens = p->have_token && p->cap.token_screens;
	pthread_mutex_unlock(&p->lock);
	if (!screens || p->c == NULL) {
		return U_DESKTOP_OK;
	}
	uint64_t start = gdbus_now_ms();
	for (;;) {
		struct dc_state dc;
		if (!dc_read(p->c, &dc)) {
			return U_DESKTOP_OK; // cannot tell: no worse than before
		}
		uint64_t now = gdbus_now_ms();
		if (dc.serial != p->cap.dc_serial) {
			p->cap.dc_serial = dc.serial;
			p->cap.dc_changed_ms = now;
		}
		bool settled = p->cap.dc_changed_ms == 0 || now - p->cap.dc_changed_ms >= LAYOUT_SETTLE_MS;
		uint32_t missing = 0;
		for (uint32_t i = 0; i < p->cap.ncovered; i++) {
			bool found = false;
			for (uint32_t k = 0; k < dc.n && !found; k++) {
				found = strcmp(dc.m[k].conn, p->cap.covered[i]) == 0;
			}
			missing += found ? 0 : 1;
		}
		if (settled && missing == 0) {
			return U_DESKTOP_OK;
		}
		if (now - start >= LAYOUT_WAIT_MAX_MS || gp_quitting(p)) {
			gp_set_msg(msg,
			           missing > 0 ? "a monitor the screen grant covers is missing — not restoring (retry when "
			                         "it is back)"
			                       : "the monitor layout is still changing — retry");
			return U_DESKTOP_ERR_TIMEOUT;
		}
		gp_pump(p, 100);
	}
}

void
gp_cap_close_request(struct gnome_portal *p)
{
	if (p->c == NULL || p->wait_path[0] == '\0') {
		return;
	}
	DBusMessage *m =
	    dbus_message_new_method_call(PORTAL_DEST, p->wait_path, "org.freedesktop.portal.Request", "Close");
	if (m == NULL) {
		return;
	}
	dbus_message_set_no_reply(m, TRUE);
	dbus_connection_send(p->c, m, NULL);
	dbus_connection_flush(p->c);
	dbus_message_unref(m);
	U_LOG_W("desktop/gnome: closed a portal dialog that a silent restore opened");
}


/*
 *
 * A standalone ScreenCast session: CAPTURE without CONTROL (decision 1).
 *
 */

static void
revoke(struct gnome_portal *p)
{
	pthread_mutex_lock(&p->lock);
	p->have_token = false;
	memset(p->token, 0, sizeof(p->token));
	gp_cap_revoked_locked(p);
	pthread_mutex_unlock(&p->lock);
}

static enum u_desktop_result
open_sc_session(struct gnome_portal *p, bool interactive, char *msg)
{
	if (p->c == NULL) {
		gp_set_msg(msg, "no D-Bus session bus");
		return U_DESKTOP_ERR_FAILED;
	}
	if (!sc_probe(p)) {
		gp_set_msg(msg, "no usable ScreenCast portal (yet)");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	gp_register_app(p);
	gp_session_close(p);
	if (!interactive) {
		enum u_desktop_result rr = gp_cap_before_restore(p, msg);
		if (rr != U_DESKTOP_OK) {
			return rr;
		}
	}
	char token[64], stoken[64];
	struct gdbus_err err;
	struct pmsg pm;

	gp_next_token(p, token, sizeof(token));
	gp_next_token(p, stoken, sizeof(stoken));
	gp_expect_response(p, token);
	gp_pmsg_begin(&pm, GP_SC_IFACE, "CreateSession", NULL, NULL);
	gp_dict_add_str(&pm.dict, "handle_token", token);
	gp_dict_add_str(&pm.dict, "session_handle_token", stoken);
	DBusMessage *reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply == NULL) {
		gp_set_msg(msg, "ScreenCast.CreateSession failed: %s %s", err.name, err.message);
		return U_DESKTOP_ERR_FAILED;
	}
	gp_take_request_handle(p, reply);
	dbus_message_unref(reply);
	if (!gp_wait_response(p, CALL_TIMEOUT_MS) || p->resp_code != 0 || p->resp_session[0] == '\0') {
		gp_set_msg(msg, "ScreenCast.CreateSession: no session (response %u)", p->resp_code);
		return U_DESKTOP_ERR_FAILED;
	}
	snprintf(p->session, sizeof(p->session), "%s", p->resp_session);
	p->session_open = true;
	p->cap.session_sc = true;

	char restore[1024] = {0};
	pthread_mutex_lock(&p->lock);
	if (p->have_token && p->cap.token_sc) {
		snprintf(restore, sizeof(restore), "%s", p->token);
	}
	pthread_mutex_unlock(&p->lock);
	gp_next_token(p, token, sizeof(token));
	gp_expect_response(p, token);
	gp_pmsg_begin(&pm, GP_SC_IFACE, "SelectSources", p->session, NULL);
	gp_dict_add_str(&pm.dict, "handle_token", token);
	gp_dict_add_u32(&pm.dict, "types", GP_SC_SOURCE_MONITOR);
	gp_dict_add_bool(&pm.dict, "multiple", true);
	if (p->cap.cursor_mode != 0) {
		gp_dict_add_u32(&pm.dict, "cursor_mode", p->cap.cursor_mode);
	}
	// A standalone ScreenCast session persists on SelectSources (portal v4+).
	gp_dict_add_u32(&pm.dict, "persist_mode", 2);
	if (restore[0] != '\0') {
		gp_dict_add_str(&pm.dict, "restore_token", restore);
	}
	memset(restore, 0, sizeof(restore));
	reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply == NULL) {
		gp_set_msg(msg, "ScreenCast.SelectSources failed: %s %s", err.name, err.message);
		gp_session_close(p);
		return U_DESKTOP_ERR_FAILED;
	}
	gp_take_request_handle(p, reply);
	dbus_message_unref(reply);
	if (!gp_wait_response(p, CALL_TIMEOUT_MS) || p->resp_code != 0) {
		gp_set_msg(msg, "ScreenCast.SelectSources refused (response %u)", p->resp_code);
		gp_session_close(p);
		return U_DESKTOP_ERR_FAILED;
	}
	p->cap.sources_selected = true;

	gp_next_token(p, token, sizeof(token));
	gp_expect_response(p, token);
	gp_pmsg_begin(&pm, GP_SC_IFACE, "Start", p->session, "");
	gp_dict_add_str(&pm.dict, "handle_token", token);
	reply = gp_pmsg_send(p, &pm, CALL_TIMEOUT_MS, &err);
	if (reply == NULL) {
		gp_set_msg(msg, "ScreenCast.Start failed: %s %s", err.name, err.message);
		gp_session_close(p);
		return U_DESKTOP_ERR_FAILED;
	}
	gp_take_request_handle(p, reply);
	dbus_message_unref(reply);
	if (!gp_wait_response(p, interactive ? INTERACTIVE_TIMEOUT_MS : RESTORE_TIMEOUT_MS)) {
		gp_cap_close_request(p); // a capture never leaves a dialog up
		gp_session_close(p);
		if (interactive) {
			gp_set_msg(msg, "no answer to the screen-sharing dialog");
			return U_DESKTOP_ERR_TIMEOUT;
		}
		revoke(p);
		gp_set_msg(msg, "the portal did not restore screen sharing silently (grant gone?) — request access again");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	if (p->resp_code != 0) {
		gp_set_msg(msg, p->resp_code == 1 ? "the user declined screen sharing" : "the portal refused the session");
		gp_session_close(p);
		if (!interactive) {
			revoke(p);
		}
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	pthread_mutex_lock(&p->lock);
	p->have_token = p->resp_has_token;
	snprintf(p->token, sizeof(p->token), "%s", p->resp_has_token ? p->resp_token : "");
	pthread_mutex_unlock(&p->lock);
	gp_cap_after_start(p);
	p->last_use_ms = gdbus_now_ms();
	if (p->cap.nstreams == 0) {
		gp_set_msg(msg, "no screen was shared");
		gp_session_close(p);
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	U_LOG_I("desktop/gnome: ScreenCast session %s (%s)", interactive ? "granted" : "restored", p->session);
	return U_DESKTOP_OK;
}


/*
 *
 * Commands on the portal thread.
 *
 */

struct scopes_req
{
	u_desktop_scopes_done_fn done;
	void *ctx;
	uint32_t scopes;
};

//! Only when the thread drops the command unrun (shutdown): never answered.
static void
scopes_tramp(void *ctx, enum u_desktop_access a)
{
	(void)a;
	struct scopes_req *r = (struct scopes_req *)ctx;
	r->done(r->ctx, U_DESKTOP_ACCESS_UNKNOWN, U_DESKTOP_ACCESS_UNKNOWN);
	free(r);
}

static enum u_desktop_result
cmd_request_scopes(struct gnome_portal *p, struct gp_cmd *c)
{
	struct scopes_req *req = (struct scopes_req *)c->done_ctx;
	c->done = NULL; // answered here, never by the drain
	c->async = true;
	uint32_t scopes = req->scopes;
	bool want_ctl = (scopes & U_DESKTOP_SCOPE_CONTROL) != 0;
	bool want_cap = (scopes & U_DESKTOP_SCOPE_CAPTURE) != 0;

	pthread_mutex_lock(&p->lock);
	bool ctl_now = gp_cap_control_granted_locked(p) || p->consent_only_granted;
	bool cap_now = (p->have_token && p->cap.token_screens) || p->cap.open_streams;
	bool reask = p->cap.reask;
	pthread_mutex_unlock(&p->lock);

	enum u_desktop_access ctl = U_DESKTOP_ACCESS_UNKNOWN, cap = U_DESKTOP_ACCESS_UNKNOWN;
	char msg[U_DESKTOP_MSG_SIZE] = {0};
	if (!reask && (!want_ctl || ctl_now) && (!want_cap || cap_now)) {
		ctl = want_ctl ? U_DESKTOP_ACCESS_GRANTED : U_DESKTOP_ACCESS_UNKNOWN;
		cap = want_cap ? U_DESKTOP_ACCESS_GRANTED : U_DESKTOP_ACCESS_UNKNOWN;
	} else if (p->input && (want_ctl || (ctl_now && !p->consent_only_granted))) {
		// ONE combined RemoteDesktop dialog (input + screens). A token cannot be
		// widened, so the grant is asked for fresh; the old one comes back if this
		// dialog fails.
		struct gp_cap_saved saved;
		pthread_mutex_lock(&p->lock);
		p->cap.want_sources = want_cap || (reask && (p->cap.token_screens || p->cap.open_streams));
		pthread_mutex_unlock(&p->lock);
		gp_cap_begin_upgrade(p, &saved);
		enum u_desktop_result r = gp_open_session(p, true, msg);
		bool selected = p->cap.sources_selected;
		gp_cap_end_upgrade(p, &saved, r == U_DESKTOP_OK);
		if (r == U_DESKTOP_OK) {
			pthread_mutex_lock(&p->lock);
			ctl = gp_cap_control_granted_locked(p) ? U_DESKTOP_ACCESS_GRANTED : U_DESKTOP_ACCESS_DENIED;
			cap = selected ? (p->cap.open_streams ? U_DESKTOP_ACCESS_GRANTED : U_DESKTOP_ACCESS_DENIED)
			               : (want_cap ? U_DESKTOP_ACCESS_DENIED : U_DESKTOP_ACCESS_UNKNOWN);
			pthread_mutex_unlock(&p->lock);
		} else if (!gp_quitting(p)) {
			ctl = want_ctl ? U_DESKTOP_ACCESS_DENIED : U_DESKTOP_ACCESS_UNKNOWN;
			cap = want_cap ? U_DESKTOP_ACCESS_DENIED : U_DESKTOP_ACCESS_UNKNOWN;
			U_LOG_W("desktop/gnome: access request: %s", msg);
		}
	} else {
		if (want_cap) {
			// CAPTURE without CONTROL: a standalone ScreenCast session.
			struct gp_cap_saved saved;
			gp_cap_begin_upgrade(p, &saved);
			enum u_desktop_result r = open_sc_session(p, true, msg);
			gp_cap_end_upgrade(p, &saved, r == U_DESKTOP_OK);
			cap = r == U_DESKTOP_OK ? U_DESKTOP_ACCESS_GRANTED
			      : gp_quitting(p)  ? U_DESKTOP_ACCESS_UNKNOWN
			                        : U_DESKTOP_ACCESS_DENIED;
			if (r != U_DESKTOP_OK) {
				U_LOG_W("desktop/gnome: screen-capture request: %s", msg);
			}
		}
		if (want_ctl) {
			// No libei / RemoteDesktop < 2: the consent-only dialog, as in spec v1.
			enum u_desktop_result r = gp_open_session(p, true, msg);
			ctl = r == U_DESKTOP_OK ? U_DESKTOP_ACCESS_GRANTED : U_DESKTOP_ACCESS_DENIED;
			if (r == U_DESKTOP_OK && !p->input) {
				pthread_mutex_lock(&p->lock);
				p->consent_only_granted = true;
				pthread_mutex_unlock(&p->lock);
			}
		}
	}

	pthread_mutex_lock(&p->lock);
	if (want_ctl || ctl != U_DESKTOP_ACCESS_UNKNOWN) {
		p->pending = false;
		if (ctl != U_DESKTOP_ACCESS_UNKNOWN) {
			p->access = ctl;
		}
	}
	p->cap.pending = false;
	p->cap.reask = false;
	if (cap != U_DESKTOP_ACCESS_UNKNOWN) {
		p->cap.access = cap;
	}
	pthread_mutex_unlock(&p->lock);
	req->done(req->ctx, ctl, cap);
	free(req);
	c->done_ctx = NULL;
	return U_DESKTOP_OK;
}

/*!
 * Make sure the session that carries the screens is open (restoring it silently,
 * never with a dialog), and hand a dup of its PipeWire remote + its streams to the
 * caller through p->cap.handoff_*.
 */
static enum u_desktop_result
cmd_capture_prepare(struct gnome_portal *p, struct gp_cmd *c)
{
	struct gp_cap *cp = &p->cap;
	if (!(p->session_open && cp->nstreams > 0)) {
		pthread_mutex_lock(&p->lock);
		bool tok = p->have_token && cp->token_screens;
		bool sc = cp->token_sc;
		pthread_mutex_unlock(&p->lock);
		if (!tok) {
			gp_set_msg(c->msg, "screen capture is not granted to this app — request access first");
			return U_DESKTOP_ERR_NOT_GRANTED;
		}
		enum u_desktop_result r = sc ? open_sc_session(p, false, c->msg) : gp_open_session(p, false, c->msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
		if (cp->nstreams == 0) {
			revoke(p);
			gp_session_close(p);
			gp_set_msg(c->msg, "the portal restored no screen — request access again");
			return U_DESKTOP_ERR_NOT_GRANTED;
		}
	}
	if (!open_pw_remote(p)) {
		gp_set_msg(c->msg, "no PipeWire remote for the shared screens");
		return U_DESKTOP_ERR_FAILED;
	}
	int fd = fcntl(cp->pw_fd, F_DUPFD_CLOEXEC, 3);
	if (fd < 0) {
		gp_set_msg(c->msg, "could not duplicate the PipeWire remote");
		return U_DESKTOP_ERR_FAILED;
	}
	pthread_mutex_lock(&p->lock);
	if (cp->handoff_fd >= 0) {
		close(cp->handoff_fd); // an abandoned earlier handoff
	}
	cp->handoff_fd = fd;
	cp->handoff_n = cp->nstreams;
	memcpy(cp->handoff, cp->streams, sizeof(cp->handoff));
	pthread_mutex_unlock(&p->lock);
	uint64_t now = gdbus_now_ms();
	p->last_use_ms = now; // a capture counts as use (spec §10.5 lifetime)
	if (p->hold_until_ms < now + CAPTURE_HOLD_MS) {
		p->hold_until_ms = now + CAPTURE_HOLD_MS;
	}
	return U_DESKTOP_OK;
}


/*
 *
 * The backend ops.
 *
 */

static enum u_desktop_access
gnome_capture_access_status(struct u_desktop_backend *b)
{
	struct gnome_portal *p = gnome(b)->portal;
	if (p == NULL) {
		return U_DESKTOP_ACCESS_DENIED;
	}
	pthread_mutex_lock(&p->lock);
	enum u_desktop_access a = p->cap.access;
	if (p->cap.pending) {
		a = U_DESKTOP_ACCESS_PENDING;
	} else if ((p->have_token && p->cap.token_screens) || p->cap.open_streams) {
		a = U_DESKTOP_ACCESS_GRANTED;
	}
	pthread_mutex_unlock(&p->lock);
	return a;
}

static void
gnome_request_scopes(struct u_desktop_backend *b,
                     const struct u_desktop_app_info *app,
                     uint32_t scopes,
                     u_desktop_scopes_done_fn done,
                     void *ctx)
{
	(void)app; // the portal names the app by its app id
	struct gnome_portal *p = gnome(b)->portal;
	struct scopes_req *req = calloc(1, sizeof(*req));
	struct gp_cmd *c = p != NULL ? gp_cmd_new(cmd_request_scopes) : NULL;
	if (req == NULL || c == NULL) {
		free(req);
		if (c != NULL) {
			gp_cmd_free(c);
		}
		done(ctx, U_DESKTOP_ACCESS_DENIED, U_DESKTOP_ACCESS_DENIED);
		return;
	}
	req->done = done;
	req->ctx = ctx;
	req->scopes = scopes & (U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE);
	c->done = scopes_tramp;
	c->done_ctx = req;
	pthread_mutex_lock(&p->lock);
	if ((scopes & U_DESKTOP_SCOPE_CONTROL) != 0) {
		p->pending = true;
	}
	if ((scopes & U_DESKTOP_SCOPE_CAPTURE) != 0) {
		p->cap.pending = true;
	}
	p->cap.reask = (scopes & U_DESKTOP_SCOPE_REASK) != 0;
	pthread_mutex_unlock(&p->lock);
	if (!gp_enqueue_async(p, c)) {
		pthread_mutex_lock(&p->lock);
		p->pending = false;
		p->cap.pending = false;
		pthread_mutex_unlock(&p->lock);
		gp_cmd_free(c);
		free(req);
		done(ctx, U_DESKTOP_ACCESS_DENIED, U_DESKTOP_ACCESS_DENIED);
	}
}

static bool
obs_locked_now(struct gnome_backend *g)
{
	pthread_mutex_lock(&g->obs_lock);
	DBusMessage *reply = gdbus_call(g->obs, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver",
	                                "org.gnome.ScreenSaver", "GetActive", GNOME_OBS_TIMEOUT_MS, NULL, DBUS_TYPE_INVALID);
	pthread_mutex_unlock(&g->obs_lock);
	dbus_bool_t active = TRUE; // no answer: fail closed
	if (reply != NULL) {
		if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &active, DBUS_TYPE_INVALID)) {
			active = TRUE;
		}
		dbus_message_unref(reply);
	}
	return active;
}

static enum u_desktop_result
gnome_monitors(struct u_desktop_backend *b, struct u_desktop_rect *out, uint32_t cap, uint32_t *count, char *msg)
{
	struct gnome_backend *g = gnome(b);
	struct dc_state dc;
	pthread_mutex_lock(&g->obs_lock);
	bool ok = dc_read(g->obs, &dc);
	gdbus_drain(g->obs);
	pthread_mutex_unlock(&g->obs_lock);
	if (!ok) {
		gnome_set_msg(msg, "cannot read the monitor layout (org.gnome.Mutter.DisplayConfig)");
		*count = 0;
		return U_DESKTOP_ERR_FAILED;
	}
	for (uint32_t i = 0; i < dc.n && i < cap; i++) {
		out[i] = dc.m[i].rect;
	}
	*count = dc.n;
	return U_DESKTOP_OK;
}

//! Withhold every capture-excluded window (frame and buffer rects). False when the
//! extension did not answer: the caller refuses the capture.
static bool
mask_excluded(struct gnome_backend *g, struct u_desktop_image *img, const struct u_desktop_rect *src)
{
	if (!g->have_ext16) {
		return false;
	}
	struct gdbus_err err;
	pthread_mutex_lock(&g->obs_lock);
	DBusMessage *reply =
	    gdbus_call(g->obs, WLG_BUS, WLG_PATH_, WLG_IFACE_, "GetAllWindows", GNOME_OBS_TIMEOUT_MS, &err, DBUS_TYPE_INVALID);
	gdbus_drain(g->obs);
	pthread_mutex_unlock(&g->obs_lock);
	const char *json = NULL;
	if (reply == NULL || !dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID)) {
		if (reply != NULL) {
			dbus_message_unref(reply);
		}
		return false;
	}
	cJSON *root = cJSON_Parse(json);
	dbus_message_unref(reply);
	const cJSON *arr = root != NULL ? u_json_get(root, "windows") : NULL;
	if (!cJSON_IsArray(arr)) {
		cJSON_Delete(root);
		return false;
	}
	int n = cJSON_GetArraySize(arr);
	for (int i = 0; i < n; i++) {
		const cJSON *w = cJSON_GetArrayItem(arr, i);
		if (!cJSON_IsTrue(u_json_get(w, "capture_excluded")) || cJSON_IsTrue(u_json_get(w, "minimized"))) {
			continue;
		}
		const char *keys[2] = {"frame", "buffer"};
		for (int k = 0; k < 2; k++) {
			int f[4] = {0, 0, 0, 0};
			if (u_json_get_int_array(u_json_get(w, keys[k]), f, 4) == 4) {
				struct u_desktop_rect r = {f[0], f[1], f[2], f[3]};
				u_desktop_image_withhold(img, src, &r);
			}
		}
	}
	cJSON_Delete(root);
	return true;
}

static enum u_desktop_result
gnome_capture(struct u_desktop_backend *b, const struct u_desktop_rect *src, struct u_desktop_image *img, char *msg)
{
	struct gnome_backend *g = gnome(b);
	struct gnome_portal *p = g->portal;
	memset(img, 0, sizeof(*img));
	if (p == NULL) {
		gnome_set_msg(msg, "no portal");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	if (obs_locked_now(g)) {
		gnome_set_msg(msg, "screen locked");
		return U_DESKTOP_ERR_REFUSED;
	}
	// Without the extension the capture-excluded windows cannot be known: refuse
	// before anything is grabbed (and again after, if it stopped answering).
	if (!g->have_ext16) {
		gnome_set_msg(msg, "cannot honour capture exclusion (the DisplayXR GNOME extension is not running)");
		return U_DESKTOP_ERR_REFUSED;
	}

	struct gp_cmd *c = gp_cmd_new(cmd_capture_prepare);
	if (c == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	// A silent restore (5 s) after waiting for a settled layout (6 s) at most.
	enum u_desktop_result r = gp_run_sync(p, c, 15000, msg);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	struct gnome_pw_stream all[MAX_STREAMS];
	uint32_t nall = 0;
	pthread_mutex_lock(&p->lock);
	int fd = p->cap.handoff_fd;
	p->cap.handoff_fd = -1;
	nall = p->cap.handoff_n > MAX_STREAMS ? MAX_STREAMS : p->cap.handoff_n;
	memcpy(all, p->cap.handoff, sizeof(all));
	pthread_mutex_unlock(&p->lock);
	if (fd < 0) {
		gnome_set_msg(msg, "the screen session went away");
		return U_DESKTOP_ERR_FAILED;
	}

	// Only the streams of monitors the region touches.
	struct gnome_pw_stream sel[MAX_STREAMS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < nall; i++) {
		struct u_desktop_rect tmp;
		if (u_desktop_rect_intersect(&all[i].rect, src, &tmp)) {
			sel[n++] = all[i];
		}
	}
	struct gnome_pw_frame frames[MAX_STREAMS];
	memset(frames, 0, sizeof(frames));
	uint64_t t0 = os_monotonic_get_ns();
	if (n > 0) {
		r = gnome_pw_grab(fd, sel, n, FRAME_TIMEOUT_MS, frames, msg);
	}
	close(fd);
	if (r != U_DESKTOP_OK) {
		return r;
	}
	pthread_mutex_lock(&p->lock);
	p->last_use_ms = gdbus_now_ms();
	pthread_mutex_unlock(&p->lock);

	// The image: the region × the densest scale among the monitors it touches (as
	// GNOME's own screenshots); a region only on unshared monitors stays withheld.
	double scale = 1.0;
	for (uint32_t i = 0; i < n; i++) {
		double s = (double)frames[i].w / sel[i].rect.w;
		scale = s > scale ? s : scale;
	}
	uint32_t w = (uint32_t)lround(src->w * scale), h = (uint32_t)lround(src->h * scale);
	if (!u_desktop_image_alloc(img, w < 1 ? 1 : w, h < 1 ? 1 : h)) {
		for (uint32_t i = 0; i < n; i++) {
			gnome_pw_frame_free(&frames[i]);
		}
		gnome_set_msg(msg, "the image is too large (%ux%u)", w, h);
		return U_DESKTOP_ERR_FAILED;
	}
	img->frame_ns = t0;
	for (uint32_t i = 0; i < n; i++) {
		u_desktop_image_draw_frame(img, src, &sel[i].rect, frames[i].data, frames[i].w, frames[i].h,
		                           frames[i].stride, frames[i].fmt);
		gnome_pw_frame_free(&frames[i]);
	}
	// Portal monitor streams show the on-screen picture, capture-excluded windows
	// included (wayland-window-geometry §6.2): withhold them, or refuse.
	if (!mask_excluded(g, img, src)) {
		u_desktop_image_free(img);
		gnome_set_msg(msg, "cannot honour capture exclusion");
		return U_DESKTOP_ERR_REFUSED;
	}
	if (obs_locked_now(g)) {
		u_desktop_image_free(img);
		gnome_set_msg(msg, "screen locked");
		return U_DESKTOP_ERR_REFUSED;
	}
	return U_DESKTOP_OK;
}

uint32_t
gnome_capture_init(struct gnome_backend *g)
{
	if (g->portal == NULL) {
		return 0;
	}
	g->portal->cap.pipewire = gnome_pw_available();
	if (!g->portal->cap.pipewire) {
		U_LOG_W("desktop/gnome: no libpipewire-0.3 — no screen capture (a screen grant is still never downgraded)");
		return 0;
	}
	g->base.capture_access_status = gnome_capture_access_status;
	g->base.request_scopes = gnome_request_scopes;
	g->base.monitors = gnome_monitors;
	g->base.capture = gnome_capture;
	return U_DESKTOP_CAP_CAPTURE;
}
