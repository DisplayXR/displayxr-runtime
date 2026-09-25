// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift per-stream mailbox + output ring state machine.
 * @ingroup aux_util
 *
 * See u_lift_mailbox.h. No locks here: the caller serializes.
 */

#include "util/u_lift_mailbox.h"

#include <string.h>

static bool
slot_ok_in(int32_t slot)
{
	return slot >= 0 && slot < U_LIFT_INPUT_SLOTS;
}

static bool
slot_ok_out(int32_t slot)
{
	return slot >= 0 && slot < U_LIFT_RING_SIZE;
}

void
u_lift_mailbox_init(struct u_lift_mailbox *mb)
{
	memset(mb, 0, sizeof(*mb));
	mb->latest = -1;
}

bool
u_lift_mailbox_begin_submit(struct u_lift_mailbox *mb, int32_t *out_slot)
{
	int32_t pick = -1;
	for (int32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		if (mb->in_state[i] == U_LIFT_IN_FREE) {
			pick = i;
			break;
		}
	}
	if (pick < 0) {
		// No free slot: overwrite the pending one — that frame never reaches
		// the module (latest wins).
		for (int32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
			if (mb->in_state[i] == U_LIFT_IN_PENDING) {
				pick = i;
				mb->dropped++;
				break;
			}
		}
	}
	if (pick < 0) {
		return false;
	}
	mb->in_state[pick] = U_LIFT_IN_WRITING;
	memset(&mb->in_meta[pick], 0, sizeof(mb->in_meta[pick]));
	*out_slot = pick;
	return true;
}

uint64_t
u_lift_mailbox_commit_submit(struct u_lift_mailbox *mb,
                             int32_t slot,
                             int64_t source_time,
                             uint64_t now_ns,
                             uint32_t width,
                             uint32_t height)
{
	if (!slot_ok_in(slot) || mb->in_state[slot] != U_LIFT_IN_WRITING) {
		return 0;
	}
	// An OLDER pending frame is superseded by this one.
	for (int32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		if (i != slot && mb->in_state[i] == U_LIFT_IN_PENDING) {
			mb->in_state[i] = U_LIFT_IN_FREE;
			mb->dropped++;
		}
	}
	struct u_lift_frame_meta *m = &mb->in_meta[slot];
	m->frame_id = ++mb->last_frame_id;
	m->source_time = source_time;
	m->submit_ns = now_ns;
	m->convert_start_ns = 0;
	m->done_ns = 0;
	m->width = width;
	m->height = height;
	mb->in_state[slot] = U_LIFT_IN_PENDING;
	mb->submitted++;
	return m->frame_id;
}

void
u_lift_mailbox_abort_submit(struct u_lift_mailbox *mb, int32_t slot)
{
	if (slot_ok_in(slot) && mb->in_state[slot] == U_LIFT_IN_WRITING) {
		mb->in_state[slot] = U_LIFT_IN_FREE;
	}
}

bool
u_lift_mailbox_has_pending(const struct u_lift_mailbox *mb)
{
	for (int32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		if (mb->in_state[i] == U_LIFT_IN_PENDING) {
			return true;
		}
	}
	return false;
}

bool
u_lift_mailbox_take_pending(struct u_lift_mailbox *mb,
                            uint64_t now_ns,
                            int32_t *out_slot,
                            struct u_lift_frame_meta *out_meta)
{
	int32_t pick = -1;
	for (int32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		if (mb->in_state[i] != U_LIFT_IN_PENDING) {
			continue;
		}
		if (pick < 0 || mb->in_meta[i].frame_id > mb->in_meta[pick].frame_id) {
			pick = i;
		}
	}
	if (pick < 0) {
		return false;
	}
	mb->in_state[pick] = U_LIFT_IN_CONVERTING;
	mb->in_meta[pick].convert_start_ns = now_ns;
	*out_slot = pick;
	if (out_meta != NULL) {
		*out_meta = mb->in_meta[pick];
	}
	return true;
}

void
u_lift_mailbox_finish_input(struct u_lift_mailbox *mb, int32_t slot)
{
	if (slot_ok_in(slot) && mb->in_state[slot] == U_LIFT_IN_CONVERTING) {
		mb->in_state[slot] = U_LIFT_IN_FREE;
	}
}

bool
u_lift_mailbox_begin_output(struct u_lift_mailbox *mb, int32_t *out_slot)
{
	for (int32_t i = 0; i < U_LIFT_RING_SIZE; i++) {
		if (i == mb->latest || mb->out_pins[i] > 0 || mb->out_state[i] == U_LIFT_OUT_WRITING) {
			continue;
		}
		mb->out_state[i] = U_LIFT_OUT_WRITING;
		*out_slot = i;
		return true;
	}
	return false;
}

void
u_lift_mailbox_publish_output(struct u_lift_mailbox *mb,
                              int32_t slot,
                              const struct u_lift_frame_meta *meta,
                              uint64_t now_ns)
{
	if (!slot_ok_out(slot) || mb->out_state[slot] != U_LIFT_OUT_WRITING || meta == NULL) {
		return;
	}
	struct u_lift_frame_meta *m = &mb->out_meta[slot];
	*m = *meta;
	m->done_ns = now_ns;
	mb->out_state[slot] = U_LIFT_OUT_READY;
	mb->latest = slot;
	mb->converted++;

	uint64_t lat = now_ns >= m->submit_ns ? now_ns - m->submit_ns : 0;
	mb->lat_last_ns = lat;
	if (mb->converted == 1) {
		mb->lat_min_ns = lat;
		mb->lat_max_ns = lat;
		mb->lat_ema_ns = lat;
	} else {
		if (lat < mb->lat_min_ns) {
			mb->lat_min_ns = lat;
		}
		if (lat > mb->lat_max_ns) {
			mb->lat_max_ns = lat;
		}
		// alpha = 1/8, integer: ema += (lat - ema) / 8, signed-safe.
		if (lat >= mb->lat_ema_ns) {
			mb->lat_ema_ns += (lat - mb->lat_ema_ns) / 8;
		} else {
			mb->lat_ema_ns -= (mb->lat_ema_ns - lat) / 8;
		}
	}
}

void
u_lift_mailbox_abort_output(struct u_lift_mailbox *mb, int32_t slot)
{
	if (slot_ok_out(slot) && mb->out_state[slot] == U_LIFT_OUT_WRITING) {
		mb->out_state[slot] = U_LIFT_OUT_EMPTY;
		memset(&mb->out_meta[slot], 0, sizeof(mb->out_meta[slot]));
		mb->failed++;
	}
}

bool
u_lift_mailbox_pin_latest(struct u_lift_mailbox *mb,
                          bool only_newer,
                          int32_t *out_slot,
                          struct u_lift_frame_meta *out_meta)
{
	int32_t s = mb->latest;
	if (!slot_ok_out(s) || mb->out_state[s] != U_LIFT_OUT_READY) {
		return false;
	}
	if (only_newer) {
		if (mb->out_meta[s].frame_id <= mb->last_acquired_frame_id) {
			return false;
		}
		mb->last_acquired_frame_id = mb->out_meta[s].frame_id;
	}
	mb->out_pins[s]++;
	*out_slot = s;
	if (out_meta != NULL) {
		*out_meta = mb->out_meta[s];
	}
	return true;
}

void
u_lift_mailbox_unpin(struct u_lift_mailbox *mb, int32_t slot)
{
	if (slot_ok_out(slot) && mb->out_pins[slot] > 0) {
		mb->out_pins[slot]--;
	}
}

bool
u_lift_mailbox_has_result(const struct u_lift_mailbox *mb)
{
	return slot_ok_out(mb->latest) && mb->out_state[mb->latest] == U_LIFT_OUT_READY;
}
