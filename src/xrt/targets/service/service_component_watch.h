// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The service's one watch over every component class's policy store
 *         (ADR-054 N3d, #2051).
 *
 * Every class — display processors, input providers, conversion providers —
 * keeps its `Enabled` / `Preferred*` policy in a store the user edits while
 * the service runs: `HKLM` + `HKCU` `Software\DisplayXR\<root>` on Windows,
 * the per-user manifest dir (`preferred`, `input-preferred`, the manifests'
 * `enabled`) on POSIX. Until #2051 each class that reacted to an edit grew
 * its own thread (the tray's HKLM-only `DisplayProcessors` waiter, the
 * conversion core's HKLM+HKCU `ConversionProviders` waiter) and the input
 * class had none. This module is the single replacement: ONE thread, every
 * root, debounced, dispatching a per-class callback — plus a poke entry the
 * DIAG call `system_request_component_reselect` and a provider's
 * `notify_changed` use to wake the same callback without a store edit.
 *
 * Why here and not a hook in the shared loader (`target_component_loader`):
 * the loader is linked into every runtime process — in-process apps included
 * — and must never own a thread; live re-selection is a SERVICE concern (an
 * in-process app re-reads policy at its next launch, by design). The service
 * is the only process that outlives a policy edit.
 *
 * Windows: the thread idles in `WaitForMultipleObjects` over the stop event,
 * the poke event and one `RegNotifyChangeKeyValue` event per (hive, class)
 * root — six — re-armed after every wake (a root that does not exist yet is
 * watched through its parent `Software\DisplayXR` key, as the two private
 * waiters did). A registry wake is debounced 500 ms so an installer's burst
 * of row writes (`Version` is written LAST, conversion spec §3.8) is one
 * callback, not twenty; a poke is dispatched at once.
 *
 * POSIX: a 2 s mtime poll on this same thread, NOT kqueue / inotify. The
 * per-user dir may not exist yet (the first `dp use` creates it), kqueue
 * needs one descriptor per watched file and inotify is Linux-only — two
 * implementations plus parent-dir re-arming for a directory that changes a
 * few times a year. Six `stat()`s every 2 s is unmeasurable and identical on
 * macOS and Linux. The fingerprint is per class (`preferred` → display,
 * `input-preferred` → input, each manifest by its suffix, a file's absence
 * included), so a change wakes only the class it belongs to. Not on the IPC
 * main loop's 20 Hz tick: that tick must not stat files.
 *
 * Threading: callbacks run on the watch thread, never on a client's IPC
 * thread, so a class may enumerate its root, re-read its policy and call
 * into its core from them (the conversion callback rescans and probes).
 * They must not block for long — the other classes wait behind them.
 *
 * @ingroup targets_service
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Why a class callback fires.
enum service_component_watch_reason
{
	//! The class's policy store changed (a registry wake / a file mtime moved).
	SERVICE_COMPONENT_WATCH_STORE_CHANGED = 1,
	//! `system_request_component_reselect` (the CLI after a policy write).
	SERVICE_COMPONENT_WATCH_POKED = 2,
	//! A loaded provider of the class called its host's `notify_changed`.
	SERVICE_COMPONENT_WATCH_PROVIDER_NOTIFY = 3,
};

/*!
 * One class's reaction. @p class_id is a `TARGET_COMPONENT_CLASS_*` value
 * (target_component_loader.h), @p reason a `SERVICE_COMPONENT_WATCH_*`.
 * Called on the watch thread; see the file comment for what it may do.
 */
typedef void (*service_component_watch_fn)(void *userdata, uint32_t class_id, uint32_t reason);

/*!
 * Register (or, with NULL, clear) the callback for @p class_id. One per
 * class; a second subscribe replaces the first. Safe before or after
 * @ref service_component_watch_start, from any thread.
 */
void
service_component_watch_subscribe(uint32_t class_id, service_component_watch_fn fn, void *userdata);

/*!
 * Start the watch thread. Idempotent; returns false when the thread could
 * not be created (the service then applies policy edits at its next start,
 * exactly as before #2051, and says so once in the log).
 */
bool
service_component_watch_start(void);

/*!
 * Wake @p class_id's callback with @p reason from any thread (a DIAG handler
 * on a client's IPC thread, a provider's own thread). Coalesced: several
 * pokes before the thread runs fire the callback once, with the LAST reason.
 * A no-op (false) for an unknown class or before the thread started.
 */
bool
service_component_watch_poke(uint32_t class_id, uint32_t reason);

//! Stop and join the thread. Idempotent.
void
service_component_watch_stop(void);

#ifdef __cplusplus
}
#endif
