// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-drawn cursor's default look - see u_cursor_look.h.
 * @author David Fattal
 * @ingroup aux_util
 */

#include "util/u_cursor_look.h"
#include "util/u_cursor_depth.h"
#include "util/u_logging.h"

#include "xrt/xrt_config_os.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

//! Largest OS cursor image we mirror, per side (accessibility sizes reach 256).
#define U_CURSOR_LOOK_MAX_PX 256u

static float
dpi_scale(uint32_t dpi)
{
	return (float)(dpi != 0 ? dpi : 96u) / 96.0f;
}

//! Take ownership of @p rgba as the new image and bump the serial.
static void
look_set(struct u_cursor_look *look,
         uint8_t *rgba,
         uint32_t w,
         uint32_t h,
         float hot_u,
         float hot_v,
         float height_px,
         bool from_os,
         uintptr_t os_handle,
         uint32_t dpi)
{
	free(look->rgba);
	look->rgba = rgba;
	look->width = w;
	look->height = h;
	look->hot_u = hot_u;
	look->hot_v = hot_v;
	look->height_px = height_px;
	look->from_os = from_os;
	look->os_handle = os_handle;
	look->dpi = dpi;
	look->serial++;
	if (look->serial == 0) {
		look->serial = 1; // 0 means "no image"
	}
}

//! The classic arrow (Amendment 2 A.6): 12 x 19 px at 96 DPI, tip = hotspot.
static bool
look_use_arrow(struct u_cursor_look *look, uint32_t dpi)
{
	if (look->serial != 0 && !look->from_os && look->dpi == dpi) {
		return true; // already the arrow, at this DPI
	}
	const uint32_t h = 76; // 4 px per arrow px: crisp up to 4x scale
	const uint32_t w = (uint32_t)ceilf((float)h * U_CURSOR_ARROW_WIDTH_PX / U_CURSOR_ARROW_HEIGHT_PX);
	uint8_t *rgba = calloc((size_t)w * h, 4);
	if (rgba == NULL) {
		return look->serial != 0;
	}
	u_cursor_draw_arrow_rgba(rgba, w, h);
	u_cursor_draw_unpremultiply(rgba, w * h);
	look_set(look, rgba, w, h, 0.0f, 0.0f, U_CURSOR_ARROW_HEIGHT_PX * dpi_scale(dpi), false, 0, dpi);
	U_LOG_I("cursor_draw: default look = the classic arrow (12x19 px at 96 DPI, tip = hotspot)");
	return true;
}

#ifdef XRT_OS_WINDOWS

typedef int(WINAPI *pfn_get_system_metrics_for_dpi)(int, UINT);

//! The OS cursor's nominal height at @p dpi (SM_CYCURSOR), or 0 if unknown.
static int
cursor_size_for_dpi(uint32_t dpi)
{
	static pfn_get_system_metrics_for_dpi fn = NULL;
	static bool looked = false;
	if (!looked) {
		looked = true;
		HMODULE user32 = GetModuleHandleA("user32.dll");
		if (user32 != NULL) {
			fn = (pfn_get_system_metrics_for_dpi)(void *)GetProcAddress(user32, "GetSystemMetricsForDpi");
		}
	}
	if (fn != NULL) {
		return fn(SM_CYCURSOR, dpi);
	}
	return 0; // pre-1607 Windows: fall back to the image's own size
}

/*!
 * Rasterise @p hc at the size the OS draws it at @p dpi, over black and over
 * white, and recover RGBA + alpha from the pair.
 */
static bool
look_snapshot_os(struct u_cursor_look *look, HCURSOR hc, uint32_t dpi)
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
		// Monochrome: the mask is the AND mask stacked on the XOR mask.
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

	// The size the OS draws it at on this panel: the per-DPI nominal size,
	// or the image's own when larger (an accessibility pointer size loads a
	// bigger image than SM_CYCURSOR reports).
	int th = cursor_size_for_dpi(dpi);
	if (th < native_h) {
		th = native_h;
	}
	int tw = (int)lrintf((float)native_w * (float)th / (float)native_h);
	if (tw <= 0 || th <= 0 || (uint32_t)tw > U_CURSOR_LOOK_MAX_PX || (uint32_t)th > U_CURSOR_LOOK_MAX_PX) {
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
		// DIB pixels are B, G, R, x: recover, then swap to R, G, B, A.
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
	look_set(look, rgba, (uint32_t)tw, (uint32_t)th, ((float)hot_x * s) / (float)tw, ((float)hot_y * s) / (float)th,
	         (float)th, true, (uintptr_t)hc, dpi);
	// Not per frame: only when the cursor shape (or the DPI) changes.
	U_LOG_I("cursor_draw: default look = the current OS cursor (%dx%d px at %u DPI, image %dx%d, hotspot %d,%d)",
	        tw, th, dpi, native_w, native_h, hot_x, hot_y);
	return true;
}

#endif // XRT_OS_WINDOWS

bool
u_cursor_look_update(struct u_cursor_look *look, uint32_t dpi)
{
	if (look == NULL) {
		return false;
	}
	if (dpi == 0) {
		dpi = 96;
	}
	if (look->force_arrow == 0) {
		const char *e = getenv("DXR_CURSOR_DEFAULT_LOOK");
		look->force_arrow = (e != NULL && strcmp(e, "arrow") == 0) ? 2 : 1;
	}

#ifdef XRT_OS_WINDOWS
	if (look->force_arrow != 2) {
		CURSORINFO ci;
		memset(&ci, 0, sizeof(ci));
		ci.cbSize = sizeof(ci);
		HCURSOR hc = GetCursorInfo(&ci) ? ci.hCursor : NULL;
		if (hc == NULL) {
			// Hidden - and it usually is, on exactly the frames we draw:
			// the app hides its cursor (SetCursor(NULL)) as soon as the
			// locate reports willDraw, before this frame's xrEndFrame. Use
			// the shape the window under the pointer would show (its class
			// cursor), else the last shape, else the system arrow.
			POINT pt;
			HWND under = GetCursorPos(&pt) ? WindowFromPoint(pt) : NULL;
			hc = under != NULL ? (HCURSOR)GetClassLongPtr(under, GCLP_HCURSOR) : NULL;
			if (hc == NULL && look->serial != 0 && look->dpi == dpi) {
				return true;
			}
			if (hc == NULL) {
				hc = (look->from_os && look->os_handle != 0) ? (HCURSOR)look->os_handle
				                                             : LoadCursor(NULL, IDC_ARROW);
			}
		}
		if (hc != NULL) {
			if (look->from_os && (uintptr_t)hc == look->os_handle && look->dpi == dpi) {
				return true; // unchanged: the common case, one GetCursorInfo per frame
			}
			if ((uintptr_t)hc != look->failed_handle) {
				if (look_snapshot_os(look, hc, dpi)) {
					if (!look->announced) {
						look->announced = true;
						U_LOG_W(
						    "cursor_draw: default look mirrors the OS cursor (%ux%u px, "
						    "one-time notice)",
						    look->width, look->height);
					}
					return true;
				}
				look->failed_handle = (uintptr_t)hc;
				U_LOG_W("cursor_draw: could not read the OS cursor %p - using the classic arrow",
				        (void *)hc);
			}
			// An unreadable shape: keep whatever we last drew, else the arrow.
			if (look->serial != 0 && look->dpi == dpi) {
				return true;
			}
		}
	}
#endif

	return look_use_arrow(look, dpi);
}

void
u_cursor_look_fini(struct u_cursor_look *look)
{
	if (look == NULL) {
		return;
	}
	free(look->rgba);
	memset(look, 0, sizeof(*look));
}
