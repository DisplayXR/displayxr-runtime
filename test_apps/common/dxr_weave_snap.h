// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  xrWeaveSnapWindowRectDXR behind DxrLinuxWindow's snap provider (#1588).
 *
 * The Linux window helper owns the drag mechanics and knows nothing about
 * OpenXR sessions; this is the ~one screen of glue that connects its plain
 * `SnapWindowOriginFn` to the real extension entry point, shared by
 * cube_handle_vk_linux and cube_zones_vk_linux so the two cannot drift.
 *
 * WHY the drag needs it. The vendor display processor weaves the interlace at
 * a phase that is a function of the window's ABSOLUTE position in physical
 * panel pixels. Drag the window one pixel and the phase moves with it; drag it
 * continuously and the 3D shimmers. The cure is invariance, not correction:
 * the window is only ever allowed to land on the lens lattice, so the pattern
 * is identical at every position the drag visits. The lattice (pitch, slant)
 * is the vendor's and never leaves the DP — the app only ever asks
 * "given I started here and want to go there, where may I land?".
 *
 * WHAT IT DOES TODAY. On desktop Linux the runtime does not yet advertise
 * XR_DXR_weave at all, so the function does not resolve, this reports identity
 * once, and every drag move goes to the raw pointer-derived target. That is
 * the correct degraded behaviour, and nothing about the drag changes when the
 * runtime + plug-in side lands: the same call then starts returning snapped
 * points. In-process sessions are also documented to answer
 * XR_ERROR_FEATURE_UNSUPPORTED for the weave service; a failing call is
 * likewise reported once and then treated as identity.
 */
#pragma once

#include "dxr_linux_window.h"

#include <openxr/openxr.h>
#include <openxr/XR_DXR_weave.h>

#include <cstdint>
#include <cstdio>

/*!
 * Resolves xrWeaveSnapWindowRectDXR once and answers DxrLinuxWindow's snap
 * callback with it. One instance per app, lifetime >= the window's.
 */
class DxrWeaveSnap
{
public:
	/*!
	 * Resolve the entry point. Safe to call with a runtime that has never
	 * heard of XR_DXR_weave (or with the extension simply not enabled):
	 * xrGetInstanceProcAddr then answers XR_ERROR_FUNCTION_UNSUPPORTED and
	 * this object stays in identity mode.
	 *
	 * @param extent_w/h the window's size in pixels. Only the rect OFFSET is
	 *                   snapped (the extent passes through unchanged per the
	 *                   spec), but the DP is handed the real size so a future
	 *                   size-aware snap is not fed a lie.
	 */
	void
	attach(XrInstance instance, XrSession session, uint32_t extent_w, uint32_t extent_h)
	{
		m_session = session;
		m_w = (int32_t)extent_w;
		m_h = (int32_t)extent_h;
		m_pfn = nullptr;

		if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE) {
			return;
		}
		PFN_xrVoidFunction fn = nullptr;
		if (xrGetInstanceProcAddr(instance, "xrWeaveSnapWindowRectDXR", &fn) == XR_SUCCESS && fn != nullptr) {
			m_pfn = reinterpret_cast<PFN_xrWeaveSnapWindowRectDXR>(fn);
		}
	}

	//! Keep the extent current after a resize. Cheap; no OpenXR call.
	void
	set_extent(uint32_t w, uint32_t h)
	{
		m_w = (int32_t)w;
		m_h = (int32_t)h;
	}

	//! The DxrLinuxWindow::SnapWindowOriginFn. `userdata` is a DxrWeaveSnap*.
	static bool
	callback(void *userdata,
	         int32_t origin_x,
	         int32_t origin_y,
	         int32_t target_x,
	         int32_t target_y,
	         int32_t *out_x,
	         int32_t *out_y)
	{
		DxrWeaveSnap *self = static_cast<DxrWeaveSnap *>(userdata);
		if (self == nullptr || self->m_pfn == nullptr || self->m_session == XR_NULL_HANDLE) {
			return false;
		}

		XrRect2Di origin = {};
		origin.offset.x = origin_x;
		origin.offset.y = origin_y;
		origin.extent.width = self->m_w;
		origin.extent.height = self->m_h;

		XrRect2Di target = origin;
		target.offset.x = target_x;
		target.offset.y = target_y;

		XrRect2Di snapped = {};
		const XrResult res = self->m_pfn(self->m_session, &origin, &target, &snapped);
		if (res != XR_SUCCESS) {
			if (!self->m_failed) {
				self->m_failed = true;
				fprintf(stderr,
				        "[WARN]  xrWeaveSnapWindowRectDXR failed (%d) — drag falls back to an "
				        "unsnapped window position for the rest of this run\n",
				        (int)res);
				fflush(stderr);
			}
			self->m_pfn = nullptr;
			return false;
		}

		*out_x = snapped.offset.x;
		*out_y = snapped.offset.y;
		return true;
	}

	//! Did the entry point resolve? (For the app's own one-line startup log.)
	bool
	available() const
	{
		return m_pfn != nullptr;
	}

private:
	PFN_xrWeaveSnapWindowRectDXR m_pfn = nullptr;
	XrSession m_session = XR_NULL_HANDLE;
	int32_t m_w = 0;
	int32_t m_h = 0;
	bool m_failed = false;
};
