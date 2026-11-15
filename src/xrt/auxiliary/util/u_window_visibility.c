// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671): the portable core, and
 *         the tracker stub for platforms without an implementation.
 * @ingroup aux_util
 */

#include "util/u_window_visibility.h"

#include <ctype.h>
#include <string.h>

/*
 *
 * Rect cover.
 *
 */

//! Work-list capacity. Each occluder splits a piece into at most four.
#define WV_MAX_PIECES 256

bool
u_wv_rect_covered(const struct u_wv_rect *target, const struct u_wv_rect *occluders, size_t occluder_count)
{
	if (target == NULL || u_wv_rect_empty(target)) {
		return false;
	}

	struct u_wv_rect a[WV_MAX_PIECES];
	struct u_wv_rect b[WV_MAX_PIECES];
	struct u_wv_rect *cur = a;
	struct u_wv_rect *next = b;
	size_t n = 1;
	cur[0] = *target;

	for (size_t i = 0; i < occluder_count && n > 0; i++) {
		const struct u_wv_rect *o = &occluders[i];
		if (u_wv_rect_empty(o)) {
			continue;
		}
		size_t m = 0;
		for (size_t p = 0; p < n; p++) {
			const struct u_wv_rect r = cur[p];
			const struct u_wv_rect x = u_wv_rect_intersect(&r, o);
			if (u_wv_rect_empty(&x)) {
				if (m >= WV_MAX_PIECES) {
					return false;
				}
				next[m++] = r;
				continue;
			}
			// Up to four remainders: the bands above and below the
			// intersection span the piece's width; left and right sit
			// beside the intersection.
			struct u_wv_rect parts[4] = {
			    {r.left, r.top, r.right, x.top},
			    {r.left, x.bottom, r.right, r.bottom},
			    {r.left, x.top, x.left, x.bottom},
			    {x.right, x.top, r.right, x.bottom},
			};
			for (int k = 0; k < 4; k++) {
				if (u_wv_rect_empty(&parts[k])) {
					continue;
				}
				if (m >= WV_MAX_PIECES) {
					// Too fragmented to tell: say visible.
					return false;
				}
				next[m++] = parts[k];
			}
		}
		struct u_wv_rect *t = cur;
		cur = next;
		next = t;
		n = m;
	}
	return n == 0;
}

/*
 *
 * Occluder classification.
 *
 */

//! Shell and compositor windows that float over everything without hiding it.
static const char *const k_overlay_classes[] = {
    "MultitaskingViewFrame",        "MultitaskingViewList",         "TaskSwitcherWnd",        "TaskListThumbnailWnd",
    "XamlExplorerHostIslandWindow", "XamlIslandChromeWindow",       "TaskManagementFrame",    "Shell_TrayWnd",
    "Shell_SecondaryTrayWnd",       "DWM_DX_FULLSCREEN_TRANSITION", "DwmInputRedirectWindow",
};

//! Overlay processes: GPU-vendor, capture/metrics and chat overlays.
static const char *const k_overlay_processes[] = {
    "Discord.exe", "EOSOverlay.exe", "NVIDIA Overlay.exe", "amdow.exe", "PresentMon.exe",
};

static bool
wv_ieq(const char *a, const char *b, size_t b_len)
{
	size_t i = 0;
	for (; i < b_len; i++) {
		if (a[i] == '\0' || tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
			return false;
		}
	}
	return a[i] == '\0';
}

//! @p name is one of the comma-separated entries in @p list (case-insensitive, spaces trimmed).
static bool
wv_in_csv(const char *name, const char *list)
{
	if (list == NULL || name == NULL || name[0] == '\0') {
		return false;
	}
	const char *s = list;
	while (*s != '\0') {
		while (*s == ',' || *s == ' ') {
			s++;
		}
		const char *e = s;
		while (*e != '\0' && *e != ',') {
			e++;
		}
		const char *t = e;
		while (t > s && t[-1] == ' ') {
			t--;
		}
		if (t > s && wv_ieq(name, s, (size_t)(t - s))) {
			return true;
		}
		s = e;
	}
	return false;
}

enum u_wv_skip
u_wv_classify_occluder(const struct u_wv_window_facts *f, const char *extra_overlay_processes)
{
	if (!f->visible) {
		return U_WV_SKIP_NOT_VISIBLE;
	}
	if (f->cloaked || f->no_redirection_bitmap) {
		return U_WV_SKIP_CLOAKED;
	}
	if (f->same_process) {
		return U_WV_SKIP_SAME_PROCESS;
	}
	if (f->input_transparent || f->layered_not_opaque) {
		return U_WV_SKIP_TRANSPARENT;
	}
	if (f->class_name != NULL) {
		for (size_t i = 0; i < sizeof(k_overlay_classes) / sizeof(k_overlay_classes[0]); i++) {
			if (strcmp(f->class_name, k_overlay_classes[i]) == 0) {
				return U_WV_SKIP_OVERLAY_CLASS;
			}
		}
	}
	if (f->process_name != NULL && f->process_name[0] != '\0') {
		for (size_t i = 0; i < sizeof(k_overlay_processes) / sizeof(k_overlay_processes[0]); i++) {
			if (wv_ieq(f->process_name, k_overlay_processes[i], strlen(k_overlay_processes[i]))) {
				return U_WV_SKIP_OVERLAY_PROCESS;
			}
		}
		if (wv_in_csv(f->process_name, extra_overlay_processes)) {
			return U_WV_SKIP_OVERLAY_PROCESS;
		}
	}
	return U_WV_OCCLUDES;
}

/*
 *
 * Tracker stub (non-Windows).
 *
 */

#ifndef _WIN32

struct u_window_visibility *
u_window_visibility_create(void *native_window,
                           const char *panel_device_name,
                           const char *who,
                           u_window_visibility_change_fn on_change,
                           void *userdata)
{
	(void)on_change;
	(void)userdata;
	(void)native_window;
	(void)panel_device_name;
	(void)who;
	return NULL;
}

bool
u_window_visibility_hidden(const struct u_window_visibility *wv)
{
	(void)wv;
	return false;
}

void
u_window_visibility_set_panel(struct u_window_visibility *wv, const char *panel_device_name)
{
	(void)wv;
	(void)panel_device_name;
}

void
u_window_visibility_set_window(struct u_window_visibility *wv, void *native_window)
{
	(void)wv;
	(void)native_window;
}

void
u_window_visibility_destroy(struct u_window_visibility **wv_ptr)
{
	if (wv_ptr != NULL) {
		*wv_ptr = NULL;
	}
}

#endif
