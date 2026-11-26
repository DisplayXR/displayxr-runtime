// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-kwin-geometry-bridge: what it accepts, with no I/O.
 *
 * The bridge republishes whatever the KWin script pushes as the canonical
 * `org.displayxr.WindowGeometry1` snapshot, so it holds that input to the
 * same contract every consumer relies on (docs/specs/runtime/
 * wayland-window-geometry.md §2 and §4) before passing it on:
 *
 * - a JSON object of schema version 1 (absent = 1; anything newer refused,
 *   the consumer's own rule), with a `windows` array;
 * - no window titles: the GNOME publisher gives them only to callers it
 *   authorises, and a bridge cannot, so `title` is stripped from every entry;
 * - bounded in size.
 *
 * And only from KWin itself (@ref kwgb_comm_is_kwin): any process on the
 * session bus can call the bridge, and a forged snapshot would move every
 * DisplayXR app's weave phase.
 *
 * @ingroup targets
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The largest snapshot accepted, in bytes (a few hundred windows).
#define KWGB_MAX_PAYLOAD (1024 * 1024)

//! The snapshot served before KWin has sent one: no windows.
#define KWGB_EMPTY_SNAPSHOT "{\"version\":1,\"layout_mode\":\"logical\",\"windows\":[]}"

/*!
 * Validate and sanitise one snapshot from the KWin script. Returns a newly
 * allocated JSON string (release with @ref kwgb_free) that is safe to publish, or NULL
 * when the input is not a schema-1 snapshot.
 */
char *
kwgb_sanitize(const char *json);

//! Release a string from @ref kwgb_sanitize (cJSON's allocator). NULL is fine.
void
kwgb_free(char *s);

/*!
 * Is @p comm (a process's /proc/<pid>/comm, possibly with a trailing
 * newline) KWin's compositor process? "kwin_wayland" (Plasma 6 Wayland,
 * SteamOS Desktop Mode) or "kwin_x11".
 */
bool
kwgb_comm_is_kwin(const char *comm);

/*!
 * May the bridge run on this desktop? Only under KDE (an entry "KDE" in
 * XDG_CURRENT_DESKTOP): anywhere else a publisher of the same name may
 * appear (the GNOME extension), and a bridge that took the name first would
 * strand it.
 */
bool
kwgb_desktop_is_kde(const char *xdg_current_desktop);

#ifdef __cplusplus
}
#endif
