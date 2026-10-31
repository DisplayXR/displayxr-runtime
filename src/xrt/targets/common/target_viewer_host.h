// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Out-of-process hosting of the viewer-tracking provider
 *         (tracking-provider-discovery.md §4a, §10 item 1; ADR-053 §5's
 *         hosting shape, ADR-054 N8's pose page).
 *
 * Three parts:
 *
 *  - **The page name** (@ref target_viewer_page_name): where the one viewer
 *    pose page of this user's desktop lives — Windows `Local\DisplayXR-ViewerPose-v1`
 *    (the session namespace), POSIX `/dxr-vpose-<uid>`. `DXR_VIEWER_POSE_PAGE`
 *    in the environment replaces it (tests / dev boxes), and `none` disables
 *    the page in that process.
 *
 *  - **The supervisor** (@ref target_vhost_sup, PURE: no process, clock, lock
 *    or log; pinned by `tests/tests_viewer_pose_page.cpp`): lazy spawn on the
 *    first demand, restart with a backoff of 0.5 s doubling to 30 s (reset
 *    after 60 s running), a crash-loop cutoff (5 abnormal exits in 2 min →
 *    FAILED), terminal exits (the provider is not usable / bad arguments →
 *    FAILED at once). FAILED is retried after 10 min. An idle stop: no
 *    demand for 10 s stops the host in order (a webcam provider then frees
 *    the camera); the next demand spawns it again. Time the machine spent
 *    asleep does not count toward it.
 *
 *  - **The manager** (@ref target_viewer_host): owns the
 *    `displayxr-tracking-host` child, its control channel and the
 *    supervisor thread the service runs (one wake per 100 ms: a reap, a
 *    heartbeat check, a demand check — never a log line unless something
 *    changed). It writes the owner block of the page; the host writes the
 *    samples and the producer block. One WARN per spawn / exit / restart.
 *
 * Not built for Android (no os_process there): the manager calls are stubs
 * that fail, so the service-side glue falls back to in-process hosting.
 *
 * @ingroup target_common
 */

#pragma once

#include "xrt/xrt_tracking_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The host binary's base name (`.exe` appended on Windows by the caller's sibling lookup).
#ifdef _WIN32
#define TARGET_VIEWER_HOST_EXE "displayxr-tracking-host.exe"
#else
#define TARGET_VIEWER_HOST_EXE "displayxr-tracking-host"
#endif

/*!
 * @name Host exit codes
 * What `displayxr-tracking-host` returns; the supervisor reads them.
 * @{
 */
#define TARGET_VHOST_EXIT_ORDERLY 0       //!< asked to stop (or the parent went away)
#define TARGET_VHOST_EXIT_USAGE 2         //!< bad arguments: terminal
#define TARGET_VHOST_EXIT_NO_PROVIDER 10  //!< the provider is not resident (declined / untrusted / ABI): terminal
#define TARGET_VHOST_EXIT_START_FAILED 11 //!< start() failed: abnormal (retried with backoff)
#define TARGET_VHOST_EXIT_PAGE 12         //!< the page could not be opened / is invalid: abnormal
#define TARGET_VHOST_EXIT_ELEVATED 13     //!< refused to run elevated: terminal
#define TARGET_VHOST_EXIT_CHANNEL 14      //!< the control channel could not be opened: abnormal
/*! @} */

//! "orderly", "usage", "no_provider", ... for logs.
const char *
target_vhost_exit_str(int32_t code);

/*!
 * The page name for this process: `DXR_VIEWER_POSE_PAGE` when set (`none` =
 * disabled: returns false), else the platform default. false when disabled.
 */
bool
target_viewer_page_name(char *out, size_t cap);


/*
 *
 * Supervisor (pure).
 *
 */

enum target_vhost_sup_state
{
	TARGET_VHOST_SUP_IDLE = 0,     //!< no process; spawns on the first demand
	TARGET_VHOST_SUP_STARTING = 1, //!< spawned, the producer has not said hello on the page yet
	TARGET_VHOST_SUP_RUNNING = 2,  //!< the producer is up
	TARGET_VHOST_SUP_BACKOFF = 3,  //!< after an abnormal exit; respawns at next_spawn_ns
	TARGET_VHOST_SUP_FAILED = 4,   //!< crash loop / terminal exit; retried after failed_retry_ns
	TARGET_VHOST_SUP_STOPPED = 5,  //!< shutting down; never respawns
	TARGET_VHOST_SUP_STOPPING = 6, //!< idle stop asked; IDLE once it exits (killed after stop_grace_ns)
};

struct target_vhost_sup_config
{
	uint64_t backoff_initial_ns;   //!< 0.5 s
	uint64_t backoff_max_ns;       //!< 30 s
	uint64_t healthy_reset_ns;     //!< 60 s RUNNING resets the backoff
	uint32_t crash_loop_count;     //!< 5 (≤ TARGET_VHOST_SUP_HISTORY)
	uint64_t crash_loop_window_ns; //!< 2 min
	uint64_t failed_retry_ns;      //!< 10 min
	uint64_t hello_timeout_ns;     //!< 10 s without a producer hello = a crash (the manager kills it)
	uint64_t idle_stop_ns;         //!< 10 s with no demand stops the host (0 = never: runs until teardown)
	uint64_t stop_grace_ns;        //!< 3 s for an idle stop before the manager kills the host
	uint64_t suspend_gap_ns;       //!< 2 s between ticks = the machine slept; that time is not idle time
};

#define TARGET_VHOST_SUP_HISTORY 8u

struct target_vhost_sup
{
	struct target_vhost_sup_config cfg;
	enum target_vhost_sup_state state;
	bool demand;             //!< wanted now (set every tick by the manager)
	uint64_t last_demand_ns; //!< the last tick that saw demand (shifted past a suspend)
	uint64_t last_tick_ns;
	bool ticked;
	uint64_t stop_ns; //!< when the idle stop was asked
	uint64_t spawned_ns;
	uint64_t running_since_ns;
	uint64_t next_spawn_ns;
	uint64_t backoff_ns;
	uint64_t failed_ns;
	uint64_t crash_ns[TARGET_VHOST_SUP_HISTORY];
	uint32_t crash_head;
	uint32_t restarts; //!< abnormal exits so far
	uint32_t spawns;   //!< spawns issued
	uint32_t idle_stops; //!< idle stops issued
	int32_t last_exit; //!< INT32_MIN = none
};

enum target_vhost_sup_action
{
	TARGET_VHOST_SUP_ACTION_NONE = 0,
	TARGET_VHOST_SUP_ACTION_SPAWN = 1, //!< start the host; report target_vhost_sup_spawned()
	TARGET_VHOST_SUP_ACTION_KILL = 2,  //!< no hello / an ignored stop: terminate it (report the exit after)
	TARGET_VHOST_SUP_ACTION_STOP = 3,  //!< idle: ask the host to stop in order (report the exit after)
};

void
target_vhost_sup_config_defaults(struct target_vhost_sup_config *cfg);

//! @p cfg NULL = defaults.
void
target_vhost_sup_init(struct target_vhost_sup *s, const struct target_vhost_sup_config *cfg);

/*!
 * Whether a viewer is wanted now; the manager sets it before every tick.
 * Not latching: once it has been false for idle_stop_ns the host is stopped.
 */
void
target_vhost_sup_set_demand(struct target_vhost_sup *s, bool demand);

//! Outcome of a SPAWN action (false = could not start: abnormal).
void
target_vhost_sup_spawned(struct target_vhost_sup *s, uint64_t now_ns, bool ok);

//! The producer said hello on the page (its pid + generation are the child's).
void
target_vhost_sup_hello(struct target_vhost_sup *s, uint64_t now_ns);

/*!
 * The host exited with @p code. @p requested: the manager asked it to stop
 * (an orderly 0 is then not a crash). Returns true when it counts as a
 * crash (the manager logs the restart).
 */
bool
target_vhost_sup_exited(struct target_vhost_sup *s, uint64_t now_ns, int32_t code, bool requested);

//! Service teardown: STOPPED, never respawns.
void
target_vhost_sup_stop(struct target_vhost_sup *s);

/*!
 * Advance time; returns the ONE action to perform now. A gap between ticks
 * longer than suspend_gap_ns (the machine slept) is not counted as idle time.
 */
enum target_vhost_sup_action
target_vhost_sup_tick(struct target_vhost_sup *s, uint64_t now_ns);

//! The page's owner-state spelling of @p st (enum u_vpp_state).
uint32_t
target_vhost_sup_page_state(enum target_vhost_sup_state st);


/*
 *
 * Manager (the service).
 *
 */

struct target_viewer_host;

struct target_viewer_host_config
{
	const char *host_exe;    //!< absolute path of displayxr-tracking-host
	const char *page_name;   //!< the page the host opens (the service created it)
	void *page_mem;          //!< the service's own mapping of it (owner block written here)
	const char *provider_id; //!< the registration id the host loads and starts
	//! The active display's geometry, handed to the provider through the host's host iface.
	struct xrt_tracking_host_display_geometry geometry;
	bool have_geometry;
	//! ADR-054 N4b: the provenance-gate override the service runs under (-1 = none, else 0/1), passed on.
	int trust_enforce;
	//! Test seam: supervisor timings (NULL = defaults).
	const struct target_vhost_sup_config *sup;
};

//! Live facts for status (`status --json` per screen `eye_tracking.host_*`).
struct target_viewer_host_status
{
	enum target_vhost_sup_state state;
	int64_t pid; //!< the running host's pid, 0 = none
	uint32_t restarts;
	uint32_t spawns;
	int32_t last_exit; //!< INT32_MIN = none
};

/*!
 * Create the manager: nothing is spawned until the first
 * @ref target_viewer_host_demand (or a reader's page demand). Starts the
 * supervisor thread. NULL on failure (Android: always).
 */
struct target_viewer_host *
target_viewer_host_create(const struct target_viewer_host_config *cfg);

/*!
 * A viewer is needed now: spawn the host (non-blocking: the thread spawns).
 * Not latching: call it again at least every few seconds while a viewer is
 * wanted (U_VPP_DEMAND_FRESH_NS); without it the host is stopped when idle.
 */
void
target_viewer_host_demand(struct target_viewer_host *h);

void
target_viewer_host_get_status(struct target_viewer_host *h, struct target_viewer_host_status *out);

//! Stop the thread, ask the host to stop (3 s grace, then terminate), write STOPPED. NULL-safe.
void
target_viewer_host_destroy(struct target_viewer_host **h_ptr);

/*!
 * Run `displayxr-tracking-host --probe` and wait (≤ @p timeout_ms): the host
 * scans the tracking class (shared loader: path guard, provenance gate, ABI
 * gate, probe), writes the rank-0 provider's id + describe into the page's
 * producer block and exits 0, or exits TARGET_VHOST_EXIT_NO_PROVIDER. So the
 * service never loads a provider binary itself. true = usable, with the id in
 * @p out_id. @p out_ran false = the host could not be run at all (the caller
 * falls back to in-process hosting).
 */
bool
target_viewer_host_probe(const char *host_exe,
                         const char *page_name,
                         const void *page_mem,
                         int trust_enforce,
                         uint32_t timeout_ms,
                         char *out_id,
                         size_t id_cap,
                         bool *out_ran);

#ifdef __cplusplus
}
#endif
