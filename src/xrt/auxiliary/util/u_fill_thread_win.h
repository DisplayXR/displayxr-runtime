// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MMCSS scheduling for the fill (repaint) threads (#1264 S2).
 *
 * The in-process fill loops measure 17-23 ms scheduling holes against a
 * 1.4 ms tick target on a busy iGPU — classic starvation of a plain
 * thread against the app's render thread. MMCSS
 * (AvSetMmThreadCharacteristics, the Windows-sanctioned mechanism for
 * real-time media threads) is the fix class for exactly this. The
 * bridge tier's fill runs at ~400 ticks/s and does not need it, but it
 * does no harm there.
 *
 * avrt.dll is loaded dynamically — no new link dependency, and a system
 * without MMCSS (or a denied join) degrades silently to the old
 * behavior. DXR_FILL_MMCSS=0 kills it for A/B.
 *
 * @ingroup aux_util
 */

#pragma once

#if defined(_WIN32)

#include "util/u_logging.h"

#include <stdlib.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Join this thread to the MMCSS "Pro Audio" class. Call once at fill
 * (repaint) thread start. One-shot logs the outcome; no-op with
 * DXR_FILL_MMCSS=0 or when avrt/MMCSS is unavailable.
 */
static inline void
u_fill_thread_join_mmcss(const char *site)
{
	{
		const char *e = getenv("DXR_FILL_MMCSS");
		if (e != NULL && e[0] == '0') {
			U_LOG_W("#1264 S2: DXR_FILL_MMCSS=0 — fill thread '%s' stays a plain thread",
			        site);
			return;
		}
	}

	typedef HANDLE(WINAPI * pfn_join_t)(LPCWSTR, LPDWORD);
	HMODULE avrt = LoadLibraryW(L"avrt.dll");
	pfn_join_t join =
	    avrt != NULL ? (pfn_join_t)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
	if (join == NULL) {
		U_LOG_W("#1264 S2: MMCSS unavailable (avrt.dll) — fill thread '%s' stays a plain "
		        "thread",
		        site);
		return;
	}
	DWORD task_index = 0;
	HANDLE h = join(L"Pro Audio", &task_index);
	if (h == NULL) {
		U_LOG_W("#1264 S2: MMCSS join REFUSED (err=%lu) — fill thread '%s' stays a plain "
		        "thread",
		        (unsigned long)GetLastError(), site);
		return;
	}
	// Deliberately never revoked: the handle lives as long as the thread,
	// and these threads run for the session.
	U_LOG_W("#1264 S2: fill thread '%s' joined MMCSS 'Pro Audio' (task %lu) — "
	        "DXR_FILL_MMCSS=0 reverts",
	        site, (unsigned long)task_index);
}

#ifdef __cplusplus
}
#endif

#endif // _WIN32
