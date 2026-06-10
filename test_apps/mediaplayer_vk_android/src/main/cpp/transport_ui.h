// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Transport bar layout + hit-testing, shared by the renderer (which draws the
// bar into each eye at these normalized fractions) and main.cpp (which hit-tests
// touches against the same regions). Single source of truth so draw + touch
// agree. Coordinates are fractions of the per-view tile / screen [0,1].
//
// Layout (bottom strip):  [▶/❚❚]  0:42 ━━━●──── 3:15            [load]
#pragma once

namespace tui {

constexpr float kRowY0 = 0.835f, kRowY1 = 0.945f;  // interactive vertical band (= panel)
constexpr float kBarY0 = 0.880f, kBarY1 = 0.900f;  // the thin track itself

constexpr float kBtnX0 = 0.020f, kBtnX1 = 0.070f;  // play / pause (left)
constexpr float kElapsedX = 0.085f;                // elapsed-time text anchor
constexpr float kBarX0 = 0.150f, kBarX1 = 0.760f;  // scrub track
constexpr float kTotalX = 0.775f;                  // total-time text anchor
constexpr float kLoadX0 = 0.910f, kLoadX1 = 0.975f;  // load / open file (right)

enum class Region { None, Button, Bar, Load };

inline Region
hit(float nx, float ny)
{
	if (ny < kRowY0 || ny > kRowY1) return Region::None;
	if (nx >= kBtnX0 && nx <= kBtnX1) return Region::Button;
	if (nx >= kLoadX0 && nx <= kLoadX1) return Region::Load;
	if (nx >= kBarX0 - 0.03f && nx <= kBarX1 + 0.03f) return Region::Bar;  // generous grab
	return Region::None;
}

// Normalized x → playback fraction [0,1] along the bar.
inline float
barFraction(float nx)
{
	float f = (nx - kBarX0) / (kBarX1 - kBarX0);
	if (f < 0.0f) f = 0.0f;
	if (f > 1.0f) f = 1.0f;
	return f;
}

}  // namespace tui
