// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Runtime-side picture policy for a LEGACY session (no XR_DXR_display_info):
 * automatic 2D while both eyes show one flat picture, and automatic
 * re-convergence by a horizontal image shift before the display processor.
 * Both default OFF; the profile keys `auto2d`, `autoConvergence`,
 * `maxShiftFraction`, `maxBehindFraction`, `maxFrontFraction` and the env knobs
 * `DXR_LEGACY_AUTO_2D` / `DXR_LEGACY_AUTO_CONVERGENCE` turn them on
 * (docs/specs/runtime/legacy-camera-profile.md). Fractions are of the EYE
 * image width; disparity is positive behind the screen.
 */
struct u_legacy_stereo_auto
{
	bool auto_2d;
	bool auto_convergence;
	//! The applied shift (sum over both eyes) never exceeds this, either sign.
	float max_shift_fraction;
	//! Comfort: the far content (p95) is kept at most this far behind the screen.
	float max_behind_fraction;
	//! Comfort: the near content (p5) is kept at most this far in front of it.
	float max_front_fraction;
};

#define U_LEGACY_STEREO_AUTO_DEFAULT {false, false, 0.03f, 0.03f, 0.02f}

//! Camera-rig tuning, plus where the camera starts. Navigation itself (the
//! voluntary pose the user drives) is never configuration data; its starting
//! framing is: many VR titles put their work area at arm's length and hand
//! height, below and too close to a camera that starts level at eye height
//! (pitchDegrees, pullbackMeters).
struct u_camera_profile
{
	float ipd_factor;
	float parallax_factor;
	float inv_convergence_distance;
	float half_tan_vfov;
	float m2v;
	//! Initial camera pitch, radians, positive = up; also what Space resets to.
	float pitch_rad;
	//! Initial distance behind the default camera position along its starting view, metres.
	float pullback_m;
	//! Not camera tuning: the legacy picture policy read from the same record.
	struct u_legacy_stereo_auto stereo;
};

//! The existing qwerty camera defaults (tan(18 degrees) is historically rounded).
#define U_CAMERA_PROFILE_DEFAULT {1.0f, 1.0f, 0.5f, 0.3249f, 1.0f, 0.0f, 0.0f, U_LEGACY_STEREO_AUTO_DEFAULT}

enum u_camera_profile_result
{
	U_CAMERA_PROFILE_INVALID = -1,
	U_CAMERA_PROFILE_NONE = 0,
	U_CAMERA_PROFILE_LOADED = 1,
};

//! Parse a complete JSON object. Missing fields use the qwerty defaults.
//! Scalar bounds match XrCameraRigDXR; geometry-dependent comfort is applied by
//! the eligible instance. Only horizontalFovDeg needs a positive, finite canvas aspect.
//! Duplicate keys, conflicting aliases, malformed JSON and non-finite/non-number
//! values of known keys are errors: out is untouched and the optional error
//! buffer receives a reason. Unknown keys are skipped whatever their value
//! (#2450): on success the error buffer is empty, or names the skipped keys so
//! the caller can warn once.
bool
u_camera_profile_parse(
    const char *json, float canvas_aspect, struct u_camera_profile *out, char *error, size_t error_size);

//! Read override (inline JSON object or UTF-8 file path), otherwise
//! <profile_dir>/<exe_basename>.json. An invalid override does not fall through
//! to the per-title file. Missing per-title files return NONE; other failures
//! return INVALID. Reads at most 64 KiB and never creates files/directories.
//! On LOADED the error buffer holds the parse notice (skipped keys) or is empty.
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

//! Parse one on/off env spelling: "1"/"on"/"true"/"yes" -> 1, "0"/"off"/"false"/"no" -> 0, else -1.
int
u_legacy_stereo_auto_parse_flag(const char *value);

/*!
 * The legacy picture policy for this process: the profile record's keys
 * (same file / DXR_LEGACY_CAMERA_RIG override as the camera tuning, so an
 * invalid record disables it as a whole), then DXR_LEGACY_AUTO_2D and
 * DXR_LEGACY_AUTO_CONVERGENCE, which win over the record when set.
 * @p out_source names where the switches came from ("profile", "env",
 * "profile+env" or "default"). Returns true when either feature is on.
 */
bool
u_legacy_stereo_auto_load_for_process(struct u_legacy_stereo_auto *out, const char **out_source);

#ifdef __cplusplus
}
#endif
