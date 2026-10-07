// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop Linux EDID enumeration: RandR monitors joined to DRM sysfs
 *         connectors (multi-screen M0, #69). See os_display_edid_linux.h.
 * @ingroup aux_os
 *
 * No libdrm and no new link dependency: sysfs is plain files, and RandR comes
 * from `os_display_desktop_enumerate`, which dlopens Xlib (aux_os no-DSO rule).
 * No logging here either (aux_os sits below u_logging); the runtime logs the
 * join per monitor where it turns these records into descriptors
 * (`target_plugin_build_descriptors`).
 */

#include "os_display_edid_linux.h"
#include "os_display_connector_linux.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRM_SYSFS_ROOT "/sys/class/drm"
#define EDID_MAX_READ 512


/*
 *
 * EDID.
 *
 */

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

bool
os_display_edid_parse(const uint8_t *edid, size_t len, struct os_display_edid_parsed *out)
{
	static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (edid == NULL || len < 128 || memcmp(edid, header, sizeof(header)) != 0) {
		return false;
	}

	out->manufacturer_id = le16(&edid[8]);
	out->product_id = le16(&edid[10]);
	out->serial_number =
	    (uint32_t)edid[12] | ((uint32_t)edid[13] << 8) | ((uint32_t)edid[14] << 16) | ((uint32_t)edid[15] << 24);

	// Bytes 21/22: max image size in cm. One of them 0 means the other is an
	// aspect ratio (EDID 1.4), not a size — report nothing then.
	if (edid[21] != 0 && edid[22] != 0) {
		out->cm_width_mm = (uint32_t)edid[21] * 10u;
		out->cm_height_mm = (uint32_t)edid[22] * 10u;
	}

	// First detailed timing descriptor (bytes 54-71). Pixel clock 0 means it
	// is a display descriptor (name, range limits...) instead — eDP panels
	// often carry their timing in a DisplayID extension and leave this empty.
	const uint8_t *d = &edid[54];
	const uint32_t clock_10khz = le16(&d[0]);
	if (clock_10khz != 0) {
		const uint32_t h_active = (uint32_t)d[2] | ((uint32_t)(d[4] & 0xF0) << 4);
		const uint32_t h_blank = (uint32_t)d[3] | ((uint32_t)(d[4] & 0x0F) << 8);
		const uint32_t v_active = (uint32_t)d[5] | ((uint32_t)(d[7] & 0xF0) << 4);
		const uint32_t v_blank = (uint32_t)d[6] | ((uint32_t)(d[7] & 0x0F) << 8);
		out->dtd_width_px = h_active;
		out->dtd_height_px = v_active;
		out->dtd_width_mm = (uint32_t)d[12] | ((uint32_t)(d[14] & 0xF0) << 4);
		out->dtd_height_mm = (uint32_t)d[13] | ((uint32_t)(d[14] & 0x0F) << 8);
		const uint64_t total = (uint64_t)(h_active + h_blank) * (uint64_t)(v_active + v_blank);
		if (total > 0) {
			out->dtd_refresh_mhz =
			    (uint32_t)(((uint64_t)clock_10khz * 10000ull * 1000ull + total / 2) / total);
		}
	}

	return true;
}

void
os_display_edid_parsed_mm(const struct os_display_edid_parsed *p, uint32_t *out_w_mm, uint32_t *out_h_mm)
{
	uint32_t w = 0;
	uint32_t h = 0;
	if (p != NULL) {
		if (p->dtd_width_mm > 0 && p->dtd_height_mm > 0) {
			w = p->dtd_width_mm;
			h = p->dtd_height_mm;
		} else {
			w = p->cm_width_mm;
			h = p->cm_height_mm;
		}
	}
	*out_w_mm = w;
	*out_h_mm = h;
}


/*
 *
 * DRM sysfs.
 *
 */

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

static int
compare_connector_name(const void *a, const void *b)
{
	return strcmp(((const struct os_display_drm_connector *)a)->name,
	              ((const struct os_display_drm_connector *)b)->name);
}

uint32_t
os_display_drm_read_connectors(const char *root, struct os_display_drm_connector *out, uint32_t max)
{
	if (root == NULL || out == NULL || max == 0) {
		return 0;
	}
	DIR *dir = opendir(root);
	if (dir == NULL) {
		return 0;
	}

	uint32_t count = 0;
	struct dirent *e;
	while ((e = readdir(dir)) != NULL && count < max) {
		// "card<N>-<connector>"; skip "card<N>", "renderD<N>", "version".
		if (strncmp(e->d_name, "card", 4) != 0) {
			continue;
		}
		const char *dash = strchr(e->d_name, '-');
		if (dash == NULL || dash[1] == '\0') {
			continue;
		}

		char path[512];
		char line[64];
		(void)snprintf(path, sizeof(path), "%s/%s/status", root, e->d_name);
		if (!read_first_line(path, line, sizeof(line)) || strncmp(line, "connected", 9) != 0) {
			continue;
		}

		struct os_display_drm_connector *c = &out[count];
		memset(c, 0, sizeof(*c));
		// Keep the card prefix: two GPUs can each have an HDMI-A-1.
		(void)snprintf(c->name, sizeof(c->name), "%.31s", e->d_name);

		// Absent on old kernels: treat as enabled, the status said connected.
		(void)snprintf(path, sizeof(path), "%s/%s/enabled", root, e->d_name);
		c->enabled = !read_first_line(path, line, sizeof(line)) || strncmp(line, "disabled", 8) != 0;

		(void)snprintf(path, sizeof(path), "%s/%s/edid", root, e->d_name);
		FILE *f = fopen(path, "rb");
		if (f != NULL) {
			uint8_t buf[EDID_MAX_READ];
			const size_t n = fread(buf, 1, sizeof(buf), f);
			fclose(f);
			c->has_edid = os_display_edid_parse(buf, n, &c->edid);
		}

		(void)snprintf(path, sizeof(path), "%s/%s/modes", root, e->d_name);
		f = fopen(path, "r");
		if (f != NULL) {
			while (c->mode_count < OS_DISPLAY_DRM_MAX_MODES && fgets(line, sizeof(line), f) != NULL) {
				unsigned w = 0, h = 0;
				if (sscanf(line, "%ux%u", &w, &h) == 2 && w > 0 && h > 0) {
					c->mode_w[c->mode_count] = w;
					c->mode_h[c->mode_count] = h;
					c->mode_count++;
				}
			}
			fclose(f);
		}

		count++;
	}
	closedir(dir);

	// readdir order is filesystem order; DRM-only records and the
	// first-candidate tie-breaks must not depend on it.
	qsort(out, count, sizeof(out[0]), compare_connector_name);
	return count;
}


/*
 *
 * The join.
 *
 */

static uint32_t
abs_diff(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

static bool
mm_close(uint32_t aw, uint32_t ah, uint32_t bw, uint32_t bh)
{
	return aw > 0 && ah > 0 && bw > 0 && bh > 0 && abs_diff(aw, bw) <= OS_DISPLAY_EDID_JOIN_MM_TOLERANCE &&
	       abs_diff(ah, bh) <= OS_DISPLAY_EDID_JOIN_MM_TOLERANCE;
}

//! Does connector @p c match RandR's mm? Either EDID size source counts.
static bool
drm_matches_mm(const struct os_display_drm_connector *c, uint32_t w_mm, uint32_t h_mm)
{
	if (!c->has_edid) {
		return false;
	}
	return mm_close(c->edid.dtd_width_mm, c->edid.dtd_height_mm, w_mm, h_mm) ||
	       mm_close(c->edid.cm_width_mm, c->edid.cm_height_mm, w_mm, h_mm);
}

static bool
drm_has_mode(const struct os_display_drm_connector *c, uint32_t w, uint32_t h)
{
	for (uint32_t i = 0; i < c->mode_count; i++) {
		if (c->mode_w[i] == w && c->mode_h[i] == h) {
			return true;
		}
	}
	return false;
}

//! The connector name the way RandR spells it: no card prefix, no subtype
//! letter ("card1-HDMI-A-1" -> "HDMI-1").
static void
drm_name_for_randr(const struct os_display_drm_connector *c, char *out, size_t size)
{
	const char *n = c->name;
	if (strncmp(n, "card", 4) == 0) {
		const char *dash = strchr(n, '-');
		if (dash != NULL) {
			n = dash + 1;
		}
	}
	os_display_connector_normalise(n, out, size);
}

/*!
 * The monitor's device mode, from a source that is independent of the DRM
 * name match: the compositor's current mode, else the RandR rect. The
 * DRM-derived native mode is NOT used — it was itself found by connector
 * name, so checking a name match against it would be circular.
 */
static void
randr_device_mode(const struct os_display_desktop_info *r, uint32_t *w, uint32_t *h)
{
	if (r->native_source == OS_DISPLAY_NATIVE_SOURCE_COMPOSITOR && r->native_width > 0 && r->native_height > 0) {
		*w = r->native_width;
		*h = r->native_height;
	} else {
		*w = r->width;
		*h = r->height;
	}
}

static bool
edid_same_identity(const struct os_display_edid_parsed *a, const struct os_display_edid_parsed *b)
{
	return a->manufacturer_id == b->manufacturer_id && a->product_id == b->product_id &&
	       a->serial_number == b->serial_number;
}

//! Copy an EDID's identity (ids, serial, mm) onto a record.
static void
apply_edid(struct os_display_edid_monitor *m, const struct os_display_edid_parsed *e)
{
	m->manufacturer_id = e->manufacturer_id;
	m->product_id = e->product_id;
	m->serial_number = e->serial_number;
	uint32_t w_mm = 0, h_mm = 0;
	os_display_edid_parsed_mm(e, &w_mm, &h_mm);
	if (w_mm > 0 && h_mm > 0) {
		m->physical_width_mm = w_mm;
		m->physical_height_mm = h_mm;
	}
}

//! Copy a connector's identity (EDID, mm, connector name) onto a record.
static void
apply_drm(struct os_display_edid_monitor *m, const struct os_display_drm_connector *c)
{
	(void)snprintf(m->connector, sizeof(m->connector), "%s", c->name);
	if (c->has_edid) {
		apply_edid(m, &c->edid);
	}
	if (m->native_width == 0 && c->mode_count > 0) {
		// The kernel lists the preferred mode first; the RandR rect when it
		// is itself a mode (the device mode on an unscaled X screen).
		m->native_width = c->mode_w[0];
		m->native_height = c->mode_h[0];
		if (drm_has_mode(c, m->pixel_width, m->pixel_height)) {
			m->native_width = m->pixel_width;
			m->native_height = m->pixel_height;
		}
	}
	// The preferred timing's refresh, only when it is the mode in use. The
	// current refresh is not in sysfs; this is the best a file read can do.
	if (c->has_edid && c->edid.dtd_refresh_mhz > 0 && c->edid.dtd_width_px == m->native_width &&
	    c->edid.dtd_height_px == m->native_height) {
		m->refresh_hz = (c->edid.dtd_refresh_mhz + 500u) / 1000u;
	}
}

uint32_t
os_display_edid_linux_join(const struct os_display_desktop_info *randr,
                           const struct os_display_randr_identity *randr_id,
                           uint32_t randr_count,
                           const struct os_display_drm_connector *drm,
                           uint32_t drm_count,
                           struct os_display_edid_monitor *out,
                           uint32_t max)
{
	if (out == NULL || max == 0) {
		return 0;
	}
	if (randr == NULL) {
		randr_count = 0;
	}
	if (drm == NULL) {
		drm_count = 0;
	}
	if (drm_count > OS_DISPLAY_DRM_MAX_CONNECTORS) {
		drm_count = OS_DISPLAY_DRM_MAX_CONNECTORS;
	}

	// No placement source: one record per enabled connector, origin unknown.
	if (randr_count == 0) {
		uint32_t n = 0;
		for (uint32_t j = 0; j < drm_count && n < max; j++) {
			if (!drm[j].enabled) {
				continue;
			}
			struct os_display_edid_monitor *m = &out[n++];
			memset(m, 0, sizeof(*m));
			m->origin_unknown = true;
			m->join = OS_EDID_JOIN_DRM_ONLY;
			if (drm[j].mode_count > 0) {
				m->pixel_width = drm[j].mode_w[0];
				m->pixel_height = drm[j].mode_h[0];
			}
			apply_drm(m, &drm[j]);
		}
		return n;
	}

	uint32_t rn = randr_count < max ? randr_count : max;
	if (rn > OS_DISPLAY_DESKTOP_MAX_MONITORS) {
		rn = OS_DISPLAY_DESKTOP_MAX_MONITORS;
	}
	bool used[OS_DISPLAY_DRM_MAX_CONNECTORS] = {0};
	int32_t pick[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	enum os_display_edid_join how[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	for (uint32_t i = 0; i < rn; i++) {
		pick[i] = -1;
		how[i] = OS_EDID_JOIN_NONE;
	}

	char rname[OS_DISPLAY_DESKTOP_MAX_MONITORS][64];
	for (uint32_t i = 0; i < rn; i++) {
		os_display_connector_normalise(randr[i].device_name, rname[i], sizeof(rname[i]));
	}

	// Pass 0: the X server's own EDID. The identity is settled; find the
	// connector carrying the same EDID, by name among several identical ones.
	for (uint32_t i = 0; i < rn; i++) {
		if (randr_id == NULL || !randr_id[i].valid) {
			continue;
		}
		how[i] = OS_EDID_JOIN_RANDR_EDID;
		int32_t found = -1;
		uint32_t hits = 0;
		int32_t by_name = -1;
		for (uint32_t j = 0; j < drm_count; j++) {
			if (used[j] || !drm[j].enabled || !drm[j].has_edid ||
			    !edid_same_identity(&drm[j].edid, &randr_id[i].edid)) {
				continue;
			}
			found = (int32_t)j;
			hits++;
			char dname[64];
			drm_name_for_randr(&drm[j], dname, sizeof(dname));
			if (rname[i][0] != '\0' && strcmp(rname[i], dname) == 0) {
				by_name = (int32_t)j;
			}
		}
		const int32_t take = hits == 1 ? found : by_name;
		if (take >= 0) {
			pick[i] = take;
			used[take] = true;
		}
	}

	// Pass 1: names, confirmed by the device mode or the physical size.
	for (uint32_t i = 0; i < rn; i++) {
		if (how[i] != OS_EDID_JOIN_NONE || rname[i][0] == '\0') {
			continue;
		}
		uint32_t w = 0, h = 0;
		randr_device_mode(&randr[i], &w, &h);
		for (uint32_t j = 0; j < drm_count; j++) {
			char dname[64];
			drm_name_for_randr(&drm[j], dname, sizeof(dname));
			if (used[j] || strcmp(rname[i], dname) != 0) {
				continue;
			}
			if (drm_has_mode(&drm[j], w, h) ||
			    drm_matches_mm(&drm[j], randr[i].physical_width_mm, randr[i].physical_height_mm)) {
				// Same name on two cards: the one that agrees physically.
				pick[i] = (int32_t)j;
				how[i] = OS_EDID_JOIN_NAME;
				used[j] = true;
				break;
			}
		}
	}

	// Pass 2: physical size, only when exactly one unused enabled connector fits.
	for (uint32_t i = 0; i < rn; i++) {
		if (how[i] != OS_EDID_JOIN_NONE || randr[i].physical_width_mm == 0 ||
		    randr[i].physical_height_mm == 0) {
			continue;
		}
		int32_t found = -1;
		uint32_t hits = 0;
		for (uint32_t j = 0; j < drm_count; j++) {
			if (!used[j] && drm[j].enabled &&
			    drm_matches_mm(&drm[j], randr[i].physical_width_mm, randr[i].physical_height_mm)) {
				found = (int32_t)j;
				hits++;
			}
		}
		if (hits == 1) {
			pick[i] = found;
			how[i] = OS_EDID_JOIN_MM;
			used[found] = true;
		}
	}

	// Pass 3: device mode, only when exactly one unused enabled connector has it.
	for (uint32_t i = 0; i < rn; i++) {
		if (how[i] != OS_EDID_JOIN_NONE) {
			continue;
		}
		uint32_t w = 0, h = 0;
		randr_device_mode(&randr[i], &w, &h);
		int32_t found = -1;
		uint32_t hits = 0;
		for (uint32_t j = 0; j < drm_count; j++) {
			if (!used[j] && drm[j].enabled && drm_has_mode(&drm[j], w, h)) {
				found = (int32_t)j;
				hits++;
			}
		}
		if (hits == 1) {
			pick[i] = found;
			how[i] = OS_EDID_JOIN_MODE;
			used[found] = true;
		}
	}

	for (uint32_t i = 0; i < rn; i++) {
		const struct os_display_desktop_info *r = &randr[i];
		struct os_display_edid_monitor *m = &out[i];
		memset(m, 0, sizeof(*m));
		m->screen_left = r->left;
		m->screen_top = r->top;
		m->pixel_width = r->width;
		m->pixel_height = r->height;
		m->is_primary = r->is_primary;
		m->native_width = r->native_width;
		m->native_height = r->native_height;
		m->physical_width_mm = r->physical_width_mm;
		m->physical_height_mm = r->physical_height_mm;
		(void)snprintf(m->output_name, sizeof(m->output_name), "%.31s", r->device_name);
		m->join = how[i];
		if (pick[i] >= 0) {
			apply_drm(m, &drm[pick[i]]);
		}
		// The X server's EDID wins over the connector's: it is this
		// output's by construction, the connector's only by the join.
		if (randr_id != NULL && randr_id[i].valid) {
			apply_edid(m, &randr_id[i].edid);
		}
	}
	return rn;
}


/*
 *
 * Public entry points.
 *
 */

bool
os_display_edid_enumerate(struct os_display_edid_list *out_list)
{
	if (out_list == NULL) {
		return false;
	}
	memset(out_list, 0, sizeof(*out_list));

	struct os_display_desktop_info randr[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	const uint32_t rn = os_display_desktop_enumerate(randr, OS_DISPLAY_DESKTOP_MAX_MONITORS);

	// The X server's own EDID per monitor, when it publishes one (native X).
	struct os_display_randr_identity randr_id[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	memset(randr_id, 0, sizeof(randr_id));
	bool have_randr_id = false;
	if (rn > 0) {
		struct os_display_randr_edid blobs[OS_DISPLAY_DESKTOP_MAX_MONITORS];
		const uint32_t bn = os_display_x11_read_monitor_edids(blobs, OS_DISPLAY_DESKTOP_MAX_MONITORS);
		for (uint32_t i = 0; i < rn; i++) {
			for (uint32_t k = 0; k < bn; k++) {
				if (blobs[k].len > 0 && strcmp(blobs[k].name, randr[i].device_name) == 0) {
					randr_id[i].valid =
					    os_display_edid_parse(blobs[k].edid, blobs[k].len, &randr_id[i].edid);
					have_randr_id = have_randr_id || randr_id[i].valid;
					break;
				}
			}
		}
	}

	struct os_display_drm_connector drm[OS_DISPLAY_DRM_MAX_CONNECTORS];
	const uint32_t dn = os_display_drm_read_connectors(DRM_SYSFS_ROOT, drm, OS_DISPLAY_DRM_MAX_CONNECTORS);

	for (uint32_t j = 0; j < dn; j++) {
		if (drm[j].has_edid) {
			out_list->diag_edid_read_count++;
		}
	}

	out_list->count = os_display_edid_linux_join(rn > 0 ? randr : NULL, have_randr_id ? randr_id : NULL, rn, drm,
	                                             dn, out_list->monitors, OS_DISPLAY_EDID_MAX_MONITORS);

	// diag_gdi_count is "monitors the placement source saw" — RandR here.
	out_list->diag_gdi_count = rn;
	if (out_list->count == 0) {
		out_list->diag_error = OS_EDID_DIAG_NO_GDI_MONITORS;
	} else if (out_list->diag_edid_read_count == 0 && !have_randr_id) {
		out_list->diag_error = OS_EDID_DIAG_NO_EDID_DATA;
	}

	return out_list->count > 0;
}

const struct os_display_edid_monitor *
os_display_edid_find_in_table(const struct os_display_edid_list *list, const uint16_t table[][2], uint32_t table_len)
{
	if (list == NULL || table == NULL || table_len == 0) {
		return NULL;
	}
	for (uint32_t m = 0; m < list->count; m++) {
		for (uint32_t t = 0; t < table_len; t++) {
			if (list->monitors[m].manufacturer_id == table[t][0] &&
			    list->monitors[m].product_id == table[t][1]) {
				return &list->monitors[m];
			}
		}
	}
	return NULL;
}
