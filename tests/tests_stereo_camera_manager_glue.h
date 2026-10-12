// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The C fixture tests_stereo_camera_manager drives the real camera
 *         manager through (see tests_stereo_camera_manager_glue.c).
 */

#pragma once

#include "xrt/xrt_stereo_camera.h"
#include "util/u_stereo_uvc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct scm_fixture;

//! A manager whose only source is the UVC source on @p cfg (use a "fake" config).
struct scm_fixture *
scm_create(const struct u_stereo_uvc_config *cfg);

void
scm_destroy(struct scm_fixture *f);

uint32_t
scm_count(struct scm_fixture *f);

xrt_result_t
scm_properties(struct scm_fixture *f, uint32_t index, struct xrt_stereo_camera_properties *out);

xrt_result_t
scm_calibration(struct scm_fixture *f, uint64_t camera_id, uint32_t output, struct xrt_stereo_camera_calibration *out);

xrt_result_t
scm_stream_create(struct scm_fixture *f, const struct xrt_stereo_camera_stream_request *req, uint64_t *out_id);

xrt_result_t
scm_stream_start(struct scm_fixture *f, uint64_t id);

//! Map the started stream's ring read-only (as a consumer does).
xrt_result_t
scm_stream_map(struct scm_fixture *f, uint64_t id, struct xrt_stereo_camera_stream_layout *out_layout);

//! @p out_slot points at the acquired frame's slot inside the mapped ring.
xrt_result_t
scm_acquire(struct scm_fixture *f,
            uint64_t id,
            bool *out_ready,
            struct xrt_stereo_camera_frame_info *out_frame,
            const uint8_t **out_slot);

xrt_result_t
scm_stats(struct scm_fixture *f, uint64_t id, struct xrt_stereo_camera_stream_stats *out);

xrt_result_t
scm_stream_destroy(struct scm_fixture *f, uint64_t id);

/*
 * #1961 re-enumeration: a manager over a FAKE display plug-in whose camera
 * list the test edits (no UVC camera, no background rescan).
 */

//! The fake plug-in now lists these cameras, in this order (its indices).
void
scm_fake_plugin_set(const char *const *identities, const char *const *names, uint32_t count);

uint32_t
scm_fake_plugin_enumerate_calls(void);

struct scm_fixture *
scm_create_fake_plugin(void);

//! Forced re-enumeration; true if the camera set changed.
bool
scm_rescan(struct scm_fixture *f);

//! Drain one queued event of the fixture's connection.
bool
scm_poll_event(struct scm_fixture *f, struct xrt_stereo_camera_event *out);

xrt_result_t
scm_set_sharing(struct scm_fixture *f, bool enabled);

#ifdef __cplusplus
}
#endif
