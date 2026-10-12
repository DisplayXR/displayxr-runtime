// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Test-only knob that drives dxr::ModeSwitch without a key press.
 *
 * DXR_TEST_MODE_TOGGLE_S=<seconds> makes the app request an alternating
 * 3D -> 2D -> 3D switch every <seconds> (first one after <seconds> of running
 * session), exactly as a V press would — through the app's dxr::ModeSwitch, so
 * the eased path is what gets exercised. While the knob is set, tracing() is
 * true and the app logs one line per frame that a ramp is in flight (a
 * transition is ~11 frames at 60 Hz, so this is bounded; never set in normal
 * runs). Lets an agent measure the ramp on a box where it cannot (or must not)
 * synthesize key presses into the app window.
 *
 * Header-only; no OpenXR / platform dependency.
 */
#pragma once

#include <cstdint>
#include <cstdlib>

namespace dxr_test {

class ModeSwitchToggleKnob
{
public:
	ModeSwitchToggleKnob()
	{
		const char *e = std::getenv("DXR_TEST_MODE_TOGGLE_S");
		if (e != nullptr && e[0] != '\0') {
			period_ = (float)std::atof(e);
			if (period_ < 0.05f) {
				period_ = 0.0f;
			}
		}
	}

	//! True when DXR_TEST_MODE_TOGGLE_S is set: log the ramp per frame.
	bool
	tracing() const
	{
		return period_ > 0.0f;
	}

	//! Call once per frame while the session runs. Returns the mode index to
	//! request this frame, or -1. Alternates between the current 3D mode and
	//! the first 2D (1-view) mode, remembering which 3D mode to come back to.
	int32_t
	poll(float dt_s, uint32_t currentMode, uint32_t modeCount, const uint32_t *modeViewCounts)
	{
		if (period_ <= 0.0f || modeCount == 0 || modeViewCounts == nullptr) {
			return -1;
		}
		acc_ += dt_s;
		if (acc_ < period_) {
			return -1;
		}
		acc_ = 0.0f;
		const bool cur3D = currentMode < modeCount && modeViewCounts[currentMode] > 1;
		if (cur3D) {
			last3D_ = currentMode;
			for (uint32_t m = 0; m < modeCount; m++) {
				if (modeViewCounts[m] <= 1) {
					return (int32_t)m;
				}
			}
			return -1; // no 2D mode to go to
		}
		return last3D_ < modeCount ? (int32_t)last3D_ : -1;
	}

private:
	float period_ = 0.0f;
	float acc_ = 0.0f;
	uint32_t last3D_ = 1;
};

} // namespace dxr_test
