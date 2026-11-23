// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Generic service-side input-event queue (macOS shell Tier 1, #48 / #61).
 * @ingroup ipc_server
 */

#include "ipc_server_input_queue.h"

#include "multi/comp_multi_workspace.h" // hit-test + focus for ipc_server_input_route
#include "os/os_time.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#ifdef XRT_OS_LINUX_DESKTOP
#include <sys/eventfd.h>
#include <stdint.h>
#include <unistd.h>
#endif

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
static struct ipc_workspace_input_event g_frame_tick; //!< Latest tick, under g_lock.
static bool g_frame_tick_pending;
static struct target_queue g_queues[INPUT_QUEUE_MAX_TARGETS];
static bool g_input_grab;
static bool g_pointer_capture;

//! Pointer owner (XR_DXR_workspace_client_input): the content client a button
//! press was routed to. While any button pressed on it is held, its motion and
//! releases keep going to it even after the cursor leaves its window (a drag),
//! as the Windows service's mouse capture does. Under g_lock.
static void *g_pointer_owner;

//! The controller's reserved chords (xrSetWorkspaceReservedKeysDXR, spec 24).
//! g_reserved_count < 0 = none registered: the built-in default applies.
//! Under g_lock.
static struct ipc_workspace_reserved_key g_reserved_keys[IPC_WORKSPACE_MAX_RESERVED_KEYS];
static int32_t g_reserved_count = -1;
static uint32_t g_pointer_owner_buttons; //!< bit0 L, bit1 R, bit2 M held on the owner.

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

#ifdef XRT_OS_LINUX_DESKTOP
//! Controller wakeup (xrAcquireWorkspaceWakeupEventDXR, POSIX form): an eventfd
//! the server keeps for its lifetime; every controller-queue push makes it
//! readable. -1 until first acquired. Guarded by g_lock.
static int g_wakeup_fd = -1;

int
ipc_server_input_queue_get_wakeup_fd(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_wakeup_fd < 0) {
		g_wakeup_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	}
	int fd = g_wakeup_fd;
	pthread_mutex_unlock(&g_lock);
	return fd;
}

//! Caller holds g_lock.
static void
signal_wakeup_locked(void)
{
	if (g_wakeup_fd >= 0) {
		uint64_t one = 1;
		// EAGAIN only when the counter is saturated — already signalled.
		(void)!write(g_wakeup_fd, &one, sizeof(one));
	}
}
#else
static inline void
signal_wakeup_locked(void)
{}
#endif

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
		if (q->target == IPC_INPUT_TARGET_CONTROLLER) {
			signal_wakeup_locked();
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
	// The coalesced FRAME_TICK rides after the controller's events, if there is
	// room; otherwise it waits (still the latest) for the next drain.
	if (target == IPC_INPUT_TARGET_CONTROLLER && g_frame_tick_pending && n < capacity) {
		out_batch->events[n++] = g_frame_tick;
		g_frame_tick_pending = false;
	}
	out_batch->count = n;
	pthread_mutex_unlock(&g_lock);
}

void
ipc_server_input_queue_publish_frame_tick(const struct ipc_workspace_input_event *tick)
{
	if (tick == NULL) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	g_frame_tick = *tick;
	g_frame_tick_pending = true;
	pthread_mutex_unlock(&g_lock);
}

void
ipc_server_input_queue_clear_frame_tick(void)
{
	pthread_mutex_lock(&g_lock);
	g_frame_tick_pending = false;
	pthread_mutex_unlock(&g_lock);
}

void
ipc_server_input_queue_frame_tick_sink(void *userdata, const struct comp_multi_frame_tick *tick)
{
	(void)userdata;
	struct ipc_workspace_input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.event_type = IPC_WORKSPACE_INPUT_EVENT_FRAME_TICK;
	ev.timestamp_ms = (uint32_t)(tick->timestamp_ns / 1000000);
	ev.u.frame_tick.timestamp_ns = tick->timestamp_ns;
	ev.u.frame_tick.viewer_x = tick->viewer_x;
	ev.u.frame_tick.viewer_y = tick->viewer_y;
	ev.u.frame_tick.viewer_z = tick->viewer_z;
	ev.u.frame_tick.viewer_valid = tick->viewer_valid ? 1u : 0u;
	ev.u.frame_tick.cursor_x = tick->cursor_x;
	ev.u.frame_tick.cursor_y = tick->cursor_y;
	ipc_server_input_queue_publish_frame_tick(&ev);
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
	if (g_pointer_owner == target) {
		g_pointer_owner = NULL;
		g_pointer_owner_buttons = 0;
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


/*
 *
 * Routing (#61) — shared by the desktop-Linux surface input and the macOS
 * AppKit pump.
 *
 */

void
ipc_server_input_queue_set_reserved_keys(const struct ipc_workspace_reserved_keys *table)
{
	pthread_mutex_lock(&g_lock);
	if (table == NULL || table->count == 0) {
		g_reserved_count = -1; // back to the built-in default
	} else {
		uint32_t n = table->count;
		if (n > IPC_WORKSPACE_MAX_RESERVED_KEYS) {
			n = IPC_WORKSPACE_MAX_RESERVED_KEYS;
		}
		memcpy(g_reserved_keys, table->keys, n * sizeof(g_reserved_keys[0]));
		g_reserved_count = (int32_t)n;
	}
	pthread_mutex_unlock(&g_lock);
}

//! The built-in reserved set, used until the controller registers its own —
//! the same list as the Windows service (comp_d3d11_window.cpp
//! is_default_reserved_key): bare TAB (Shift+TAB reaches the app), DELETE,
//! ESC, [ and ]. Every Ctrl chord reaches the app.
static bool
is_default_reserved_key(uint32_t vk, uint32_t mods)
{
	switch (vk) {
	case 0x09: return (mods & 1u) == 0; // VK_TAB: bare TAB cycles focus
	case 0x2E:                          // VK_DELETE: close the focused app
	case 0x1B:                          // VK_ESCAPE: restore a maximized window
	case 0xDB:                          // VK_OEM_4 [: window Z back
	case 0xDD:                          // VK_OEM_6 ]: window Z forward
		return true;
	default: return false;
	}
}

/*!
 * A reserved key stays with the controller and never reaches a content app
 * (XR_DXR_spatial_workspace spec 24, the Windows policy). With a registered
 * table the match is exact on (vk, modifiers) — {TAB, 0} reserves bare TAB
 * while Shift+TAB is forwarded; without one, the built-in default. The
 * modifiers compared are SHIFT / CTRL / ALT, as the table carries them.
 */
static bool
is_workspace_key(uint32_t vk, uint32_t mods)
{
	const uint32_t m = mods & 0x7u;
	bool reserved = false;
	pthread_mutex_lock(&g_lock);
	if (g_reserved_count < 0) {
		reserved = is_default_reserved_key(vk, m);
	} else {
		for (int32_t i = 0; i < g_reserved_count; i++) {
			if (g_reserved_keys[i].vk_code == vk && g_reserved_keys[i].modifiers == m) {
				reserved = true;
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_lock);
	return reserved;
}

static uint32_t
button_bit(uint32_t button)
{
	return (button >= 1 && button <= 3) ? (1u << (button - 1)) : 0u;
}

/*!
 * Push a content app's copy of a pointer / motion / scroll event, with the
 * cursor normalized to that app's content rect (NaN when it has no placed
 * rect; XR_DXR_workspace_client_input). The controller's copy is pushed
 * separately and never rewritten: it hit-tests on its own.
 */
static void
push_app_copy(void *app, const struct ipc_workspace_input_event *ev)
{
	struct ipc_workspace_input_event c = *ev;
	int64_t cx = 0;
	int64_t cy = 0;
	switch (ev->event_type) {
	case IPC_WORKSPACE_INPUT_EVENT_POINTER:
		cx = ev->u.pointer.cursor_x;
		cy = ev->u.pointer.cursor_y;
		break;
	case IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION:
		cx = ev->u.pointer_motion.cursor_x;
		cy = ev->u.pointer_motion.cursor_y;
		break;
	case IPC_WORKSPACE_INPUT_EVENT_SCROLL:
		cx = ev->u.scroll.cursor_x;
		cy = ev->u.scroll.cursor_y;
		break;
	default: ipc_server_input_queue_push(app, ev); return;
	}
	float u = NAN;
	float v = NAN;
	if (!comp_multi_workspace_window_local_uv((struct xrt_compositor *)app, (int32_t)cx, (int32_t)cy, &u, &v)) {
		u = NAN;
		v = NAN;
	}
	switch (ev->event_type) {
	case IPC_WORKSPACE_INPUT_EVENT_POINTER:
		c.u.pointer.local_u = u;
		c.u.pointer.local_v = v;
		break;
	case IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION:
		c.u.pointer_motion.local_u = u;
		c.u.pointer_motion.local_v = v;
		break;
	default:
		c.u.scroll.local_u = u;
		c.u.scroll.local_v = v;
		break;
	}
	ipc_server_input_queue_push(app, &c);
}

/*!
 * The content target of a button event. A press goes to the current pointer
 * owner when another button is already held on it, else to the window under
 * the cursor, and makes that window the owner. A release goes to the owner
 * wherever the cursor is — even under grab / capture, so no app is left with a
 * stuck button — and ends the ownership once no button is held. Returns NULL
 * when the event goes to no content app.
 */
static void *
route_button_target(const struct ipc_workspace_input_event *ev, bool forward)
{
	const uint32_t bit = button_bit(ev->u.pointer.button);
	void *target = NULL;
	bool hit_test = false;

	pthread_mutex_lock(&g_lock);
	if (ev->u.pointer.is_down) {
		if (g_pointer_owner != NULL && (g_pointer_owner_buttons & bit) != 0) {
			// A press of a button we think is already held: its release was
			// lost (focus loss, a pointer grab elsewhere). Start over.
			g_pointer_owner = NULL;
			g_pointer_owner_buttons = 0;
		}
		if (forward) {
			target = g_pointer_owner;
			hit_test = (target == NULL);
		}
	} else if (g_pointer_owner != NULL && (g_pointer_owner_buttons & bit) != 0) {
		target = g_pointer_owner;
		g_pointer_owner_buttons &= ~bit;
		if (g_pointer_owner_buttons == 0) {
			g_pointer_owner = NULL;
		}
	} else if (forward) {
		hit_test = true;
	}
	pthread_mutex_unlock(&g_lock);

	if (hit_test) {
		target = comp_multi_workspace_hit_test_window_px((int32_t)ev->u.pointer.cursor_x,
		                                                 (int32_t)ev->u.pointer.cursor_y);
		if (target != NULL && ev->u.pointer.is_down && bit != 0) {
			pthread_mutex_lock(&g_lock);
			g_pointer_owner = target;
			g_pointer_owner_buttons |= bit;
			pthread_mutex_unlock(&g_lock);
		}
	} else if (target != NULL && ev->u.pointer.is_down) {
		pthread_mutex_lock(&g_lock);
		g_pointer_owner_buttons |= bit;
		pthread_mutex_unlock(&g_lock);
	}
	return target;
}

void
ipc_server_input_route(const struct ipc_workspace_input_event *ev)
{
	const bool grab = ipc_server_input_queue_input_grabbed();
	const bool cap = ipc_server_input_queue_pointer_captured();

	switch (ev->event_type) {
	case IPC_WORKSPACE_INPUT_EVENT_KEY:
		ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev);
		if (!grab && !is_workspace_key(ev->u.key.vk_code, ev->u.key.modifiers)) {
			void *app = comp_multi_workspace_get_focused_client();
			if (app != NULL) {
				ipc_server_input_queue_push(app, ev);
			}
		}
		break;
	case IPC_WORKSPACE_INPUT_EVENT_SCROLL:
		if (grab || cap) {
			ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev);
		} else {
			void *app = comp_multi_workspace_hit_test_window_px((int32_t)ev->u.scroll.cursor_x,
			                                                    (int32_t)ev->u.scroll.cursor_y);
			if (app != NULL) {
				push_app_copy(app, ev);
			} else {
				ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev);
			}
		}
		break;
	case IPC_WORKSPACE_INPUT_EVENT_POINTER: {
		ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev);
		void *app = route_button_target(ev, !grab && !cap);
		if (app != NULL) {
			push_app_copy(app, ev);
		}
		break;
	}
	case IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION: {
		ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev);
		void *owner = NULL;
		pthread_mutex_lock(&g_lock);
		if (ev->u.pointer_motion.button_mask == 0) {
			// Nothing held: any drag is over (also covers a lost release).
			g_pointer_owner = NULL;
			g_pointer_owner_buttons = 0;
		}
		owner = g_pointer_owner;
		pthread_mutex_unlock(&g_lock);
		if (!grab && !cap) {
			// During a drag the owner keeps the motion, unclamped, even
			// outside its window; otherwise the window under the cursor.
			void *app = owner;
			if (app == NULL) {
				app = comp_multi_workspace_hit_test_window_px((int32_t)ev->u.pointer_motion.cursor_x,
				                                              (int32_t)ev->u.pointer_motion.cursor_y);
			}
			if (app != NULL) {
				push_app_copy(app, ev);
			}
		}
		break;
	}
	default: ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, ev); break;
	}
}

void
ipc_server_input_queue_push_controller_key(uint32_t vk_code, uint32_t modifiers)
{
	struct ipc_workspace_input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.event_type = IPC_WORKSPACE_INPUT_EVENT_KEY;
	ev.timestamp_ms = (uint32_t)(os_monotonic_get_ns() / 1000000);
	ev.u.key.vk_code = vk_code;
	ev.u.key.modifiers = modifiers;
	ev.u.key.is_down = 1;
	ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, &ev);
	ev.u.key.is_down = 0;
	ipc_server_input_queue_push(IPC_INPUT_TARGET_CONTROLLER, &ev);
}
