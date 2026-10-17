// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Process-wide pointer-event ring (see the header).
 * @ingroup aux_util
 */

#include "u_pointer_sink.h"

#include "os/os_threading.h"

#include <string.h>


/*
 *
 * State. One ring per process; the mutex is initialised on first use from
 * whichever side gets there first, which is always long before a second
 * thread exists (the UI thread's first touch is well after xrCreateInstance).
 *
 */

static struct
{
	struct os_mutex mutex;
	bool mutex_ready;

	struct xrt_input_pointer_event ring[U_POINTER_SINK_CAPACITY];
	uint32_t head;  //!< next slot to write
	uint32_t count; //!< live events
	uint64_t pushed;
	uint64_t dropped;
} g_sink;

static bool
sink_lock(void)
{
	if (!g_sink.mutex_ready) {
		g_sink.mutex_ready = os_mutex_init(&g_sink.mutex) == 0;
	}
	if (!g_sink.mutex_ready) {
		return false;
	}
	os_mutex_lock(&g_sink.mutex);
	return true;
}

static void
sink_unlock(void)
{
	os_mutex_unlock(&g_sink.mutex);
}


/*
 *
 * 'Exported' functions.
 *
 */

void
u_pointer_sink_push(const struct xrt_input_pointer_event *ev)
{
	if (ev == NULL || ev->struct_size < sizeof(uint32_t) || !sink_lock()) {
		return;
	}

	struct xrt_input_pointer_event *slot = &g_sink.ring[g_sink.head];
	memset(slot, 0, sizeof(*slot));
	// Prefix copy under the publisher's struct_size, then stamp ours.
	size_t copy = ev->struct_size;
	if (copy > sizeof(*slot)) {
		copy = sizeof(*slot);
	}
	memcpy(slot, ev, copy);
	slot->struct_size = (uint32_t)sizeof(*slot);

	g_sink.head = (g_sink.head + 1) % U_POINTER_SINK_CAPACITY;
	if (g_sink.count < U_POINTER_SINK_CAPACITY) {
		g_sink.count++;
	} else {
		g_sink.dropped++; // the slot just overwritten was the oldest live event
	}
	g_sink.pushed++;

	sink_unlock();
}

uint32_t
u_pointer_sink_poll(struct xrt_input_pointer_event *out, uint32_t max)
{
	if (out == NULL || max == 0 || !sink_lock()) {
		return 0;
	}

	uint32_t n = g_sink.count < max ? g_sink.count : max;
	// Oldest live event sits `count` slots behind the head.
	uint32_t idx = (g_sink.head + U_POINTER_SINK_CAPACITY - g_sink.count) % U_POINTER_SINK_CAPACITY;
	for (uint32_t i = 0; i < n; i++) {
		out[i] = g_sink.ring[idx];
		idx = (idx + 1) % U_POINTER_SINK_CAPACITY;
	}
	g_sink.count -= n;

	sink_unlock();
	return n;
}

void
u_pointer_sink_get_stats(uint64_t *out_pushed, uint64_t *out_dropped)
{
	if (!sink_lock()) {
		return;
	}
	if (out_pushed != NULL) {
		*out_pushed = g_sink.pushed;
	}
	if (out_dropped != NULL) {
		*out_dropped = g_sink.dropped;
	}
	sink_unlock();
}

void
u_pointer_sink_reset(void)
{
	if (!sink_lock()) {
		return;
	}
	g_sink.head = 0;
	g_sink.count = 0;
	g_sink.pushed = 0;
	g_sink.dropped = 0;
	sink_unlock();
}
