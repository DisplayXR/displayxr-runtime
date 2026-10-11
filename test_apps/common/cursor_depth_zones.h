// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_cursor_depth on a display-zones app (runtime#1949) — the
 *         cube_zones_*_macos reference adoption.
 *
 * With display zones the cursor hint rides the ZONE-SCOPED locate of the zone
 * under the pointer: that locate's canvas is the zone rect, so the hint's
 * cursorUV is zone-normalised, and the runtime draws the cursor into that
 * zone's tiles only (one cursor per frame). Everything else is
 * displayxr-common's dxr::CursorDepth, exactly as in the cube_handle apps:
 * the footprint hit test against the previous locate's views, the struct
 * chain, and hiding the OS cursor exactly while the runtime reports willDraw.
 *
 * Per frame:
 *   begin(windowUV, rects)          pick the topmost 3D zone under the pointer
 *   for each zone: chain(zi, ...)   before its xrLocateViews (no-op unless picked)
 *                  afterLocate(zi, views, n)
 *   end()                           pointer over no 3D zone: OS cursor back
 *
 * The pointer is the window's (macOS: the NSView's), or a scripted
 * WINDOW-normalised UV from DISPLAYXR_CURSOR_DEPTH_UV=u,v for headless
 * verification. Opt out with DISPLAYXR_CURSOR_DEPTH=0.
 */

#pragma once

#include <openxr/openxr.h>
#include <openxr/XR_DXR_cursor_depth.h>

#include "cursor_depth.h" // dxr::CursorDepth (displayxr-common >= v2.29.0)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#ifdef __OBJC__
#import <Cocoa/Cocoa.h>
#endif

namespace dxr_zones_cursor {

/*!
 * Slab test against the zone cube as the zone apps place it: centred at
 * (0, size/2, 0), edge @p size, rotated @p rot about +Y.
 */
inline bool
rayHitCube(const XrVector3f &o, const XrVector3f &d, float size, float rot, float &tOut)
{
	const float c = cosf(rot), s = sinf(rot), h = size * 0.5f;
	const float ox = o.x, oy = o.y - h, oz = o.z;
	const float lo[3] = {c * ox - s * oz, oy, s * ox + c * oz};
	const float ld[3] = {c * d.x - s * d.z, d.y, s * d.x + c * d.z};
	float t0 = 0.0f, t1 = 1e30f;
	for (int i = 0; i < 3; i++) {
		if (fabsf(ld[i]) < 1e-9f) {
			if (lo[i] < -h || lo[i] > h) return false;
			continue;
		}
		float ta = (-h - lo[i]) / ld[i], tb = (h - lo[i]) / ld[i];
		if (ta > tb) std::swap(ta, tb);
		if (ta > t0) t0 = ta;
		if (tb < t1) t1 = tb;
		if (t0 > t1) return false;
	}
	tOut = t0;
	return true;
}

class ZoneCursor
{
public:
	/*!
	 * @param spec     the runtime's XR_DXR_cursor_depth spec version, 0 = off.
	 * @param nsView   the canvas view (hides the OS cursor; reads the pointer).
	 * @param cubeSize the zone cube's edge (the ray-cast target).
	 */
	void init(uint32_t spec, void *nsView, float cubeSize)
	{
		m_cubeSize = cubeSize;
		m_view = nsView;
		m_cursor.enable(spec);
		if (spec == 0) return;
		m_cursor.setWindowCocoa(nsView);
		m_cursor.setCursorHeight(0.04f);
		m_cursor.setWantPlacement(true);
		m_cursor.setRayCast([this](const dxr::CursorRay &r) -> std::optional<float> {
			float t;
			return rayHitCube(r.origin, r.dir, m_cubeSize, m_rot, t) ? std::optional<float>(t) : std::nullopt;
		});
		if (const char *fx = getenv("DISPLAYXR_CURSOR_DEPTH_UV")) {
			float u, v;
			if (sscanf(fx, "%f,%f", &u, &v) == 2) {
				m_scripted = true;
				m_scriptedUV = {u, v};
			}
		}
	}

	bool enabled() const { return m_cursor.enabled(); }

	/*!
	 * Pick the topmost 3D zone (the last in layer order) under the pointer and
	 * hand dxr::CursorDepth the pointer in THAT zone's UV.
	 *
	 * @param rects  the zones' rects, client-window (backing) pixels, in
	 *               layer order.
	 * @param rots   each zone's cube rotation (the ray-cast target).
	 */
	void begin(int32_t winW, int32_t winH, const XrRect2Di *rects, const float *rots, uint32_t count)
	{
		m_zone = -1;
		if (!enabled() || winW <= 0 || winH <= 0) return;
		XrVector2f uv{};
		if (!windowUV(uv)) return;
		const float px = uv.x * (float)winW, py = uv.y * (float)winH;
		for (int i = (int)count - 1; i >= 0; i--) {
			const XrRect2Di &r = rects[i];
			if (r.extent.width <= 0 || r.extent.height <= 0) continue;
			const float zu = (px - (float)r.offset.x) / (float)r.extent.width;
			const float zv = (py - (float)r.offset.y) / (float)r.extent.height;
			if (zu < 0.0f || zu > 1.0f || zv < 0.0f || zv > 1.0f) continue;
			m_zone = i;
			m_rot = rots[i];
			m_cursor.setPointerUV(zu, zv);
			return;
		}
	}

	//! Before zone @p zi's xrLocateViews: the hint rides only the picked zone's.
	void chain(uint32_t zi, XrViewLocateInfo &locateInfo, XrViewState &viewState)
	{
		if (enabled() && (int)zi == m_zone) m_cursor.chain(locateInfo, viewState);
	}

	void afterLocate(uint32_t zi, const XrView *views, uint32_t activeViewCount)
	{
		if (!enabled() || (int)zi != m_zone) return;
		m_cursor.afterLocate(views, activeViewCount);
		logState();
	}

	//! After the frame's locates: over no 3D zone, the OS cursor is the cursor.
	void end()
	{
		if (!enabled() || m_zone >= 0) return;
		m_cursor.setPointerUV(-1.0f, -1.0f); // off every canvas: no hint
		XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
		XrViewState vs{XR_TYPE_VIEW_STATE};
		m_cursor.chain(li, vs);
		m_cursor.afterLocate(nullptr, 0);
	}

private:
	bool windowUV(XrVector2f &uv) const
	{
		if (m_scripted) {
			uv = m_scriptedUV;
			return true;
		}
#ifdef __OBJC__
		NSView *view = (__bridge NSView *)m_view;
		if (view == nil || view.window == nil) return false;
		NSPoint p = [view convertPoint:[view.window mouseLocationOutsideOfEventStream] fromView:nil];
		const NSRect b = view.bounds;
		return dxr::cursorCanvasUV(p.x, p.y, b.size.width, b.size.height, !view.isFlipped, uv);
#else
		(void)uv;
		return false;
#endif
	}

	void logState()
	{
		const int will = m_cursor.drawState().willDraw == XR_TRUE ? 1 : 0;
		if (will != m_lastWill && m_logged < 6) {
			m_lastWill = will;
			m_logged++;
			fprintf(stderr, "[INFO] cursor_depth (zones): zone #%d uv=(%.3f,%.3f) hasContent=%d disparity=%.3f willDraw=%s\n",
			        m_zone, m_cursor.hint().cursorUV.x, m_cursor.hint().cursorUV.y, (int)m_cursor.hint().hasContent,
			        m_cursor.placement().disparity, will ? "TRUE" : "FALSE");
		}
	}

	dxr::CursorDepth m_cursor;
	void *m_view = nullptr;
	float m_cubeSize = 0.06f;
	float m_rot = 0.0f;
	int m_zone = -1;
	bool m_scripted = false;
	XrVector2f m_scriptedUV{};
	int m_lastWill = -1;
	int m_logged = 0;
};

} // namespace dxr_zones_cursor
