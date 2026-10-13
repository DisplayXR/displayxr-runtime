// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion-host supervisor: the pure lifecycle state machine of one
 *         satellite host (ADR-053 D5; roadmap conversion-providers.md §3.4,
 *         §3.5 "Crash -> WARMING", §3.6 respawn rules).
 *
 * PURE: no process, no clock, no lock, no log. The owner (the satellite proxy,
 * conv_host_proxy.c) feeds it events with the current time, calls
 * conv_supervisor_tick() and performs the ONE action it returns, then reports
 * the outcome as the next event. Tests drive it with a scripted clock.
 *
 * Rules implemented (§3.4 table):
 *  - Lazy spawn: nothing runs until there is demand (a warm / stream / job
 *    needs the host; the describe cache answers before that).
 *  - Hello timeout: a host that has not said an accepted hello within
 *    hello_timeout_ns is killed (exit 30) and counts as a crash.
 *  - Restart backoff 0.5 s doubling to a 30 s cap; reset after 10 min
 *    healthy (continuously RUNNING).
 *  - Crash-loop cutoff: 5 abnormal exits within 10 min -> FAILED(crash_loop)
 *    for all the provider's tasks. Cleared by a configuration change
 *    (registry row / manifest), an explicit retry (conv_supervisor_retry) or
 *    crash_loop_retry_ns (30 min, as conv_readiness retries it).
 *  - Exit codes: 0 after a requested shutdown = orderly; 0 unrequested,
 *    30 (watchdog), a broken channel, a failed spawn and every unlisted code
 *    = abnormal (backoff + crash-loop accounting); 10 / 11 / 12 / 13 / 14 =
 *    FAILED with a matching reason, no respawn until a configuration change
 *    or a retry; 20 = closed for update: no backoff, no accounting, respawn
 *    only on a configuration change, or when demand exists and the
 *    maintenance mutex is free — never on a timer.
 *  - Watchdog: a breach reported by the call watchdog, or no heartbeat for
 *    heartbeat_timeout_ns, kills the host (exit 30).
 *  - Idle exit: no demand for idle_exit_ns while RUNNING -> orderly shutdown.
 *  - Orderly shutdown: the host must exit within shutdown_grace_ns (3 s),
 *    else it is terminated; that late exit is not a crash.
 *  - Every spawn (first, backoff expiry, update) waits while the maintenance
 *    mutex is held (an installer's Restart Manager session, §3.6).
 *
 * Reported readiness (conv_supervisor_get_status): while the host is down
 * after an abnormal exit or a kill, every task of the provider is
 * WARMING(restarting, n); after exit 20 WARMING(updating); after a cutoff or
 * a terminal exit FAILED(reason). Otherwise the override is
 * XRT_CONV_STATE_UNKNOWN, meaning "use the provider's own state".
 *
 * @ingroup aux_conversion
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum conv_supervisor_state
{
	CONV_SUP_IDLE = 0,     //!< no process; spawns on demand
	CONV_SUP_STARTING = 1, //!< SPAWN issued; waiting for spawned() then an accepted hello
	CONV_SUP_RUNNING = 2,  //!< hello accepted
	CONV_SUP_STOPPING = 3, //!< SHUTDOWN sent; waiting for the exit (grace deadline)
	CONV_SUP_KILLING = 4,  //!< TERMINATE issued; waiting for the exit
	CONV_SUP_BACKOFF = 5,  //!< after an abnormal exit; respawns at next_spawn_ns if demand
	CONV_SUP_UPDATING = 6, //!< after exit 20; respawns on config change or (demand && mutex free)
	CONV_SUP_FAILED = 7,   //!< terminal until config change / retry
	CONV_SUP_STOPPED = 8,  //!< the service is shutting down; never respawns
};

enum conv_supervisor_action
{
	CONV_SUP_ACTION_NONE = 0,
	CONV_SUP_ACTION_SPAWN = 1,     //!< start the host; report conv_supervisor_spawned()
	CONV_SUP_ACTION_SHUTDOWN = 2,  //!< send SHUTDOWN(orderly) on the channel
	CONV_SUP_ACTION_TERMINATE = 3, //!< kill the process with conv_supervisor_kill_code()
};

//! Why the supervisor killed / is killing the host.
enum conv_supervisor_kill_reason
{
	CONV_SUP_KILL_NONE = 0,
	CONV_SUP_KILL_WATCHDOG = 1,      //!< call watchdog breach or heartbeat silence
	CONV_SUP_KILL_HELLO_TIMEOUT = 2, //!< no accepted hello in time
	CONV_SUP_KILL_CHANNEL = 3,       //!< the control channel broke / the peer misbehaved
	CONV_SUP_KILL_LATE_SHUTDOWN = 4, //!< orderly shutdown overran its grace period
	CONV_SUP_KILL_REJECTED = 5,      //!< hello rejected and the host did not exit by itself
};

struct conv_supervisor_config
{
	uint64_t backoff_initial_ns;   //!< 0.5 s
	uint64_t backoff_max_ns;       //!< 30 s
	uint64_t healthy_reset_ns;     //!< 10 min RUNNING resets the backoff
	uint32_t crash_loop_count;     //!< 5 (≤ CONV_SUP_MAX_CRASH_HISTORY)
	uint64_t crash_loop_window_ns; //!< 10 min
	uint64_t crash_loop_retry_ns;  //!< 30 min after the cutoff, FAILED(crash_loop) clears (conv_readiness.h)
	uint64_t idle_exit_ns;         //!< 10 min without demand
	uint64_t shutdown_grace_ns;    //!< 3 s
	uint64_t hello_timeout_ns;     //!< 10 s
	uint64_t heartbeat_timeout_ns; //!< 10 s (heartbeats come at 1 Hz)
	uint64_t kill_grace_ns;        //!< after TERMINATE: re-issue it if no exit was seen (5 s)
};

#define CONV_SUP_MAX_CRASH_HISTORY 16u

/*! @name conv_supervisor::warming @{ */
#define CONV_SUP_WARMING_NONE 0u
#define CONV_SUP_WARMING_RESTARTING 1u //!< after an abnormal exit / kill
#define CONV_SUP_WARMING_UPDATING 2u   //!< after exit 20
/*! @} */

//! Exit code to report through conv_supervisor_exited() when SPAWN failed.
#define CONV_SUP_EXIT_SPAWN_FAILED (-1)

struct conv_supervisor_status
{
	enum conv_supervisor_state state;
	uint32_t conv_state;     //!< XRT_CONV_STATE_* override; UNKNOWN (0) = use the provider's own
	uint32_t failure_reason; //!< XRT_CONV_FAIL_* when conv_state is FAILED
	uint32_t retryable;
	const char *detail;  //!< "restarting", "updating", "crash_loop", "abi", ...; static string
	uint32_t restarts;   //!< abnormal exits since init / the last clear
	uint32_t spawns;     //!< SPAWN actions issued
	int32_t last_exit;   //!< last exit code seen (INT32_MIN = none)
	uint64_t next_spawn_ns; //!< BACKOFF: when the next spawn is due
	uint64_t backoff_ns;    //!< the backoff the NEXT abnormal exit will use
};

struct conv_supervisor
{
	struct conv_supervisor_config cfg;
	enum conv_supervisor_state state;

	bool demand;
	uint64_t no_demand_since_ns; //!< valid while !demand
	bool maintenance_held;
	bool service_shutdown; //!< STOPPED once the host is down
	uint32_t warming;      //!< CONV_SUP_WARMING_*: why the tasks report WARMING until RUNNING
	bool pending_spawn_report; //!< SPAWN issued, spawned() not yet reported

	enum conv_supervisor_kill_reason kill_reason;
	bool terminate_due;     //!< a TERMINATE action is owed
	uint64_t terminate_at_ns; //!< when the last TERMINATE went out
	bool shutdown_due;      //!< a SHUTDOWN action is owed

	uint64_t start_ns;        //!< STARTING: SPAWN issued
	uint64_t running_since_ns; //!< RUNNING: hello accepted
	uint64_t last_heartbeat_ns;
	uint64_t stop_deadline_ns; //!< STOPPING
	uint64_t next_spawn_ns;    //!< BACKOFF
	uint64_t backoff_ns;       //!< next abnormal exit's delay

	uint64_t crash_ns[CONV_SUP_MAX_CRASH_HISTORY]; //!< ring of abnormal exit times
	uint32_t crash_head;                           //!< total recorded (ring cursor)

	uint32_t failure_reason;
	uint32_t retryable;
	const char *detail;
	uint64_t failed_at_ns; //!< FAILED: when
	uint32_t restarts;
	uint32_t spawns;
	int32_t last_exit;
};

//! The §3.4 defaults.
void
conv_supervisor_config_defaults(struct conv_supervisor_config *cfg);

//! @p cfg NULL = defaults.
void
conv_supervisor_init(struct conv_supervisor *s, const struct conv_supervisor_config *cfg);

/*
 * Events. Every one takes the current monotonic time.
 */

//! Demand present (a bound task, an open stream, a job, a prewarm pin) or not.
void
conv_supervisor_set_demand(struct conv_supervisor *s, uint64_t now_ns, bool demand);

//! The maintenance mutex (`Global\DisplayXR-ConvMaintenance-<id>`) is held. POSIX: always false.
void
conv_supervisor_set_maintenance(struct conv_supervisor *s, bool held);

//! Outcome of a SPAWN action. false = the process could not be started (abnormal).
void
conv_supervisor_spawned(struct conv_supervisor *s, uint64_t now_ns, bool ok);

//! The host's hello was checked: accepted (RUNNING) or rejected (the host should exit 10; killed if it does not).
void
conv_supervisor_hello(struct conv_supervisor *s, uint64_t now_ns, bool accepted);

//! A heartbeat arrived.
void
conv_supervisor_heartbeat(struct conv_supervisor *s, uint64_t now_ns);

//! The call watchdog saw a breach (roadmap §5.3 limits; decided by the proxy).
void
conv_supervisor_watchdog_breach(struct conv_supervisor *s, uint64_t now_ns);

//! The control channel broke or the peer violated the protocol (ring corrupt, bad frame).
void
conv_supervisor_channel_broken(struct conv_supervisor *s, uint64_t now_ns);

//! The process exited with @p exit_code (as the OS reports it).
void
conv_supervisor_exited(struct conv_supervisor *s, uint64_t now_ns, int32_t exit_code);

//! The provider's registry row or manifest changed: clears FAILED and UPDATING gates.
void
conv_supervisor_config_changed(struct conv_supervisor *s, uint64_t now_ns);

//! Explicit retry (a CTRL re-warm): clears FAILED (crash history too).
void
conv_supervisor_retry(struct conv_supervisor *s, uint64_t now_ns);

//! The service is shutting down: stop the host (orderly), never respawn.
void
conv_supervisor_service_shutdown(struct conv_supervisor *s, uint64_t now_ns);

/*!
 * Advance timers and return the one action to perform now. @p out_next_ns
 * (may be NULL) receives the earliest time a later tick could act without a
 * new event (UINT64_MAX = only on an event).
 */
enum conv_supervisor_action
conv_supervisor_tick(struct conv_supervisor *s, uint64_t now_ns, uint64_t *out_next_ns);

//! The exit code to terminate with (CONV_HOST_EXIT_WATCHDOG for every kill).
int32_t
conv_supervisor_kill_code(const struct conv_supervisor *s);

void
conv_supervisor_get_status(const struct conv_supervisor *s, struct conv_supervisor_status *out);

//! Process is (or may be) alive: STARTING, RUNNING, STOPPING, KILLING.
bool
conv_supervisor_host_alive(const struct conv_supervisor *s);

const char *
conv_supervisor_state_str(enum conv_supervisor_state state);

#ifdef __cplusplus
}
#endif
