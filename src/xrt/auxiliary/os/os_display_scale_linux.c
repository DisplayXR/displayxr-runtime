// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Linux desktop: pair each RandR monitor with its DRM/KMS connector
 *         and solve for the X11 global scale (see os_display_scale.h).
 * @ingroup aux_os
 *
 * ## Why sysfs and not libdrm
 *
 * `/sys/class/drm/card<N>-<connector>/{status,modes}` is world-readable and
 * needs no device open, no DRM master and no new dependency — the same
 * no-extra-DSO discipline `os_display_desktop_x11.c` keeps by dlopen'ing Xlib.
 * libdrm would give the CRTC's *current* mode exactly; sysfs gives the mode
 * LIST, first entry preferred. The gap is closed by one rule below, chosen so
 * that it can only err toward "device pixels" (the pre-existing assumption),
 * never toward a false "quantized":
 *
 *   if the X11 size of an output is one of its connector's modes, that IS its
 *   native size; otherwise the preferred mode is.
 *
 * A user running a panel at a non-preferred mode therefore does not trip a
 * false alarm, and an X11 size no hardware mode can produce (3456x2160 on a
 * 2880x1800 laptop) is exactly the evidence of scaling we are looking for.
 *
 * ## Connector names
 *
 * The kernel calls the DS1's port `HDMI-A-1`; Mutter, and therefore XWayland's
 * RandR, calls it `HDMI-1`. Names are compared after dropping that one-letter
 * connector-subtype segment, which is the only difference observed.
 */

#include "os/os_display_scale.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define DRM_SYSFS_ROOT "/sys/class/drm"
#define DRM_MAX_CONNECTORS 32
#define DRM_MAX_MODES 64

struct drm_connector
{
	char name[64]; // normalised, e.g. "HDMI-1"
	uint32_t mode_w[DRM_MAX_MODES];
	uint32_t mode_h[DRM_MAX_MODES];
	uint32_t mode_count;
};

/*!
 * "HDMI-A-1" -> "HDMI-1", "DP-2" -> "DP-2". Removes a single uppercase letter
 * segment that sits between the type and the index.
 */
static void
normalise_connector_name(const char *in, char *out, size_t out_size)
{
	size_t o = 0;
	for (size_t i = 0; in[i] != '\0' && o + 1 < out_size; i++) {
		if (in[i] == '-' && in[i + 1] >= 'A' && in[i + 1] <= 'Z' && in[i + 2] == '-' && in[i + 3] >= '0' &&
		    in[i + 3] <= '9') {
			i += 1; // skip "-X", keep the following "-<digit>"
			continue;
		}
		out[o++] = in[i];
	}
	out[o] = '\0';
}

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
		normalise_connector_name(dash + 1, c->name, sizeof(c->name));

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

bool
os_display_scale_query(struct os_display_scale_report *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->verdict.culprit = -1;

	struct os_display_desktop_info mons[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	const uint32_t mon_count = os_display_desktop_enumerate(mons, OS_DISPLAY_DESKTOP_MAX_MONITORS);
	if (mon_count == 0) {
		return false;
	}

	struct drm_connector drm[DRM_MAX_CONNECTORS];
	const uint32_t drm_count = enumerate_drm(drm, DRM_MAX_CONNECTORS);
	out->drm_available = drm_count > 0;

	int64_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
	for (uint32_t i = 0; i < mon_count; i++) {
		const struct os_display_desktop_info *m = &mons[i];
		struct u_x11_output_sizes *o = &out->outputs[out->output_count++];
		snprintf(o->name, sizeof(o->name), "%s", m->device_name);
		o->x11_w = m->width;
		o->x11_h = m->height;

		const int64_t l = m->left, t = m->top, r = l + (int64_t)m->width, b = t + (int64_t)m->height;
		if (i == 0 || l < min_x)
			min_x = l;
		if (i == 0 || t < min_y)
			min_y = t;
		if (i == 0 || r > max_x)
			max_x = r;
		if (i == 0 || b > max_y)
			max_y = b;

		char norm[64];
		normalise_connector_name(m->device_name, norm, sizeof(norm));
		for (uint32_t k = 0; k < drm_count; k++) {
			if (strcmp(norm, drm[k].name) != 0) {
				continue;
			}
			// Preferred mode unless the X11 size is itself a mode.
			o->native_w = drm[k].mode_w[0];
			o->native_h = drm[k].mode_h[0];
			for (uint32_t j = 0; j < drm[k].mode_count; j++) {
				if (drm[k].mode_w[j] == o->x11_w && drm[k].mode_h[j] == o->x11_h) {
					o->native_w = o->x11_w;
					o->native_h = o->x11_h;
					break;
				}
			}
			out->native_sum_w += o->native_w;
			break;
		}
	}
	out->root_w = (uint32_t)(max_x - min_x);
	out->root_h = (uint32_t)(max_y - min_y);

	u_x11_scale_solve(out->outputs, out->output_count, &out->verdict);
	return true;
}

const char *
os_display_scale_state_str(enum u_x11_scale_state state)
{
	switch (state) {
	case U_X11_SCALE_DEVICE_PIXELS: return "device pixels";
	case U_X11_SCALE_QUANTIZED: return "quantized";
	case U_X11_SCALE_UNKNOWN:
	default: return "unknown";
	}
}
