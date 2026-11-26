// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hyprland IPC replies and events: pure parsing, every platform.
 * @ingroup aux_os
 */

#include "os_hyprland.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! Copy one line (without its newline) into @p buf; returns the next line or NULL.
static const char *
next_line(const char *p, char *buf, size_t size)
{
	if (p == NULL || *p == '\0') {
		return NULL;
	}
	const char *end = strchr(p, '\n');
	size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
	if (len > 0 && p[len - 1] == '\r') {
		len--;
	}
	const size_t n = len < size - 1 ? len : size - 1;
	memcpy(buf, p, n);
	buf[n] = '\0';
	return end != NULL ? end + 1 : p + strlen(p);
}

static const char *
skip_ws(const char *s)
{
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	return s;
}

uint32_t
os_hyprland_parse_monitors_text(const char *text, struct os_hyprland_monitor *out, uint32_t max)
{
	if (text == NULL || out == NULL || max == 0) {
		return 0;
	}
	uint32_t count = 0;
	struct os_hyprland_monitor *cur = NULL;
	char line[512];
	const char *p = text;
	while ((p = next_line(p, line, sizeof(line))) != NULL) {
		// "Monitor DP-1 (ID 0):" starts a block.
		if (strncmp(line, "Monitor ", 8) == 0) {
			if (count >= max) {
				cur = NULL;
				break;
			}
			cur = &out[count++];
			memset(cur, 0, sizeof(*cur));
			cur->transform = UINT32_MAX;
			const char *name = line + 8;
			const char *sp = strchr(name, ' ');
			size_t len = sp != NULL ? (size_t)(sp - name) : strlen(name);
			if (len > 0 && name[len - 1] == ':') {
				len--;
			}
			if (len >= sizeof(cur->name)) {
				len = sizeof(cur->name) - 1;
			}
			memcpy(cur->name, name, len);
			cur->name[len] = '\0';
			continue;
		}
		if (cur == NULL) {
			continue;
		}
		const char *s = skip_ws(line);

		// "2560x1600@60.00000 at 0x0" (position may be negative).
		unsigned w = 0, h = 0;
		double hz = 0.0;
		int x = 0, y = 0;
		if (isdigit((unsigned char)s[0]) && sscanf(s, "%ux%u@%lf at %dx%d", &w, &h, &hz, &x, &y) == 5) {
			cur->mode_w = w;
			cur->mode_h = h;
			cur->refresh_mhz = hz > 0.0 ? (uint32_t)(hz * 1000.0 + 0.5) : 0;
			cur->x = x;
			cur->y = y;
			continue;
		}
		double scale = 0.0;
		if (sscanf(s, "scale: %lf", &scale) == 1) {
			cur->scale = scale > 0.0 ? scale : 0.0;
			continue;
		}
		unsigned t = 0;
		if (sscanf(s, "transform: %u", &t) == 1) {
			cur->transform = t <= 7 ? t : UINT32_MAX;
			continue;
		}
		if (strncmp(s, "disabled: ", 10) == 0) {
			cur->disabled = strncmp(s + 10, "true", 4) == 0;
			continue;
		}
	}

	// Drop blocks with no mode line (a format we do not understand).
	uint32_t kept = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (out[i].mode_w > 0 && out[i].mode_h > 0 && out[i].name[0] != '\0') {
			if (kept != i) {
				out[kept] = out[i];
			}
			kept++;
		}
	}
	return kept;
}

bool
os_hyprland_event_moves_windows(const char *line)
{
	if (line == NULL || *line == '\0') {
		return false;
	}
	const char *end = strstr(line, ">>");
	const size_t len = end != NULL ? (size_t)(end - line) : strlen(line);

	// Events that change nothing about where a window is or whether it shows.
	// Everything else (including events newer than this list) re-queries.
	static const char *const quiet[] = {
	    "windowtitle",     "windowtitlev2", "activelayout", "submap", "screencast", "screencastv2",
	    "urgent",          "bell",          "ignoregrouplock", "lockgroups", "renameworkspace",
	    "createworkspace", "createworkspacev2", "destroyworkspace", "destroyworkspacev2", "custom",
	};
	for (size_t i = 0; i < sizeof(quiet) / sizeof(quiet[0]); i++) {
		if (strlen(quiet[i]) == len && strncmp(line, quiet[i], len) == 0) {
			return false;
		}
	}
	return true;
}

bool
os_hyprland_session_detected(const char *xdg_current_desktop, const char *instance_signature)
{
	if (instance_signature != NULL && instance_signature[0] != '\0') {
		return true;
	}
	const char *p = xdg_current_desktop;
	while (p != NULL && *p != '\0') {
		const char *end = strchr(p, ':');
		const size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
		if (len == 8) {
			char low[9];
			for (size_t i = 0; i < 8; i++) {
				low[i] = (char)tolower((unsigned char)p[i]);
			}
			low[8] = '\0';
			if (strcmp(low, "hyprland") == 0) {
				return true;
			}
		}
		p = end != NULL ? end + 1 : NULL;
	}
	return false;
}
