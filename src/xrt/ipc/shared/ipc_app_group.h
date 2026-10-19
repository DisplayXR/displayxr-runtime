// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Input focus follows the APPLICATION, not the connection (#962 follow-up).
 *
 * The service grants input focus to ONE client connection at a time (the
 * active / presenting one). Some applications hold several connections — the
 * browser presents from its GPU process and locates the rig and owns the hand
 * trackers from the browser process; an engine plug-in may hold a lift-only
 * link next to its session. Before this rule only the presenting connection
 * was input-allowed, so the sibling that actually asked for hands got an
 * inactive joint set, silently.
 *
 * Decision (David, 2026-10-19, #tmp-input-providers row 7): two connections
 * belong to one application — and the non-active one shares the active one's
 * INPUT focus — when they declare the same application name AND come from the
 * same process tree (parent / child / siblings under one parent), with the
 * executable path as the tie-break where the tree is unknown. Only INPUT focus
 * is shared: the session state the sibling sees (FOCUSED / VISIBLE events), the
 * presenter and the workspace's focused slot are unchanged.
 *
 * Pure, header-only, so it is unit-tested without the server.
 */

#pragma once

#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Are connections A and B two connections of one application?
 *
 * @param name_a / name_b  Declared application names ("" = unknown → never).
 * @param exe_a / exe_b    OS-derived executable paths ("" = could not derive).
 * @param pid_a / pid_b    OS-derived process ids (0 = unknown).
 * @param ppid_a / ppid_b  Parent process ids (0 = unknown on this platform / peer).
 *
 * Rule: same non-empty name; then, if both parent pids are known, the two
 * processes must be related (same process, parent/child either way, or
 * siblings under one parent); if a parent pid is unknown, the executable paths
 * must match when both are known. Two unrelated instances of the same
 * application therefore do NOT share focus when the tree is visible.
 */
static inline bool
ipc_app_group_same(const char *name_a,
                   const char *exe_a,
                   long pid_a,
                   long ppid_a,
                   const char *name_b,
                   const char *exe_b,
                   long pid_b,
                   long ppid_b)
{
	if (name_a == NULL || name_b == NULL || name_a[0] == '\0' || name_b[0] == '\0') {
		return false;
	}
	if (strcmp(name_a, name_b) != 0) {
		return false;
	}
	if (pid_a > 0 && pid_a == pid_b) {
		return true; // two connections of the same process
	}
	if (ppid_a > 0 && ppid_b > 0 && pid_a > 0 && pid_b > 0) {
		// The tree is visible on both sides: it decides.
		return ppid_a == pid_b || ppid_b == pid_a || ppid_a == ppid_b;
	}
	// Tree unknown on at least one side: the executable is the tie-break.
	if (exe_a != NULL && exe_b != NULL && exe_a[0] != '\0' && exe_b[0] != '\0') {
		return strcmp(exe_a, exe_b) == 0;
	}
	// Name alone: accept — the declared name is already the app's own claim,
	// and a platform with neither tree nor path (a stub) has nothing better.
	return true;
}

#ifdef __cplusplus
}
#endif
