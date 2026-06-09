// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  View parameters struct for 3D display rendering
 *
 * UI-facing struct for IPD, parallax, perspective, and scale factors.
 * The math that applies these factors lives in display3d_view.h.
 */

#pragma once

struct ViewParams {
    float ipdFactor = 1.0f;             // [0, 1] — 0=mono, 1=normal IPD
    float parallaxFactor = 1.0f;        // [0, 1] — 0=no head tracking, 1=full

    // Display-centric mode
    float perspectiveFactor = 1.0f;     // [0.1, 10] — scales eye in Kooima only
    float scaleFactor = 1.0f;           // [0.1, 10] — zoom via vHeight/scale
    float virtualDisplayHeight = 0.0f;  // virtual display height in app units (0 = disabled, 1:1 meters)

    // Camera-centric mode
    float invConvergenceDistance = 0.5f; // [0.1, 10] — 1/convergence_dist (default 0.5 = 2m)
    float zoomFactor = 1.0f;            // [0.1, 10] — divides half_tan_vfov
};
