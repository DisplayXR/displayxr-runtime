// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen plug-in pin + per-monitor winner rule — see target_screen_pin.h.
 * @ingroup targets_common
 */

#include "target_screen_pin.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void
copy_trimmed(char *dst, size_t cap, const char *b, const char *e)
{
	while (b < e && isspace((unsigned char)*b)) {
		b++;
	}
	while (e > b && isspace((unsigned char)e[-1])) {
		e--;
	}
	size_t n = (size_t)(e - b);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, b, n);
	dst[n] = '\0';
}

uint32_t
target_screen_pin_parse(const char *spec, struct target_screen_pins *out, uint32_t *out_malformed)
{
	uint32_t bad = 0;
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	if (spec == NULL || out == NULL) {
		if (out_malformed != NULL) {
			*out_malformed = 0;
		}
		return 0;
	}
	const char *p = spec;
	while (*p != '\0') {
		const char *end = strchr(p, ',');
		if (end == NULL) {
			end = p + strlen(p);
		}
		const char *eq = memchr(p, '=', (size_t)(end - p));
		char match[TARGET_SCREEN_PIN_STR];
		char id[TARGET_SCREEN_PIN_STR];
		match[0] = id[0] = '\0';
		if (eq != NULL) {
			copy_trimmed(match, sizeof(match), p, eq);
			copy_trimmed(id, sizeof(id), eq + 1, end);
		}
		bool blank = true;
		for (const char *q = p; q < end; q++) {
			if (!isspace((unsigned char)*q)) {
				blank = false;
				break;
			}
		}
		if (!blank) {
			if (eq == NULL || match[0] == '\0' || id[0] == '\0' || out->count >= TARGET_SCREEN_PIN_MAX) {
				bad++;
			} else {
				struct target_screen_pin *pin = &out->pin[out->count++];
				snprintf(pin->match, sizeof(pin->match), "%s", match);
				snprintf(pin->plugin_id, sizeof(pin->plugin_id), "%s", id);
			}
		}
		p = (*end == ',') ? end + 1 : end;
	}
	if (out_malformed != NULL) {
		*out_malformed = bad;
	}
	return out->count;
}

static bool
name_eq(const char *a, const char *b)
{
	if (a == NULL || b == NULL || a[0] == '\0' || b[0] == '\0') {
		return false;
	}
	for (; *a != '\0' && *b != '\0'; a++, b++) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
			return false;
		}
	}
	return *a == '\0' && *b == '\0';
}

bool
target_screen_pin_matches(const struct target_screen_pin *pin,
                          uint64_t monitor_id,
                          const char *output_name,
                          const char *connector)
{
	if (pin == NULL || pin->match[0] == '\0') {
		return false;
	}
	if (name_eq(pin->match, output_name) || name_eq(pin->match, connector)) {
		return true;
	}
	// A monitor id in hex, `0x` optional; the whole string must be hex.
	const char *h = pin->match;
	if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) {
		h += 2;
	}
	if (*h == '\0' || strlen(h) > 16) {
		return false;
	}
	for (const char *q = h; *q != '\0'; q++) {
		if (!isxdigit((unsigned char)*q)) {
			return false;
		}
	}
	return strtoull(h, NULL, 16) == monitor_id && monitor_id != 0;
}

int
target_screen_pin_find(const struct target_screen_pins *pins,
                       uint64_t monitor_id,
                       const char *output_name,
                       const char *connector)
{
	if (pins == NULL) {
		return -1;
	}
	for (uint32_t i = 0; i < pins->count && i < TARGET_SCREEN_PIN_MAX; i++) {
		if (target_screen_pin_matches(&pins->pin[i], monitor_id, output_name, connector)) {
			return (int)i;
		}
	}
	return -1;
}

static int
first_with_id(const struct target_screen_candidate *c, uint32_t n, const char *id)
{
	if (id == NULL || id[0] == '\0') {
		return -1;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (c[i].plugin_id != NULL && strcmp(c[i].plugin_id, id) == 0) {
			return (int)i;
		}
	}
	return -1;
}

int
target_screen_pick(const struct target_screen_candidate *cands,
                   uint32_t count,
                   const char *pin_plugin,
                   const char *preferred,
                   enum target_screen_pick_reason *out_reason,
                   bool *out_pin_unclaimed)
{
	enum target_screen_pick_reason reason = TARGET_SCREEN_PICK_NONE;
	int pick = -1;
	bool pin_unclaimed = false;

	if (cands != NULL && count > 0) {
		if (pin_plugin != NULL && pin_plugin[0] != '\0') {
			pick = first_with_id(cands, count, pin_plugin);
			if (pick >= 0) {
				reason = TARGET_SCREEN_PICK_PIN;
			} else {
				pin_unclaimed = true;
			}
		}
		if (pick < 0) {
			pick = first_with_id(cands, count, preferred);
			if (pick >= 0) {
				reason = TARGET_SCREEN_PICK_PREFERRED;
			}
		}
		if (pick < 0) {
			for (uint32_t i = 0; i < count; i++) {
				if (cands[i].is_active) {
					pick = (int)i;
					reason = TARGET_SCREEN_PICK_ACTIVE;
					break;
				}
			}
		}
		if (pick < 0) {
			for (uint32_t i = 0; i < count; i++) {
				// Strict '>': a tie keeps the earlier (lower ProbeOrder) source.
				if (pick < 0 || cands[i].confidence > cands[pick].confidence) {
					pick = (int)i;
				}
			}
			reason = TARGET_SCREEN_PICK_CONFIDENCE;
		}
	} else if (pin_plugin != NULL && pin_plugin[0] != '\0') {
		pin_unclaimed = true;
	}

	if (out_reason != NULL) {
		*out_reason = reason;
	}
	if (out_pin_unclaimed != NULL) {
		*out_pin_unclaimed = pin_unclaimed;
	}
	return pick;
}
