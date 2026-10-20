// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux display LAYOUT feeder for the service's display
 *         re-probe worker (#2251; ADR-051 D6, ADR-045 R-c).
 *
 * The DRM feeder (service_drm_hotplug_linux.h) reports connectors coming and
 * going. A display-scale or arrangement change made in Settings → Displays
 * (or through `org.gnome.Mutter.DisplayConfig.ApplyMonitorsConfig`) touches
 * no connector, so the kernel sends no uevent — and the service kept the
 * panel's desktop geometry (X11 origin, logical extent, device factor) it
 * resolved at start-up, feeding the weave's present origin with the old
 * factor until it was restarted (#2251).
 *
 * This module is that missing feed: one thread on a private session-bus
 * connection, subscribed to `org.gnome.Mutter.DisplayConfig.MonitorsChanged`
 * — what Mutter emits after every monitor-configuration change, on Wayland and
 * on X11 sessions alike. It does no display work: each signal posts a request
 * to the (debounced) re-probe worker, which re-resolves the panel geometry and
 * publishes a new screen table only when something actually changed.
 *
 * Not GNOME (no Mutter on the bus): the subscription simply never fires; a
 * connector change is still caught by the DRM feeder. No session bus: logged
 * once, no thread.
 *
 * @ingroup ipc
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Called on the feeder thread for every layout change it sees.
typedef void (*service_display_layout_fn)(const char *reason);

/*!
 * Pure: is a signal with this @p interface / @p member a display-layout
 * change this feeder reports? (Mutter's `MonitorsChanged`.)
 */
bool
service_display_layout_is_change_signal(const char *interface, const char *member);

/*!
 * Start the feeder thread. @p fn must stay valid until
 * service_display_layout_stop(). Returns false when no thread could start
 * (built without D-Bus, no session bus — the reason is logged).
 */
bool
service_display_layout_start(service_display_layout_fn fn);

//! Stop and join the feeder thread (no-op when it is not running).
void
service_display_layout_stop(void);

#ifdef __cplusplus
}
#endif
