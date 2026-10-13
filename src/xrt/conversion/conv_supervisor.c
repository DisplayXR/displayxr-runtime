// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion-host supervisor state machine (see conv_supervisor.h).
 * @ingroup aux_conversion
 */

#include "conv_supervisor.h"
#include "conv_host_wire.h"

#include "xrt/xrt_conversion_plugin.h"

#include <string.h>

#define MS(x) ((uint64_t)(x) * 1000000ull)
#define SEC(x) (MS(x) * 1000ull)

static uint64_t
min_u64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

void
conv_supervisor_config_defaults(struct conv_supervisor_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->backoff_initial_ns = MS(500);
	cfg->backoff_max_ns = SEC(30);
	cfg->healthy_reset_ns = SEC(600);
	cfg->crash_loop_count = 5;
	cfg->crash_loop_window_ns = SEC(600);
	cfg->crash_loop_retry_ns = SEC(1800);
	cfg->idle_exit_ns = SEC(600);
	cfg->shutdown_grace_ns = SEC(3);
	cfg->hello_timeout_ns = SEC(10);
	cfg->heartbeat_timeout_ns = SEC(10);
	cfg->kill_grace_ns = SEC(5);
}

void
conv_supervisor_init(struct conv_supervisor *s, const struct conv_supervisor_config *cfg)
{
	memset(s, 0, sizeof(*s));
	if (cfg != NULL) {
		s->cfg = *cfg;
	} else {
		conv_supervisor_config_defaults(&s->cfg);
	}
	if (s->cfg.crash_loop_count == 0) {
		s->cfg.crash_loop_count = 1;
	}
	if (s->cfg.crash_loop_count > CONV_SUP_MAX_CRASH_HISTORY) {
		s->cfg.crash_loop_count = CONV_SUP_MAX_CRASH_HISTORY;
	}
	s->state = CONV_SUP_IDLE;
	s->backoff_ns = s->cfg.backoff_initial_ns;
	s->last_exit = INT32_MIN;
}


/*
 *
 * Internal transitions.
 *
 */

static void
clear_crash_history(struct conv_supervisor *s)
{
	memset(s->crash_ns, 0, sizeof(s->crash_ns));
	s->crash_head = 0;
	s->backoff_ns = s->cfg.backoff_initial_ns;
}

static void
go_failed(struct conv_supervisor *s, uint64_t now_ns, uint32_t reason, uint32_t retryable, const char *detail)
{
	s->state = CONV_SUP_FAILED;
	s->failed_at_ns = now_ns;
	s->failure_reason = reason;
	s->retryable = retryable;
	s->detail = detail;
	s->warming = CONV_SUP_WARMING_NONE;
}

static void
go_down_idle(struct conv_supervisor *s)
{
	s->state = s->service_shutdown ? CONV_SUP_STOPPED : CONV_SUP_IDLE;
	s->warming = CONV_SUP_WARMING_NONE;
	s->detail = NULL;
}

//! Down with tasks still bound: count it, then BACKOFF or the cutoff.
static void
on_abnormal(struct conv_supervisor *s, uint64_t now_ns)
{
	s->restarts++;

	// A long healthy run earns a fresh backoff (§3.4: reset after 10 min).
	if (s->running_since_ns != 0 && now_ns - s->running_since_ns >= s->cfg.healthy_reset_ns) {
		s->backoff_ns = s->cfg.backoff_initial_ns;
	}

	s->crash_ns[s->crash_head % CONV_SUP_MAX_CRASH_HISTORY] = now_ns;
	s->crash_head++;

	uint32_t recorded = s->crash_head < CONV_SUP_MAX_CRASH_HISTORY ? s->crash_head : CONV_SUP_MAX_CRASH_HISTORY;
	uint32_t in_window = 0;
	for (uint32_t i = 0; i < recorded; i++) {
		uint64_t t = s->crash_ns[(s->crash_head - 1 - i) % CONV_SUP_MAX_CRASH_HISTORY];
		if (now_ns - t < s->cfg.crash_loop_window_ns) {
			in_window++;
		}
	}
	if (in_window >= s->cfg.crash_loop_count) {
		go_failed(s, now_ns, XRT_CONV_FAIL_CRASH_LOOP, 1, "crash_loop");
		return;
	}

	if (s->service_shutdown) {
		go_down_idle(s);
		return;
	}

	s->state = CONV_SUP_BACKOFF;
	s->warming = CONV_SUP_WARMING_RESTARTING;
	s->detail = "restarting";
	s->next_spawn_ns = now_ns + s->backoff_ns;
	s->backoff_ns = min_u64(s->backoff_ns * 2, s->cfg.backoff_max_ns);
}

//! The exit-code table for a host that went down on its own (or that we killed).
static void
classify_exit(struct conv_supervisor *s, uint64_t now_ns, int32_t code)
{
	switch (code) {
	case CONV_HOST_EXIT_UPDATE:
		// Closed for update: no backoff, no accounting, no timer (§3.6).
		if (s->service_shutdown) {
			go_down_idle(s);
			return;
		}
		s->state = CONV_SUP_UPDATING;
		s->warming = CONV_SUP_WARMING_UPDATING;
		s->detail = "updating";
		return;
	case CONV_HOST_EXIT_ABI_REJECT: go_failed(s, now_ns, XRT_CONV_FAIL_VERSION, 0, "abi_reject"); return;
	case CONV_HOST_EXIT_NEGOTIATE_FAILED: go_failed(s, now_ns, XRT_CONV_FAIL_LOAD, 0, "negotiate_failed"); return;
	case CONV_HOST_EXIT_LICENCE: go_failed(s, now_ns, XRT_CONV_FAIL_LICENCE, 1, "licence"); return;
	case CONV_HOST_EXIT_NO_DEVICE: go_failed(s, now_ns, XRT_CONV_FAIL_PLATFORM, 1, "no_device"); return;
	case CONV_HOST_EXIT_ELEVATED: go_failed(s, now_ns, XRT_CONV_FAIL_PLATFORM, 0, "elevated"); return;
	default:
		// 0 unrequested, 30 (watchdog), a failed spawn, a crash.
		on_abnormal(s, now_ns);
		return;
	}
}

static void
begin_kill(struct conv_supervisor *s, enum conv_supervisor_kill_reason reason)
{
	s->state = CONV_SUP_KILLING;
	s->kill_reason = reason;
	s->terminate_due = true;
}

static enum conv_supervisor_action
issue_spawn(struct conv_supervisor *s, uint64_t now_ns)
{
	s->state = CONV_SUP_STARTING;
	s->start_ns = now_ns;
	s->running_since_ns = 0;
	s->last_heartbeat_ns = 0;
	s->pending_spawn_report = true;
	s->kill_reason = CONV_SUP_KILL_NONE;
	s->terminate_due = false;
	s->shutdown_due = false;
	s->spawns++;
	return CONV_SUP_ACTION_SPAWN;
}

static bool
may_spawn(const struct conv_supervisor *s)
{
	return s->demand && !s->maintenance_held && !s->service_shutdown;
}


/*
 *
 * Events.
 *
 */

void
conv_supervisor_set_demand(struct conv_supervisor *s, uint64_t now_ns, bool demand)
{
	if (demand == s->demand) {
		return;
	}
	s->demand = demand;
	if (!demand) {
		s->no_demand_since_ns = now_ns;
	}
}

void
conv_supervisor_set_maintenance(struct conv_supervisor *s, bool held)
{
	s->maintenance_held = held;
}

void
conv_supervisor_spawned(struct conv_supervisor *s, uint64_t now_ns, bool ok)
{
	if (!s->pending_spawn_report) {
		return;
	}
	s->pending_spawn_report = false;
	if (!ok) {
		// No process: exactly an immediate exit.
		conv_supervisor_exited(s, now_ns, CONV_SUP_EXIT_SPAWN_FAILED);
	}
}

void
conv_supervisor_hello(struct conv_supervisor *s, uint64_t now_ns, bool accepted)
{
	if (s->state != CONV_SUP_STARTING) {
		return;
	}
	s->pending_spawn_report = false;
	if (!accepted) {
		// The host exits 10 on a rejected hello; if it lingers, kill_grace
		// re-issues the TERMINATE. Whatever its code, it is a version reject.
		s->state = CONV_SUP_KILLING;
		s->kill_reason = CONV_SUP_KILL_REJECTED;
		s->terminate_due = false;
		s->terminate_at_ns = now_ns;
		return;
	}
	s->state = CONV_SUP_RUNNING;
	s->running_since_ns = now_ns;
	s->last_heartbeat_ns = now_ns;
	s->warming = CONV_SUP_WARMING_NONE;
	s->detail = NULL;
	s->failure_reason = XRT_CONV_FAIL_NONE;
	s->retryable = 0;
	if (s->service_shutdown) {
		s->state = CONV_SUP_STOPPING;
		s->shutdown_due = true;
		s->stop_deadline_ns = now_ns + s->cfg.shutdown_grace_ns;
	}
}

void
conv_supervisor_heartbeat(struct conv_supervisor *s, uint64_t now_ns)
{
	s->last_heartbeat_ns = now_ns;
}

void
conv_supervisor_watchdog_breach(struct conv_supervisor *s, uint64_t now_ns)
{
	(void)now_ns;
	switch (s->state) {
	case CONV_SUP_STARTING:
	case CONV_SUP_RUNNING:
	case CONV_SUP_STOPPING: begin_kill(s, CONV_SUP_KILL_WATCHDOG); break;
	default: break;
	}
}

void
conv_supervisor_channel_broken(struct conv_supervisor *s, uint64_t now_ns)
{
	(void)now_ns;
	switch (s->state) {
	case CONV_SUP_STARTING:
	case CONV_SUP_RUNNING:
		// The host may still be alive (a protocol violation, a closed pipe):
		// make sure it is gone. Its real exit code still decides (an exit 20
		// closes the pipe first).
		begin_kill(s, CONV_SUP_KILL_CHANNEL);
		break;
	default:
		// STOPPING: expected. KILLING / down: nothing to add.
		break;
	}
}

void
conv_supervisor_exited(struct conv_supervisor *s, uint64_t now_ns, int32_t exit_code)
{
	enum conv_supervisor_state was = s->state;
	enum conv_supervisor_kill_reason why = s->kill_reason;

	switch (was) {
	case CONV_SUP_STARTING:
	case CONV_SUP_RUNNING:
	case CONV_SUP_STOPPING:
	case CONV_SUP_KILLING: break;
	default: return; // stale: no process was alive
	}

	s->last_exit = exit_code;
	s->pending_spawn_report = false;
	s->terminate_due = false;
	s->shutdown_due = false;
	s->kill_reason = CONV_SUP_KILL_NONE;

	if (was == CONV_SUP_STOPPING) {
		// Asked to stop: orderly whatever the code.
		go_down_idle(s);
	} else if (was == CONV_SUP_KILLING && why == CONV_SUP_KILL_LATE_SHUTDOWN) {
		go_down_idle(s);
	} else if (was == CONV_SUP_KILLING && why == CONV_SUP_KILL_REJECTED) {
		go_failed(s, now_ns, XRT_CONV_FAIL_VERSION, 0, "hello_rejected");
	} else if (was == CONV_SUP_KILLING && (why == CONV_SUP_KILL_WATCHDOG || why == CONV_SUP_KILL_HELLO_TIMEOUT)) {
		// We killed it: always abnormal, whatever it managed to return.
		on_abnormal(s, now_ns);
	} else {
		// Unexpected exit (or a broken channel): the code decides.
		classify_exit(s, now_ns, exit_code);
	}
	s->running_since_ns = 0;
}

void
conv_supervisor_idle_exited(struct conv_supervisor *s, uint64_t now_ns)
{
	(void)now_ns;
	switch (s->state) {
	case CONV_SUP_STARTING:
	case CONV_SUP_RUNNING:
	case CONV_SUP_STOPPING:
	case CONV_SUP_KILLING: break;
	default: return; // stale: no process was alive
	}
	s->last_exit = CONV_HOST_EXIT_ORDERLY;
	s->pending_spawn_report = false;
	s->terminate_due = false;
	s->shutdown_due = false;
	s->kill_reason = CONV_SUP_KILL_NONE;
	go_down_idle(s);
	s->running_since_ns = 0;
}

void
conv_supervisor_config_changed(struct conv_supervisor *s, uint64_t now_ns)
{
	(void)now_ns;
	if (s->state == CONV_SUP_FAILED || s->state == CONV_SUP_UPDATING) {
		clear_crash_history(s);
		s->failure_reason = XRT_CONV_FAIL_NONE;
		s->retryable = 0;
		// UPDATING keeps reporting WARMING(updating) until the respawn is up.
		uint32_t warming = s->state == CONV_SUP_UPDATING ? CONV_SUP_WARMING_UPDATING : CONV_SUP_WARMING_NONE;
		go_down_idle(s);
		s->warming = warming;
		s->detail = warming != CONV_SUP_WARMING_NONE ? "updating" : NULL;
	}
}

void
conv_supervisor_retry(struct conv_supervisor *s, uint64_t now_ns)
{
	(void)now_ns;
	if (s->state == CONV_SUP_FAILED) {
		clear_crash_history(s);
		s->failure_reason = XRT_CONV_FAIL_NONE;
		s->retryable = 0;
		go_down_idle(s);
	}
}

void
conv_supervisor_service_shutdown(struct conv_supervisor *s, uint64_t now_ns)
{
	s->service_shutdown = true;
	switch (s->state) {
	case CONV_SUP_RUNNING:
		s->state = CONV_SUP_STOPPING;
		s->shutdown_due = true;
		s->stop_deadline_ns = now_ns + s->cfg.shutdown_grace_ns;
		break;
	case CONV_SUP_STARTING:
		// No channel to send on yet: terminate; not a crash.
		begin_kill(s, CONV_SUP_KILL_LATE_SHUTDOWN);
		break;
	case CONV_SUP_STOPPING:
	case CONV_SUP_KILLING: break;
	default:
		s->state = CONV_SUP_STOPPED;
		s->warming = CONV_SUP_WARMING_NONE;
		break;
	}
}


/*
 *
 * Tick.
 *
 */

enum conv_supervisor_action
conv_supervisor_tick(struct conv_supervisor *s, uint64_t now_ns, uint64_t *out_next_ns)
{
	uint64_t next = UINT64_MAX;
	enum conv_supervisor_action act = CONV_SUP_ACTION_NONE;

	switch (s->state) {
	case CONV_SUP_IDLE:
		if (may_spawn(s)) {
			act = issue_spawn(s, now_ns);
			next = s->start_ns + s->cfg.hello_timeout_ns;
		}
		break;

	case CONV_SUP_STARTING:
		if (now_ns - s->start_ns >= s->cfg.hello_timeout_ns) {
			begin_kill(s, CONV_SUP_KILL_HELLO_TIMEOUT);
			s->terminate_due = false;
			s->terminate_at_ns = now_ns;
			act = CONV_SUP_ACTION_TERMINATE;
			next = now_ns + s->cfg.kill_grace_ns;
		} else {
			next = s->start_ns + s->cfg.hello_timeout_ns;
		}
		break;

	case CONV_SUP_RUNNING:
		if (now_ns - s->running_since_ns >= s->cfg.healthy_reset_ns) {
			s->backoff_ns = s->cfg.backoff_initial_ns;
		} else if (s->backoff_ns != s->cfg.backoff_initial_ns) {
			next = min_u64(next, s->running_since_ns + s->cfg.healthy_reset_ns);
		}
		if (now_ns - s->last_heartbeat_ns >= s->cfg.heartbeat_timeout_ns) {
			begin_kill(s, CONV_SUP_KILL_WATCHDOG);
			s->terminate_due = false;
			s->terminate_at_ns = now_ns;
			act = CONV_SUP_ACTION_TERMINATE;
			next = now_ns + s->cfg.kill_grace_ns;
			break;
		}
		next = min_u64(next, s->last_heartbeat_ns + s->cfg.heartbeat_timeout_ns);
		if (!s->demand) {
			if (now_ns - s->no_demand_since_ns >= s->cfg.idle_exit_ns) {
				s->state = CONV_SUP_STOPPING;
				s->stop_deadline_ns = now_ns + s->cfg.shutdown_grace_ns;
				act = CONV_SUP_ACTION_SHUTDOWN;
				next = s->stop_deadline_ns;
				break;
			}
			next = min_u64(next, s->no_demand_since_ns + s->cfg.idle_exit_ns);
		}
		break;

	case CONV_SUP_STOPPING:
		if (s->shutdown_due) {
			s->shutdown_due = false;
			act = CONV_SUP_ACTION_SHUTDOWN;
			next = s->stop_deadline_ns;
		} else if (now_ns >= s->stop_deadline_ns) {
			begin_kill(s, CONV_SUP_KILL_LATE_SHUTDOWN);
			s->terminate_due = false;
			s->terminate_at_ns = now_ns;
			act = CONV_SUP_ACTION_TERMINATE;
			next = now_ns + s->cfg.kill_grace_ns;
		} else {
			next = s->stop_deadline_ns;
		}
		break;

	case CONV_SUP_KILLING:
		if (s->terminate_due || now_ns - s->terminate_at_ns >= s->cfg.kill_grace_ns) {
			s->terminate_due = false;
			s->terminate_at_ns = now_ns;
			act = CONV_SUP_ACTION_TERMINATE;
		}
		next = s->terminate_at_ns + s->cfg.kill_grace_ns;
		break;

	case CONV_SUP_BACKOFF:
		if (s->service_shutdown) {
			go_down_idle(s);
			break;
		}
		if (now_ns < s->next_spawn_ns) {
			next = s->next_spawn_ns;
			break;
		}
		if (!s->demand) {
			// Nobody is waiting any more: no respawn until there is demand.
			go_down_idle(s);
			break;
		}
		if (may_spawn(s)) {
			act = issue_spawn(s, now_ns);
			next = s->start_ns + s->cfg.hello_timeout_ns;
		}
		// Maintenance mutex held: wait for set_maintenance(false) + a tick.
		break;

	case CONV_SUP_UPDATING:
		if (s->service_shutdown) {
			go_down_idle(s);
			break;
		}
		// Never on a timer: demand and a free maintenance mutex (or a
		// config change, which moved us to IDLE).
		if (may_spawn(s)) {
			act = issue_spawn(s, now_ns);
			next = s->start_ns + s->cfg.hello_timeout_ns;
		}
		break;

	case CONV_SUP_FAILED:
		if (s->failure_reason == XRT_CONV_FAIL_CRASH_LOOP && !s->service_shutdown) {
			uint64_t due = s->failed_at_ns + s->cfg.crash_loop_retry_ns;
			if (now_ns >= due) {
				conv_supervisor_retry(s, now_ns);
				if (may_spawn(s)) {
					act = issue_spawn(s, now_ns);
					next = s->start_ns + s->cfg.hello_timeout_ns;
				}
			} else {
				next = due;
			}
		}
		break;
	case CONV_SUP_STOPPED: break;
	}

	if (out_next_ns != NULL) {
		*out_next_ns = next;
	}
	return act;
}

int32_t
conv_supervisor_kill_code(const struct conv_supervisor *s)
{
	(void)s;
	return CONV_HOST_EXIT_WATCHDOG;
}

void
conv_supervisor_get_status(const struct conv_supervisor *s, struct conv_supervisor_status *out)
{
	memset(out, 0, sizeof(*out));
	out->state = s->state;
	out->restarts = s->restarts;
	out->spawns = s->spawns;
	out->last_exit = s->last_exit;
	out->next_spawn_ns = s->state == CONV_SUP_BACKOFF ? s->next_spawn_ns : 0;
	out->backoff_ns = s->backoff_ns;
	out->detail = s->detail;

	if (s->state == CONV_SUP_FAILED) {
		out->conv_state = XRT_CONV_STATE_FAILED;
		out->failure_reason = s->failure_reason;
		out->retryable = s->retryable;
	} else if (s->warming != CONV_SUP_WARMING_NONE && s->state != CONV_SUP_RUNNING) {
		out->conv_state = XRT_CONV_STATE_WARMING;
	} else {
		out->conv_state = XRT_CONV_STATE_UNKNOWN;
	}
}

bool
conv_supervisor_host_alive(const struct conv_supervisor *s)
{
	switch (s->state) {
	case CONV_SUP_STARTING:
	case CONV_SUP_RUNNING:
	case CONV_SUP_STOPPING:
	case CONV_SUP_KILLING: return true;
	default: return false;
	}
}

const char *
conv_supervisor_state_str(enum conv_supervisor_state state)
{
	switch (state) {
	case CONV_SUP_IDLE: return "idle";
	case CONV_SUP_STARTING: return "starting";
	case CONV_SUP_RUNNING: return "running";
	case CONV_SUP_STOPPING: return "stopping";
	case CONV_SUP_KILLING: return "killing";
	case CONV_SUP_BACKOFF: return "backoff";
	case CONV_SUP_UPDATING: return "updating";
	case CONV_SUP_FAILED: return "failed";
	case CONV_SUP_STOPPED: return "stopped";
	}
	return "?";
}
