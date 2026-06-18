// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS AppKit pump for the service main thread (shell Tier 1, #48).
 * @ingroup ipc_server
 *
 * The macOS service compositor (null + comp_multi + comp_window_macos) creates
 * its NSWindow on the main thread via `dispatch_sync(dispatch_get_main_queue())`
 * from the comp_multi render thread. The service main thread runs the kqueue IPC
 * mainloop (@ref ipc_server_mainloop_macos.c), not an NSApp runloop, so without
 * draining the main dispatch queue that `dispatch_sync` would deadlock. This
 * pump is called once per (non-blocking) mainloop poll to service the main
 * dispatch queue + AppKit events.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Service the main thread's GCD queue + AppKit event queue, non-blocking.
 * Lazily bootstraps NSApplication on first call. Safe to call every poll
 * iteration; cheap when idle. Must be called from the main thread.
 */
void
ipc_server_macos_pump_main_thread(void);

#ifdef __cplusplus
}
#endif
