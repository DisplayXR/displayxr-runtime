// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Does the ACTIVE rendering mode track the viewer? (XR_DXR_display_info v14)
 *
 * A rendering mode advertises whether it consumes live viewer tracking through
 * XrDisplayRenderingModeTrackingInfoDXR::hasTracking, chained on each
 * XrDisplayRenderingModeInfoDXR of xrEnumerateDisplayRenderingModesDXR. In a
 * mode with hasTracking == XR_FALSE the runtime reports isTracking = false
 * whatever the tracker does, so an app must not read that as "face lost".
 * The answer is per mode and the table can change at run time, so it is read
 * from the runtime, never assumed from a mode index or name.
 *
 * ModeTrackingWatch keeps the answer for the active mode current: it re-reads
 * when the active mode index changes (XrEventDataRenderingModeChangedDXR) and
 * once a second, which covers XrEventDataRenderingModeTableChangedDXR (sent at
 * most once a second; the shared event pump the Windows cubes use does not
 * hand it to the app). One enumerate call per second is negligible.
 *
 * Feeds TrackingLossDirector::SetModeTracking (tracking_loss_director.h).
 */
#pragma once

#include <stdint.h>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/XR_DXR_display_info.h>

namespace dxr_demo {

/*!
 * Ask the runtime whether rendering mode @p modeIndex consumes viewer tracking.
 * Returns false and leaves @p out untouched when there is no answer (no
 * function, no session, the mode is not in the table). A runtime that does not
 * fill the chained struct answers "tracked" (the pre-set value), which keeps
 * the old behaviour.
 */
inline bool
QueryModeHasTracking(PFN_xrEnumerateDisplayRenderingModesDXR pfn, XrSession session, uint32_t modeIndex, bool *out)
{
	if (pfn == nullptr || session == XR_NULL_HANDLE || out == nullptr) {
		return false;
	}
	uint32_t count = 0;
	if (XR_FAILED(pfn(session, 0, &count, nullptr)) || count == 0) {
		return false;
	}
	std::vector<XrDisplayRenderingModeInfoDXR> modes(count);
	std::vector<XrDisplayRenderingModeTrackingInfoDXR> tracking(count);
	for (uint32_t i = 0; i < count; i++) {
		tracking[i] = {};
		tracking[i].type = (XrStructureType)XR_TYPE_DISPLAY_RENDERING_MODE_TRACKING_INFO_DXR;
		tracking[i].hasTracking = XR_TRUE;
		modes[i] = {};
		modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
		modes[i].next = &tracking[i];
	}
	if (XR_FAILED(pfn(session, count, &count, modes.data()))) {
		return false;
	}
	for (uint32_t i = 0; i < count && i < (uint32_t)modes.size(); i++) {
		if (modes[i].modeIndex == modeIndex) {
			*out = tracking[i].hasTracking == XR_TRUE;
			return true;
		}
	}
	return false;
}

//! The active mode's hasTracking, kept current once per frame (see the file comment).
struct ModeTrackingWatch
{
	bool hasTracking = true; //!< true until the runtime says otherwise
	uint32_t lastMode = UINT32_MAX;
	float sinceRefresh = 0.0f;

	//! Call once per frame; returns the active mode's hasTracking.
	bool
	Update(PFN_xrEnumerateDisplayRenderingModesDXR pfn, XrSession session, uint32_t activeModeIndex, float dt)
	{
		sinceRefresh += dt;
		if (activeModeIndex != lastMode || sinceRefresh >= 1.0f) {
			sinceRefresh = 0.0f;
			lastMode = activeModeIndex; // no answer: keep the last one, retry in a second
			bool v = true;
			if (QueryModeHasTracking(pfn, session, activeModeIndex, &v)) {
				hasTracking = v;
			}
		}
		return hasTracking;
	}
};

} // namespace dxr_demo
