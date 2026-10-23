// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The webcam eye tracker as a host-agnostic SOURCE (#1855 moved into
 *         sim_tracking, #2144 stack part 3): settings -> config, estimator
 *         choice, capture backend, the worker — everything both hosts share.
 *
 * Two hosts compile this, and nothing else of theirs is in here:
 *  - sim_tracking, `SOURCE=webcam`: the viewer-tracking provider. Settings
 *    from its host iface (`displayxr-cli tracking set sim-tracking ...`).
 *  - sim_display, `WEBCAM_TRACKING=1`: the one-release deprecation alias.
 *    Settings from sim_display's shim (`dp set sim-display ...`, plus the
 *    legacy `SIM_DISPLAY_*` environment names).
 * One copy of the code; the @ref sim_webcam_settings callback is the seam.
 *
 * Settings (same key names in both hosts):
 *  - `WEBCAM_ESTIMATOR` — `auto` (default: the estimator built into this
 *    binary, see sim_face_estimator.h) | `none` (never open the camera) | the
 *    built-in estimator's name (`yunet-onnx`). Which estimator is BUILT IN is
 *    a build-time choice: `-DXRT_SIM_WEBCAM_ESTIMATOR=onnx` (fetches ONNX
 *    Runtime + YuNet, pinned by SHA256); the default build carries the stub
 *    and so never tracks.
 *  - `WEBCAM_MODEL` — model path override (onnx builds).
 *  - `WEBCAM_DEVICE` — camera index ("0", "1", ...) or a case-insensitive
 *    substring of its name; unset = the first camera.
 *  - `WEBCAM_HFOV_DEG`, `WEBCAM_IPD_MM`, `WEBCAM_OFFSET_MM` ("x,y,z" from
 *    the panel's top-centre), `WEBCAM_MIRROR` — the camera model
 *    (sim_webcam_tracking.h).
 *
 * macOS: there is no capture backend on macOS yet (aux_os_uvc builds its
 * stubs there), so the source reports NO_SOURCE. When an AVFoundation backend
 * lands, camera access is gated by the HOSTING process's
 * `NSCameraUsageDescription`: in-process (both hosts today) that is the app's
 * own permission; a standalone provider process (N8 hosting) will need its own
 * usage string and TCC grant (follow-up).
 *
 * @ingroup drv_sim_tracking
 */

#pragma once

#include "sim_webcam_settings.h"
#include "sim_webcam_tracker.h"
#include "sim_webcam_tracking.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_stereo_uvc_backend;
struct sim_face_estimator;
struct sim_webcam_source;

//! Read the camera-model knobs (`WEBCAM_HFOV_DEG`, `_IPD_MM`, `_OFFSET_MM`, `_MIRROR`) over the defaults.
void
sim_webcam_config_from_settings(const struct sim_webcam_settings *s, struct sim_webcam_config *cfg);

/*!
 * Would the configured estimator run in this binary? `WEBCAM_ESTIMATOR`
 * unset / `auto` / the built-in name -> the built-in one's availability;
 * `none` -> false; anything else -> false with one WARN. Cheap (no load).
 */
bool
sim_webcam_estimator_usable(const struct sim_webcam_settings *s);

struct sim_webcam_source_params
{
	const struct sim_webcam_settings *settings; //!< required; read during create only
	float screen_h_m;                           //!< the panel's active-area height (the camera's mount)
	//! NULL = this OS's capture backend (Media Foundation / V4L2). Copied.
	const struct u_stereo_uvc_backend *backend;
	//! NULL = the configured built-in estimator (owned by the source). Non-NULL = BORROWED (tests).
	struct sim_face_estimator *estimator;
	const char *log_name; //!< who is tracking, for the WARNs ("sim-tracking", "sim_display")
	//! Optional camera -> display mapping replacing the bezel-offset model (a calibration). NULL = none.
	sim_webcam_map_fn map_fn;
	void *map_ctx; //!< BORROWED; must outlive the source
};

/*!
 * Start tracking: resolve the estimator and backend, start the worker. Never
 * blocks on the camera (it opens on the worker). Returns NULL — with one WARN
 * saying why — when this build / platform / configuration cannot track.
 */
struct sim_webcam_source *
sim_webcam_source_create(const struct sim_webcam_source_params *p);

//! Stop the worker (closes the camera), free the owned estimator.
void
sim_webcam_source_destroy(struct sim_webcam_source **src_ptr);

//! The running worker (sample it with sim_webcam_tracker_sample / _camera / _state).
struct sim_webcam_tracker *
sim_webcam_source_tracker(struct sim_webcam_source *src);

//! What @ref sim_webcam_selected_camera found.
struct sim_webcam_camera_info
{
	char id[256];   //!< the OS device id (what ADR-043 Amendment 5 claims)
	char key[128];  //!< @ref sim_webcam_camera_key
	char name[128]; //!< friendly name
};

/*!
 * The camera `WEBCAM_DEVICE` selects (what ADR-043 Amendment 5 claims and the
 * stereo-camera manager refuses): enumerates, opens nothing. @p backend NULL
 * = this OS's. @return 1 with @p out filled (may be NULL), or 0.
 */
uint32_t
sim_webcam_selected_camera(const struct sim_webcam_settings *s,
                           const struct u_stereo_uvc_backend *backend,
                           struct sim_webcam_camera_info *out);

#ifdef __cplusplus
}
#endif
