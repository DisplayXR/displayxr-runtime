// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The service's one watch over every component class's policy store
 *         (implementation; see service_component_watch.h for the design).
 * @ingroup targets_service
 */

#include "service_component_watch.h"

#include "xrt/xrt_config_os.h"

#include "target_component_loader.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include <stdio.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include "xrt/xrt_windows.h"
#else
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#endif

//! Pending-reason slots: one per class id (index 0 unused).
#define WATCH_CLASSES TARGET_COMPONENT_CLASS_COUNT

//! An installer writes a row as several values; `Version` comes last (§3.8).
#define WATCH_DEBOUNCE_MS 500

//! POSIX: how often the per-user dir is fingerprinted while idle.
#define WATCH_POLL_NS (2ull * 1000ull * 1000ull * 1000ull)

static struct
{
	struct os_mutex mtx;
	bool mtx_ready;
	bool running;

	struct
	{
		service_component_watch_fn fn;
		void *userdata;
	} subs[WATCH_CLASSES];

	//! SERVICE_COMPONENT_WATCH_* per class, 0 = nothing pending (guarded by mtx).
	uint32_t pending[WATCH_CLASSES];

#ifdef XRT_OS_WINDOWS
	HANDLE thread;
	HANDLE stop_ev; //!< manual-reset
	HANDLE poke_ev; //!< auto-reset
#else
	struct os_thread thread;
	struct os_cond cond;
	bool stop;
	uint64_t fingerprint[WATCH_CLASSES];
	bool fingerprint_ready;
#endif
} g;


/*
 *
 * Dispatch (both platforms).
 *
 */

static const char *
reason_str(uint32_t reason)
{
	switch (reason) {
	case SERVICE_COMPONENT_WATCH_STORE_CHANGED: return "policy store changed";
	case SERVICE_COMPONENT_WATCH_POKED: return "reselect requested";
	case SERVICE_COMPONENT_WATCH_PROVIDER_NOTIFY: return "provider notify_changed";
	default: return "?";
	}
}

//! Caller holds g.mtx. Note @p reason for @p class_id; an earlier pending reason is kept.
static void
note_pending_locked(uint32_t class_id, uint32_t reason)
{
	if (class_id == 0 || class_id >= WATCH_CLASSES) {
		return;
	}
	if (g.pending[class_id] == 0) {
		g.pending[class_id] = reason;
	}
}

//! Watch thread: fire every pending class's callback, off the lock.
static void
dispatch_pending(void)
{
	uint32_t reasons[WATCH_CLASSES];
	service_component_watch_fn fns[WATCH_CLASSES];
	void *uds[WATCH_CLASSES];

	os_mutex_lock(&g.mtx);
	for (uint32_t c = 0; c < WATCH_CLASSES; c++) {
		reasons[c] = g.pending[c];
		g.pending[c] = 0;
		fns[c] = g.subs[c].fn;
		uds[c] = g.subs[c].userdata;
	}
	os_mutex_unlock(&g.mtx);

	for (uint32_t c = 1; c < WATCH_CLASSES; c++) {
		if (reasons[c] == 0) {
			continue;
		}
		if (fns[c] == NULL) {
			U_LOG_I("component watch: %s — %s, but no subscriber.", target_component_class_id_str(c),
			        reason_str(reasons[c]));
			continue;
		}
		// WARN, not INFO: one line per user action (a policy write), and the
		// only trace the display class leaves when the re-probe has nothing
		// to swap (the primary DP waits for the next session, §4.2).
		U_LOG_W("component watch: %s — %s; re-evaluating now.", target_component_class_id_str(c),
		        reason_str(reasons[c]));
		fns[c](uds[c], c, reasons[c]);
	}
}

static bool
ensure_mutex(void)
{
	if (!g.mtx_ready) {
		g.mtx_ready = os_mutex_init(&g.mtx) == 0;
	}
	return g.mtx_ready;
}


/*
 *
 * Windows: RegNotifyChangeKeyValue over the eight roots (four classes x HKLM + HKCU).
 *
 */

#ifdef XRT_OS_WINDOWS

static const char *const k_registry_roots[WATCH_CLASSES] = {
    NULL,
    "Software\\DisplayXR\\DisplayProcessors",
    "Software\\DisplayXR\\InputProviders",
    "Software\\DisplayXR\\ConversionProviders",
    "Software\\DisplayXR\\TrackingProviders",
};

//! Open @p root for KEY_NOTIFY. While it does not exist yet, open its parent
//! instead and set @p *on_parent: the parent watch covers every sibling class
//! too, so a wake from such a slot only means something if the root has since
//! been created (root_exists()) — otherwise it was another class's write.
static HKEY
open_for_notify(HKEY hive, const char *root, bool *on_parent)
{
	const REGSAM sam = KEY_NOTIFY | (hive == HKEY_LOCAL_MACHINE ? KEY_WOW64_64KEY : 0);
	HKEY key = NULL;
	*on_parent = false;
	if (RegOpenKeyExA(hive, root, 0, sam, &key) == ERROR_SUCCESS) {
		return key;
	}
	if (RegOpenKeyExA(hive, "Software\\DisplayXR", 0, sam, &key) == ERROR_SUCCESS) {
		*on_parent = true;
		return key;
	}
	return NULL;
}

static bool
root_exists(HKEY hive, const char *root)
{
	const REGSAM sam = KEY_READ | (hive == HKEY_LOCAL_MACHINE ? KEY_WOW64_64KEY : 0);
	HKEY key = NULL;
	if (RegOpenKeyExA(hive, root, 0, sam, &key) != ERROR_SUCCESS) {
		return false;
	}
	RegCloseKey(key);
	return true;
}

static DWORD WINAPI
watch_thread_body(LPVOID param)
{
	(void)param;
	// Slot i: class 1 + i/2, hive HKLM for even i, HKCU for odd i.
	enum
	{
		REG_SLOTS = 2 * (WATCH_CLASSES - 1)
	};
	HANDLE evs[REG_SLOTS];
	for (int i = 0; i < REG_SLOTS; i++) {
		evs[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
		if (evs[i] == NULL) {
			for (int k = 0; k < i; k++) {
				CloseHandle(evs[k]);
			}
			return 1;
		}
	}
	const DWORD filter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC;

	for (;;) {
		// Arm first, dispatch second: a write that lands while a callback
		// re-reads its store signals the freshly armed event and the next
		// wait returns at once, so nothing falls between a read and a re-arm.
		HKEY keys[REG_SLOTS];
		bool on_parent[REG_SLOTS];
		int armed = 0;
		for (int i = 0; i < REG_SLOTS; i++) {
			const uint32_t cls = 1u + (uint32_t)(i / 2);
			keys[i] = open_for_notify((i % 2) == 0 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
			                          k_registry_roots[cls], &on_parent[i]);
			if (keys[i] != NULL) {
				if (RegNotifyChangeKeyValue(keys[i], TRUE, filter, evs[i], TRUE) == ERROR_SUCCESS) {
					armed++;
				} else {
					RegCloseKey(keys[i]);
					keys[i] = NULL;
				}
			}
		}

		dispatch_pending();

		HANDLE hs[2 + REG_SLOTS];
		hs[0] = g.stop_ev;
		hs[1] = g.poke_ev;
		for (int i = 0; i < REG_SLOTS; i++) {
			hs[2 + i] = evs[i];
		}
		// Nothing armed (no DisplayXR key at all): still wake on poke / stop,
		// and re-open slowly.
		const DWORD w = WaitForMultipleObjects(2 + REG_SLOTS, hs, FALSE, armed > 0 ? INFINITE : 10000);
		for (int i = 0; i < REG_SLOTS; i++) {
			if (keys[i] != NULL) {
				RegCloseKey(keys[i]);
			}
		}
		if (w == WAIT_OBJECT_0) {
			break;
		}
		if (w == WAIT_OBJECT_0 + 1) {
			continue; // poke: dispatched at the top of the loop, undebounced
		}
		if (w >= WAIT_OBJECT_0 + 2 && w < WAIT_OBJECT_0 + 2 + REG_SLOTS) {
			// One write can signal several slots (a parent-armed slot plus
			// the owning class's own; an installer touching both hives),
			// WaitForMultipleObjects reports only the lowest, and the
			// re-arm at the top of the loop loses the rest. So: debounce
			// the burst first, then drain every slot event and decide per
			// slot — a parent-armed slot counts only if its own root exists
			// by now (the write created it); a sibling's write is the
			// sibling's own event, which has fired by the end of the
			// debounce. No fan-out to classes the write did not touch.
			bool fired[REG_SLOTS];
			for (int i = 0; i < REG_SLOTS; i++) {
				fired[i] = i == (int)(w - (WAIT_OBJECT_0 + 2));
			}
			if (WaitForSingleObject(g.stop_ev, WATCH_DEBOUNCE_MS) == WAIT_OBJECT_0) {
				break;
			}
			for (int i = 0; i < REG_SLOTS; i++) {
				fired[i] = fired[i] || WaitForSingleObject(evs[i], 0) == WAIT_OBJECT_0;
			}
			bool noted = false;
			bool parent_only = false;
			os_mutex_lock(&g.mtx);
			for (int i = 0; i < REG_SLOTS; i++) {
				if (!fired[i]) {
					continue;
				}
				const uint32_t cls = 1u + (uint32_t)(i / 2);
				const HKEY hive = (i % 2) == 0 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
				if (!on_parent[i] || root_exists(hive, k_registry_roots[cls])) {
					note_pending_locked(cls, SERVICE_COMPONENT_WATCH_STORE_CHANGED);
					noted = true;
				} else {
					parent_only = true;
				}
			}
			if (!noted && parent_only) {
				// Only parent-tree wakes, and none of their roots exists:
				// something under Software\DisplayXR changed that no slot
				// owns (a root deleted, or a sibling's event not yet
				// delivered). Re-read whatever of ours exists in that hive
				// — rare, and the re-reads are idempotent.
				for (int i = 0; i < REG_SLOTS; i++) {
					const HKEY hive = (i % 2) == 0 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
					if (fired[i] && root_exists(hive, k_registry_roots[1u + (uint32_t)(i / 2)])) {
						note_pending_locked(1u + (uint32_t)(i / 2),
						                    SERVICE_COMPONENT_WATCH_STORE_CHANGED);
					}
				}
			}
			os_mutex_unlock(&g.mtx);
		}
		// WAIT_TIMEOUT / WAIT_FAILED: loop, re-open.
	}
	for (int i = 0; i < REG_SLOTS; i++) {
		CloseHandle(evs[i]);
	}
	return 0;
}

bool
service_component_watch_start(void)
{
	if (g.running) {
		return true;
	}
	if (!ensure_mutex()) {
		return false;
	}
	g.stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
	g.poke_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (g.stop_ev != NULL && g.poke_ev != NULL) {
		g.thread = CreateThread(NULL, 0, watch_thread_body, NULL, 0, NULL);
	}
	if (g.thread == NULL) {
		U_LOG_W(
		    "component watch: could not start the policy-store watch thread (error %lu) — Enabled / "
		    "Preferred edits apply at the next service start.",
		    GetLastError());
		if (g.stop_ev != NULL) {
			CloseHandle(g.stop_ev);
			g.stop_ev = NULL;
		}
		if (g.poke_ev != NULL) {
			CloseHandle(g.poke_ev);
			g.poke_ev = NULL;
		}
		return false;
	}
	g.running = true;
	U_LOG_W(
	    "component watch: up — HKLM + HKCU DisplayProcessors / InputProviders / ConversionProviders "
	    "(debounce %u ms).",
	    (unsigned)WATCH_DEBOUNCE_MS);
	return true;
}

bool
service_component_watch_poke(uint32_t class_id, uint32_t reason)
{
	if (!g.running || class_id == 0 || class_id >= WATCH_CLASSES) {
		return false;
	}
	os_mutex_lock(&g.mtx);
	g.pending[class_id] = reason; // explicit: the last poke's reason wins
	os_mutex_unlock(&g.mtx);
	SetEvent(g.poke_ev);
	return true;
}

void
service_component_watch_stop(void)
{
	if (!g.running) {
		return;
	}
	SetEvent(g.stop_ev);
	WaitForSingleObject(g.thread, 5000);
	CloseHandle(g.thread);
	g.thread = NULL;
	CloseHandle(g.stop_ev);
	g.stop_ev = NULL;
	CloseHandle(g.poke_ev);
	g.poke_ev = NULL;
	g.running = false;
}

#else /* POSIX: an mtime poll over the per-user dir */

//! FNV-1a over @p bytes.
static uint64_t
fold(uint64_t h, const void *bytes, size_t n)
{
	const unsigned char *p = (const unsigned char *)bytes;
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

#ifndef XRT_OS_ANDROID
static uint64_t
fold_stat(uint64_t h, const struct stat *sb)
{
	h = fold(h, &sb->st_size, sizeof(sb->st_size));
	h = fold(h, &sb->st_ino, sizeof(sb->st_ino));
	h = fold(h, &sb->st_mtime, sizeof(sb->st_mtime));
#ifdef __APPLE__
	h = fold(h, &sb->st_mtimespec.tv_nsec, sizeof(sb->st_mtimespec.tv_nsec));
#else
	h = fold(h, &sb->st_mtim.tv_nsec, sizeof(sb->st_mtim.tv_nsec));
#endif
	return h;
}

static uint64_t
fold_path(uint64_t h, const char *dir, const char *name)
{
	char path[PATH_MAX];
	(void)snprintf(path, sizeof(path), "%s/%s", dir, name);
	struct stat sb;
	if (stat(path, &sb) != 0) {
		return fold(h, "-", 1); // absent is a state too
	}
	h = fold(h, name, strlen(name));
	return fold_stat(h, &sb);
}
#endif

/*!
 * One hash per class over what that class reads from the per-user dir:
 * `preferred` (display), `input-preferred` + `emulated_controllers` + every per-app
 * `input-preferred-<exe>` pin (input), every manifest by its
 * suffix (the manifest's `enabled` is the POSIX Enabled switch). A file's
 * absence is folded too, and a manifest added or removed changes its
 * class's name set — so the dir's own mtime is deliberately NOT folded: it
 * moves on every file in the dir and would wake all three classes for an
 * `input-preferred` write (seen on the first live run).
 */
static void
fingerprint_now(uint64_t out[WATCH_CLASSES])
{
	for (uint32_t c = 0; c < WATCH_CLASSES; c++) {
		out[c] = 14695981039346656037ull;
	}
#ifndef XRT_OS_ANDROID
	char dir[PATH_MAX];
	if (!target_component_user_dir(dir, sizeof(dir))) {
		return;
	}
	out[TARGET_COMPONENT_CLASS_DISPLAY] = fold_path(out[TARGET_COMPONENT_CLASS_DISPLAY], dir, "preferred");
	out[TARGET_COMPONENT_CLASS_INPUT] = fold_path(out[TARGET_COMPONENT_CLASS_INPUT], dir, "input-preferred");
	out[TARGET_COMPONENT_CLASS_TRACKING] =
	    fold_path(out[TARGET_COMPONENT_CLASS_TRACKING], dir, "tracking-preferred");
	// N7 (#2143): the EmulatedControllers gate — what `input emulated on|off`
	// writes; the Windows twin lives under the InputProviders root the
	// registry watch already covers.
	out[TARGET_COMPONENT_CLASS_INPUT] = fold_path(out[TARGET_COMPONENT_CLASS_INPUT], dir, "emulated_controllers");

	DIR *d = opendir(dir);
	if (d == NULL) {
		return;
	}
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		uint32_t cls;
		// ADR-034 Am.5 (#2181): a per-app input pin (`input-preferred-<exe>`)
		// is input policy — a write re-ranks live like `input-preferred`.
		if (strncmp(ent->d_name, "input-preferred-", 16) == 0) {
			out[TARGET_COMPONENT_CLASS_INPUT] =
			    fold_path(out[TARGET_COMPONENT_CLASS_INPUT], dir, ent->d_name);
			continue;
		}
		switch (target_plugin_classify_manifest(ent->d_name)) {
		case TARGET_PLUGIN_MANIFEST_DISPLAY: cls = TARGET_COMPONENT_CLASS_DISPLAY; break;
		case TARGET_PLUGIN_MANIFEST_INPUT: cls = TARGET_COMPONENT_CLASS_INPUT; break;
		case TARGET_PLUGIN_MANIFEST_CONVERSION: cls = TARGET_COMPONENT_CLASS_CONVERSION; break;
		case TARGET_PLUGIN_MANIFEST_TRACKING: cls = TARGET_COMPONENT_CLASS_TRACKING; break;
		default: continue;
		}
		out[cls] = fold_path(out[cls], dir, ent->d_name);
	}
	closedir(d);
#endif
}

static void *
watch_thread_body(void *ptr)
{
	(void)ptr;
	os_mutex_lock(&g.mtx);
	while (!g.stop) {
		os_mutex_unlock(&g.mtx);
		uint64_t now[WATCH_CLASSES];
		fingerprint_now(now);
		os_mutex_lock(&g.mtx);
		for (uint32_t c = 1; c < WATCH_CLASSES; c++) {
			if (g.fingerprint_ready && now[c] != g.fingerprint[c]) {
				note_pending_locked(c, SERVICE_COMPONENT_WATCH_STORE_CHANGED);
			}
			g.fingerprint[c] = now[c];
		}
		g.fingerprint_ready = true;
		bool any = false;
		for (uint32_t c = 1; c < WATCH_CLASSES; c++) {
			any |= g.pending[c] != 0;
		}
		if (any) {
			os_mutex_unlock(&g.mtx);
			dispatch_pending();
			os_mutex_lock(&g.mtx);
			continue; // re-fingerprint right after a callback ran
		}
		// Idle here: a poke signals the cond, otherwise the next poll.
		(void)os_cond_wait_timeout_ns(&g.cond, &g.mtx, WATCH_POLL_NS);
	}
	os_mutex_unlock(&g.mtx);
	return NULL;
}

bool
service_component_watch_start(void)
{
	if (g.running) {
		return true;
	}
	if (!ensure_mutex() || os_cond_init(&g.cond) != 0) {
		return false;
	}
	g.stop = false;
	g.fingerprint_ready = false;
	if (os_thread_init(&g.thread) != 0 || os_thread_start(&g.thread, watch_thread_body, NULL) != 0) {
		U_LOG_W(
		    "component watch: could not start the policy-store watch thread — Enabled / Preferred edits "
		    "apply at the next service start.");
		os_cond_destroy(&g.cond);
		return false;
	}
	os_thread_name(&g.thread, "dxr-component-watch");
	g.running = true;
	U_LOG_W(
	    "component watch: up — per-user manifest dir polled every %u s (preferred, input-preferred, "
	    "manifests).",
	    (unsigned)(WATCH_POLL_NS / 1000000000ull));
	return true;
}

bool
service_component_watch_poke(uint32_t class_id, uint32_t reason)
{
	if (!g.running || class_id == 0 || class_id >= WATCH_CLASSES) {
		return false;
	}
	os_mutex_lock(&g.mtx);
	g.pending[class_id] = reason; // explicit: the last poke's reason wins
	os_cond_signal(&g.cond);
	os_mutex_unlock(&g.mtx);
	return true;
}

void
service_component_watch_stop(void)
{
	if (!g.running) {
		return;
	}
	os_mutex_lock(&g.mtx);
	g.stop = true;
	os_cond_signal(&g.cond);
	os_mutex_unlock(&g.mtx);
	os_thread_join(&g.thread);
	os_thread_destroy(&g.thread);
	os_cond_destroy(&g.cond);
	g.running = false;
}

#endif /* platform */


/*
 *
 * Subscription (both platforms).
 *
 */

void
service_component_watch_subscribe(uint32_t class_id, service_component_watch_fn fn, void *userdata)
{
	if (class_id == 0 || class_id >= WATCH_CLASSES || !ensure_mutex()) {
		return;
	}
	os_mutex_lock(&g.mtx);
	g.subs[class_id].fn = fn;
	g.subs[class_id].userdata = userdata;
	os_mutex_unlock(&g.mtx);
}
