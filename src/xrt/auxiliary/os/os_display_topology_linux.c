// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux topology read: the DRM connectors in sysfs — see
 *         os_display_topology.h.
 * @ingroup aux_os
 *
 * `status` and `enabled` are cached attributes: reading them does not make
 * the kernel probe the connector (only writing `status` does), so this is a
 * handful of tiny file reads. The service's own feeder
 * (`service_drm_hotplug_linux.c`) hashes the same `status` files.
 */

#include "os_display_topology.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DRM_CLASS_DIR "/sys/class/drm"

static int
cmp_entry(const void *a, const void *b)
{
	return strcmp(((const struct os_display_topology_entry *)a)->key,
	              ((const struct os_display_topology_entry *)b)->key);
}

static void
read_attr(const char *conn, const char *attr, char *buf, size_t size)
{
	buf[0] = '\0';
	char path[256];
	(void)snprintf(path, sizeof(path), DRM_CLASS_DIR "/%s/%s", conn, attr);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return;
	}
	ssize_t r = read(fd, buf, size - 1);
	buf[r > 0 ? r : 0] = '\0';
	close(fd);
}

uint32_t
os_display_topology_read(struct os_display_topology_entry *out, uint32_t max)
{
	if (out == NULL || max == 0) {
		return 0;
	}
	DIR *d = opendir(DRM_CLASS_DIR);
	if (d == NULL) {
		return 0;
	}
	uint32_t n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL && n < max) {
		// Connectors only: "card1-eDP-1", not "card1" / "renderD128" / "version".
		const size_t nl = strlen(e->d_name);
		if (strncmp(e->d_name, "card", 4) != 0 || strchr(e->d_name, '-') == NULL || nl >= sizeof(out[n].key)) {
			continue;
		}
		memset(&out[n], 0, sizeof(out[n]));
		memcpy(out[n].key, e->d_name, nl + 1);
		n++;
	}
	closedir(d);
	qsort(out, n, sizeof(out[0]), cmp_entry);

	for (uint32_t i = 0; i < n; i++) {
		char status[32];
		char enabled[32];
		read_attr(out[i].key, "status", status, sizeof(status));
		read_attr(out[i].key, "enabled", enabled, sizeof(enabled));
		uint32_t st = 0;
		if (strncmp(status, "connected", 9) == 0) {
			st |= 1u;
		} else if (strncmp(status, "disconnected", 12) != 0) {
			st |= 2u; // "unknown" (or unreadable)
		}
		if (strncmp(enabled, "enabled", 7) == 0) {
			st |= 4u;
		}
		out[i].state = st;
	}
	return n;
}
