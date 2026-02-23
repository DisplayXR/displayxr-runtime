// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stereo camera parameters for Kooima asymmetric frustum projection
 *
 * Platform-independent header with inline math for IPD, parallax, perspective,
 * and scale factors. Depends only on <openxr/openxr.h> for XrVector3f.
 *
 * Mathematical pipeline (given raw eye positions from xrLocateViews):
 *
 *   Step 1 - IPD factor: scale inter-eye vector, keep center fixed
 *   Step 2 - Parallax factor: lerp center toward nominal viewer position
 *
 *   View matrix:   viewEye = processedEye / scaleFactor
 *   Kooima eye:    kooimaEye = processedEye * perspectiveFactor / scaleFactor
 *   Kooima screen: screenW,H = screenW,H / scaleFactor
 */

#pragma once

#include <openxr/openxr.h>

struct StereoParams {
    float ipdFactor = 1.0f;         // [0, 1] — 0=mono, 1=normal IPD
    float parallaxFactor = 1.0f;    // [0, 1] — 0=no head tracking, 1=full
    float perspectiveFactor = 1.0f; // [0.1, 10] — scales eye in Kooima only
    float scaleFactor = 1.0f;       // [0.1, 10] — zoom (was zoomScale)
};

// Apply IPD and parallax factors to raw eye positions.
// nominalX/Y/Z = nominal viewer position from XR_EXT_display_info.
// Outputs processed left/right eye positions (still in display space).
inline void ApplyEyeFactors(
    const XrVector3f& rawLeft, const XrVector3f& rawRight,
    float nominalX, float nominalY, float nominalZ,
    float ipdFactor, float parallaxFactor,
    XrVector3f& outLeft, XrVector3f& outRight)
{
    // Step 1: IPD factor — scale inter-eye vector, keep center fixed
    float cx = (rawLeft.x + rawRight.x) * 0.5f;
    float cy = (rawLeft.y + rawRight.y) * 0.5f;
    float cz = (rawLeft.z + rawRight.z) * 0.5f;

    float lvx = (rawLeft.x - cx) * ipdFactor;
    float lvy = (rawLeft.y - cy) * ipdFactor;
    float lvz = (rawLeft.z - cz) * ipdFactor;

    float rvx = (rawRight.x - cx) * ipdFactor;
    float rvy = (rawRight.y - cy) * ipdFactor;
    float rvz = (rawRight.z - cz) * ipdFactor;

    // Step 2: Parallax factor — lerp center toward nominal viewer position
    float cx2 = nominalX + parallaxFactor * (cx - nominalX);
    float cy2 = nominalY + parallaxFactor * (cy - nominalY);
    float cz2 = nominalZ + parallaxFactor * (cz - nominalZ);

    outLeft  = {cx2 + lvx, cy2 + lvy, cz2 + lvz};
    outRight = {cx2 + rvx, cy2 + rvy, cz2 + rvz};
}

// Compute Kooima eye position: apply perspectiveFactor and scaleFactor.
// Used for ComputeKooimaProjection() and ComputeKooimaFov().
inline XrVector3f KooimaEyePos(const XrVector3f& processedEye,
                                float perspectiveFactor, float scaleFactor)
{
    float s = perspectiveFactor / scaleFactor;
    return {processedEye.x * s, processedEye.y * s, processedEye.z * s};
}

// Compute Kooima screen dimensions: divide by scaleFactor.
inline void KooimaScreenDim(float screenW, float screenH, float scaleFactor,
                            float& outW, float& outH)
{
    outW = screenW / scaleFactor;
    outH = screenH / scaleFactor;
}
