// Copyright 2026, DisplayXR.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the Android ADPF hint-session wrapper.
 * @ingroup aux_android
 *
 * The NDK @c APerformanceHint symbols are marked @c __INTRODUCED_IN(33), so
 * referencing them directly is a hard compile error under the project's minSdk 29
 * (and @c __builtin_available(android 33, *) does not reliably guard them for the
 * Android target). We therefore resolve them at runtime with @c dlsym from the
 * already-loaded @c libandroid.so: on an API-33+ device the symbols resolve and
 * ADPF is used; on anything older they come back NULL and every call no-ops.
 */

#include "android_perf_hint.h"

#ifdef XRT_OS_ANDROID

#include "util/u_logging.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define DEFAULT_TARGET_NS (16666666) // 60 Hz

// Opaque NDK handle types — we only ever hold pointers, never dereference them.
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;

typedef APerformanceHintManager *(*pfn_get_manager)(void);
typedef APerformanceHintSession *(*pfn_create_session)(APerformanceHintManager *,
                                                       const int32_t *,
                                                       size_t,
                                                       int64_t);
typedef int (*pfn_update_target)(APerformanceHintSession *, int64_t);
typedef int (*pfn_report_actual)(APerformanceHintSession *, int64_t);
typedef void (*pfn_close_session)(APerformanceHintSession *);

static struct
{
	bool resolved;
	bool ok;
	pfn_get_manager get_manager;
	pfn_create_session create_session;
	pfn_update_target update_target;
	pfn_report_actual report_actual;
	pfn_close_session close_session;
} adpf;

static void
resolve_adpf(void)
{
	if (adpf.resolved) {
		return;
	}
	adpf.resolved = true;

	// libandroid is already linked into the process; dlopen just hands back its
	// handle (kept open for process lifetime — intentionally not dlclose'd).
	void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
	if (lib == NULL) {
		return;
	}

	adpf.get_manager = (pfn_get_manager)dlsym(lib, "APerformanceHint_getManager");
	adpf.create_session = (pfn_create_session)dlsym(lib, "APerformanceHint_createSession");
	adpf.update_target = (pfn_update_target)dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
	adpf.report_actual = (pfn_report_actual)dlsym(lib, "APerformanceHint_reportActualWorkDuration");
	adpf.close_session = (pfn_close_session)dlsym(lib, "APerformanceHint_closeSession");

	adpf.ok = adpf.get_manager != NULL && adpf.create_session != NULL && adpf.update_target != NULL &&
	          adpf.report_actual != NULL && adpf.close_session != NULL;
}

struct android_perf_hint_session
{
	APerformanceHintSession *session;
};

struct android_perf_hint_session *
android_perf_hint_session_create(int32_t tid, int64_t target_duration_ns)
{
	if (target_duration_ns <= 0) {
		target_duration_ns = DEFAULT_TARGET_NS;
	}

	resolve_adpf();
	if (!adpf.ok) {
		U_LOG_W("ADPF: APerformanceHint unavailable (device API < 33?); perf hints off");
		return NULL;
	}

	APerformanceHintManager *manager = adpf.get_manager();
	if (manager == NULL) {
		U_LOG_W("ADPF: no PerformanceHintManager (device opted out); perf hints off");
		return NULL;
	}

	int32_t thread_ids[1] = {tid};
	APerformanceHintSession *ndk_session = adpf.create_session(manager, thread_ids, 1, target_duration_ns);
	if (ndk_session == NULL) {
		U_LOG_W("ADPF: createSession failed; perf hints off");
		return NULL;
	}

	struct android_perf_hint_session *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		adpf.close_session(ndk_session);
		return NULL;
	}
	s->session = ndk_session;

	U_LOG_W("ADPF: perf-hint session created (tid %d, target %.2f ms)", tid, target_duration_ns / 1e6);
	return s;
}

void
android_perf_hint_session_update_target(struct android_perf_hint_session *s, int64_t target_duration_ns)
{
	if (s == NULL || s->session == NULL || target_duration_ns <= 0 || !adpf.ok) {
		return;
	}
	adpf.update_target(s->session, target_duration_ns);
}

void
android_perf_hint_session_report_actual(struct android_perf_hint_session *s, int64_t actual_duration_ns)
{
	if (s == NULL || s->session == NULL || actual_duration_ns <= 0 || !adpf.ok) {
		return;
	}
	adpf.report_actual(s->session, actual_duration_ns);
}

void
android_perf_hint_session_destroy(struct android_perf_hint_session *s)
{
	if (s == NULL) {
		return;
	}
	if (s->session != NULL && adpf.ok) {
		adpf.close_session(s->session);
	}
	free(s);
}

#endif // XRT_OS_ANDROID
