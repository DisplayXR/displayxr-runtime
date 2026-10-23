// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-owned camera calibration store: where a camera sits
 *         relative to a screen, keyed by (camera key, screen key), per user
 *         with an optional machine-wide layer — plus the per-user viewer iris
 *         scale, and the pure geometry / precedence the host answer uses.
 *
 * A camera's pose relative to a screen belongs to the camera and the screen,
 * not to a provider: one record serves every provider (viewer tracking, hand
 * tracking) that reads that camera. Contract:
 * `docs/specs/runtime/tracking-provider-discovery.md` §6.
 *
 * Storage:
 *  - Windows: `HKCU\Software\DisplayXR\Cameras\Calibration\<camera>@<screen>`
 *    (user-calibration) and `...\Cameras\Placement\<camera>@<screen>`
 *    (placement-hint), REG_SZ values; the machine layer is the same path under
 *    HKLM (writes need an elevated process). `\` in a key is written as `#`
 *    in the subkey NAME; the real keys are the `Camera` / `Screen` values,
 *    which is what lookups match. Iris scale: `HKCU\Software\DisplayXR\Viewer`
 *    value `IrisScale`.
 *  - POSIX: `camera-calibration.json` in the user config dir
 *    (`$XDG_CONFIG_HOME/displayxr`, else `~/.config/monado`, like every other
 *    runtime file), machine layer `/etc/displayxr/camera-calibration.json`
 *    (macOS `/Library/Application Support/DisplayXR/camera-calibration.json`);
 *    iris scale `viewer.json` beside the user file. Writes create missing
 *    directories.
 *
 * The user layer wins over the machine layer for the same (camera, screen,
 * kind). Thread-safe in the sense every call is self-contained (read, modify,
 * write); concurrent writers race last-wins, like every settings store here.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_camera_pose.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Size of a camera / screen key, NUL included.
#define U_CAMERA_CALIB_KEY_MAX 128

//! Most records a list returns (both layers together).
#define U_CAMERA_CALIB_MAX_RECORDS 64

/*!
 * One record: a stored calibration / placement hint, or a resolved answer.
 * `source` is an XRT_CAMERA_POSE_SOURCE_* value.
 */
struct u_camera_calib_record
{
	char camera_key[U_CAMERA_CALIB_KEY_MAX];
	char screen_key[U_CAMERA_CALIB_KEY_MAX];
	uint32_t source;
	double t_m[3];          //!< camera position, display space, metres
	double q[4];            //!< x, y, z, w; camera -> display
	double focal_scale;     //!< 1 = none
	double rms_mm;          //!< user-calibration only
	uint32_t sample_count;  //!< user-calibration only
	int64_t timestamp_unix; //!< UTC seconds the record was written; 0 = never (the prior)
	char solver_mode[8];    //!< user-calibration: "a" / "b" / "c"
	uint32_t edge;          //!< placement-hint: XRT_CAMERA_PLACEMENT_EDGE_*
	double offset_mm;       //!< placement-hint: along the edge
	double margin_mm;       //!< placement-hint: extra margin beyond XRT_CAMERA_POSE_BEZEL_MARGIN_M
	bool machine;           //!< read from the machine layer
};

//! Write outcome.
enum u_camera_calib_store_result
{
	U_CAMERA_CALIB_STORE_OK = 0,
	U_CAMERA_CALIB_STORE_DENIED = 1,      //!< no permission (the machine layer, unelevated)
	U_CAMERA_CALIB_STORE_FAILED = 2,      //!< I/O error
	U_CAMERA_CALIB_STORE_UNSUPPORTED = 3, //!< no store path (no HOME), or a bad record
};


/*
 *
 * Names.
 *
 */

//! "default-prior" / "placement-hint" / "user-calibration" / "display-declared" / "none".
const char *
u_camera_calib_source_str(uint32_t source);

//! Inverse of @ref u_camera_calib_source_str. False (out untouched) on an unknown word.
bool
u_camera_calib_source_parse(const char *s, uint32_t *out);

//! "top" / "bottom" / "left" / "right" (anything else: "top").
const char *
u_camera_calib_edge_str(uint32_t edge);

//! Inverse of @ref u_camera_calib_edge_str. False on an unknown word.
bool
u_camera_calib_edge_parse(const char *s, uint32_t *out);


/*
 *
 * Pure geometry + precedence.
 *
 */

/*!
 * The default prior: R = I, camera on the top centre of the bezel,
 * T = (0, H/2 + XRT_CAMERA_POSE_BEZEL_MARGIN_M, 0). False (outputs untouched)
 * when @p panel_h_m is not positive.
 */
bool
u_camera_calib_default_prior(double panel_h_m, double t[3], double q[4]);

/*!
 * A placement hint as a pose, R = I (an edge-mounted webcam stands upright):
 *  - top:    T = ( offset, +(H/2 + m), 0)
 *  - bottom: T = ( offset, -(H/2 + m), 0)
 *  - left:   T = (-(W/2 + m), offset, 0)
 *  - right:  T = (+(W/2 + m), offset, 0)
 * with m = XRT_CAMERA_POSE_BEZEL_MARGIN_M + margin_mm and the offset along the
 * edge (+X right on top / bottom, +Y up on left / right). False when the
 * panel dimension the edge needs is not positive or the edge is unknown.
 */
bool
u_camera_calib_placement_pose(
    uint32_t edge, double offset_mm, double margin_mm, double panel_w_m, double panel_h_m, double t[3], double q[4]);

/*!
 * The precedence: @p declared (display-declared) > @p calibration
 * (user-calibration) > @p hint (placement-hint, turned into a pose from the
 * panel) > the default prior. Any input may be NULL. @p out gets the winning
 * record with `source` set and a pose filled (keys copied from the winner, or
 * from @p camera_key / @p screen_key for the prior).
 *
 * @return false only when nothing stored applies AND the prior cannot be
 * computed (panel height unknown); @p out is then zeroed.
 */
bool
u_camera_calib_resolve(const struct u_camera_calib_record *declared,
                       const struct u_camera_calib_record *calibration,
                       const struct u_camera_calib_record *hint,
                       const char *camera_key,
                       const char *screen_key,
                       double panel_w_m,
                       double panel_h_m,
                       struct u_camera_calib_record *out);

//! A 64-bit hash of an answer (+ the iris scale) — what `xrt_camera_pose::generation` carries.
uint64_t
u_camera_calib_answer_generation(const struct u_camera_calib_record *answer, double viewer_iris_scale);


/*
 *
 * The store.
 *
 */

/*!
 * Where the records live, for display ("HKCU\\Software\\DisplayXR\\Cameras"
 * on Windows, the JSON file paths on POSIX). Either out may be NULL.
 */
void
u_camera_calib_store_paths(char *user, size_t user_cap, char *machine, size_t machine_cap);

/*!
 * Every stored record, user layer first, then machine; @p out[i].machine says
 * which. Returns the count (<= @p max).
 */
int
u_camera_calib_store_list(struct u_camera_calib_record *out, int max);

/*!
 * The stored record of kind @p source (USER_CALIBRATION or PLACEMENT_HINT) for
 * @p camera_key on @p screen_key, user layer over machine. @p screen_key NULL
 * or "" matches any screen; the newest (timestamp) wins. False when absent.
 */
bool
u_camera_calib_store_find(const char *camera_key,
                          const char *screen_key,
                          uint32_t source,
                          struct u_camera_calib_record *out);

/*!
 * Write @p rec (its camera, screen and source = USER_CALIBRATION or
 * PLACEMENT_HINT) into the user or the machine layer, replacing the record
 * of the same (camera, screen, source). A zero `timestamp_unix` is stamped
 * with now. Creates missing directories.
 */
enum u_camera_calib_store_result
u_camera_calib_store_put(const struct u_camera_calib_record *rec, bool machine);

/*!
 * Remove the records of one layer matching @p camera_key (NULL = any),
 * @p screen_key (NULL = any) and @p source (0 = any). Returns the count
 * removed, or -1 on a write failure (@p out_result says why; may be NULL).
 */
int
u_camera_calib_store_remove(const char *camera_key,
                            const char *screen_key,
                            uint32_t source,
                            bool machine,
                            enum u_camera_calib_store_result *out_result);

//! The per-user viewer iris scale; false (out = 1) when none is stored.
bool
u_camera_calib_viewer_iris_scale(double *out);

//! Store the per-user viewer iris scale; a value <= 0 clears it.
enum u_camera_calib_store_result
u_camera_calib_set_viewer_iris_scale(double value);

/*!
 * TEST HOOK: use JSON files at these paths on EVERY platform (the Windows
 * registry backend is bypassed), so a unit test is isolated from the user's
 * real store. NULL for all three restores the real store. Not thread-safe;
 * call before anything reads the store.
 */
void
u_camera_calib_store_set_test_paths(const char *user_file, const char *machine_file, const char *viewer_file);

#ifdef __cplusplus
}
#endif
