// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Generic service-side input-event queue (macOS shell Tier 1, #48).
 * @ingroup ipc_server
 */

#include "ipc_server_input_queue.h"

#include <pthread.h>
#include <string.h>

// Power-of-two ring so head/tail wrap with a mask. Sized well above a frame's
// worth of events at 60 Hz; on overflow the oldest event is overwritten.
#define INPUT_QUEUE_CAPACITY 256
#define INPUT_QUEUE_MASK (INPUT_QUEUE_CAPACITY - 1)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ipc_workspace_input_event g_ring[INPUT_QUEUE_CAPACITY];
static uint32_t g_head; // next slot to write
static uint32_t g_tail; // next slot to read
static uint32_t g_count;

void
ipc_server_input_queue_push(const struct ipc_workspace_input_event *event)
{
	if (event == NULL) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	g_ring[g_head & INPUT_QUEUE_MASK] = *event;
	g_head++;
	if (g_count < INPUT_QUEUE_CAPACITY) {
		g_count++;
	} else {
		// Full: the write above clobbered the oldest slot, so advance tail too.
		g_tail++;
	}
	pthread_mutex_unlock(&g_lock);
}

void
ipc_server_input_queue_drain(uint32_t capacity, struct ipc_workspace_input_event_batch *out_batch)
{
	if (out_batch == NULL) {
		return;
	}
	if (capacity > IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX) {
		capacity = IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX;
	}

	pthread_mutex_lock(&g_lock);
	uint32_t n = (g_count < capacity) ? g_count : capacity;
	for (uint32_t i = 0; i < n; i++) {
		out_batch->events[i] = g_ring[g_tail & INPUT_QUEUE_MASK];
		g_tail++;
	}
	g_count -= n;
	out_batch->count = n;
	pthread_mutex_unlock(&g_lock);
}
