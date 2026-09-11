// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Camera-rig tuning only: navigation/pose is never configuration data.
struct u_camera_profile
{
	float ipd_factor;
	float parallax_factor;
	float inv_convergence_distance;
	float half_tan_vfov;
	float m2v;
};

//! The existing qwerty camera defaults (tan(18 degrees) is historically rounded).
#define U_CAMERA_PROFILE_DEFAULT {1.0f, 1.0f, 0.5f, 0.3249f, 1.0f}

enum u_camera_profile_result
{
	U_CAMERA_PROFILE_INVALID = -1,
	U_CAMERA_PROFILE_NONE = 0,
	U_CAMERA_PROFILE_LOADED = 1,
};

//! Parse a complete JSON object. Missing fields use the qwerty defaults.
//! Scalar bounds match XrCameraRigDXR; geometry-dependent comfort is applied by
//! the eligible instance. Only horizontalFovDeg needs a positive, finite canvas aspect.
//! Unknown/duplicate keys, conflicting aliases and non-finite/non-number values
//! are errors. On failure out is untouched. Optional error receives a reason.
bool
u_camera_profile_parse(
    const char *json, float canvas_aspect, struct u_camera_profile *out, char *error, size_t error_size);

//! Read override (inline JSON object or UTF-8 file path), otherwise
//! <profile_dir>/<exe_basename>.json. An invalid override does not fall through
//! to the per-title file. Missing per-title files return NONE; other failures
//! return INVALID. Reads at most 64 KiB and never creates files/directories.
enum u_camera_profile_result
u_camera_profile_load(const char *exe_basename,
                      const char *profile_dir,
                      const char *override,
                      float canvas_aspect,
                      struct u_camera_profile *out,
                      char *error,
                      size_t error_size);

//! Resolve the actual executable basename and the platform configuration root.
//! Windows: %ProgramData%/DisplayXR/app-profiles; POSIX: the existing config root
//! plus app-profiles. DXR_LEGACY_CAMERA_RIG is the optional developer override.
enum u_camera_profile_result
u_camera_profile_load_for_process(float canvas_aspect, struct u_camera_profile *out, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
