// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief MANUAL eye-tracking demo: the app's own tracker-loss transition.
 *
 * What an app can do on tracker loss when the display leaves the transition to
 * it (MANUAL; in MANAGED the display waits ~3 s, eases to 2D and flips the lens
 * itself). Fed the session's isTracking edges; its output is a depth factor
 * the app multiplies into the display rig's ipdFactor (the runtime scales each
 * eye about the centre eye and rebuilds the frusta, so the screen plane stays
 * pixel-locked while depth collapses onto it, no swim).
 *
 * Presets (cycled by a key: P on Linux, L on Windows where P is parallax):
 *   CONVERGE (default): hold 250 ms, collapse to ipd 0 over 600 ms, stay flat
 *            with the lens in 3D. Recover: 150 ms stable, ramp to 1 over 400 ms.
 *   LENS_2D: on loss request the lens 2D at once (xrRequestDisplayModeDXR) and
 *            collapse the same way; on recovery lens 3D, then ramp back. The
 *            CONTENT mode stays tracked, so recovery edges keep arriving.
 *   OFF:     MANUAL with no app reaction (the raw held weave), for contrast.
 * Flapping: 3 losses within 5 s need 1 s of stable tracking to come back.
 * Only active in MANUAL; in MANAGED the factor ramps back to 1 and the display
 * acts on its own.
 *
 * Header-only, no dependencies: shared by the Windows cube apps (the Linux
 * cube carries the same logic inline, cube_handle_vk_linux/main.cpp). Each
 * app calls TrackingLossStep once per frame, multiplies Ipd() into its rig's
 * ipdFactor and may draw a zero-disparity status bar with BarColor().
 */
#pragma once

#include <chrono>
#include <cmath>
#include <cstdio>

namespace dxr_demo {

struct TrackingLossDirector
{
	enum Preset
	{
		CONVERGE = 0,
		LENS_2D = 1,
		OFF = 2,
		PRESET_COUNT = 3
	};
	enum Phase
	{
		TRACKING,
		HOLD,
		COLLAPSE,
		FLAT,
		RECOVER_WAIT,
		RECOVER,
		IDLE
	};
	static constexpr float kHoldS = 0.25f, kCollapseS = 0.6f, kRecoverWaitS = 0.15f, kRecoverS = 0.4f,
	                       kFlapWindowS = 5.0f, kFlapWaitS = 1.0f;
	static constexpr int kFlapLosses = 3;

	Preset preset = CONVERGE;
	Phase phase = TRACKING;
	bool tracked = true;
	float sinceEdge = 1e9f; // seconds since the last tracking edge
	float u = 1.0f;         // 1 = full depth, 0 = flat (ipd = smoothstep(u))
	float clock = 0.0f;
	float lossTimes[kFlapLosses] = {-1e9f, -1e9f, -1e9f};
	int lossHead = 0;
	bool lens2DRequested = false;

	static const char *
	PresetName(Preset p)
	{
		return p == CONVERGE ? "converge" : p == LENS_2D ? "lens-2D" : "off";
	}
	static const char *
	PhaseName(Phase p)
	{
		switch (p) {
		case TRACKING: return "tracking";
		case HOLD: return "hold";
		case COLLAPSE: return "collapse";
		case FLAT: return "flat";
		case RECOVER_WAIT: return "recover-wait";
		case RECOVER: return "recover";
		default: return "idle";
		}
	}
	void
	CyclePreset()
	{
		preset = (Preset)((preset + 1) % PRESET_COUNT);
	}
	float
	Ipd() const
	{
		return u * u * (3.0f - 2.0f * u);
	}
	bool
	Flapping() const
	{
		int n = 0;
		for (float t : lossTimes) {
			n += (clock - t) <= kFlapWindowS ? 1 : 0;
		}
		return n >= kFlapLosses;
	}
	//! Feed the session's isTracking; a no-op unless it changed.
	void
	OnEdge(bool isTracking)
	{
		if (isTracking == tracked) {
			return;
		}
		tracked = isTracking;
		sinceEdge = 0.0f;
		if (!isTracking) {
			lossTimes[lossHead] = clock;
			lossHead = (lossHead + 1) % kFlapLosses;
		}
	}
	/*!
	 * Advance by @p dt seconds. @p manual: the session's mode is MANUAL.
	 * Returns the lens request to issue this frame: -1 none, 0 = 2D, 1 = 3D.
	 * @p out_before is the phase before this step (log when it differs).
	 */
	int
	Update(float dt, bool manual, Phase *out_before)
	{
		clock += dt;
		sinceEdge += dt;
		const Phase before = phase;
		int lens = -1;
		if (!manual || preset == OFF) {
			u = u + dt / kRecoverS > 1.0f ? 1.0f : u + dt / kRecoverS;
			if (lens2DRequested) {
				lens = 1;
				lens2DRequested = false;
			}
			phase = IDLE;
		} else if (!tracked) {
			if (preset == LENS_2D && !lens2DRequested) {
				lens = 0;
				lens2DRequested = true;
			}
			if (sinceEdge < kHoldS && u >= 1.0f) {
				phase = HOLD;
			} else {
				u = u - dt / kCollapseS < 0.0f ? 0.0f : u - dt / kCollapseS;
				phase = u > 0.0f ? COLLAPSE : FLAT;
			}
		} else {
			const float wait = Flapping() ? kFlapWaitS : kRecoverWaitS;
			if (u >= 1.0f && !lens2DRequested) {
				phase = TRACKING;
			} else if (sinceEdge < wait) {
				phase = RECOVER_WAIT;
			} else {
				if (lens2DRequested) {
					lens = 1;
					lens2DRequested = false;
				}
				u = u + dt / kRecoverS > 1.0f ? 1.0f : u + dt / kRecoverS;
				phase = u < 1.0f ? RECOVER : TRACKING;
			}
		}
		if (out_before != nullptr) {
			*out_before = before;
		}
		return lens;
	}
	//! Zero-disparity status bar colour (display-referred RGB).
	void
	BarColor(bool manual, float rgb[3]) const
	{
		auto set = [&](float r, float g, float b) {
			rgb[0] = r;
			rgb[1] = g;
			rgb[2] = b;
		};
		if (!manual) { // MANAGED: blue / red
			tracked ? set(0.15f, 0.45f, 1.0f) : set(1.0f, 0.15f, 0.15f);
			return;
		}
		if (preset == OFF) { // purple / red
			tracked ? set(0.7f, 0.3f, 1.0f) : set(1.0f, 0.15f, 0.15f);
			return;
		}
		switch (phase) {
		case HOLD:
		case COLLAPSE: set(1.0f, 0.65f, 0.0f); break;     // amber: transition
		case FLAT: set(1.0f, 0.15f, 0.15f); break;        // red: flat, no viewer
		case RECOVER_WAIT:
		case RECOVER: set(0.6f, 1.0f, 0.2f); break;       // yellow-green: coming back
		default: set(0.1f, 0.9f, 0.3f); break;            // green: tracked
		}
	}
};

/*!
 * One frame of the demo for an app: consume the preset-cycle key, feed the
 * session's isTracking (logging each edge with a steady-clock ms stamp, the
 * same "Eye tracking state:" line as the Linux cube), advance the director and
 * issue its lens request. @p log takes a finished line; @p request_lens takes
 * 0 (2D) or 1 (3D) and returns whether the request succeeded.
 */
template <typename LogFn, typename LensFn>
inline void
TrackingLossStep(TrackingLossDirector &d, float dt, bool manual, bool isTracking, bool *cycle_requested,
                 LogFn log, LensFn request_lens)
{
	char line[192];
	if (cycle_requested != nullptr && *cycle_requested) {
		*cycle_requested = false;
		d.CyclePreset();
		std::snprintf(line, sizeof(line), "Tracking-loss preset (L) -> %s%s", TrackingLossDirector::PresetName(d.preset),
		              manual ? "" : " (applies in MANUAL; press T)");
		log(line);
	}
	if (isTracking != d.tracked) {
		const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
		                         std::chrono::steady_clock::now().time_since_epoch())
		                         .count();
		std::snprintf(line, sizeof(line), "Eye tracking state: isTracking=%s mode=%s (t=%lld ms)",
		              isTracking ? "true" : "false", manual ? "MANUAL" : "MANAGED", ms);
		log(line);
		d.OnEdge(isTracking);
	}
	TrackingLossDirector::Phase before = d.phase;
	const int lens = d.Update(dt, manual, &before);
	if (d.phase != before) {
		std::snprintf(line, sizeof(line), "Tracking-loss director: %s -> %s (preset %s, ipd %.2f%s)",
		              TrackingLossDirector::PhaseName(before), TrackingLossDirector::PhaseName(d.phase),
		              TrackingLossDirector::PresetName(d.preset), d.Ipd(), d.Flapping() ? ", flapping" : "");
		log(line);
	}
	if (lens >= 0) {
		const bool ok = request_lens(lens);
		std::snprintf(line, sizeof(line), "Tracking-loss director: lens %s request: %s", lens ? "3D" : "2D",
		              ok ? "ok" : "FAILED");
		log(line);
	}
}

//! sRGB EOTF: a display-referred value for an _SRGB render target's clear.
inline float
DisplayReferredToLinear(float c)
{
	return c <= 0.04045f ? c / 12.92f : (float)std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace dxr_demo
