// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Minimized-window cache with pins. See u_desktop_mini_cache.h.
 */

#include "u_desktop_mini_cache.h"

#include <string.h>

static void
apply(struct u_desktop_mini_cache *c, const struct u_desktop_mini_pin *p)
{
	uint32_t i = 0;
	while (i < c->n && c->w[i].id != p->id) {
		i++;
	}
	if (p->present) {
		if (i == c->n) {
			if (c->n == U_DESKTOP_MINI_CACHE_MAX) {
				return; // full: the next snapshot decides
			}
			c->n++;
		}
		c->w[i] = p->w;
		c->w[i].state |= U_DESKTOP_WINDOW_MINIMIZED;
	} else if (i < c->n) {
		memmove(&c->w[i], &c->w[i + 1], sizeof(c->w[0]) * (c->n - i - 1));
		c->n--;
	}
}

void
u_desktop_mini_cache_pin(struct u_desktop_mini_cache *c,
                         uint64_t id,
                         bool present,
                         const struct u_desktop_window *w,
                         uint64_t now_ns)
{
	if (id == 0 || (present && w == NULL)) {
		return;
	}
	struct u_desktop_mini_pin p;
	memset(&p, 0, sizeof(p));
	p.id = id;
	p.present = present;
	p.at_ns = now_ns;
	if (present) {
		p.w = *w;
		p.w.id = id;
	}
	// One pin per window: the newest outcome wins.
	uint32_t j = 0;
	for (uint32_t i = 0; i < c->pin_n; i++) {
		if (c->pin[i].id != id) {
			c->pin[j++] = c->pin[i];
		}
	}
	c->pin_n = j;
	if (c->pin_n == U_DESKTOP_MINI_CACHE_PINS) { // full: drop the oldest
		memmove(&c->pin[0], &c->pin[1], sizeof(c->pin[0]) * (c->pin_n - 1));
		c->pin_n--;
	}
	c->pin[c->pin_n++] = p;
	apply(c, &p);
}

static bool
snapshot_has(const struct u_desktop_window *snap, uint32_t n, uint64_t id)
{
	for (uint32_t i = 0; i < n; i++) {
		if (snap[i].id == id) {
			return true;
		}
	}
	return false;
}

void
u_desktop_mini_cache_publish(struct u_desktop_mini_cache *c,
                             const struct u_desktop_window *snap,
                             uint32_t n,
                             uint64_t started_ns)
{
	if (n > U_DESKTOP_MINI_CACHE_MAX) {
		n = U_DESKTOP_MINI_CACHE_MAX;
	}
	// Pins are judged against the snapshot as delivered, before it is merged.
	uint32_t j = 0;
	for (uint32_t i = 0; i < c->pin_n; i++) {
		const struct u_desktop_mini_pin *p = &c->pin[i];
		bool agrees = snapshot_has(snap, n, p->id) == p->present;
		bool young = p->at_ns + U_DESKTOP_MINI_CACHE_PIN_TTL_NS >= started_ns;
		if (!agrees && young) {
			c->pin[j++] = *p; // the sweep missed it (started before, or a stale app table)
		}
	}
	c->pin_n = j;
	if (n > 0) {
		memcpy(c->w, snap, sizeof(c->w[0]) * n);
	}
	c->n = n;
	c->published_ns = started_ns;
	for (uint32_t i = 0; i < c->pin_n; i++) {
		apply(c, &c->pin[i]);
	}
}

uint32_t
u_desktop_mini_cache_get(const struct u_desktop_mini_cache *c, struct u_desktop_window *out, uint32_t cap)
{
	uint32_t n = c->n < cap ? c->n : cap;
	if (n > 0) {
		memcpy(out, c->w, sizeof(out[0]) * n);
	}
	return n;
}
