// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1277 P2(a) — the weave satellite's overlay ownership lease.
 * @ingroup aux_util
 *
 * See u_overlay_lease.h for the contract. Nothing in this file calls into the
 * compositor, the platform, or IPC: the policy is a pure function and the
 * mechanism is a five-entry table plus an optional backend. It has no callers
 * yet — it is the API surface the P2(a) plan is written against, landed ahead
 * of the arbiter so the shape can be reviewed on its own.
 */

#include "util/u_overlay_lease.h"

#include <string.h>


/*
 *
 * Policy — pure, host-tested.
 *
 */

const char *
u_overlay_lease_reason_string(enum u_overlay_lease_reason reason)
{
	switch (reason) {
	case U_OVERLAY_LEASE_REASON_NONE: return "none";
	case U_OVERLAY_LEASE_REASON_ONLY_CANDIDATE: return "only-candidate";
	case U_OVERLAY_LEASE_REASON_SCALED: return "scaled";
	case U_OVERLAY_LEASE_REASON_FOCUS: return "focus";
	case U_OVERLAY_LEASE_REASON_INCUMBENT: return "incumbent";
	case U_OVERLAY_LEASE_REASON_LOWEST_SLOT: return "lowest-slot";
	default: return "?";
	}
}

int
u_overlay_lease_select(const struct u_overlay_lease_candidate *candidates,
                       size_t count,
                       enum u_overlay_lease_reason *out_reason)
{
	enum u_overlay_lease_reason reason = U_OVERLAY_LEASE_REASON_NONE;
	int winner = -1;
	size_t wanting = 0;
	size_t any_scaled = 0;
	bool scaled_only = false;

	if (candidates == NULL) {
		count = 0;
	}

	// Rule 1 + 2: only real candidates count, and none means nobody holds it.
	for (size_t i = 0; i < count; i++) {
		if (!candidates[i].wants_overlay) {
			continue;
		}
		wanting++;
		if (candidates[i].container_scaled) {
			any_scaled++;
		}
		if (winner < 0) {
			winner = (int)i; // provisional, so rule 3 needs no second pass
		}
	}
	if (wanting == 0) {
		goto out;
	}

	// Rule 3: one candidate takes it unconditionally, scaled or not. This is
	// what preserves the shipped single-client behaviour exactly — a lone
	// fullscreen browser is unscaled and still gets the overlay.
	if (wanting == 1) {
		reason = U_OVERLAY_LEASE_REASON_ONLY_CANDIDATE;
		goto out;
	}

	// Rule 4: with a rival present, only what the overlay actually FIXES is
	// entitled to it. An unscaled window weaves correctly in its own surface.
	scaled_only = any_scaled > 0;

	// Rule 5: among equals — focus, then incumbency, then the lowest slot.
	// Evaluated as three ranked passes so the result never depends on the
	// order the caller happened to build the array in.
	winner = -1;
	for (size_t i = 0; i < count; i++) {
		const struct u_overlay_lease_candidate *c = &candidates[i];
		if (!c->wants_overlay || (scaled_only && !c->container_scaled)) {
			continue;
		}
		if (c->focused) {
			winner = (int)i;
			reason = scaled_only && any_scaled == 1 ? U_OVERLAY_LEASE_REASON_SCALED
			                                        : U_OVERLAY_LEASE_REASON_FOCUS;
			goto out;
		}
	}
	for (size_t i = 0; i < count; i++) {
		const struct u_overlay_lease_candidate *c = &candidates[i];
		if (!c->wants_overlay || (scaled_only && !c->container_scaled)) {
			continue;
		}
		if (c->incumbent) {
			winner = (int)i;
			reason = scaled_only && any_scaled == 1 ? U_OVERLAY_LEASE_REASON_SCALED
			                                        : U_OVERLAY_LEASE_REASON_INCUMBENT;
			goto out;
		}
	}
	for (size_t i = 0; i < count; i++) {
		const struct u_overlay_lease_candidate *c = &candidates[i];
		if (!c->wants_overlay || (scaled_only && !c->container_scaled)) {
			continue;
		}
		if (winner < 0 || c->slot < candidates[winner].slot) {
			winner = (int)i;
		}
	}
	if (winner >= 0) {
		reason =
		    scaled_only && any_scaled == 1 ? U_OVERLAY_LEASE_REASON_SCALED : U_OVERLAY_LEASE_REASON_LOWEST_SLOT;
	}

out:
	if (out_reason != NULL) {
		*out_reason = reason;
	}
	return winner;
}


/*
 *
 * Mechanism — a table and an optional backend.
 *
 */

//! Static storage: zero-initialised, i.e. no backend, i.e. always-grant.
static struct u_overlay_lease_backend u_overlay_lease_backend_g;
static bool u_overlay_lease_have_backend_g = false;
static bool u_overlay_lease_held_g[U_OVERLAY_LEASE_MAX_SLOTS];

//! Slot to table index, or a negative value for "not a slot we track".
static int
u_overlay_lease_index(int32_t slot)
{
	const int index = (int)slot + 1; // U_OVERLAY_LEASE_SLOT_MAIN (-1) is index 0
	if (index < 0 || index >= U_OVERLAY_LEASE_MAX_SLOTS) {
		return -1;
	}
	return index;
}

void
u_overlay_lease_set_backend(const struct u_overlay_lease_backend *backend)
{
	if (backend == NULL) {
		memset(&u_overlay_lease_backend_g, 0, sizeof(u_overlay_lease_backend_g));
		u_overlay_lease_have_backend_g = false;
		return;
	}
	u_overlay_lease_backend_g = *backend;
	u_overlay_lease_have_backend_g = true;
}

bool
u_overlay_lease_acquire(int32_t slot, const struct u_overlay_lease_candidate *self)
{
	const int index = u_overlay_lease_index(slot);
	if (index < 0) {
		return false;
	}
	if (u_overlay_lease_held_g[index]) {
		return true;
	}

	bool granted = true; // No backend: always grant. See the header.
	if (u_overlay_lease_have_backend_g && u_overlay_lease_backend_g.acquire != NULL) {
		granted = u_overlay_lease_backend_g.acquire(u_overlay_lease_backend_g.data, slot, self);
	}

	u_overlay_lease_held_g[index] = granted;
	return granted;
}

void
u_overlay_lease_release(int32_t slot)
{
	const int index = u_overlay_lease_index(slot);
	if (index < 0 || !u_overlay_lease_held_g[index]) {
		return;
	}

	// Clear the local flag FIRST: the backend's release is synchronous and may
	// hand the overlay straight to another process, so this slot must already
	// have stopped considering itself the holder when that happens.
	u_overlay_lease_held_g[index] = false;

	if (u_overlay_lease_have_backend_g && u_overlay_lease_backend_g.release != NULL) {
		u_overlay_lease_backend_g.release(u_overlay_lease_backend_g.data, slot);
	}
}

bool
u_overlay_lease_held(int32_t slot)
{
	const int index = u_overlay_lease_index(slot);
	return index >= 0 && u_overlay_lease_held_g[index];
}
