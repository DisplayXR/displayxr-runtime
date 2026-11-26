// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Serving org.displayxr.WindowGeometry1 from a stand-alone bridge.
 *
 * Shared by the bridges that publish window geometry for compositors with no
 * in-process publisher (docs/specs/runtime/wayland-window-geometry.md):
 * displayxr-kwin-geometry-bridge (KWin script feed) and
 * displayxr-hypr-geometry-bridge (Hyprland IPC). It owns the canonical bus
 * name the way every bridge must:
 *
 * - never queues for it and allows replacement (DO_NOT_QUEUE |
 *   ALLOW_REPLACEMENT): a real, in-compositor publisher always wins, and the
 *   bridge leaves on NameLost;
 * - serves `GetWindows() -> s` (the latest snapshot; no windows until the
 *   first publish) and emits `WindowsChanged(s)` only when the snapshot text
 *   actually changes.
 *
 * @ingroup targets
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WGEOM_BUS_NAME "org.displayxr.WindowGeometry"
#define WGEOM_BUS_PATH "/org/displayxr/WindowGeometry"
#define WGEOM_BUS_IFACE "org.displayxr.WindowGeometry1"

//! The snapshot served before the first publish: no windows.
#define WGEOM_EMPTY_SNAPSHOT "{\"version\":1,\"layout_mode\":\"logical\",\"windows\":[]}"

struct wgeom_bus;
typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;

/*!
 * An extra method handler (e.g. the KWin bridge's Feed1.Publish). Return true
 * when it handled (and replied to) @p msg.
 */
typedef bool (*wgeom_bus_method_fn)(struct wgeom_bus *bus, DBusConnection *conn, DBusMessage *msg, void *user);

/*!
 * Connect to the session bus and take the name. Returns NULL (after logging
 * why, prefixed with @p log_name) when there is no bus or the name is already
 * owned; that is a normal exit (status 0) for a bridge.
 *
 * @param extra_introspect_xml  extra `<interface>` elements, or NULL.
 */
struct wgeom_bus *
wgeom_bus_open(const char *log_name, const char *extra_introspect_xml, wgeom_bus_method_fn extra, void *user);

/*!
 * Make @p snapshot (copied) what GetWindows returns, emitting WindowsChanged
 * when it differs from the previous one. Returns true when it changed.
 */
bool
wgeom_bus_publish(struct wgeom_bus *bus, const char *snapshot);

//! The connection's fd, to poll for readability beside other fds; -1 if none.
int
wgeom_bus_fd(struct wgeom_bus *bus);

/*!
 * Read, write and dispatch, waiting at most @p timeout_ms (0 = do what is
 * ready now). Returns false when the bridge must exit: the bus went away or
 * another publisher took the name.
 */
bool
wgeom_bus_dispatch(struct wgeom_bus *bus, int timeout_ms);

void
wgeom_bus_close(struct wgeom_bus **bus_ptr);

#ifdef __cplusplus
}
#endif
