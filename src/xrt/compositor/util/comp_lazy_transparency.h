// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lazy-transparency policy: when to tell the display processor that a
 *         transparency-capable session's content is actually transparent.
 *
 * Pure state machine, header-only, no graphics API — the measurement lives in
 * the compositor (a per-frame alpha probe of the atlas), the resources live in
 * the display processor, and this file is only the debounce between them, so
 * every backend that grows a probe runs the SAME policy and it can be unit
 * tested without a GPU.
 *
 * - **Start ACTIVE on the first transparent verdict.** One transparent verdict
 *   is enough: the cost of reacting late is a frame whose transparent
 *   background weaves opaque.
 * - **Go IDLE only after @ref COMP_LAZY_TRANSPARENCY_IDLE_FRAMES consecutive
 *   opaque verdicts.** Stopping and restarting a desktop capture is not free
 *   (a D-Bus session and a PipeWire stream on Linux), so a user toggling
 *   transparency back and forth, or content that flickers a transparent pixel
 *   now and then, must not thrash it.
 * - **The initial state is IDLE.** The compositor declares idle to the DP
 *   before it enables transparency, so a session that starts opaque never
 *   starts the capture at all.
 *
 * @ingroup comp_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Consecutive fully-opaque app frames before the DP is told to idle (~1 s at 60 Hz).
#define COMP_LAZY_TRANSPARENCY_IDLE_FRAMES 60u

enum comp_lazy_transparency_transition
{
	COMP_LAZY_TRANSPARENCY_NONE = 0,
	COMP_LAZY_TRANSPARENCY_TO_ACTIVE,
	COMP_LAZY_TRANSPARENCY_TO_IDLE,
};

struct comp_lazy_transparency
{
	//! The policy is running: the DP supports the slot and the session is
	//! transparency-capable. False ⟹ never probe, DP stays active (legacy).
	bool engaged;
	//! What the DP was last told.
	bool active;
	//! Consecutive opaque verdicts while active.
	uint32_t opaque_run;
	//! Opaque verdicts required to idle; 0 ⟹ COMP_LAZY_TRANSPARENCY_IDLE_FRAMES.
	uint32_t idle_after;
};

//! Start the policy in the IDLE state (the caller has just told the DP so).
static inline void
comp_lazy_transparency_engage(struct comp_lazy_transparency *s, uint32_t idle_after)
{
	s->engaged = true;
	s->active = false;
	s->opaque_run = 0;
	s->idle_after = idle_after != 0 ? idle_after : COMP_LAZY_TRANSPARENCY_IDLE_FRAMES;
}

/*!
 * Feed one app frame's verdict. Returns the transition the caller must forward
 * to the DP, if any. Call once per APP frame — never on a repaint, which
 * replays a frame rather than producing one.
 */
static inline enum comp_lazy_transparency_transition
comp_lazy_transparency_update(struct comp_lazy_transparency *s, bool transparent)
{
	if (!s->engaged) {
		return COMP_LAZY_TRANSPARENCY_NONE;
	}
	if (transparent) {
		s->opaque_run = 0;
		if (!s->active) {
			s->active = true;
			return COMP_LAZY_TRANSPARENCY_TO_ACTIVE;
		}
		return COMP_LAZY_TRANSPARENCY_NONE;
	}
	if (!s->active) {
		return COMP_LAZY_TRANSPARENCY_NONE;
	}
	if (++s->opaque_run >= s->idle_after) {
		s->active = false;
		s->opaque_run = 0;
		return COMP_LAZY_TRANSPARENCY_TO_IDLE;
	}
	return COMP_LAZY_TRANSPARENCY_NONE;
}

#ifdef __cplusplus
}
#endif
