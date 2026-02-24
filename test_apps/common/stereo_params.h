// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stereo camera parameters struct
 *
 * UI-facing struct for IPD, parallax, perspective, and scale factors.
 * The math that applies these factors lives in display3d_view.h.
 */

#pragma once

struct StereoParams {
    float ipdFactor = 1.0f;         // [0, 1] — 0=mono, 1=normal IPD
    float parallaxFactor = 1.0f;    // [0, 1] — 0=no head tracking, 1=full
    float perspectiveFactor = 1.0f; // [0.1, 10] — scales eye in Kooima only
    float scaleFactor = 1.0f;       // [0.1, 10] — zoom (was zoomScale)
};
