// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Minimal systemd notify protocol (sd_notify) without libsystemd (#2515).
 *
 * The desktop-Linux service is socket-activated by a systemd user unit. A
 * service that wedges (a plug-in destructor deadlocked in exit(), #2515) keeps
 * the unit "active (running)" and the activated socket attached to a process
 * that will never accept again; every later client hangs. The unit therefore
 * carries `WatchdogSec=` + `TimeoutStopSec=`, and the service speaks the notify
 * protocol: `WATCHDOG=1` from its main loop (so a wedged main loop stops the
 * pings and systemd kills it), `STOPPING=1` when its exit path starts (so the
 * stop timeout bounds the exit itself).
 *
 * The protocol is one datagram of `KEY=VALUE` lines to the AF_UNIX socket named
 * by `$NOTIFY_SOCKET` (a path, or `@name` for the abstract namespace). It is
 * implemented here in a few lines so the .deb gains no libsystemd DT_NEEDED.
 * With `$NOTIFY_SOCKET` unset (not under systemd, or a dev run) every call is a
 * no-op. Sending is real on desktop Linux only; elsewhere the functions compile
 * to no-ops, the parsers are pure and testable everywhere.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Parse a `$NOTIFY_SOCKET` value into the bytes of a `sockaddr_un::sun_path`.
 *
 * `/path` is a filesystem socket (copied with its terminator); `@name` is the
 * abstract namespace (a leading NUL, then `name`, no terminator). Anything
 * else (empty, relative, too long for @p out_size) is refused.
 *
 * @return the number of meaningful bytes written to @p out_path (the length to
 *         add to `offsetof(struct sockaddr_un, sun_path)`), or 0 when refused.
 */
size_t
u_sd_notify_parse_socket(const char *value, char *out_path, size_t out_size);

/*!
 * Parse `$WATCHDOG_USEC` / `$WATCHDOG_PID`: the watchdog interval in
 * microseconds, or 0 when there is no watchdog for @p self_pid (unset, not a
 * positive integer, or `$WATCHDOG_PID` set and naming another process).
 */
uint64_t
u_sd_notify_parse_watchdog_usec(const char *watchdog_usec, const char *watchdog_pid, long self_pid);

/*!
 * Read and cache `$NOTIFY_SOCKET` / `$WATCHDOG_USEC` / `$WATCHDOG_PID`, then
 * unset them so spawned children (the workspace controller, satellites) do
 * not inherit them. Call once, early in main(), before other threads start.
 */
void
u_sd_notify_init(void);

//! Send @p state (e.g. "READY=1", "STOPPING=1") to the manager. False when not under systemd or the send failed.
bool
u_sd_notify_send(const char *state);

//! True when the manager asked for watchdog pings for this process.
bool
u_sd_notify_watchdog_enabled(void);

/*!
 * Send `WATCHDOG=1` when at least half the watchdog interval passed since the
 * last ping. Call from a loop that stops running when the process wedges
 * (never from an independent timer thread). Cheap no-op without a watchdog.
 */
void
u_sd_notify_watchdog_tick(uint64_t now_ns);

#ifdef __cplusplus
}
#endif
