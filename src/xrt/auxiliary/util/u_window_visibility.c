// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window visibility for the lens wish (#2671): the portable core, and
 *         the tracker stub for platforms without an implementation.
 * @ingroup aux_util
 */

#include "util/u_window_visibility.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"
#include "xrt/xrt_screen.h"

#include <ctype.h>
#include <string.h>

/*
 *
 * Rect cover.
 *
 */

//! Work-list capacity. Each occluder splits a piece into at most four.
#define WV_MAX_PIECES 256

/*!
 * Subtract every occluder from @p target. The remainder pieces are disjoint,
 * so their areas add up to the uncovered area. Returns false when the work
 * list overflows (too fragmented to tell).
 */
static bool
wv_uncovered(const struct u_wv_rect *target,
             const struct u_wv_rect *occluders,
             size_t occluder_count,
             uint64_t *out_area);

bool
u_wv_rect_covered(const struct u_wv_rect *target, const struct u_wv_rect *occluders, size_t occluder_count)
{
	if (target == NULL || u_wv_rect_empty(target)) {
		return false;
	}
	uint64_t area = 0;
	return wv_uncovered(target, occluders, occluder_count, &area) && area == 0;
}

bool
u_wv_rect_uncovered_area(const struct u_wv_rect *target,
                         const struct u_wv_rect *occluders,
                         size_t occluder_count,
                         uint64_t *out_area)
{
	*out_area = 0;
	if (target == NULL || u_wv_rect_empty(target)) {
		return true;
	}
	return wv_uncovered(target, occluders, occluder_count, out_area);
}

bool
u_wv_rect_mostly_covered(const struct u_wv_rect *target, const struct u_wv_rect *occluders, size_t occluder_count)
{
	if (target == NULL || u_wv_rect_empty(target)) {
		return false;
	}
	uint64_t visible = 0;
	if (!wv_uncovered(target, occluders, occluder_count, &visible)) {
		return false; // too fragmented to tell: visible
	}
	const uint64_t part = (uint64_t)(target->right - target->left) * (uint64_t)(target->bottom - target->top);
	return visible < u_wv_min_visible_area(part);
}

static bool
wv_uncovered(const struct u_wv_rect *target,
             const struct u_wv_rect *occluders,
             size_t occluder_count,
             uint64_t *out_area)
{
	*out_area = 0;

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
	uint64_t area = 0;
	for (size_t p = 0; p < n; p++) {
		area += (uint64_t)(cur[p].right - cur[p].left) * (uint64_t)(cur[p].bottom - cur[p].top);
	}
	*out_area = area;
	return true;
}

const char *
u_wv_reason_str(enum u_wv_reason r)
{
	switch (r) {
	case U_WV_REASON_VISIBLE: return "visible";
	case U_WV_REASON_PARTLY_COVERED: return "partly-covered";
	case U_WV_REASON_NOT_SHOWN: return "not-shown";
	case U_WV_REASON_OFF_SCREEN: return "off-screen";
	case U_WV_REASON_GONE: return "gone";
	case U_WV_REASON_MINIMIZED: return "minimized";
	case U_WV_REASON_NOT_VISIBLE: return "not-visible";
	case U_WV_REASON_CLOAKED: return "cloaked";
	case U_WV_REASON_LOCKED: return "locked";
	case U_WV_REASON_DISPLAY_OFF: return "display-off";
	case U_WV_REASON_COVERED: return "covered";
	}
	return "?";
}

enum u_wv_reason
u_wv_window_reason(const struct u_wv_window_state *s)
{
	if (!s->exists) {
		return U_WV_REASON_GONE;
	}
	// Session-wide states first: nothing on the panel can be seen then.
	if (s->locked) {
		return U_WV_REASON_LOCKED;
	}
	if (s->display_off) {
		return U_WV_REASON_DISPLAY_OFF;
	}
	// A window never shown is not hidden (an app that renders its first
	// frame and then calls ShowWindow), even if it starts minimized.
	if (!s->ever_shown && !s->visible) {
		return U_WV_REASON_NOT_SHOWN;
	}
	if (s->minimized) {
		return U_WV_REASON_MINIMIZED;
	}
	if (!s->visible) {
		return U_WV_REASON_NOT_VISIBLE;
	}
	if (s->cloaked) {
		return U_WV_REASON_CLOAKED;
	}
	return U_WV_REASON_VISIBLE;
}

enum u_wv_part
u_wv_part_state(const struct u_wv_rect *client,
                const struct u_wv_rect *screen,
                const struct u_wv_rect *occluders,
                size_t occluder_count)
{
	if (client == NULL || screen == NULL) {
		return U_WV_PART_OFF;
	}
	const struct u_wv_rect part = u_wv_rect_intersect(client, screen);
	if (u_wv_rect_empty(&part)) {
		return U_WV_PART_OFF;
	}
	return u_wv_rect_mostly_covered(&part, occluders, occluder_count) ? U_WV_PART_HIDDEN : U_WV_PART_VISIBLE;
}

void
u_window_visibility_set_screen_list(struct u_window_visibility *wv, const struct xrt_screen_list *list)
{
	if (wv == NULL) {
		return;
	}
	const char *names[U_WINDOW_VISIBILITY_MAX_SCREENS];
	uint32_t n = 0;
	for (uint32_t i = 0;
	     list != NULL && i < list->count && i < XRT_SCREEN_LIST_MAX && n < U_WINDOW_VISIBILITY_MAX_SCREENS; i++) {
		names[n++] = list->screens[i].device_name;
	}
	u_window_visibility_set_screens(wv, names, n);
}

/*
 *
 * Occluder classification.
 *
 */

//! Shell and compositor windows that float over everything without hiding it.
static const char *const k_overlay_classes[] = {
    "MultitaskingViewFrame",        "MultitaskingViewList",   "TaskSwitcherWnd",
    "TaskListThumbnailWnd",         "XamlExplorerHostIslandWindow", "XamlIslandChromeWindow",
    "TaskManagementFrame",          "DWM_DX_FULLSCREEN_TRANSITION", "DwmInputRedirectWindow",
};

//! The taskbars: occluders unless the taskbar auto-hides (ADR-062 D1).
static const char *const k_taskbar_classes[] = {"Shell_TrayWnd", "Shell_SecondaryTrayWnd"};

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
		for (size_t i = 0; i < sizeof(k_taskbar_classes) / sizeof(k_taskbar_classes[0]); i++) {
			if (strcmp(f->class_name, k_taskbar_classes[i]) == 0) {
				return f->taskbar_auto_hide ? U_WV_SKIP_OVERLAY_CLASS : U_WV_OCCLUDES;
			}
		}
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
 * Settings.
 *
 */

DEBUG_GET_ONCE_BOOL_OPTION(hidden_skip_present, U_WINDOW_VISIBILITY_ENV_SKIP_PRESENT, true)

bool
u_window_visibility_skip_present_enabled(void)
{
	return debug_get_bool_option_hidden_skip_present();
}

bool
u_window_visibility_skip_step(struct u_window_visibility_skip *k,
                              const struct u_window_visibility *wv,
                              const char *who,
                              uint64_t pace_period_ns)
{
	if (k == NULL) {
		return false;
	}
	const bool hidden = u_window_visibility_hidden_everywhere(wv);
	const uint64_t now = os_monotonic_get_ns();
	if (hidden != k->hidden) {
		k->hidden = hidden;
		if (hidden) {
			k->skipped = 0;
			k->since_ns = now;
			k->next_ns = 0;
			U_LOG_W("ADR-062 %s: the window is hidden on every screen - weave and present skipped (the weaver, "
			        "the target and the swapchains stay alive)",
			        who != NULL ? who : "?");
		} else {
			U_LOG_W("ADR-062 %s: the window is visible again after %.2f s hidden - %llu frame(s) were not woven "
			        "or presented; weaving resumes",
			        who != NULL ? who : "?", (double)(now - k->since_ns) / 1e9, (unsigned long long)k->skipped);
		}
	}
	if (!hidden) {
		return false;
	}
	k->skipped++;
	if (pace_period_ns != 0) {
		uint64_t next = k->next_ns + pace_period_ns;
		if (k->next_ns == 0 || next + pace_period_ns < now) {
			next = now; // first skipped frame, or the grid fell behind
		}
		if (next > now) {
			os_nanosleep((int64_t)(next - now));
		}
		k->next_ns = next;
	}
	return true;
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
u_window_visibility_set_screens(struct u_window_visibility *wv, const char *const *device_names, uint32_t count)
{
	(void)wv;
	(void)device_names;
	(void)count;
}

bool
u_window_visibility_screen_hidden(const struct u_window_visibility *wv, const char *device_name)
{
	(void)wv;
	(void)device_name;
	return false;
}

bool
u_window_visibility_hidden_everywhere(const struct u_window_visibility *wv)
{
	(void)wv;
	return false;
}

void
u_window_visibility_get_state(const struct u_window_visibility *wv, struct u_window_visibility_state *out)
{
	(void)wv;
	memset(out, 0, sizeof(*out));
}

void
u_window_visibility_stop(struct u_window_visibility *wv)
{
	(void)wv;
}

void
u_window_visibility_destroy(struct u_window_visibility **wv_ptr)
{
	if (wv_ptr != NULL) {
		*wv_ptr = NULL;
	}
}

#endif
