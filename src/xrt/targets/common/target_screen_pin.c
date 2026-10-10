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
first_with_id(const struct target_screen_candidate *c, uint32_t n, const char *id, bool ignore_case)
{
	if (id == NULL || id[0] == '\0') {
		return -1;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (c[i].plugin_id == NULL) {
			continue;
		}
		if (ignore_case ? name_eq(c[i].plugin_id, id) : strcmp(c[i].plugin_id, id) == 0) {
			return (int)i;
		}
	}
	return -1;
}

int
target_screen_pick_ex(const struct target_screen_candidate *cands,
                      uint32_t count,
                      const char *pin_plugin,
                      const char *screen_pref,
                      const char *preferred,
                      enum target_screen_pick_reason *out_reason,
                      bool *out_pin_unclaimed,
                      bool *out_pref_unclaimed)
{
	enum target_screen_pick_reason reason = TARGET_SCREEN_PICK_NONE;
	int pick = -1;
	bool pin_unclaimed = false;
	bool pref_unclaimed = false;
	const bool have_pin = pin_plugin != NULL && pin_plugin[0] != '\0';
	const bool have_pref = screen_pref != NULL && screen_pref[0] != '\0';

	if (cands != NULL && count > 0) {
		if (have_pin) {
			// A pin is typed by hand: its plug-in id matches
			// case-insensitively, like its monitor name.
			pick = first_with_id(cands, count, pin_plugin, true);
			if (pick >= 0) {
				reason = TARGET_SCREEN_PICK_PIN;
			} else {
				pin_unclaimed = true;
			}
		}
		if (have_pref) {
			// Looked up even when a pin decided, so a preference whose
			// plug-in has no claim on the monitor is always reported.
			const int p = first_with_id(cands, count, screen_pref, true);
			if (p < 0) {
				pref_unclaimed = true;
			} else if (pick < 0) {
				pick = p;
				reason = TARGET_SCREEN_PICK_SCREEN_PREF;
			}
		}
		if (pick < 0) {
			pick = first_with_id(cands, count, preferred, false);
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
	} else {
		pin_unclaimed = have_pin;
		pref_unclaimed = have_pref;
	}

	if (out_reason != NULL) {
		*out_reason = reason;
	}
	if (out_pin_unclaimed != NULL) {
		*out_pin_unclaimed = pin_unclaimed;
	}
	if (out_pref_unclaimed != NULL) {
		*out_pref_unclaimed = pref_unclaimed;
	}
	return pick;
}

int
target_screen_pick(const struct target_screen_candidate *cands,
                   uint32_t count,
                   const char *pin_plugin,
                   const char *preferred,
                   enum target_screen_pick_reason *out_reason,
                   bool *out_pin_unclaimed)
{
	return target_screen_pick_ex(cands, count, pin_plugin, NULL, preferred, out_reason, out_pin_unclaimed, NULL);
}

bool
target_screen_active_decides_every_monitor(const char *active_id,
                                           const char *preferred,
                                           const struct target_screen_pins *pins,
                                           const struct target_screen_monitor *mons,
                                           uint32_t count)
{
	if (active_id == NULL || active_id[0] == '\0' || mons == NULL || count == 0) {
		return false;
	}
	if (preferred != NULL && preferred[0] != '\0' && strcmp(preferred, active_id) != 0) {
		return false; // a different plug-in is preferred: it outranks the active one
	}
	for (uint32_t i = 0; i < count; i++) {
		if (!mons[i].active_claims) {
			return false; // another plug-in may claim it
		}
		const int pin =
		    target_screen_pin_find(pins, mons[i].monitor_id, mons[i].output_name, mons[i].connector);
		if (pin >= 0 && !name_eq(pins->pin[pin].plugin_id, active_id)) {
			return false; // a pin names another plug-in for this monitor
		}
		if (mons[i].screen_pref != NULL && mons[i].screen_pref[0] != '\0' &&
		    !name_eq(mons[i].screen_pref, active_id)) {
			return false; // a per-screen preference names another plug-in
		}
	}
	return true;
}

void
target_screen_pnp_code(uint16_t manufacturer_id, char out[4])
{
	const uint16_t v = (uint16_t)((manufacturer_id >> 8) | (manufacturer_id << 8)); // big-endian spec value
	const int c0 = ((v >> 10) & 0x1F) + 'A' - 1;
	const int c1 = ((v >> 5) & 0x1F) + 'A' - 1;
	const int c2 = (v & 0x1F) + 'A' - 1;
	out[0] = (c0 >= 'A' && c0 <= 'Z') ? (char)c0 : '?';
	out[1] = (c1 >= 'A' && c1 <= 'Z') ? (char)c1 : '?';
	out[2] = (c2 >= 'A' && c2 <= 'Z') ? (char)c2 : '?';
	out[3] = '\0';
}

static void
key_base(const struct target_screen_key_input *m, char *out, size_t cap)
{
	char pnp[4];
	target_screen_pnp_code(m->manufacturer_id, pnp);
	(void)snprintf(out, cap, "%s-%04X-%08X", pnp, (unsigned)m->product_id, (unsigned)m->serial);
}

void
target_screen_keys_build(const struct target_screen_key_input *in, uint32_t n, char (*out)[TARGET_SCREEN_KEY_MAX])
{
	if (in == NULL || out == NULL) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		char base[TARGET_SCREEN_KEY_MAX];
		key_base(&in[i], base, sizeof(base));
		bool qualify = in[i].serial == 0;
		for (uint32_t j = 0; j < n && !qualify; j++) {
			if (j == i) {
				continue;
			}
			char other[TARGET_SCREEN_KEY_MAX];
			key_base(&in[j], other, sizeof(other));
			qualify = strcmp(base, other) == 0;
		}
		const char *dev = in[i].device_name;
		// Windows GDI names carry the `\\.\` device namespace; the key
		// keeps only `DISPLAYn`, so it can be typed in any shell.
		if (dev != NULL && strncmp(dev, "\\\\.\\", 4) == 0) {
			dev += 4;
		}
		if (qualify && dev != NULL && dev[0] != '\0') {
			const int room = (int)(TARGET_SCREEN_KEY_MAX - 2 - strlen(base));
			(void)snprintf(out[i], TARGET_SCREEN_KEY_MAX, "%s@%.*s", base, room > 0 ? room : 0, dev);
		} else {
			(void)snprintf(out[i], TARGET_SCREEN_KEY_MAX, "%s", base);
		}
	}
}
