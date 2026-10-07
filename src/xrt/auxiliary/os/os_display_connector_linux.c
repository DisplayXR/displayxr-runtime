// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop Linux: the device mode (and compositor scale) behind each
 *         X11 monitor, by connector name (#1831). See
 *         os_display_connector_linux.h.
 * @ingroup aux_os
 *
 * ## Why both sources
 *
 * Mutter's DisplayConfig is the authority on a GNOME session: it knows the
 * CURRENT mode and the logical monitor's scale, for real connectors and for
 * virtual ones alike. It is a session-bus call, so it is absent on other
 * desktops and in sandboxes without the bus. DRM sysfs is world-readable, needs
 * nothing but the kernel, and covers every compositor — but it lists modes
 * rather than naming the current one, and knows nothing about scale.
 *
 * ## Why libdbus is dlopen'd, with no dbus headers
 *
 * aux_os ends up inside arbitrary host processes and must not add a DSO to
 * their link line (see the rule at the top of its CMakeLists) — the same reason
 * `os_display_desktop_x11.c` dlopens Xlib. The handful of entry points used here
 * are declared locally, so the file builds on a box with no libdbus-1-dev; the
 * one struct the API makes us allocate (DBusMessageIter) is given an oversized,
 * pointer-aligned buffer, which is all libdbus requires of it.
 */

#include "os_display_connector_linux.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 *
 * Connector names.
 *
 */

void
os_display_connector_normalise(const char *in, char *out, size_t out_size)
{
	if (out == NULL || out_size == 0) {
		return;
	}
	size_t o = 0;
	for (size_t i = 0; in != NULL && in[i] != '\0' && o + 1 < out_size; i++) {
		if (in[i] == '-' && in[i + 1] >= 'A' && in[i + 1] <= 'Z' && in[i + 2] == '-' && in[i + 3] >= '0' &&
		    in[i + 3] <= '9') {
			i += 1; // skip "-X", keep the following "-<digit>"
			continue;
		}
		out[o++] = in[i];
	}
	out[o] = '\0';
}


/*
 *
 * Source 1: Mutter DisplayConfig over the session bus.
 *
 */

#define MAX_CONNECTORS 32
#define MUTTER_TIMEOUT_MS 500

struct connector_mode
{
	char name[64]; // normalised
	uint32_t w, h;
	double scale;         // 0 = unknown
	uint32_t refresh_mhz; // current mode's refresh, 0 = unknown
};

// Local declarations of the libdbus ABI used here (stable since dbus 1.0).
typedef struct os_dbus_connection os_dbus_connection;
typedef struct os_dbus_message os_dbus_message;
typedef struct
{
	// sizeof(DBusMessageIter) is 72 on LP64; libdbus only needs at least that
	// much pointer-aligned storage.
	void *storage[16];
} os_dbus_iter;

#define OS_DBUS_TYPE_INVALID 0
#define OS_DBUS_TYPE_ARRAY 'a'
#define OS_DBUS_TYPE_STRUCT 'r'
#define OS_DBUS_TYPE_DICT_ENTRY 'e'
#define OS_DBUS_TYPE_VARIANT 'v'
#define OS_DBUS_TYPE_STRING 's'
#define OS_DBUS_TYPE_INT32 'i'
#define OS_DBUS_TYPE_DOUBLE 'd'
#define OS_DBUS_TYPE_BOOLEAN 'b'

struct dbus_fns
{
	void *lib;
	os_dbus_connection *(*connection_open_private)(const char *, void *);
	uint32_t (*bus_register)(os_dbus_connection *, void *);
	void (*connection_close)(os_dbus_connection *);
	void (*connection_unref)(os_dbus_connection *);
	void (*connection_set_exit_on_disconnect)(os_dbus_connection *, uint32_t);
	os_dbus_message *(*message_new_method_call)(const char *, const char *, const char *, const char *);
	os_dbus_message *(*connection_send_with_reply_and_block)(os_dbus_connection *, os_dbus_message *, int, void *);
	void (*message_unref)(os_dbus_message *);
	uint32_t (*message_iter_init)(os_dbus_message *, os_dbus_iter *);
	uint32_t (*message_iter_next)(os_dbus_iter *);
	int (*message_iter_get_arg_type)(os_dbus_iter *);
	void (*message_iter_recurse)(os_dbus_iter *, os_dbus_iter *);
	void (*message_iter_get_basic)(os_dbus_iter *, void *);
};

static bool
dbus_fns_load(struct dbus_fns *f)
{
	memset(f, 0, sizeof(*f));
	// NODELETE: libdbus keeps process-global state, so it must never be
	// unmapped under a host that may also be using it.
	f->lib = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
	if (f->lib == NULL) {
		return false;
	}
#define LOAD(member, name)                                                                                             \
	do {                                                                                                           \
		*(void **)(&f->member) = dlsym(f->lib, name);                                                          \
		if (f->member == NULL) {                                                                               \
			dlclose(f->lib);                                                                               \
			memset(f, 0, sizeof(*f));                                                                      \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)
	LOAD(connection_open_private, "dbus_connection_open_private");
	LOAD(bus_register, "dbus_bus_register");
	LOAD(connection_close, "dbus_connection_close");
	LOAD(connection_unref, "dbus_connection_unref");
	LOAD(connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
	LOAD(message_new_method_call, "dbus_message_new_method_call");
	LOAD(connection_send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
	LOAD(message_unref, "dbus_message_unref");
	LOAD(message_iter_init, "dbus_message_iter_init");
	LOAD(message_iter_next, "dbus_message_iter_next");
	LOAD(message_iter_get_arg_type, "dbus_message_iter_get_arg_type");
	LOAD(message_iter_recurse, "dbus_message_iter_recurse");
	LOAD(message_iter_get_basic, "dbus_message_iter_get_basic");
#undef LOAD
	return true;
}

/*!
 * The session bus address WITHOUT autolaunch: `dbus_bus_get()` would spawn
 * `dbus-launch` on a box with no bus, which a display query has no business
 * doing inside someone else's process.
 */
static bool
session_bus_address(char *out, size_t size)
{
	const char *env = getenv("DBUS_SESSION_BUS_ADDRESS");
	if (env != NULL && env[0] != '\0') {
		(void)snprintf(out, size, "%s", env);
		return true;
	}
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (rt == NULL || rt[0] == '\0') {
		return false;
	}
	char path[512];
	(void)snprintf(path, sizeof(path), "%s/bus", rt);
	if (access(path, F_OK) != 0) {
		return false;
	}
	(void)snprintf(out, size, "unix:path=%s", path);
	return true;
}

//! Read the next basic value of @p type from @p it, advancing it.
static bool
iter_take(struct dbus_fns *f, os_dbus_iter *it, int type, void *out)
{
	if (f->message_iter_get_arg_type(it) != type) {
		return false;
	}
	f->message_iter_get_basic(it, out);
	f->message_iter_next(it);
	return true;
}

//! Read a `(ssss)` monitor spec; returns the connector name.
static bool
read_spec_connector(struct dbus_fns *f, os_dbus_iter *spec, const char **out_connector)
{
	if (f->message_iter_get_arg_type(spec) != OS_DBUS_TYPE_STRUCT) {
		return false;
	}
	os_dbus_iter s;
	f->message_iter_recurse(spec, &s);
	return iter_take(f, &s, OS_DBUS_TYPE_STRING, out_connector);
}

//! In an `a{sv}`, is boolean key @p key true?
static bool
dict_bool(struct dbus_fns *f, os_dbus_iter *dict_array, const char *key)
{
	if (f->message_iter_get_arg_type(dict_array) != OS_DBUS_TYPE_ARRAY) {
		return false;
	}
	os_dbus_iter d;
	f->message_iter_recurse(dict_array, &d);
	while (f->message_iter_get_arg_type(&d) == OS_DBUS_TYPE_DICT_ENTRY) {
		os_dbus_iter e;
		f->message_iter_recurse(&d, &e);
		const char *k = NULL;
		if (iter_take(f, &e, OS_DBUS_TYPE_STRING, &k) && k != NULL && strcmp(k, key) == 0 &&
		    f->message_iter_get_arg_type(&e) == OS_DBUS_TYPE_VARIANT) {
			os_dbus_iter v;
			f->message_iter_recurse(&e, &v);
			uint32_t b = 0;
			return iter_take(f, &v, OS_DBUS_TYPE_BOOLEAN, &b) && b != 0;
		}
		f->message_iter_next(&d);
	}
	return false;
}

static struct connector_mode *
find_connector(struct connector_mode *cm, uint32_t count, const char *norm)
{
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(cm[i].name, norm) == 0) {
			return &cm[i];
		}
	}
	return NULL;
}

/*!
 * `GetCurrentState() -> (u serial,
 *    a((ssss) a(siiddada{sv}) a{sv}) monitors,
 *    a(iiduba(ssss)a{sv}) logical_monitors,
 *    a{sv} properties)`
 *
 * Monitors give each connector's current mode (the mode whose properties carry
 * `is-current`); logical monitors give the scale each connector is painted at.
 */
static uint32_t
query_mutter(struct connector_mode *out, uint32_t max)
{
	char address[600];
	if (!session_bus_address(address, sizeof(address))) {
		return 0;
	}

	struct dbus_fns f;
	if (!dbus_fns_load(&f)) {
		return 0;
	}

	uint32_t count = 0;
	os_dbus_connection *conn = f.connection_open_private(address, NULL);
	if (conn == NULL) {
		goto out_lib;
	}
	f.connection_set_exit_on_disconnect(conn, 0);
	if (!f.bus_register(conn, NULL)) {
		goto out_conn;
	}

	os_dbus_message *call =
	    f.message_new_method_call("org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
	                              "org.gnome.Mutter.DisplayConfig", "GetCurrentState");
	if (call == NULL) {
		goto out_conn;
	}
	os_dbus_message *reply = f.connection_send_with_reply_and_block(conn, call, MUTTER_TIMEOUT_MS, NULL);
	f.message_unref(call);
	if (reply == NULL) {
		goto out_conn; // not GNOME, or Mutter did not answer in time
	}

	os_dbus_iter top;
	if (!f.message_iter_init(reply, &top)) {
		goto out_reply;
	}
	f.message_iter_next(&top); // serial

	// Physical monitors: connector + current mode.
	if (f.message_iter_get_arg_type(&top) == OS_DBUS_TYPE_ARRAY) {
		os_dbus_iter mons;
		f.message_iter_recurse(&top, &mons);
		while (f.message_iter_get_arg_type(&mons) == OS_DBUS_TYPE_STRUCT && count < max) {
			os_dbus_iter mon;
			f.message_iter_recurse(&mons, &mon);
			const char *connector = NULL;
			if (read_spec_connector(&f, &mon, &connector) && connector != NULL) {
				f.message_iter_next(&mon); // past the spec, to the modes array
				uint32_t cw = 0, ch = 0;
				double crefresh = 0.0;
				if (f.message_iter_get_arg_type(&mon) == OS_DBUS_TYPE_ARRAY) {
					os_dbus_iter modes;
					f.message_iter_recurse(&mon, &modes);
					while (f.message_iter_get_arg_type(&modes) == OS_DBUS_TYPE_STRUCT) {
						os_dbus_iter m;
						f.message_iter_recurse(&modes, &m);
						const char *id = NULL;
						int32_t w = 0, h = 0;
						double refresh = 0, pref = 0;
						if (iter_take(&f, &m, OS_DBUS_TYPE_STRING, &id) &&
						    iter_take(&f, &m, OS_DBUS_TYPE_INT32, &w) &&
						    iter_take(&f, &m, OS_DBUS_TYPE_INT32, &h) &&
						    iter_take(&f, &m, OS_DBUS_TYPE_DOUBLE, &refresh) &&
						    iter_take(&f, &m, OS_DBUS_TYPE_DOUBLE, &pref)) {
							f.message_iter_next(&m); // supported scales
							if (dict_bool(&f, &m, "is-current") && w > 0 && h > 0) {
								cw = (uint32_t)w;
								ch = (uint32_t)h;
								crefresh = refresh;
							}
						}
						f.message_iter_next(&modes);
					}
				}
				if (cw > 0 && ch > 0) {
					struct connector_mode *c = &out[count++];
					memset(c, 0, sizeof(*c));
					os_display_connector_normalise(connector, c->name, sizeof(c->name));
					c->w = cw;
					c->h = ch;
					c->refresh_mhz = crefresh > 0.0 ? (uint32_t)(crefresh * 1000.0 + 0.5) : 0u;
				}
			}
			f.message_iter_next(&mons);
		}
	}
	f.message_iter_next(&top);

	// Logical monitors: the scale each connector is painted at.
	if (f.message_iter_get_arg_type(&top) == OS_DBUS_TYPE_ARRAY) {
		os_dbus_iter lms;
		f.message_iter_recurse(&top, &lms);
		while (f.message_iter_get_arg_type(&lms) == OS_DBUS_TYPE_STRUCT) {
			os_dbus_iter lm;
			f.message_iter_recurse(&lms, &lm);
			int32_t x = 0, y = 0;
			double scale = 0.0;
			if (iter_take(&f, &lm, OS_DBUS_TYPE_INT32, &x) && iter_take(&f, &lm, OS_DBUS_TYPE_INT32, &y) &&
			    iter_take(&f, &lm, OS_DBUS_TYPE_DOUBLE, &scale)) {
				f.message_iter_next(&lm); // transform
				f.message_iter_next(&lm); // primary
				if (f.message_iter_get_arg_type(&lm) == OS_DBUS_TYPE_ARRAY) {
					os_dbus_iter specs;
					f.message_iter_recurse(&lm, &specs);
					while (f.message_iter_get_arg_type(&specs) == OS_DBUS_TYPE_STRUCT) {
						const char *connector = NULL;
						if (read_spec_connector(&f, &specs, &connector) && connector != NULL) {
							char norm[64];
							os_display_connector_normalise(connector, norm, sizeof(norm));
							struct connector_mode *c = find_connector(out, count, norm);
							if (c != NULL && scale > 0.0) {
								c->scale = scale;
							}
						}
						f.message_iter_next(&specs);
					}
				}
			}
			f.message_iter_next(&lms);
		}
	}

out_reply:
	f.message_unref(reply);
out_conn:
	f.connection_close(conn);
	f.connection_unref(conn);
out_lib:
	dlclose(f.lib);
	return count;
}


/*
 *
 * Source 2: DRM/KMS connectors in sysfs.
 *
 */

#define DRM_SYSFS_ROOT "/sys/class/drm"
#define DRM_MAX_MODES 64

struct drm_connector
{
	char name[64]; // normalised, e.g. "HDMI-1"
	uint32_t mode_w[DRM_MAX_MODES];
	uint32_t mode_h[DRM_MAX_MODES];
	uint32_t mode_count;
};

static bool
read_first_line(const char *path, char *buf, size_t size)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return false;
	}
	const bool ok = fgets(buf, (int)size, f) != NULL;
	fclose(f);
	return ok;
}

static uint32_t
enumerate_drm(struct drm_connector *out, uint32_t max)
{
	DIR *d = opendir(DRM_SYSFS_ROOT);
	if (d == NULL) {
		return 0;
	}
	uint32_t count = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL && count < max) {
		// Connector entries are "card<N>-<connector>"; skip "card<N>",
		// "renderD<N>", "version".
		if (strncmp(e->d_name, "card", 4) != 0) {
			continue;
		}
		const char *dash = strchr(e->d_name, '-');
		if (dash == NULL) {
			continue;
		}

		char path[512];
		char line[64];
		snprintf(path, sizeof(path), DRM_SYSFS_ROOT "/%s/status", e->d_name);
		if (!read_first_line(path, line, sizeof(line)) || strncmp(line, "connected", 9) != 0) {
			continue;
		}

		struct drm_connector *c = &out[count];
		memset(c, 0, sizeof(*c));
		os_display_connector_normalise(dash + 1, c->name, sizeof(c->name));

		snprintf(path, sizeof(path), DRM_SYSFS_ROOT "/%s/modes", e->d_name);
		FILE *f = fopen(path, "r");
		if (f == NULL) {
			continue;
		}
		while (c->mode_count < DRM_MAX_MODES && fgets(line, sizeof(line), f) != NULL) {
			unsigned w = 0, h = 0;
			if (sscanf(line, "%ux%u", &w, &h) == 2 && w > 0 && h > 0) {
				c->mode_w[c->mode_count] = w;
				c->mode_h[c->mode_count] = h;
				c->mode_count++;
			}
		}
		fclose(f);
		if (c->mode_count > 0) {
			count++;
		}
	}
	closedir(d);
	return count;
}


/*
 *
 * The join.
 *
 */

void
os_display_connector_annotate(struct os_display_desktop_info *mons, uint32_t count)
{
	if (mons == NULL || count == 0) {
		return;
	}

	struct connector_mode mutter[MAX_CONNECTORS];
	const uint32_t mutter_count = query_mutter(mutter, MAX_CONNECTORS);

	struct drm_connector drm[MAX_CONNECTORS];
	const uint32_t drm_count = enumerate_drm(drm, MAX_CONNECTORS);

	for (uint32_t i = 0; i < count; i++) {
		struct os_display_desktop_info *m = &mons[i];
		char norm[64];
		os_display_connector_normalise(m->device_name, norm, sizeof(norm));
		if (norm[0] == '\0') {
			continue;
		}

		const struct connector_mode *mc = find_connector(mutter, mutter_count, norm);
		if (mc != NULL) {
			m->native_width = mc->w;
			m->native_height = mc->h;
			m->scale = mc->scale;
			m->native_refresh_mhz = mc->refresh_mhz;
			m->native_source = OS_DISPLAY_NATIVE_SOURCE_COMPOSITOR;
			continue;
		}

		for (uint32_t k = 0; k < drm_count; k++) {
			if (strcmp(norm, drm[k].name) != 0) {
				continue;
			}
			// Preferred mode unless the X11 size is itself a mode. Errs only
			// toward "the X11 rect is device pixels" — the pre-existing
			// assumption — never toward a false "scaled".
			m->native_width = drm[k].mode_w[0];
			m->native_height = drm[k].mode_h[0];
			for (uint32_t j = 0; j < drm[k].mode_count; j++) {
				if (drm[k].mode_w[j] == m->width && drm[k].mode_h[j] == m->height) {
					m->native_width = m->width;
					m->native_height = m->height;
					break;
				}
			}
			m->native_source = OS_DISPLAY_NATIVE_SOURCE_DRM;
			break;
		}
	}
}
