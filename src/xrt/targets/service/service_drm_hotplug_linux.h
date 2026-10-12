// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux display hotplug feeder for the service's display
 *         re-probe worker (ADR-051 D6, ADR-045 R-c).
 *
 * Windows feeds `ipc_server_request_display_reprobe()` from the tray's
 * `WM_DISPLAYCHANGE` / `WM_DEVICECHANGE`; nothing fed it on Linux. This module
 * is that feeder: one thread on a raw `NETLINK_KOBJECT_UEVENT` socket (no
 * libudev: the .deb's `STABLE_SONAMES` list has no room for it) that reports
 * every kernel uevent with `SUBSYSTEM=drm` and `ACTION=change` — what the
 * kernel sends when a connector's status changes (`HOTPLUG=1`). Where the
 * socket cannot be opened (a sandbox, a seccomp profile), it polls the
 * `/sys/class/drm/card*-*` connectors' `status` files every 2 s instead and
 * reports any change. Which mode runs is logged once.
 *
 * The worker does no display work itself: it only calls the callback, which
 * posts a request to the (debounced) re-probe worker.
 *
 * @ingroup ipc
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Called on the feeder thread for every display hotplug it sees.
typedef void (*service_drm_hotplug_fn)(const char *reason);

/*!
 * Pure: is @p buf (one `NETLINK_KOBJECT_UEVENT` datagram of @p len bytes: a
 * `ACTION@DEVPATH` header followed by NUL-separated `KEY=VALUE` pairs) a DRM
 * connector change? True iff it carries `SUBSYSTEM=drm` and `ACTION=change`.
 * udev's re-broadcasts (`libudev` magic header) are not kernel uevents and are
 * rejected.
 */
bool
service_drm_uevent_is_hotplug(const char *buf, size_t len);

/*!
 * Pure: FNV-1a over the connectors' `status` strings, in the order given;
 * pairs of `name`, `status`. Used by the polling fallback.
 */
uint64_t
service_drm_status_hash(const char *const *names, const char *const *statuses, uint32_t count);

/*!
 * Start the feeder thread. @p fn must stay valid until
 * service_drm_hotplug_stop(). Returns false when no thread could start (the
 * reason is logged).
 */
bool
service_drm_hotplug_start(service_drm_hotplug_fn fn);

//! Stop and join the feeder thread (no-op when it is not running).
void
service_drm_hotplug_stop(void);

#ifdef __cplusplus
}
#endif
