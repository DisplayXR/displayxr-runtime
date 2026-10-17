// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  gamepad hub — the poll thread, the latched sample, the rig integrator.
 *
 * One thread owns the platform backend: it polls at @ref GAMEPAD_POLL_NS
 * while a pad is attached and at @ref GAMEPAD_HOTPLUG_NS while none is, so
 * hot-plug is free. Every tick it latches the sample, integrates the
 * navigation pose with the mapping table's velocity over the real elapsed
 * time, handles the recenter edge, and expires any rumble. Readers take the
 * mutex for a scalar copy only — nothing in here blocks on the backend from
 * a client thread (same rule the arbiter's presence poll follows, #958).
 *
 * The provider's frame F (spec §4b.3) is wherever the integrator started:
 * identity at hub creation, identity again after a recenter. The runtime
 * aligns against it, never interprets it.
 *
 * @ingroup drv_gamepad
 */

#include "gamepad_interface.h"
#include "gamepad_backend.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <stdlib.h>
#include <string.h>


//! Poll period with a pad attached: 250 Hz, well above any display refresh.
#define GAMEPAD_POLL_NS (4LL * 1000 * 1000)

//! Poll period with no pad: hot-plug latency, and the cost of an empty XInput slot query.
#define GAMEPAD_HOTPLUG_NS (500LL * 1000 * 1000)

//! Never integrate more than this per tick (a stalled thread must not leap the rig).
#define GAMEPAD_MAX_TICK_S 0.1

//! How far beyond the latch time a nav sample is extrapolated with the last velocity.
#define GAMEPAD_EXTRAPOLATE_MAX_NS (50LL * 1000 * 1000)

struct gamepad_hub
{
	struct os_thread_helper oth;

	//! Guards everything below.
	struct os_mutex mutex;

	struct gamepad_hub_config cfg;

	//! Latched sample; `state.timestamp_ns` is when it was read.
	struct gamepad_state state;
	bool have_sample;
	bool backend_ok;

	//! The integrated navigation pose and the velocity it was last advanced with.
	struct gamepad_nav_pose nav;
	struct gamepad_nav_velocity nav_vel;
	int64_t nav_latched_ns;

	//! Durable recenter level: the monotonic time of the most recent reset, 0 = never.
	int64_t recenter_ns;
	bool recenter_was_down;

	//! Rumble request: applied by the poll thread, expired by deadline.
	float rumble_low, rumble_high;
	int64_t rumble_deadline_ns;
	bool rumble_dirty;
	bool rumble_active;

	//! Monotonic time of the last stick / button / trigger activity; 0 = none since plug-in.
	int64_t last_activity_ns;
	//! The idle verdict as of the last tick, for the one-off edge log.
	bool was_active;

	//! Diagnostics.
	uint64_t connect_count;

	int refcount;
};

/*!
 * The live hub, for the plug-in's `get_presence`. Set at create, cleared
 * when the last reference frees it. A plain pointer: the arbiter's presence
 * poll is stopped BEFORE the devices (and so the hub) are torn down (#958),
 * and the builder path is single-threaded, so no reader races the clear.
 */
static struct gamepad_hub *g_current = NULL;


/*
 *
 * Poll thread.
 *
 */

static void
hub_tick_locked(struct gamepad_hub *hub, const struct gamepad_state *fresh, int64_t now)
{
	const bool was_connected = hub->state.connected;
	hub->state = *fresh;
	hub->state.timestamp_ns = now;
	hub->have_sample = true;

	if (fresh->connected != was_connected) {
		// One-off lifecycle edge, not per frame: the arbiter logs the
		// role move itself; this says WHY it moved.
		if (fresh->connected) {
			hub->connect_count++;
			U_LOG_W(
			    "gamepad: pad attached (%s) — it takes the gamepad role (and the rig / hand roles it "
			    "was created with).",
			    gamepad_backend_name());
		} else {
			U_LOG_W("gamepad: pad detached — roles return to the floor (fly camera / qwerty).");
			hub->rumble_active = false;
		}
		// A plug / unplug resets the idle clock: a freshly plugged pad is
		// untouched until the user reaches for it.
		hub->last_activity_ns = 0;
	}

	// "Last-used input wins": timestamp any touch; presence follows it.
	if (gamepad_map_has_activity(&hub->state, hub->cfg.map.deadzone)) {
		hub->last_activity_ns = now;
	}
	const bool active =
	    hub->state.connected && gamepad_map_recently_active(now, hub->last_activity_ns, hub->cfg.idle_timeout_s);
	if (active != hub->was_active && hub->state.connected && hub->cfg.idle_timeout_s > 0.0f) {
		// One-off edge, never per frame: the idle rule moving the roles.
		if (active) {
			U_LOG_W("gamepad: pad touched — it takes its roles (idle timeout %.0f s).",
			        (double)hub->cfg.idle_timeout_s);
		} else {
			U_LOG_W("gamepad: pad idle for %.0f s — roles return to the floor until it is touched again.",
			        (double)hub->cfg.idle_timeout_s);
		}
	}
	hub->was_active = active;

	// Rig: integrate what the table says over the real elapsed time.
	double dt = (double)(now - hub->nav_latched_ns) * 1e-9;
	if (dt > GAMEPAD_MAX_TICK_S) {
		dt = GAMEPAD_MAX_TICK_S;
	}
	struct gamepad_nav_velocity vel;
	gamepad_map_nav_velocity(&hub->state, hub->cfg.table, &hub->cfg.map, &vel);
	gamepad_map_integrate(&hub->nav, &vel, dt);
	hub->nav_vel = vel;
	hub->nav_latched_ns = now;

	// Recenter on the rising edge: the durable timestamp advances AND the
	// pose jumps to F's origin in the SAME publication (spec §4b.3).
	const bool down = gamepad_map_action_pressed(&hub->state, hub->cfg.table, GAMEPAD_ACT_RECENTER);
	if (down && !hub->recenter_was_down) {
		hub->recenter_ns = now;
		hub->nav = (struct gamepad_nav_pose)GAMEPAD_NAV_POSE_IDENTITY;
		hub->nav_vel = (struct gamepad_nav_velocity){0};
	}
	hub->recenter_was_down = down;
}

static void *
hub_thread(void *ptr)
{
	struct gamepad_hub *hub = (struct gamepad_hub *)ptr;

	const bool ok = gamepad_backend_init();
	os_mutex_lock(&hub->mutex);
	hub->backend_ok = ok;
	hub->nav_latched_ns = (int64_t)os_monotonic_get_ns();
	os_mutex_unlock(&hub->mutex);
	if (!ok) {
		U_LOG_W("gamepad: backend '%s' failed to initialise — the provider reports ABSENT for good.",
		        gamepad_backend_name());
	} else {
		U_LOG_I("gamepad: backend '%s' up; polling for a pad.", gamepad_backend_name());
	}

	while (ok && os_thread_helper_is_running(&hub->oth)) {
		struct gamepad_state fresh;
		U_ZERO(&fresh);
		gamepad_backend_poll(&fresh); // OFF-lock: the platform call may be slow
		int64_t now = (int64_t)os_monotonic_get_ns();

		float low = 0.0f, high = 0.0f;
		bool apply_rumble = false;

		os_mutex_lock(&hub->mutex);
		hub_tick_locked(hub, &fresh, now);
		if (hub->rumble_dirty) {
			hub->rumble_dirty = false;
			low = hub->rumble_low;
			high = hub->rumble_high;
			hub->rumble_active = (low > 0.0f || high > 0.0f) && now < hub->rumble_deadline_ns;
			apply_rumble = true;
		} else if (hub->rumble_active && now >= hub->rumble_deadline_ns) {
			hub->rumble_active = false;
			apply_rumble = true; // low == high == 0: stop
		}
		const bool connected = hub->state.connected;
		os_mutex_unlock(&hub->mutex);

		if (apply_rumble && connected) {
			gamepad_backend_rumble(low, high);
		}

		// Sleep in small chunks so signal_stop is prompt even on the slow path.
		const int64_t period = connected ? GAMEPAD_POLL_NS : GAMEPAD_HOTPLUG_NS;
		for (int64_t slept = 0; slept < period && os_thread_helper_is_running(&hub->oth);
		     slept += GAMEPAD_POLL_NS) {
			os_nanosleep(GAMEPAD_POLL_NS);
		}
	}

	if (ok) {
		gamepad_backend_fini();
	}
	return NULL;
}


/*
 *
 * 'Exported' functions.
 *
 */

struct gamepad_hub *
gamepad_hub_create(const struct gamepad_hub_config *cfg)
{
	if (cfg == NULL || cfg->table == NULL) {
		return NULL;
	}
	struct gamepad_hub *hub = U_TYPED_CALLOC(struct gamepad_hub);
	if (hub == NULL) {
		return NULL;
	}
	hub->cfg = *cfg;
	hub->nav = (struct gamepad_nav_pose)GAMEPAD_NAV_POSE_IDENTITY;
	hub->refcount = 1;

	if (os_mutex_init(&hub->mutex) != 0) {
		free(hub);
		return NULL;
	}
	if (os_thread_helper_init(&hub->oth) != 0) {
		os_mutex_destroy(&hub->mutex);
		free(hub);
		return NULL;
	}
	if (os_thread_helper_start(&hub->oth, hub_thread, hub) != 0) {
		U_LOG_E("gamepad: poll thread failed to start.");
		os_thread_helper_destroy(&hub->oth);
		os_mutex_destroy(&hub->mutex);
		free(hub);
		return NULL;
	}

	g_current = hub;
	U_LOG_W("gamepad: hub up — deadzone %.2f, nav %.2f m/s, turn %.0f deg/s%s, idle timeout %.0f s%s.",
	        (double)cfg->map.deadzone, (double)cfg->map.nav_speed_mps, (double)cfg->map.nav_turn_dps,
	        cfg->map.invert_look_y ? ", look-Y inverted" : "", (double)cfg->idle_timeout_s,
	        cfg->idle_timeout_s > 0.0f ? " (last-used input wins)" : " (connected-only)");
	return hub;
}

void
gamepad_hub_retain(struct gamepad_hub *hub)
{
	if (hub == NULL) {
		return;
	}
	os_mutex_lock(&hub->mutex);
	hub->refcount++;
	os_mutex_unlock(&hub->mutex);
}

void
gamepad_hub_release(struct gamepad_hub *hub)
{
	if (hub == NULL) {
		return;
	}
	os_mutex_lock(&hub->mutex);
	const int remaining = --hub->refcount;
	os_mutex_unlock(&hub->mutex);
	if (remaining > 0) {
		return;
	}

	// Last reference: stop the thread (which finalises the backend on its
	// way out) before the memory goes.
	if (g_current == hub) {
		g_current = NULL;
	}
	os_thread_helper_stop_and_wait(&hub->oth);
	os_thread_helper_destroy(&hub->oth);
	os_mutex_destroy(&hub->mutex);
	free(hub);
}

struct gamepad_hub *
gamepad_hub_current(void)
{
	return g_current;
}

enum xrt_input_provider_presence
gamepad_hub_presence(struct gamepad_hub *hub)
{
	if (hub == NULL) {
		return XRT_INPUT_PROVIDER_PRESENCE_UNKNOWN;
	}
	os_mutex_lock(&hub->mutex);
	enum xrt_input_provider_presence p = XRT_INPUT_PROVIDER_PRESENCE_UNKNOWN;
	if (!hub->backend_ok && hub->have_sample) {
		p = XRT_INPUT_PROVIDER_PRESENCE_ABSENT;
	} else if (hub->have_sample) {
		// Connected AND recently touched (or the idle rule is off). The
		// verdict is a pure function of the latched tick, so a client
		// thread never runs provider code here.
		const bool present =
		    hub->state.connected && gamepad_map_recently_active((int64_t)os_monotonic_get_ns(),
		                                                        hub->last_activity_ns, hub->cfg.idle_timeout_s);
		p = present ? XRT_INPUT_PROVIDER_PRESENCE_PRESENT : XRT_INPUT_PROVIDER_PRESENCE_ABSENT;
	}
	os_mutex_unlock(&hub->mutex);
	return p;
}

void
gamepad_hub_snapshot(struct gamepad_hub *hub, struct gamepad_state *out)
{
	U_ZERO(out);
	if (hub == NULL) {
		return;
	}
	os_mutex_lock(&hub->mutex);
	*out = hub->state;
	os_mutex_unlock(&hub->mutex);
}

void
gamepad_hub_nav_sample(struct gamepad_hub *hub,
                       int64_t at_timestamp_ns,
                       struct xrt_pose *out_pose,
                       bool *out_valid,
                       int64_t *out_recenter_ns)
{
	*out_pose = (struct xrt_pose)XRT_POSE_IDENTITY;
	*out_valid = false;
	*out_recenter_ns = 0;
	if (hub == NULL) {
		return;
	}

	os_mutex_lock(&hub->mutex);
	struct gamepad_nav_pose pose = hub->nav;
	struct gamepad_nav_velocity vel = hub->nav_vel;
	const int64_t latched = hub->nav_latched_ns;
	const bool valid = hub->have_sample && hub->backend_ok;
	*out_recenter_ns = hub->recenter_ns;
	os_mutex_unlock(&hub->mutex);

	// Bounded forward extrapolation with the last velocity: smooth at the
	// head's predicted display time without ever running away from a
	// stalled poll. A request in the past (or for the latch) is served
	// from the latch — no history, by contract (§4b.5 "No history").
	int64_t ahead = at_timestamp_ns - latched;
	if (ahead > 0) {
		if (ahead > GAMEPAD_EXTRAPOLATE_MAX_NS) {
			ahead = GAMEPAD_EXTRAPOLATE_MAX_NS;
		}
		gamepad_map_integrate(&pose, &vel, (double)ahead * 1e-9);
	}
	gamepad_map_nav_pose_to_xrt(&pose, out_pose);
	*out_valid = valid;
}

void
gamepad_hub_rumble(struct gamepad_hub *hub, float low, float high, int64_t duration_ns)
{
	if (hub == NULL) {
		return;
	}
	const int64_t now = (int64_t)os_monotonic_get_ns();
	os_mutex_lock(&hub->mutex);
	hub->rumble_low = low;
	hub->rumble_high = high;
	hub->rumble_deadline_ns = duration_ns > 0 ? now + duration_ns : now;
	hub->rumble_dirty = true;
	os_mutex_unlock(&hub->mutex);
}
