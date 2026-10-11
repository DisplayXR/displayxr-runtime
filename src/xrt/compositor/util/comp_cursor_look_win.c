// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-drawn cursor's default look, Windows half: mirror the
 *         current Win32 cursor (XR_DXR_cursor_depth v5, ADR-046 Amendment 2
 *         A.6 / B).
 * @author David Fattal
 * @ingroup comp_util
 *
 * GetCursorInfo() -> HCURSOR, drawn with DrawIconEx() into a top-down 32-bit
 * DIB twice - over black and over white - and recovered by
 * u_cursor_draw_unblend(). That reads every cursor kind the same way (32-bit
 * alpha, colour + AND mask, monochrome AND/XOR) without branching on its type;
 * the XOR ("invert the screen") pixels come out opaque white.
 *
 * GetCursorInfo() is global - not per thread or per process - so the
 * compositor thread may call it. The image is drawn at the size the OS draws
 * the cursor on this panel (SM_CYCURSOR at the panel's DPI, or the image's own
 * size when larger) and cached by HCURSOR + scale: one GetCursorInfo() per
 * drawn frame, a re-raster only when the shape or the DPI changes.
 *
 * Hidden cursor: an app hides its cursor with SetCursor(NULL) as soon as a
 * locate reports willDraw, often before that frame's xrEndFrame, so on most
 * drawn frames GetCursorInfo() has no cursor to report. The shape is then the
 * CLASS cursor of the window under the pointer, else the last mirrored shape,
 * else the system arrow (IDC_ARROW).
 */

#include "comp_cursor_look.h"

#include "util/u_cursor_depth.h"
#include "util/u_logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

//! Largest OS cursor image we mirror, per side (accessibility sizes reach 256).
#define CURSOR_LOOK_MAX_PX 256

typedef int(WINAPI *pfn_get_system_metrics_for_dpi)(int, UINT);
typedef UINT(WINAPI *pfn_get_dpi_for_system)(void);

static FARPROC
user32_proc(const char *name)
{
	HMODULE user32 = GetModuleHandleA("user32.dll");
	return user32 != NULL ? GetProcAddress(user32, name) : NULL;
}

//! The OS cursor's nominal height at @p dpi (SM_CYCURSOR), or 0 if unknown.
static int
cursor_size_for_dpi(UINT dpi)
{
	static pfn_get_system_metrics_for_dpi fn = NULL;
	static bool looked = false;
	if (!looked) {
		looked = true;
		fn = (pfn_get_system_metrics_for_dpi)(void *)user32_proc("GetSystemMetricsForDpi");
	}
	return fn != NULL ? fn(SM_CYCURSOR, dpi) : 0; // pre-1607: the image's own size
}

//! Rasterise @p hc over black and white; on success the look holds it.
static bool
snapshot(struct comp_cursor_look *look, HCURSOR hc, float scale)
{
	ICONINFO ii;
	memset(&ii, 0, sizeof(ii));
	if (!GetIconInfo(hc, &ii)) {
		return false;
	}
	BITMAP bm;
	memset(&bm, 0, sizeof(bm));
	int native_w = 0;
	int native_h = 0;
	if (ii.hbmColor != NULL && GetObject(ii.hbmColor, sizeof(bm), &bm) != 0) {
		native_w = bm.bmWidth;
		native_h = bm.bmHeight;
	} else if (ii.hbmMask != NULL && GetObject(ii.hbmMask, sizeof(bm), &bm) != 0) {
		// Monochrome: the AND mask stacked on the XOR mask.
		native_w = bm.bmWidth;
		native_h = bm.bmHeight / 2;
	}
	const int hot_x = (int)ii.xHotspot;
	const int hot_y = (int)ii.yHotspot;
	if (ii.hbmColor != NULL) {
		DeleteObject(ii.hbmColor);
	}
	if (ii.hbmMask != NULL) {
		DeleteObject(ii.hbmMask);
	}
	if (native_w <= 0 || native_h <= 0) {
		return false;
	}

	// The size the OS draws it at on this panel: the per-DPI nominal size, or
	// the image's own when larger (an accessibility pointer size loads a
	// bigger image than SM_CYCURSOR reports).
	const UINT dpi = (UINT)lrintf(96.0f * scale);
	int th = cursor_size_for_dpi(dpi);
	if (th < native_h) {
		th = native_h;
	}
	const int tw = (int)lrintf((float)native_w * (float)th / (float)native_h);
	if (tw <= 0 || th <= 0 || tw > CURSOR_LOOK_MAX_PX || th > CURSOR_LOOK_MAX_PX) {
		return false;
	}

	const size_t n = (size_t)tw * (size_t)th;
	uint8_t *black = malloc(n * 4);
	uint8_t *white = malloc(n * 4);
	uint8_t *rgba = malloc(n * 4);
	HDC screen = GetDC(NULL);
	HDC mem = screen != NULL ? CreateCompatibleDC(screen) : NULL;
	HBITMAP dib = NULL;
	void *bits = NULL;
	bool ok = black != NULL && white != NULL && rgba != NULL && mem != NULL;
	if (ok) {
		BITMAPINFO bmi;
		memset(&bmi, 0, sizeof(bmi));
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = tw;
		bmi.bmiHeader.biHeight = -th; // top-down: row 0 is the top row
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
		ok = dib != NULL && bits != NULL;
	}
	if (ok) {
		HGDIOBJ old = SelectObject(mem, dib);
		memset(bits, 0x00, n * 4);
		ok = DrawIconEx(mem, 0, 0, hc, tw, th, 0, NULL, DI_NORMAL) != 0;
		GdiFlush();
		memcpy(black, bits, n * 4);
		memset(bits, 0xFF, n * 4);
		ok = ok && DrawIconEx(mem, 0, 0, hc, tw, th, 0, NULL, DI_NORMAL) != 0;
		GdiFlush();
		memcpy(white, bits, n * 4);
		SelectObject(mem, old);
	}
	if (ok) {
		// DIB pixels are B, G, R, x: recover (premultiplied), then swap to
		// R, G, B, A - the look's contract.
		u_cursor_draw_unblend(black, white, (uint32_t)n, rgba);
		for (size_t i = 0; i < n; i++) {
			const uint8_t b = rgba[i * 4 + 0];
			rgba[i * 4 + 0] = rgba[i * 4 + 2];
			rgba[i * 4 + 2] = b;
		}
	}
	if (dib != NULL) {
		DeleteObject(dib);
	}
	if (mem != NULL) {
		DeleteDC(mem);
	}
	if (screen != NULL) {
		ReleaseDC(NULL, screen);
	}
	free(black);
	free(white);
	if (!ok) {
		free(rgba);
		return false;
	}

	const float s = (float)th / (float)native_h;
	free(look->rgba);
	look->rgba = rgba;
	look->w = (uint32_t)tw;
	look->h = (uint32_t)th;
	look->aspect = (float)tw / (float)th;
	look->hot_u = ((float)hot_x * s) / (float)tw;
	look->hot_v = ((float)hot_y * s) / (float)th;
	look->height_pt = (float)th / scale; // OS points = 96-DPI pixels
	look->os_cursor = (void *)hc;        // shared USER object: nothing to retain
	look->os_scale = scale;
	look->generation++;
	// Not per frame: only when the cursor shape (or the DPI) changes. The
	// FIRST mirror is a one-time WARN (INFO is dropped from the log file, so
	// an INFO-only line never showed that the mirror ran); later shape
	// changes stay INFO.
	static bool announced = false; // process-wide: one notice, whichever compositor mirrors first
	if (!announced) {
		announced = true;
		U_LOG_W("cursor_draw: default look mirrors the OS cursor (%dx%d px at %u DPI, image %dx%d, hotspot "
		        "%d,%d; one-time notice, later shape changes log at INFO)",
		        tw, th, dpi, native_w, native_h, hot_x, hot_y);
	} else {
		U_LOG_I("cursor_draw: default look = the current OS cursor (%dx%d px at %u DPI, image %dx%d, hotspot %d,%d)",
		        tw, th, dpi, native_w, native_h, hot_x, hot_y);
	}
	return true;
}

bool
comp_cursor_look_platform_mirror(struct comp_cursor_look *look, float backing_scale)
{
	const float scale = backing_scale > 0.0f ? backing_scale : comp_cursor_look_platform_backing_scale();

	CURSORINFO ci;
	memset(&ci, 0, sizeof(ci));
	ci.cbSize = sizeof(ci);
	HCURSOR hc = GetCursorInfo(&ci) ? ci.hCursor : NULL;
	if (hc == NULL) {
		// Hidden (see the file comment): the class cursor of the window
		// under the pointer, else the last mirrored shape, else IDC_ARROW.
		POINT pt;
		HWND under = GetCursorPos(&pt) ? WindowFromPoint(pt) : NULL;
		hc = under != NULL ? (HCURSOR)GetClassLongPtr(under, GCLP_HCURSOR) : NULL;
		if (hc == NULL) {
			hc = look->os_cursor != NULL ? (HCURSOR)look->os_cursor : LoadCursor(NULL, IDC_ARROW);
		}
	}
	if (hc == NULL) {
		return false;
	}
	if ((void *)hc == look->os_cursor && look->os_scale == scale && look->rgba != NULL) {
		return true; // unchanged: the common case
	}
	if ((void *)hc == look->os_failed) {
		return false; // an unreadable shape is not retried every frame
	}
	if (snapshot(look, hc, scale)) {
		return true;
	}
	look->os_failed = (void *)hc;
	U_LOG_W("cursor_draw: could not read the OS cursor %p - keeping the last shape or the arrow", (void *)hc);
	return false;
}

void
comp_cursor_look_platform_release(void *os_cursor)
{
	// An HCURSOR from GetCursorInfo / LoadCursor(NULL, ...) / a class is a
	// shared USER object: it is never destroyed by its reader.
	(void)os_cursor;
}

float
comp_cursor_look_platform_backing_scale(void)
{
	static float cached = 0.0f;
	if (cached <= 0.0f) {
		pfn_get_dpi_for_system fn = (pfn_get_dpi_for_system)(void *)user32_proc("GetDpiForSystem");
		const UINT dpi = fn != NULL ? fn() : 96;
		cached = (float)(dpi != 0 ? dpi : 96) / 96.0f;
	}
	return cached;
}
