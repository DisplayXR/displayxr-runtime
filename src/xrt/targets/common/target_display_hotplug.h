// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Display hotplug: keep the per-monitor display-processor claims live
 *         (ADR-047 Amendment 7).
 * @ingroup targets_common
 *
 * The per-monitor DP registry used to be built once, at instance create, so a
 * 3D panel plugged in while an app ran stayed unclaimed until the app was
 * restarted, and one unplugged stayed listed. This module is the trigger and
 * the pacing of the re-run; the re-run itself (re-enumerate monitors, re-probe
 * the plug-ins that are not claim sources yet, re-resolve, publish the new
 * screen table) belongs to the instance and is passed in as a callback.
 *
 * Three ways in, all funnelled into one debounced scheduler on one worker
 * thread:
 *
 * - **the OS** — `os_display_hotplug_subscribe` (macOS: the CoreGraphics
 *   reconfiguration callback, delivered on the main run loop);
 * - **a poll** of a cheap topology signature (the OS monitor list: ids, rects,
 *   modes) — the safety net for a process whose main thread runs no run loop,
 *   and the only trigger where the OS notification is not wired;
 * - **a plug-in** — `xrt_plugin_host_iface::request_display_rescan`, for the
 *   vendor backend that recognises a panel seconds after the OS saw the monitor.
 *
 * An OS / poll event also arms a few follow-up re-runs over the next seconds
 * (@ref TARGET_HOTPLUG_FOLLOWUPS): the OS reports the monitor at once, but a
 * vendor service may need a few seconds to authenticate the panel before its
 * plug-in will claim it, and an older plug-in cannot say when it is ready.
 *
 * The pure parts — the scheduler, the screen-list comparison, the topology
 * hash — are unit-tested by `tests/tests_target_display_hotplug.cpp`.
 */

#pragma once

#include "xrt/xrt_screen.h"

#include "os/os_display_desktop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Scheduler (pure).
 *
 */

//! Quiet time after the last event before the re-run (a plug storm is one re-run).
#define TARGET_HOTPLUG_SETTLE_NS (300ull * 1000ull * 1000ull)
//! The longest an event waits however busy the storm (from its first event).
#define TARGET_HOTPLUG_MAX_DELAY_NS (1000ull * 1000ull * 1000ull)
//! Follow-up re-runs after an OS / poll event's re-run.
#define TARGET_HOTPLUG_FOLLOWUPS 8u
//! "No deadline".
#define TARGET_HOTPLUG_NEVER UINT64_MAX

//! Where an event came from (a bit each, OR-ed into @ref target_hotplug_sched::sources).
enum target_hotplug_source
{
	TARGET_HOTPLUG_SOURCE_OS = 1u << 0,     //!< The OS display-reconfiguration notification.
	TARGET_HOTPLUG_SOURCE_POLL = 1u << 1,   //!< The topology-signature poll saw a change.
	TARGET_HOTPLUG_SOURCE_PLUGIN = 1u << 2, //!< A plug-in's request_display_rescan.
	TARGET_HOTPLUG_SOURCE_TEST = 1u << 3,   //!< A test / debug request.
};

/*!
 * Debounce + follow-up pacing. Not thread-safe: the owner serialises.
 */
struct target_hotplug_sched
{
	uint64_t settle_ns;
	uint64_t max_delay_ns;
	//! Follow-up offsets from the triggering re-run, ascending.
	uint64_t followup_ns[TARGET_HOTPLUG_FOLLOWUPS];
	uint32_t followup_count;

	bool pending;
	uint64_t first_ns;
	uint64_t last_ns;
	uint32_t sources;
	//! The pending event wants follow-ups (an OS / poll / test event; a plug-in's does not).
	bool want_followups;

	bool followups_armed;
	uint64_t followup_base_ns;
	uint32_t followup_next;
};

/*!
 * Initialise. 0 for @p settle_ns / @p max_delay_ns selects the default; the
 * follow-ups are 0.5, 1, 1.5, 2, 3, 4, 6 and 8 s after the triggering re-run.
 */
void
target_hotplug_sched_init(struct target_hotplug_sched *s, uint64_t settle_ns, uint64_t max_delay_ns);

//! Record an event at @p now_ns.
void
target_hotplug_sched_event(struct target_hotplug_sched *s, uint64_t now_ns, enum target_hotplug_source src);

//! When the next re-run is due (@ref TARGET_HOTPLUG_NEVER = nothing scheduled).
uint64_t
target_hotplug_sched_deadline(const struct target_hotplug_sched *s);

/*!
 * Is a re-run due at @p now_ns? Consumes it when it is.
 *
 * @param[out] out_followup  (may be NULL) true for a follow-up re-run.
 * @param[out] out_sources   (may be NULL) the event sources it serves (0 for a follow-up).
 */
bool
target_hotplug_sched_take(struct target_hotplug_sched *s, uint64_t now_ns, bool *out_followup, uint32_t *out_sources);


/*
 *
 * Screen-list comparison (pure).
 *
 */

//! What changed between two published screen lists.
struct target_screens_change
{
	uint32_t added;            //!< Screens only in the new list.
	uint32_t removed;          //!< Screens only in the old list.
	uint32_t claimant_changed; //!< Same screen, another winning plug-in.
	uint32_t geometry_changed; //!< Same screen, another desktop rect / mode / scale.
	uint32_t other_changed;    //!< Same screen, other fields (flags, info, confidence, names).
	bool default_moved;        //!< The SYSTEM_DEFAULT screen is another one.
};

/*!
 * Compare two screen lists by screen id.
 *
 * @return true when anything differs (byte-for-byte equal lists are "no change").
 */
bool
target_screens_compare(const struct xrt_screen_list *before,
                       const struct xrt_screen_list *after,
                       struct target_screens_change *out);

/*!
 * One human-readable line naming each change ("+0x… 'name' (sim-display)",
 * "-0x…", "0x… claimant sim-display -> leia-sr", "0x… rect …").
 */
void
target_screens_change_format(const struct xrt_screen_list *before,
                             const struct xrt_screen_list *after,
                             char *buf,
                             size_t size);


/*
 *
 * Topology signature (pure hash + the OS read).
 *
 */

//! Hash a monitor list (count, ids by device name, rects, modes, scale, primary).
uint64_t
target_display_topology_hash(const struct os_display_desktop_info *mons, uint32_t count);

//! The current OS monitor list's hash (`os_display_desktop_enumerate`).
uint64_t
target_display_topology_signature(void);


/*
 *
 * The monitor (worker thread).
 *
 */

/*!
 * The re-run: re-enumerate, re-probe, re-resolve, publish. Runs on the worker
 * thread. @p sources is the OR of @ref target_hotplug_source the run serves
 * (0 for a follow-up). Returns true when the published screen list changed.
 */
typedef bool (*target_display_hotplug_rebuild_fn)(void *user, uint32_t sources, bool followup);

struct target_display_hotplug;

/*!
 * Is the monitor enabled in this process? `DXR_DISPLAY_HOTPLUG=0` turns it
 * off, `=1` on; unset = on where the OS notification is wired (macOS), off
 * elsewhere (TODO: Windows / Linux, ADR-047 Amendment 7).
 */
bool
target_display_hotplug_enabled(void);

/*!
 * Start the worker (one per process; a second start fails). The poll period
 * is 500 ms (`DXR_DISPLAY_HOTPLUG_POLL_MS`, 0 = no poll). Returns NULL when
 * disabled (@ref target_display_hotplug_enabled) or on failure.
 */
struct target_display_hotplug *
target_display_hotplug_start(target_display_hotplug_rebuild_fn fn, void *user);

/*!
 * Stop and join the worker. After it returns the rebuild callback is not
 * running and will not run again. NULL-safe.
 */
void
target_display_hotplug_stop(struct target_display_hotplug **hp_ptr);

/*!
 * Request a re-run from anywhere (any thread, never blocks on a re-run). A
 * no-op when no monitor runs.
 */
void
target_display_hotplug_request(enum target_hotplug_source src, const char *reason);

/*!
 * The `xrt_plugin_host_iface::request_display_rescan` slot: a plug-in says its
 * displays may have changed.
 */
void
target_display_hotplug_plugin_request(const char *reason);

#ifdef __cplusplus
}
#endif
