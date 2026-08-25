// Copyright 2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Various helpers for doing Windows specific things.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 *
 * @ingroup aux_os
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_windows.h"
#include "util/u_logging.h"


#ifdef __cplusplus
extern "C" {
#endif


/*!
 * This function formats a Windows error number, as returned by `GetLastError`,
 * and writes it into the given buffer.
 *
 * @param buffer     Buffer to format the error into.
 * @param size       Size of the given buffer.
 * @param err        Error number to format a string for.
 * @param remove_end Removes and trailing `\n`, `\r` and `.` characters.
 */
const char *
u_winerror(char *buffer, size_t size, DWORD err, bool remove_end);

/*!
 * Tries to grant the 'SeIncreaseBasePriorityPrivilege' privilege to this
 * process. It is needed for HIGH and REALTIME priority Vulkan queues on NVIDIA.
 *
 * @param log_level Control the amount of logging this function does.
 */
bool
u_win_grant_inc_base_priorty_base_privileges(enum u_logging_level log_level);

/*!
 * Tries to raise the CPU priority of the process as high as possible. Returns
 * false if it could not raise the priority at all. Normal processes can raise
 * themselves from NORMAL to HIGH, while REALTIME requires either administrator
 * privileges or the 'SeIncreaseBasePriorityPrivilege' privilege to be granted.
 *
 * @param log_level Control the amount of logging this function does.
 */
bool
u_win_raise_cpu_priority(enum u_logging_level log_level);

/*!
 * Small helper function that checks process arguments for which to try.
 *
 * The parsing is really simplistic and only looks at the first argument for the
 * values `nothing`, `priority`, `privilege`, `both`. No argument at all implies
 * the value `both` making the function try to set both.
 *
 * @param log_level Control the amount of logging this function does.
 * @param argc      Number of arguments, as passed into main.
 * @param argv      Array of argument strings, as passed into main.
 */
void
u_win_try_privilege_or_priority_from_args(enum u_logging_level log_level, int argc, char *argv[]);


/*!
 * Per-process DPI awareness, as Windows reports it.
 *
 * @ingroup aux_os
 */
enum u_win_dpi_awareness
{
	//! GDI hands this process VIRTUALISED (scaled-down) coordinates.
	U_WIN_DPI_UNAWARE = 0,
	//! Real pixels, but frozen at the primary monitor's DPI at logon.
	U_WIN_DPI_SYSTEM_AWARE = 1,
	//! Real pixels, per monitor, tracked across DPI changes. What we want.
	U_WIN_DPI_PER_MONITOR_AWARE = 2,
	//! Could not be queried (OS predates the query API).
	U_WIN_DPI_UNKNOWN = 3,
};

/*!
 * Make THIS PROCESS per-monitor-DPI-aware (v2), so every Win32/GDI call it
 * makes — `EnumDisplayMonitors` rects, `GetMonitorInfo`, `GetClientRect`,
 * `ClientToScreen` — returns PHYSICAL pixels instead of the scaled logical
 * coordinates a DPI-unaware process is handed (issue #1201: on a 4K panel at
 * 150% an unaware process sees a "2560x1440 display").
 *
 * Call this as the very first statement of `main`/`WinMain`, before anything
 * touches GDI. It is a belt-and-braces backstop for the embedded per-monitor-v2
 * MANIFEST (`src/xrt/targets/common/dpi_aware.manifest`), which is the primary
 * mechanism because it takes effect before any code — including static
 * initialisers and `DllMain` — runs. Where the manifest is already in force
 * this call is a no-op that reports @p out_was_already_aware.
 *
 * DPI awareness is a *process* property, so only an EXE can set it: the runtime
 * DLL and the vendor plug-ins inherit whatever their host declared.
 *
 * @param out_was_already_aware Optional. Set to true when the process was
 *                              already per-monitor aware (i.e. the manifest did
 *                              the job and this call changed nothing).
 * @return true if the process is per-monitor aware when this returns.
 */
bool
u_win_make_process_dpi_aware(bool *out_was_already_aware);

/*!
 * Query this process's DPI awareness. Never fails; returns
 * @ref U_WIN_DPI_UNKNOWN when the OS is too old to answer.
 */
enum u_win_dpi_awareness
u_win_get_process_dpi_awareness(void);

/*!
 * Human-readable name for @p awareness, for diagnostics output.
 */
const char *
u_win_dpi_awareness_to_string(enum u_win_dpi_awareness awareness);


#ifdef __cplusplus
}
#endif
