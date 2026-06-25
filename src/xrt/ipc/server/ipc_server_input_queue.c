// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Generic service-side input-event queue (macOS shell Tier 1, #48 / #61).
 * @ingroup ipc_server
 */

#include "ipc_server_input_queue.h"

#include <pthread.h>
#include <string.h>

// Power-of-two ring so head/tail wrap with a mask. Sized well above a frame's
// worth of events at 60 Hz; on overflow the oldest event is overwritten.
#define INPUT_QUEUE_CAPACITY 128
#define INPUT_QUEUE_MASK (INPUT_QUEUE_CAPACITY - 1)

// One queue per active input target: the controller plus the placed content
// clients. Tier-1/Tier-2 macOS shows a handful of windows; 8 is plenty (the
// controller + 7 content clients) and a target that can't get a slot simply
// falls back to the controller queue (see push()).
#define INPUT_QUEUE_MAX_TARGETS 8

struct target_queue
{
	bool used;     //!< false = free slot.
	void *target;  //!< Key: IPC_INPUT_TARGET_CONTROLLER (NULL) or a content client's xrt_compositor*.
	struct ipc_workspace_input_event ring[INPUT_QUEUE_CAPACITY];
	uint32_t head; //!< next slot to write
	uint32_t tail; //!< next slot to read
	uint32_t count;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct target_queue g_queues[INPUT_QUEUE_MAX_TARGETS];
static bool g_input_grab;
static bool g_pointer_capture;

// Caller holds g_lock. A free slot has used==false; the controller queue has
// used==true && target==NULL, so used must be checked to distinguish them.
static struct target_queue *
find_locked(void *target)
{
	for (int i = 0; i < INPUT_QUEUE_MAX_TARGETS; i++) {
		if (g_queues[i].used && g_queues[i].target == target) {
			return &g_queues[i];
		}
	}
	return NULL;
}

// Caller holds g_lock. Returns the existing queue for @p target or allocates a
// free slot; NULL only if the table is full.
static struct target_queue *
find_or_add_locked(void *target)
{
	struct target_queue *q = find_locked(target);
	if (q != NULL) {
		return q;
	}
	for (int i = 0; i < INPUT_QUEUE_MAX_TARGETS; i++) {
		if (!g_queues[i].used) {
			struct target_queue *slot = &g_queues[i];
			memset(slot, 0, sizeof(*slot));
			slot->used = true;
			slot->target = target;
			return slot;
		}
	}
	return NULL;
}

void
ipc_server_input_queue_push(void *target, const struct ipc_workspace_input_event *event)
{
	if (event == NULL) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	struct target_queue *q = find_or_add_locked(target);
	if (q == NULL) {
		// Table full: don't silently lose the event — fall back to the
		// controller queue (which is always present once anything routes).
		q = find_or_add_locked(IPC_INPUT_TARGET_CONTROLLER);
	}
	if (q != NULL) {
		q->ring[q->head & INPUT_QUEUE_MASK] = *event;
		q->head++;
		if (q->count < INPUT_QUEUE_CAPACITY) {
			q->count++;
		} else {
			// Full: the write above clobbered the oldest slot, advance tail too.
			q->tail++;
		}
	}
	pthread_mutex_unlock(&g_lock);
}

void
ipc_server_input_queue_drain(void *target, uint32_t capacity, struct ipc_workspace_input_event_batch *out_batch)
{
	if (out_batch == NULL) {
		return;
	}
	if (capacity > IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX) {
		capacity = IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX;
	}

	pthread_mutex_lock(&g_lock);
	struct target_queue *q = find_locked(target);
	uint32_t n = 0;
	if (q != NULL) {
		n = (q->count < capacity) ? q->count : capacity;
		for (uint32_t i = 0; i < n; i++) {
			out_batch->events[i] = q->ring[q->tail & INPUT_QUEUE_MASK];
			q->tail++;
		}
		q->count -= n;
	}
	out_batch->count = n;
	pthread_mutex_unlock(&g_lock);
}

void
ipc_server_input_queue_drop(void *target)
{
	// The controller queue is process-lifetime; never drop it.
	if (target == IPC_INPUT_TARGET_CONTROLLER) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	struct target_queue *q = find_locked(target);
	if (q != NULL) {
		memset(q, 0, sizeof(*q));
	}
	pthread_mutex_unlock(&g_lock);
}

void
ipc_server_input_queue_set_input_grab(bool grabbed)
{
	pthread_mutex_lock(&g_lock);
	g_input_grab = grabbed;
	pthread_mutex_unlock(&g_lock);
}

bool
ipc_server_input_queue_input_grabbed(void)
{
	pthread_mutex_lock(&g_lock);
	bool v = g_input_grab;
	pthread_mutex_unlock(&g_lock);
	return v;
}

void
ipc_server_input_queue_set_pointer_capture(bool captured)
{
	pthread_mutex_lock(&g_lock);
	g_pointer_capture = captured;
	pthread_mutex_unlock(&g_lock);
}

bool
ipc_server_input_queue_pointer_captured(void)
{
	pthread_mutex_lock(&g_lock);
	bool v = g_pointer_capture;
	pthread_mutex_unlock(&g_lock);
	return v;
}
