// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Texture target binding (ADR-060): the Win32 read of the reference
 *         window (Windows-only, header-only, C-compatible).
 *
 * The reference window is only ever READ (`GetClientRect` is not even needed:
 * the app names the rect) — never subclassed, reparented or handed to a DP.
 * The read is pinned to DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 on the
 * calling thread, because the runtime runs inside the HOST's process and a
 * DPI-unaware host would otherwise get DPI-virtualised coordinates
 * (docs/reference/dpi-awareness.md). The rect is defined in physical device
 * px, the space the screen registry's monitor rects are in.
 *
 * @ingroup comp_util
 */

#pragma once

#ifdef XRT_OS_WINDOWS

#include "util/comp_texture_target.h"

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef DPI_AWARENESS_CONTEXT(WINAPI *comp_texture_target_pfn_set_thread_dpi_ctx)(DPI_AWARENESS_CONTEXT);

static inline comp_texture_target_pfn_set_thread_dpi_ctx
comp_texture_target_dpi_setter(void)
{
	// Resolved once; Windows 10 1607+. NULL on older systems, where the read
	// simply runs in the host's own awareness.
	static comp_texture_target_pfn_set_thread_dpi_ctx pfn = NULL;
	static bool looked = false;
	if (!looked) {
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (user32 != NULL) {
			pfn = (comp_texture_target_pfn_set_thread_dpi_ctx)(void *)GetProcAddress(
			    user32, "SetThreadDpiAwarenessContext");
		}
		looked = true;
	}
	return pfn;
}

/*!
 * Is @p ref usable as a reference window: alive and top-level. The binding is
 * validated at create (same process, top-level); a frame re-checks liveness.
 */
static inline bool
comp_texture_target_ref_alive_win32(void *ref)
{
	return ref == NULL || IsWindow((HWND)ref);
}

/*!
 * Resolve @p tt for this weave: read the reference window's client origin on
 * the desktop NOW (a drag moves it), under a per-monitor-v2 pin, then compose
 * and clamp to the shared texture. Returns false when the reference window is
 * gone (the caller keeps its last resolve) or the target is empty.
 */
static inline bool
comp_texture_target_resolve_win32(const struct comp_texture_target *tt,
                                  uint32_t tex_w,
                                  uint32_t tex_h,
                                  struct comp_texture_target_resolved *out)
{
	out->valid = false;
	if (tt == NULL || !tt->bound) {
		return false;
	}
	POINT origin = {0, 0};
	if (tt->ref != NULL) {
		HWND ref = (HWND)tt->ref;
		if (!IsWindow(ref)) {
			return false;
		}
		comp_texture_target_pfn_set_thread_dpi_ctx set_ctx = comp_texture_target_dpi_setter();
		DPI_AWARENESS_CONTEXT prev = NULL;
		if (set_ctx != NULL) {
			prev = set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		}
		const BOOL ok = ClientToScreen(ref, &origin);
		if (set_ctx != NULL && prev != NULL) {
			(void)set_ctx(prev);
		}
		if (!ok) {
			return false;
		}
	}
	return comp_texture_target_compose(tt, (int32_t)origin.x, (int32_t)origin.y, tex_w, tex_h, out);
}

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_WINDOWS
