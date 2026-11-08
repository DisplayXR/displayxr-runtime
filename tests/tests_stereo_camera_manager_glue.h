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
#include "xrt/xrt_share_link.h"

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

//! Spec v5: the screen (monitor_id) each listed camera reports; reset to 0 by scm_fake_plugin_set.
void
scm_fake_plugin_set_monitors(const uint64_t *monitor_ids, uint32_t count);

//! Spec v7: the xrt_plugin_stereo_camera_frame_orientation each listed camera
//! writes (0 = the field is not written, like an older plug-in); reset by
//! scm_fake_plugin_set.
void
scm_fake_plugin_set_orientations(const uint32_t *plugin_values, uint32_t count);

uint32_t
scm_fake_plugin_enumerate_calls(void);

struct scm_fixture *
scm_create_fake_plugin(void);

//! Same, with the developer override forced: > 0 on, < 0 off, 0 = from the env.
struct scm_fixture *
scm_create_fake_plugin_ex(int32_t dev_override);

/*
 * A2 front-camera rule (option R): the platform flag that makes a
 * USER_FACING camera (the fake plug-in's cameras are) need a live weave, and
 * a second connection of the fixture's app that is / is not weaving.
 * scm_set_front_rule(false) clears the platform hooks again.
 */
void
scm_set_front_rule(bool enabled);

void
scm_set_live_weave(struct scm_fixture *f, bool live);

/*
 * The second connection of the fixture's app is (or is not) a weave
 * present-owner that has not submitted a weave yet, and the platform says
 * the app is (or is not) on top. Call after scm_set_front_rule(true).
 */
void
scm_set_present_owner(struct scm_fixture *f, bool present_owner, bool foreground);

/*
 * ADR-043 Amendment 5: a fake display plug-in with NO camera slots that only
 * DECLARES the cameras its display owns (get_claimed_cameras), next to the
 * UVC source on @p cfg (a "fake" config; the synthetic device's id is
 * "dxr-fake-uvc"). Background rescan off.
 */
struct scm_fixture *
scm_create_claiming_plugin(const struct u_stereo_uvc_config *cfg);

//! The ids the claiming plug-in declares from now on.
void
scm_fake_claims_set(const char *const *ids, uint32_t count);

//! Forced re-enumeration; true if the camera set changed.
bool
scm_rescan(struct scm_fixture *f);

//! Drain one queued event of the fixture's connection.
bool
scm_poll_event(struct scm_fixture *f, struct xrt_stereo_camera_event *out);

xrt_result_t
scm_set_sharing(struct scm_fixture *f, bool enabled);

/*
 * ADR-061 app share: a manager over the fake plug-in (its camera list from
 * scm_fake_plugin_set) with app entries, fake app connections in the server's
 * connection table, a scripted share prompt and an optional fake share tap.
 */
struct scm_app_options
{
	int32_t dev;         //!< DXR_APP_SHARE_DEV: > 0 on, < 0 off
	int32_t fake;        //!< DXR_APP_SHARE_FAKE: > 0 on, < 0 off
	int32_t linger_ms;   //!< 0 = 10 s
	int32_t prompt_mode; //!< 1 = always, 2 = per-app-run
	bool consumer_sees_apps; //!< the consumer enabled XR_DXR_app_share
	int32_t fake_link;       //!< DXR_APP_SHARE_FAKE=link: > 0 on, < 0 off (0 = off here)
};

struct scm_fixture *
scm_create_apps(const struct scm_app_options *o);

//! The consumer connection's internal XR_DXR_app_share flag.
void
scm_set_consumer_sees_apps(struct scm_fixture *f, bool on);

//! A connection with a session + compositor in server slot @p slot (1..).
void
scm_app_connect(struct scm_fixture *f, uint32_t slot, uint32_t client_id, long pid, const char *name, uint32_t cls,
                bool opted_out);

//! The connection in @p slot loses its session (and goes away).
void
scm_app_disconnect(struct scm_fixture *f, uint32_t slot);

//! What the service does at session teardown, before the compositor goes.
void
scm_app_session_ending(struct scm_fixture *f, uint32_t slot);

bool
scm_rescan_apps(struct scm_fixture *f);

xrt_result_t
scm_app_source(struct scm_fixture *f, uint64_t camera_id, struct xrt_app_share_source_info *out);

//! The app metadata of the last scm_acquire'd frame.
struct xrt_app_share_frame_info
scm_last_app(struct scm_fixture *f);

//! The scripted share prompt's answer; returns how often it was shown so far.
void
scm_set_share_answer(int answer);
int
scm_share_prompts(void);

//! A fake share tap (platform hooks): AVAILABLE, frames from the synthetic pattern. false clears.
void
scm_set_tap(bool enabled);
int
scm_tap_attached(void); //!< currently attached taps

//! Release-hook calls of the fake tap. The hook takes the manager lock itself
//! (through a status read), so a call made WITH the lock held deadlocks the test.
int
scm_tap_releases(void);

//! What the service calls when a session is created / begun / ended.
void
scm_apps_changed(struct scm_fixture *f);

/*
 * ADR-061 arm 2: in-process service links, driven through their IPC handlers
 * (service_link_*) on a sessionless connection in server slot @p slot.
 */

//! A sessionless connection (an in-process app's service link) in slot @p slot.
void
scm_link_connect(struct scm_fixture *f, uint32_t slot, uint32_t client_id, long pid, uint32_t cls);

xrt_result_t
scm_link_register(struct scm_fixture *f, uint32_t slot, const struct xrt_share_link_registration *reg);

xrt_result_t
scm_link_poll(struct scm_fixture *f, uint32_t slot, struct xrt_share_link_poll *out);

//! A ring with one stand-in texture handle per slot (the fake importer closes them).
xrt_result_t
scm_link_publish_ring(struct scm_fixture *f, uint32_t slot, const struct xrt_share_link_ring_desc *desc);

//! Same, with exactly @p texture_count handles (to exercise the n == slot_count rule).
xrt_result_t
scm_link_publish_ring_n(struct scm_fixture *f,
                        uint32_t slot,
                        const struct xrt_share_link_ring_desc *desc,
                        uint32_t texture_count);

//! A stand-in fence handle for ring @p generation.
xrt_result_t
scm_link_publish_fence(struct scm_fixture *f, uint32_t slot, uint32_t generation);

//! Register in @p slot as connection @p client_id (table-capacity tests).
void
scm_link_register_as(struct scm_fixture *f,
                     uint32_t slot,
                     uint32_t client_id,
                     const struct xrt_share_link_registration *reg,
                     xrt_result_t *out);

//! The fake importer refuses every ring from now on (it still closes the handles).
void
scm_set_link_import_fails(bool fails);

uint32_t
scm_link_last_import_texture_count(void);
bool
scm_link_last_import_had_fence(void);

//! Install an incomplete share_link_* table (import + convert, no unimport).
void
scm_set_link_partial_hooks(void);

xrt_result_t
scm_link_frame_ready(struct scm_fixture *f,
                     uint32_t slot,
                     const struct xrt_share_link_frame *frame,
                     struct xrt_share_link_poll *out);

//! The link's connection closes (what the service does at client teardown).
void
scm_link_disconnect(struct scm_fixture *f, uint32_t slot);

/*!
 * A fake ring importer (the share_link_* platform hooks): frames come from the
 * synthetic pattern at the stream extent, metadata from the frame_ready.
 * @p service_luid: the adapter the service "publishes" (NULL = unknown).
 * false clears the platform hooks.
 */
void
scm_set_link_importer(bool enabled, const uint8_t *service_luid);

int
scm_link_imports(void); //!< successful imports so far
int
scm_link_live_imports(void); //!< imports not yet unimported
uint32_t
scm_link_last_import_generation(void);
uint32_t
scm_link_last_import_slot_width(void);

uint32_t
scm_stop_all(void);

bool
scm_status(uint32_t *out_share_streams, char *out_shares, size_t cap);

#ifdef __cplusplus
}
#endif
