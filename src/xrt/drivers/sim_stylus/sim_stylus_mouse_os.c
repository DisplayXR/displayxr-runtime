// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_stylus `MODE=mouse`: the OS sampler. Windows reads the global
 *         cursor and the async key state — no hooks, no window, no focus —
 *         under a pinned per-monitor-v2 DPI context; every other platform
 *         gets stubs that make the provider fall back to `MODE=draw`.
 *
 * Only ever READS the desktop: it never moves the cursor, injects input or
 * changes a key's toggle state.
 *
 * @ingroup drv_sim_stylus
 */

#include "xrt/xrt_config_os.h"

#include "sim_stylus_mouse.h"

#include <string.h>

#ifdef XRT_OS_WINDOWS

#include "util/u_logging.h"

#include <windows.h>

#include <stdio.h>


/*
 * SetThreadDpiAwarenessContext is resolved dynamically (Windows 10 1607+;
 * MinGW headers may not declare it), as in os_display_desktop_win32.c. A DLL
 * inherits its host's DPI awareness, so without the pin a DPI-unaware host
 * would see virtualised cursor and monitor coordinates
 * (docs/reference/dpi-awareness.md).
 */
typedef HANDLE sim_dpi_context_t;
typedef sim_dpi_context_t(WINAPI *pfn_set_thread_dpi_awareness_context)(sim_dpi_context_t);
#define SIM_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((sim_dpi_context_t)(intptr_t)-4)

//! Re-read the picked monitor's rect this often (a rearrangement, a mode change).
#define SIM_STYLUS_MOUSE_REFRESH_NS (2000LL * 1000000LL)

static struct
{
	SRWLOCK lock;
	bool inited;
	pfn_set_thread_dpi_awareness_context set_ctx;

	struct sim_stylus_mouse_config cfg;
	char screen[64];

	struct sim_stylus_mouse_monitor panel;
	bool have_panel;
	int64_t panel_checked_ns;

	struct sim_stylus_mouse_machine machine;
	struct sim_stylus_mouse_toggle scroll;

	//! The latest sample time: the machine's clock must not run backwards.
	int64_t last_ns;
} g_mouse = {SRWLOCK_INIT};

static sim_dpi_context_t
dpi_pin(void)
{
	if (g_mouse.set_ctx == NULL) {
		return NULL;
	}
	return g_mouse.set_ctx(SIM_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

static void
dpi_restore(sim_dpi_context_t prev)
{
	if (g_mouse.set_ctx != NULL && prev != NULL) {
		g_mouse.set_ctx(prev);
	}
}

struct enum_ctx
{
	struct sim_stylus_mouse_monitor *mons;
	uint32_t count;
	bool with_mm;
};

static BOOL CALLBACK
enum_proc(HMONITOR hmon, HDC hdc, LPRECT rect, LPARAM lparam)
{
	(void)hdc;
	(void)rect;
	struct enum_ctx *ctx = (struct enum_ctx *)lparam;
	if (ctx->count >= SIM_STYLUS_MOUSE_MAX_MONITORS) {
		return FALSE;
	}
	MONITORINFOEXW mi;
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(hmon, (LPMONITORINFO)&mi)) {
		return TRUE;
	}
	struct sim_stylus_mouse_monitor *m = &ctx->mons[ctx->count];
	memset(m, 0, sizeof(*m));
	m->left = (int32_t)mi.rcMonitor.left;
	m->top = (int32_t)mi.rcMonitor.top;
	m->width = (int32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
	m->height = (int32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
	m->is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
	if (WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, m->name, (int)sizeof(m->name), NULL, NULL) <= 0) {
		m->name[0] = '\0';
	}
	if (ctx->with_mm) {
		// The monitor's own DC reports the physical size Windows took from its EDID.
		HDC dc = CreateDCW(mi.szDevice, mi.szDevice, NULL, NULL);
		if (dc != NULL) {
			m->width_mm = GetDeviceCaps(dc, HORZSIZE);
			m->height_mm = GetDeviceCaps(dc, VERTSIZE);
			DeleteDC(dc);
		}
	}
	ctx->count++;
	return TRUE;
}

static uint32_t
enumerate(struct sim_stylus_mouse_monitor *mons, bool with_mm)
{
	struct enum_ctx ctx = {mons, 0, with_mm};
	const sim_dpi_context_t prev = dpi_pin();
	EnumDisplayMonitors(NULL, NULL, enum_proc, (LPARAM)&ctx);
	dpi_restore(prev);
	return ctx.count;
}

//! Choose the panel monitor from scratch. Lock held.
static void
pick_panel(bool log)
{
	struct sim_stylus_mouse_monitor mons[SIM_STYLUS_MOUSE_MAX_MONITORS];
	const uint32_t count = enumerate(mons, true);
	enum sim_stylus_mouse_pick_rule rule = SIM_STYLUS_MOUSE_PICK_NONE;
	bool unmatched = false;
	const int32_t i = sim_stylus_mouse_pick_monitor(
	    mons, count, g_mouse.cfg.display_width_m, g_mouse.cfg.display_height_m, g_mouse.screen, &rule, &unmatched);
	g_mouse.have_panel = i >= 0;
	if (i >= 0) {
		g_mouse.panel = mons[i];
	}
	if (!log) {
		return;
	}
	for (uint32_t k = 0; k < count; k++) {
		U_LOG_W("sim-stylus: mouse: monitor %u %s %dx%d at (%d,%d) %dx%d mm%s%s", k, mons[k].name,
		        mons[k].width, mons[k].height, mons[k].left, mons[k].top, mons[k].width_mm, mons[k].height_mm,
		        mons[k].is_primary ? " primary" : "", (int32_t)k == i ? "  <- panel" : "");
	}
	if (unmatched) {
		U_LOG_W("sim-stylus: mouse: SCREEN='%s' names no monitor - picking automatically.", g_mouse.screen);
	}
	if (i < 0) {
		U_LOG_W("sim-stylus: mouse: no monitor found - the pen stays out of range.");
	} else {
		U_LOG_W("sim-stylus: mouse: the panel is monitor %d (%s, %s); set SCREEN to override.", i,
		        g_mouse.panel.name, sim_stylus_mouse_pick_rule_str(rule));
	}
}

//! Re-read the picked monitor's rect by name; re-pick if it went away. Lock held.
static void
refresh_panel(int64_t now_ns)
{
	if (g_mouse.have_panel && now_ns - g_mouse.panel_checked_ns < SIM_STYLUS_MOUSE_REFRESH_NS) {
		return;
	}
	g_mouse.panel_checked_ns = now_ns;

	struct sim_stylus_mouse_monitor mons[SIM_STYLUS_MOUSE_MAX_MONITORS];
	const uint32_t count = enumerate(mons, false);
	for (uint32_t k = 0; k < count; k++) {
		if (g_mouse.have_panel && strcmp(mons[k].name, g_mouse.panel.name) == 0) {
			const int32_t w_mm = g_mouse.panel.width_mm;
			const int32_t h_mm = g_mouse.panel.height_mm;
			g_mouse.panel = mons[k];
			g_mouse.panel.width_mm = w_mm;
			g_mouse.panel.height_mm = h_mm;
			return;
		}
	}
	pick_panel(false);
}

static bool
key(int vk)
{
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

//! The gate, lock held.
static bool
gate_open_locked(void)
{
	if (g_mouse.cfg.gate == SIM_STYLUS_MOUSE_GATE_NONE) {
		return true;
	}
	return sim_stylus_mouse_toggle_update(&g_mouse.scroll, (GetKeyState(VK_SCROLL) & 1) != 0, key(VK_SCROLL));
}

bool
sim_stylus_mouse_os_init(const struct sim_stylus_mouse_config *cfg, const char *screen)
{
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	pfn_set_thread_dpi_awareness_context set_ctx =
	    user32 != NULL
	        ? (pfn_set_thread_dpi_awareness_context)(void *)GetProcAddress(user32, "SetThreadDpiAwarenessContext")
	        : NULL;

	AcquireSRWLockExclusive(&g_mouse.lock);
	g_mouse.set_ctx = set_ctx;
	g_mouse.cfg = *cfg;
	snprintf(g_mouse.screen, sizeof(g_mouse.screen), "%s", screen != NULL ? screen : "");
	sim_stylus_mouse_machine_init(&g_mouse.machine, &g_mouse.cfg);
	memset(&g_mouse.scroll, 0, sizeof(g_mouse.scroll));
	pick_panel(true);
	g_mouse.panel_checked_ns = 0;
	g_mouse.last_ns = 0;
	g_mouse.inited = true;
	ReleaseSRWLockExclusive(&g_mouse.lock);
	return true;
}

void
sim_stylus_mouse_os_eval(int64_t now_ns, struct sim_stylus_state *out)
{
	AcquireSRWLockExclusive(&g_mouse.lock);
	if (!g_mouse.inited) {
		ReleaseSRWLockExclusive(&g_mouse.lock);
		memset(out, 0, sizeof(*out));
		out->tip.orientation.w = 1.0f;
		out->grip.orientation.w = 1.0f;
		return;
	}
	// Two threads may read the clock and then race for the lock.
	if (now_ns < g_mouse.last_ns) {
		now_ns = g_mouse.last_ns;
	}
	g_mouse.last_ns = now_ns;
	refresh_panel(now_ns);

	struct sim_stylus_mouse_sample s;
	memset(&s, 0, sizeof(s));
	s.gate_open = gate_open_locked();
	if (s.gate_open) {
		POINT pt;
		const sim_dpi_context_t prev = dpi_pin();
		const bool have_pt = GetCursorPos(&pt) != 0;
		dpi_restore(prev);

		const float w = g_mouse.cfg.display_width_m > 0.0f ? g_mouse.cfg.display_width_m
		                                                   : SIM_STYLUS_DISPLAY_WIDTH_FALLBACK_M;
		const float h = g_mouse.cfg.display_height_m > 0.0f ? g_mouse.cfg.display_height_m
		                                                    : SIM_STYLUS_DISPLAY_HEIGHT_FALLBACK_M;
		s.on_panel =
		    have_pt && g_mouse.have_panel &&
		    sim_stylus_mouse_map_pixel(&g_mouse.panel, (int32_t)pt.x, (int32_t)pt.y, w, h, &s.x_m, &s.y_m);

		// GetAsyncKeyState reads PHYSICAL buttons; the tip follows the primary one.
		const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
		s.left = key(swapped ? VK_RBUTTON : VK_LBUTTON);
		s.right = key(swapped ? VK_LBUTTON : VK_RBUTTON);
		s.middle = key(VK_MBUTTON);
		s.x1 = key(VK_XBUTTON1);
		s.x2 = key(VK_XBUTTON2);
		s.ctrl = key(VK_CONTROL);
		s.shift = key(VK_SHIFT);
		s.page_up = key(VK_PRIOR);
		s.page_down = key(VK_NEXT);
		s.home = key(VK_HOME);
	}
	sim_stylus_mouse_machine_step(&g_mouse.machine, &g_mouse.cfg, &s, now_ns, out);
	ReleaseSRWLockExclusive(&g_mouse.lock);
}

bool
sim_stylus_mouse_os_gate_open(void)
{
	AcquireSRWLockExclusive(&g_mouse.lock);
	const bool open = g_mouse.inited && gate_open_locked();
	ReleaseSRWLockExclusive(&g_mouse.lock);
	return open;
}

int64_t
sim_stylus_mouse_os_double_click_ms(void)
{
	return (int64_t)GetDoubleClickTime();
}

#else // !XRT_OS_WINDOWS

bool
sim_stylus_mouse_os_init(const struct sim_stylus_mouse_config *cfg, const char *screen)
{
	(void)cfg;
	(void)screen;
	return false;
}

void
sim_stylus_mouse_os_eval(int64_t now_ns, struct sim_stylus_state *out)
{
	(void)now_ns;
	memset(out, 0, sizeof(*out));
	out->tip.orientation.w = 1.0f;
	out->grip.orientation.w = 1.0f;
}

bool
sim_stylus_mouse_os_gate_open(void)
{
	return false;
}

int64_t
sim_stylus_mouse_os_double_click_ms(void)
{
	return 0;
}

#endif // XRT_OS_WINDOWS
