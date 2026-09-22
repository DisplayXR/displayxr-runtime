// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Wayland window-geometry provider implementation (#817).
 *
 * libdbus-1 client of the GNOME Shell extension `window-geometry@displayxr.org`
 * (contrib/gnome-shell/). One initial GetWindows snapshot at create, then a
 * non-blocking per-query pump of WindowsChanged signals. The publisher is
 * intermittent by design — GNOME disables user extensions whenever the screen
 * shield is up — so NameOwnerChanged is tracked too, and the cache is dropped
 * and re-taken across that gap. Single-threaded use from the compositor frame
 * loop — no locking.
 *
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_wl_geom.h"

#include "util/u_logging.h"
#include "util/u_wayland_geom.h"
#include "util/u_json.h"
#include "util/u_misc.h"
#include "os/os_time.h"

#include <dbus/dbus.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WLG_BUS_NAME "org.displayxr.WindowGeometry"
#define WLG_OBJ_PATH "/org/displayxr/WindowGeometry"
#define WLG_IFACE "org.displayxr.WindowGeometry1"
#define WLG_EXT_UUID "window-geometry@displayxr.org"
#define WLG_MATCH_RULE "type='signal',interface='" WLG_IFACE "',member='WindowsChanged'"

//! The publisher comes and goes (see wlg_pump), so we also watch who owns its
//! well-known name. The daemon supports arg0 matching, so this delivers only
//! transitions of OUR name — not every name change on the session bus.
#define WLG_OWNER_MATCH_RULE                                                                                           \
	"type='signal',sender='" DBUS_SERVICE_DBUS "',interface='" DBUS_INTERFACE_DBUS                                 \
	"',"                                                                                                           \
	"member='NameOwnerChanged',arg0='" WLG_BUS_NAME "'"

//! Bounded-retry period for the lazy GetWindows when we have no snapshot.
#define WLG_RETRY_PERIOD_NS ((int64_t)5 * 1000 * 1000 * 1000)

//! Highest payload schema version this consumer understands. Additive changes
//! keep the version; anything that changes the MEANING of an existing field
//! bumps it (and older consumers then refuse the payload rather than misread it).
#define WLG_SCHEMA_VERSION_MAX 1

// One entry per published window we care about (own-PID filter is applied at
// query time, not parse time, so the cache mirrors the full snapshot).
//
// EVERY geometry field here is LOGICAL — this is the wire payload, unconverted.
// The `logical_` prefix is not decoration: the 2026-09-20 faults all came from
// two coordinate spaces sharing one variable name, so the cache keeps the
// payload's space in the name and the conversion happens at exactly one place
// (@ref comp_vk_native_wl_geom_get_window_rect, which is the public boundary).
struct wlg_window
{
	int32_t pid;
	bool focus;
	//! Frame rect, LOGICAL px, in Mutter's global stage coordinates.
	int32_t logical_x, logical_y;
	int32_t logical_w, logical_h;
	//! The monitor this window is on, LOGICAL px, same coordinates.
	int32_t mon_logical_x, mon_logical_y;
	int32_t mon_logical_w, mon_logical_h;
	//! Mutter's FRACTIONAL monitor scale (Meta.Display.get_monitor_scale) —
	//! 1.6667 on the measured box's laptop. Never an integer wl_output.scale.
	float mon_scale;
	//! False when Mutter published no `monitor` object (window on no monitor).
	bool have_monitor;
	//! Buffer rect SIZE, LOGICAL px (Meta.Window.get_buffer_rect): the size
	//! the compositor actually gives the committed surface, as opposed to the
	//! frame the toplevel was configured to. False when not published.
	int32_t buffer_logical_w, buffer_logical_h;
	bool have_buffer;
};

#define WLG_MAX_WINDOWS 64

struct comp_vk_native_wl_geom
{
	DBusConnection *conn; //!< private session-bus connection, NULL when unavailable

	struct wlg_window windows[WLG_MAX_WINDOWS];
	uint32_t window_count;
	bool have_snapshot;      //!< at least one successfully parsed payload
	bool warned_unavailable; //!< one-shot WARN guard (extension missing)
	bool warned_scale;       //!< one-shot INFO guard (says which scale is being applied)
	bool warned_schema;      //!< one-shot WARN guard (publisher schema too new)
	bool warned_no_monitor;  //!< one-shot WARN guard (payload carried no monitor rect)
	//! Last surface-vs-frame comparison logged (logical px), on change only.
	int32_t logged_surface_w, logged_surface_h, logged_frame_w, logged_frame_h;
	int64_t next_retry_ns;   //!< earliest monotonic time for the next blocking GetWindows retry
};


/*
 *
 * Payload parsing.
 *
 */

static void
wlg_parse_snapshot(struct comp_vk_native_wl_geom *g, const char *json)
{
	cJSON *root = cJSON_Parse(json);
	if (root == NULL) {
		return;
	}

	// Schema gate. The publisher is a SHARED asset that a vendor runtime
	// package may ship (see the packaging contract in
	// docs/specs/runtime/wayland-window-geometry.md), so the publisher on a
	// given box is NOT necessarily the one this runtime shipped with. Within a
	// version the schema is additive-only, so a newer minor publisher is safe
	// to read (unknown fields are simply not looked up); a MAJOR bump means
	// changed semantics — e.g. physical instead of logical pixels — which would
	// weave at a silently wrong phase. Refuse it and stay display-scoped.
	int version = 0;
	if (!u_json_get_int(u_json_get(root, "version"), &version)) {
		version = 1; // pre-versioning publisher — treat as v1
	}
	if (version > WLG_SCHEMA_VERSION_MAX) {
		if (!g->warned_schema) {
			U_LOG_W("wl_geom: geometry service speaks schema v%d, this runtime understands up to v%d "
			        "— ignoring it and weaving display-scoped. Update the runtime.",
			        version, WLG_SCHEMA_VERSION_MAX);
			g->warned_schema = true;
		}
		cJSON_Delete(root);
		return;
	}

	const cJSON *windows = u_json_get(root, "windows");
	if (!cJSON_IsArray(windows)) {
		cJSON_Delete(root);
		return;
	}

	uint32_t count = 0;
	const cJSON *win = NULL;
	cJSON_ArrayForEach(win, windows)
	{
		if (count >= WLG_MAX_WINDOWS) {
			break;
		}

		int pid = 0;
		if (!u_json_get_int(u_json_get(win, "pid"), &pid) || pid <= 0) {
			continue;
		}

		const cJSON *frame = u_json_get(win, "frame");
		if (!cJSON_IsArray(frame) || cJSON_GetArraySize(frame) != 4) {
			continue;
		}

		struct wlg_window *out = &g->windows[count];
		U_ZERO(out);
		out->pid = (int32_t)pid;
		out->logical_x = (int32_t)cJSON_GetArrayItem(frame, 0)->valuedouble;
		out->logical_y = (int32_t)cJSON_GetArrayItem(frame, 1)->valuedouble;
		out->logical_w = (int32_t)cJSON_GetArrayItem(frame, 2)->valuedouble;
		out->logical_h = (int32_t)cJSON_GetArrayItem(frame, 3)->valuedouble;

		const cJSON *buffer = u_json_get(win, "buffer");
		if (cJSON_IsArray(buffer) && cJSON_GetArraySize(buffer) == 4) {
			out->buffer_logical_w = (int32_t)cJSON_GetArrayItem(buffer, 2)->valuedouble;
			out->buffer_logical_h = (int32_t)cJSON_GetArrayItem(buffer, 3)->valuedouble;
			out->have_buffer = out->buffer_logical_w > 0 && out->buffer_logical_h > 0;
		}

		bool focus = false;
		u_json_get_bool(u_json_get(win, "focus"), &focus);
		out->focus = focus;

		// The monitor rect + scale are what make the logical payload
		// convertible (#1596). Schema v1 has published them since #817; they
		// were simply never read, which is why the provider could only refuse
		// a scaled monitor instead of converting it.
		out->mon_scale = 1.0f;
		const cJSON *monitor = u_json_get(win, "monitor");
		if (cJSON_IsObject(monitor)) {
			int mx = 0, my = 0, mw = 0, mh = 0;
			float scale = 1.0f;
			if (u_json_get_int(u_json_get(monitor, "x"), &mx) &&
			    u_json_get_int(u_json_get(monitor, "y"), &my) &&
			    u_json_get_int(u_json_get(monitor, "w"), &mw) &&
			    u_json_get_int(u_json_get(monitor, "h"), &mh) && mw > 0 && mh > 0) {
				out->mon_logical_x = (int32_t)mx;
				out->mon_logical_y = (int32_t)my;
				out->mon_logical_w = (int32_t)mw;
				out->mon_logical_h = (int32_t)mh;
				out->have_monitor = true;
			}
			if (u_json_get_float(u_json_get(monitor, "scale"), &scale) && scale > 0.0f) {
				out->mon_scale = scale;
			}
		}

		count++;
	}

	g->window_count = count;
	g->have_snapshot = true;
	cJSON_Delete(root);
}

static bool
wlg_request_snapshot(struct comp_vk_native_wl_geom *g, int timeout_ms);

//! Forget the cached snapshot. Callers then report "no rect" and the weave
//! falls back to display-scoped — the honest outcome once we can no longer
//! vouch for the cached origin.
static void
wlg_invalidate(struct comp_vk_native_wl_geom *g)
{
	g->window_count = 0;
	g->have_snapshot = false;
}

//! Handle one NameOwnerChanged for our well-known name. Returns true when the
//! caller should re-snapshot (the publisher just gained an owner).
static bool
wlg_handle_owner_changed(struct comp_vk_native_wl_geom *g, DBusMessage *msg)
{
	const char *name = NULL;
	const char *old_owner = NULL;
	const char *new_owner = NULL;
	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner, DBUS_TYPE_STRING,
	                           &new_owner, DBUS_TYPE_INVALID)) {
		return false;
	}
	// arg0 matching should already have filtered this, but the rule is a hint
	// to the daemon, not a guarantee about what a buggy peer can send us.
	if (name == NULL || new_owner == NULL || strcmp(name, WLG_BUS_NAME) != 0) {
		return false;
	}

	// Either direction invalidates: while the publisher was gone the window
	// may have been moved or resized, and nothing told us. Serving the stale
	// cache would weave at the pre-gap origin until the window next moves.
	wlg_invalidate(g);

	if (new_owner[0] != '\0') {
		return true;
	}

	U_LOG_W("wl_geom: geometry service " WLG_BUS_NAME
	        " went away (GNOME disables user extensions "
	        "while the screen shield is up) — dropped the cached geometry rather than weave at an "
	        "origin we can no longer vouch for; display-scoped until it returns.");
	// The name has no owner, so an immediate GetWindows buys only an error
	// reply. Hand the wait back to the bounded retry path.
	g->next_retry_ns = os_monotonic_get_ns() + WLG_RETRY_PERIOD_NS;
	return false;
}

//! Drain pending bus messages without blocking; keep the latest snapshot.
static void
wlg_pump(struct comp_vk_native_wl_geom *g)
{
	if (g->conn == NULL) {
		return;
	}

	dbus_connection_read_write(g->conn, 0);

	bool owner_appeared = false;

	DBusMessage *msg = NULL;
	while ((msg = dbus_connection_pop_message(g->conn)) != NULL) {
		if (dbus_message_is_signal(msg, WLG_IFACE, "WindowsChanged")) {
			const char *json = NULL;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) &&
			    json != NULL) {
				wlg_parse_snapshot(g, json);
			}
		} else if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
			owner_appeared |= wlg_handle_owner_changed(g, msg);
		}
		dbus_message_unref(msg);
	}

	if (!owner_appeared) {
		return;
	}

	// Re-snapshot NOW rather than waiting for the publisher's next
	// WindowsChanged: it only emits on the next geometry CHANGE, so a window
	// that moved while the service was down would otherwise stay unknown (and
	// pre-invalidation, stale) until the user happened to move it again.
	// Same bounded call the retry path already makes — the pump never blocks
	// for anything else.
	const bool ok = wlg_request_snapshot(g, 25);
	// Whether or not that answered, don't let get_window_rect's lazy retry
	// fire a second GetWindows in this same frame.
	g->next_retry_ns = os_monotonic_get_ns() + WLG_RETRY_PERIOD_NS;

	if (ok) {
		U_LOG_W("wl_geom: geometry service " WLG_BUS_NAME
		        " came back — re-snapshotted %u windows; "
		        "windowed weaving active again at the window's current origin.",
		        g->window_count);
	} else {
		U_LOG_W("wl_geom: geometry service " WLG_BUS_NAME
		        " came back but GetWindows did not answer "
		        "— display-scoped until the bounded retry succeeds.");
	}
}

//! Synchronous GetWindows — used once at create (and as a lazy retry when the
//! extension wasn't up yet), short timeout so a missing service can't stall a
//! frame for long.
static bool
wlg_request_snapshot(struct comp_vk_native_wl_geom *g, int timeout_ms)
{
	if (g->conn == NULL) {
		return false;
	}

	DBusMessage *call = dbus_message_new_method_call(WLG_BUS_NAME, WLG_OBJ_PATH, WLG_IFACE, "GetWindows");
	if (call == NULL) {
		return false;
	}

	DBusMessage *reply = dbus_connection_send_with_reply_and_block(g->conn, call, timeout_ms, NULL);
	dbus_message_unref(call);
	if (reply == NULL) {
		return false;
	}

	const char *json = NULL;
	bool ok = false;
	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) && json != NULL) {
		wlg_parse_snapshot(g, json);
		ok = g->have_snapshot;
	}
	dbus_message_unref(reply);
	return ok;
}


/*!
 * Find the extension on disk the way GNOME Shell does: the user data dir, then
 * each XDG data dir. Returns false when no copy is installed.
 */
static bool
wlg_find_installed_extension(char *out, size_t out_size)
{
	struct stat st;
	const char *data_home = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	if (data_home != NULL && data_home[0] != '\0') {
		snprintf(out, out_size, "%s/gnome-shell/extensions/" WLG_EXT_UUID, data_home);
	} else if (home != NULL && home[0] != '\0') {
		snprintf(out, out_size, "%s/.local/share/gnome-shell/extensions/" WLG_EXT_UUID, home);
	} else {
		out[0] = '\0';
	}
	if (out[0] != '\0' && stat(out, &st) == 0 && S_ISDIR(st.st_mode)) {
		return true;
	}

	const char *dirs = getenv("XDG_DATA_DIRS");
	if (dirs == NULL || dirs[0] == '\0') {
		dirs = "/usr/local/share:/usr/share";
	}
	while (*dirs != '\0') {
		const char *end = strchr(dirs, ':');
		size_t len = end != NULL ? (size_t)(end - dirs) : strlen(dirs);
		if (len > 0) {
			snprintf(out, out_size, "%.*s/gnome-shell/extensions/" WLG_EXT_UUID, (int)len, dirs);
			if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) {
				return true;
			}
		}
		if (end == NULL) {
			break;
		}
		dirs = end + 1;
	}
	out[0] = '\0';
	return false;
}

/*!
 * The one startup line for "no geometry publisher": say which of the three
 * situations this is, and what fixes it. Every cause ends in display-scoped
 * weaving, so without this line they are indistinguishable from the outside.
 */
static void
wlg_log_publisher_missing(void)
{
	const char *desktop = getenv("XDG_CURRENT_DESKTOP");
	if (desktop == NULL || strstr(desktop, "GNOME") == NULL) {
		U_LOG_W(
		    "wl_geom: no window-geometry publisher on this desktop (XDG_CURRENT_DESKTOP=%s) — only GNOME "
		    "Shell has one (the " WLG_EXT_UUID " extension). Weaving stays display-scoped.",
		    desktop != NULL ? desktop : "unset");
		return;
	}

	char path[1024];
	if (!wlg_find_installed_extension(path, sizeof(path))) {
		U_LOG_W("wl_geom: GNOME Shell extension " WLG_EXT_UUID
		        " is NOT INSTALLED — install the displayxr-runtime package "
		        "(or contrib/gnome-shell/ by hand), then log out and back in. Until then weaving stays "
		        "display-scoped and transparent apps cannot exclude themselves from capture.");
		return;
	}

	U_LOG_W("wl_geom: GNOME Shell extension " WLG_EXT_UUID
	        " is installed (%s) but NOT ACTIVE in this session. "
	        "If it was just installed or updated, LOG OUT AND BACK IN — a Wayland session cannot reload "
	        "GNOME Shell. If you disabled it: `gnome-extensions enable " WLG_EXT_UUID
	        "`, then log out and back in. (It needs GNOME Shell 45 or newer.) Weaving stays "
	        "display-scoped until it appears.",
	        path);
}


/*
 *
 * Public API.
 *
 */

struct comp_vk_native_wl_geom *
comp_vk_native_wl_geom_create(void)
{
	struct comp_vk_native_wl_geom *g = U_TYPED_CALLOC(struct comp_vk_native_wl_geom);
	if (g == NULL) {
		return NULL;
	}

	DBusError err;
	dbus_error_init(&err);
	// Private connection: shared connections must never be closed, and the
	// compositor owns this one's lifetime.
	g->conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (g->conn == NULL) {
		U_LOG_W("wl_geom: no session D-Bus (%s) — Wayland windowed weaving unavailable, staying display-scoped",
		        dbus_error_is_set(&err) ? err.message : "unknown");
		dbus_error_free(&err);
		free(g);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(g->conn, FALSE);

	dbus_bus_add_match(g->conn, WLG_MATCH_RULE, &err);
	if (dbus_error_is_set(&err)) {
		U_LOG_W("wl_geom: add_match failed (%s)", err.message);
		dbus_error_free(&err);
	}
	// Added BEFORE the first GetWindows, so a publisher that shows up during
	// or right after create is noticed by the pump rather than only by the 5 s
	// retry.
	dbus_bus_add_match(g->conn, WLG_OWNER_MATCH_RULE, &err);
	if (dbus_error_is_set(&err)) {
		U_LOG_W(
		    "wl_geom: NameOwnerChanged add_match failed (%s) — the cache will not be dropped when "
		    "the geometry service restarts (e.g. across a screen lock)",
		    err.message);
		dbus_error_free(&err);
	}
	dbus_connection_flush(g->conn);

	if (!wlg_request_snapshot(g, 200)) {
		wlg_log_publisher_missing();
		g->warned_unavailable = true;
	} else {
		U_LOG_W("wl_geom: compositor geometry service connected (%u windows) — "
		        "Wayland windowed weaving active",
		        g->window_count);
	}

	return g;
}

bool
comp_vk_native_wl_geom_get_window_rect(struct comp_vk_native_wl_geom *g, struct comp_vk_native_wl_window_rect *out_rect)
{
	if (g == NULL || g->conn == NULL || out_rect == NULL) {
		return false;
	}

	wlg_pump(g);

	if (!g->have_snapshot) {
		// Extension may have been enabled after we started. Retry the
		// blocking snapshot at most every 5 s so a missing service costs
		// one bounded call occasionally, never per frame (the common miss
		// path is the fast org.freedesktop.DBus error reply anyway, the
		// 25 ms cap is the worst case).
		const int64_t now_ns = os_monotonic_get_ns();
		if (now_ns < g->next_retry_ns) {
			return false;
		}
		g->next_retry_ns = now_ns + WLG_RETRY_PERIOD_NS;
		wlg_request_snapshot(g, 25);
		if (!g->have_snapshot) {
			return false;
		}
	}

	const int32_t pid = (int32_t)getpid();
	const struct wlg_window *best = NULL;
	for (uint32_t i = 0; i < g->window_count; i++) {
		const struct wlg_window *w = &g->windows[i];
		if (w->pid != pid || w->logical_w <= 0 || w->logical_h <= 0) {
			continue;
		}
		if (best == NULL || (w->focus && !best->focus) ||
		    (w->focus == best->focus &&
		     (int64_t)w->logical_w * w->logical_h > (int64_t)best->logical_w * best->logical_h)) {
			best = w;
		}
	}
	if (best == NULL) {
		return false;
	}

	/*
	 * THE conversion (#1596). Everything above this line is logical;
	 * everything the caller sees is device pixels.
	 *
	 * Until #1596 this is where the provider gave up instead: a rect from a
	 * monitor at any scale other than 1.0 was REFUSED (#1557), on the grounds
	 * that logical pixels cannot anchor a weave phase. True, and the wrong
	 * remedy — the payload carries the monitor rect AND Mutter's fractional
	 * scale, so the logical rect is convertible, and refusing it left the box
	 * with no present origin at all. (The other half of that refusal — that
	 * the compositor may resample the surface on its way to the panel, which
	 * no phase correction survives — is real, but it is a property of the
	 * BUFFER vs the destination, not of the origin's units. It is enforced
	 * where it belongs, in the compositor's 1:1 gate, #1595.)
	 *
	 * The monitor rect is required, not optional: without it there is no
	 * scale to apply and no way to tell whether this window is even on the 3D
	 * panel, and a rect whose space we cannot name is exactly what produced a
	 * silently wrong phase before.
	 */
	if (!best->have_monitor) {
		if (!g->warned_no_monitor) {
			g->warned_no_monitor = true;
			U_LOG_W(
			    "wl_geom: the geometry payload carries no monitor rect for this window, so its "
			    "logical coordinates cannot be converted to device pixels — display-scoped "
			    "weaving. (Mutter omits `monitor` only for a window on no monitor.)");
		}
		return false;
	}

	const struct u_wl_monitor mon = {
	    .logical_x = best->mon_logical_x,
	    .logical_y = best->mon_logical_y,
	    .logical_w = best->mon_logical_w,
	    .logical_h = best->mon_logical_h,
	    .scale = (double)best->mon_scale,
	    .mode_w = 0, // Mutter publishes the fractional scale, not the mode.
	    .mode_h = 0,
	};

	struct u_wl_rect_px win_px = {0, 0, 0, 0};
	int32_t mon_w_px = 0, mon_h_px = 0;
	if (!u_wl_window_rect_px_on_monitor(&mon, best->logical_x, best->logical_y, best->logical_w, best->logical_h,
	                                    &win_px) ||
	    !u_wl_monitor_size_px(&mon, &mon_w_px, &mon_h_px) || win_px.w <= 0 || win_px.h <= 0) {
		return false;
	}

	if (!g->warned_scale) {
		g->warned_scale = true;
		// One INFO, at the first conversion: the line that lets an
		// unattended run prove WHICH space reached the weaver. Not a WARN —
		// a scaled desktop is now a supported configuration for the phase
		// feed, not a degradation.
		U_LOG_I(
		    "wl_geom: monitor scale %.4f — window logical %d,%d %dx%d on a %dx%d logical monitor "
		    "converts to DEVICE %d,%d %dx%d on a %dx%d px monitor (#1596)",
		    (double)best->mon_scale, best->logical_x, best->logical_y, best->logical_w, best->logical_h,
		    best->mon_logical_w, best->mon_logical_h, win_px.x, win_px.y, win_px.w, win_px.h, mon_w_px,
		    mon_h_px);
	}

	out_rect->left_px = win_px.x;
	out_rect->top_px = win_px.y;
	out_rect->width_px = (uint32_t)win_px.w;
	out_rect->height_px = (uint32_t)win_px.h;
	out_rect->monitor_width_px = (uint32_t)mon_w_px;
	out_rect->monitor_height_px = (uint32_t)mon_h_px;
	out_rect->scale = best->mon_scale;

	/*
	 * The committed SURFACE, which is not necessarily the frame above. The
	 * frame is what the toplevel was configured to; the buffer rect is what
	 * the compositor actually sized the surface to from the attached buffer,
	 * its buffer scale and its viewport. They differ when the client attaches
	 * a device-pixel buffer with no wp_viewport destination and no
	 * wl_surface.set_buffer_scale: at 200 % a 3840x2160 buffer then IS a
	 * 3840x2160-logical surface, twice the output, and spills onto the next
	 * monitor while the frame still reads 1920x1080. Logged on change so a
	 * run can never again look right in the log and wrong on the glass.
	 */
	out_rect->surface_width_px = 0;
	out_rect->surface_height_px = 0;
	if (best->have_buffer) {
		out_rect->surface_width_px =
		    (uint32_t)u_wl_logical_to_px(best->buffer_logical_w, (double)best->mon_scale);
		out_rect->surface_height_px =
		    (uint32_t)u_wl_logical_to_px(best->buffer_logical_h, (double)best->mon_scale);
		if (best->buffer_logical_w != g->logged_surface_w || best->buffer_logical_h != g->logged_surface_h ||
		    best->logical_w != g->logged_frame_w || best->logical_h != g->logged_frame_h) {
			g->logged_surface_w = best->buffer_logical_w;
			g->logged_surface_h = best->buffer_logical_h;
			g->logged_frame_w = best->logical_w;
			g->logged_frame_h = best->logical_h;
			const bool match = abs(best->buffer_logical_w - best->logical_w) <= 1 &&
			                   abs(best->buffer_logical_h - best->logical_h) <= 1;
			if (match) {
				U_LOG_I("wl_geom: committed surface %dx%d logical matches the window frame %dx%d",
				        best->buffer_logical_w, best->buffer_logical_h, best->logical_w,
				        best->logical_h);
			} else {
				U_LOG_W(
				    "wl_geom: committed SURFACE is %dx%d logical but the window FRAME is %dx%d — the "
				    "attached buffer is not mapped to the configured size (the client set no "
				    "wp_viewport "
				    "destination / wl_surface.set_buffer_scale matching it), so the window on screen "
				    "is the "
				    "surface size, not the frame. (Monitor scale %.4f.)",
				    best->buffer_logical_w, best->buffer_logical_h, best->logical_w, best->logical_h,
				    (double)best->mon_scale);
			}
		}
	}
	return true;
}

void
comp_vk_native_wl_geom_destroy(struct comp_vk_native_wl_geom **g_ptr)
{
	if (g_ptr == NULL || *g_ptr == NULL) {
		return;
	}
	struct comp_vk_native_wl_geom *g = *g_ptr;
	if (g->conn != NULL) {
		dbus_connection_close(g->conn);
		dbus_connection_unref(g->conn);
	}
	free(g);
	*g_ptr = NULL;
}
