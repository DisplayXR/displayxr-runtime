// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The id table behind XR_DXR_weave v21 weave targets (XrWeaveTargetDXR,
 *         browser-pvt#288): several present-owner windows on one session.
 *
 * A weave client names each of its windows with a caller-chosen 64-bit id. Id
 * 0 is the session's DEFAULT window — every pre-v21 caller — and is never
 * stored here: its state lives where it always lived. Every other id maps to a
 * slot of a small fixed table, so the per-window state next to it (the
 * service engine's per-window engine pointer, the OpenXR state tracker's
 * per-window output-export latch) can be a plain array.
 *
 * The table is a bare array of ids where 0 marks a free slot. Pure and
 * lock-free by itself: each user holds its own lock (or relies on its own
 * single-thread rule) around a lookup and the slot state it then touches.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! No slot: the id is unknown (find), or the table is full (claim).
#define U_WEAVE_TARGET_NO_SLOT (-1)

/*!
 * The slot holding @p id, or U_WEAVE_TARGET_NO_SLOT. Id 0 (the default target,
 * never stored) is never found.
 *
 * @ingroup aux_util
 */
static inline int
u_weave_target_find(const uint64_t *ids, uint32_t cap, uint64_t id)
{
	if (id == 0) {
		return U_WEAVE_TARGET_NO_SLOT;
	}
	for (uint32_t k = 0; k < cap; k++) {
		if (ids[k] == id) {
			return (int)k;
		}
	}
	return U_WEAVE_TARGET_NO_SLOT;
}

/*!
 * The slot of @p id, claiming the lowest free one when it has none yet
 * (@p out_created then reports true, so the caller initialises the slot's
 * state). U_WEAVE_TARGET_NO_SLOT when the table is full or @p id is 0 (the
 * default target has no slot).
 *
 * @ingroup aux_util
 */
static inline int
u_weave_target_claim(uint64_t *ids, uint32_t cap, uint64_t id, bool *out_created)
{
	if (out_created != NULL) {
		*out_created = false;
	}
	if (id == 0) {
		return U_WEAVE_TARGET_NO_SLOT;
	}
	int free_k = U_WEAVE_TARGET_NO_SLOT;
	for (uint32_t k = 0; k < cap; k++) {
		if (ids[k] == id) {
			return (int)k;
		}
		if (ids[k] == 0 && free_k == U_WEAVE_TARGET_NO_SLOT) {
			free_k = (int)k;
		}
	}
	if (free_k != U_WEAVE_TARGET_NO_SLOT) {
		ids[free_k] = id;
		if (out_created != NULL) {
			*out_created = true;
		}
	}
	return free_k;
}

/*!
 * Free the slot of @p id. Returns the slot it held (whose state the caller
 * now tears down), or U_WEAVE_TARGET_NO_SLOT when it held none.
 *
 * @ingroup aux_util
 */
static inline int
u_weave_target_release(uint64_t *ids, uint32_t cap, uint64_t id)
{
	const int k = u_weave_target_find(ids, cap, id);
	if (k != U_WEAVE_TARGET_NO_SLOT) {
		ids[k] = 0;
	}
	return k;
}

/*!
 * How many slots are in use.
 *
 * @ingroup aux_util
 */
static inline uint32_t
u_weave_target_count(const uint64_t *ids, uint32_t cap)
{
	uint32_t n = 0;
	for (uint32_t k = 0; k < cap; k++) {
		n += ids[k] != 0 ? 1u : 0u;
	}
	return n;
}

#ifdef __cplusplus
}
#endif
