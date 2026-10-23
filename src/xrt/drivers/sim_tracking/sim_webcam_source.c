// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The webcam eye tracker as a host-agnostic source. See the header.
 * @ingroup drv_sim_tracking
 */

#include "sim_webcam_source.h"
#include "sim_face_estimator.h"

#include "util/u_logging.h"
#include "util/u_stereo_uvc.h"
#include "os/os_uvc_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sim_webcam_source
{
	struct sim_webcam_tracker *tracker;
	struct sim_face_estimator *est;
	bool owns_est;
};

void
sim_webcam_config_from_settings(const struct sim_webcam_settings *s, struct sim_webcam_config *cfg)
{
	sim_webcam_config_defaults(cfg);
	char e[64];
	if (sim_webcam_setting(s, "WEBCAM_HFOV_DEG", e, sizeof(e))) {
		const float v = (float)atof(e);
		if (v > 10.0f && v < 170.0f) {
			cfg->hfov_deg = v;
		}
	}
	if (sim_webcam_setting(s, "WEBCAM_IPD_MM", e, sizeof(e))) {
		const float v = (float)atof(e);
		if (v > 40.0f && v < 90.0f) {
			cfg->ipd_m = v / 1000.0f;
		}
	}
	if (sim_webcam_setting(s, "WEBCAM_OFFSET_MM", e, sizeof(e)) &&
	    !sim_webcam_parse_offset_mm(e, cfg->cam_offset_m)) {
		U_LOG_W("webcam tracking: WEBCAM_OFFSET_MM='%s' is not \"x,y,z\" - ignored", e);
	}
	cfg->mirrored = sim_webcam_setting_on(s, "WEBCAM_MIRROR");
}

bool
sim_webcam_estimator_usable(const struct sim_webcam_settings *s)
{
	char v[64];
	if (sim_webcam_setting(s, "WEBCAM_ESTIMATOR", v, sizeof(v)) && strcmp(v, "auto") != 0) {
		if (strcmp(v, "none") == 0) {
			return false;
		}
		if (strcmp(v, sim_face_estimator_default_name()) != 0) {
			static bool warned = false; // one WARN per process; a racing repeat is harmless
			if (!warned) {
				warned = true;
				U_LOG_W(
				    "webcam tracking (#1855): WEBCAM_ESTIMATOR='%s' is not built into this binary "
				    "(built in: '%s'; build with -DXRT_SIM_WEBCAM_ESTIMATOR=onnx for yunet-onnx) - not "
				    "tracking",
				    v, sim_face_estimator_default_name());
			}
			return false;
		}
	}
	return sim_face_estimator_default_available(s);
}

struct sim_webcam_source *
sim_webcam_source_create(const struct sim_webcam_source_params *p)
{
	if (p == NULL || p->settings == NULL) {
		return NULL;
	}
	const char *who = p->log_name != NULL ? p->log_name : "webcam";

	struct sim_face_estimator *est = p->estimator;
	const bool owns_est = est == NULL;
	if (owns_est) {
		if (!sim_webcam_estimator_usable(p->settings)) {
			U_LOG_W(
			    "%s webcam tracking (#1855): no usable face estimator in this build (built in: '%s') - "
			    "camera NOT opened, not tracking",
			    who, sim_face_estimator_default_name());
			return NULL;
		}
	}

	struct u_stereo_uvc_backend backend;
	if (p->backend != NULL) {
		backend = *p->backend;
	} else if (!os_uvc_capture_backend(&backend)) {
		U_LOG_W("%s webcam tracking (#1855): no camera capture backend on this platform - not tracking", who);
		return NULL;
	}

	if (owns_est) {
		est = sim_face_estimator_create_default(p->settings);
		if (est == NULL) {
			U_LOG_W("%s webcam tracking (#1855): the face estimator failed to load - not tracking", who);
			return NULL;
		}
	}

	struct sim_webcam_config cfg;
	sim_webcam_config_from_settings(p->settings, &cfg);
	char device[256];
	const char *device_sel =
	    sim_webcam_setting(p->settings, "WEBCAM_DEVICE", device, sizeof(device)) ? device : NULL;

	struct sim_webcam_source *src = (struct sim_webcam_source *)calloc(1, sizeof(*src));
	if (src == NULL) {
		if (owns_est) {
			sim_face_estimator_destroy(&est);
		}
		return NULL;
	}
	src->tracker =
	    sim_webcam_tracker_create_mapped(&cfg, &backend, device_sel, est, p->screen_h_m, p->map_fn, p->map_ctx);
	if (src->tracker == NULL) {
		if (owns_est) {
			sim_face_estimator_destroy(&est);
		}
		free(src);
		return NULL;
	}
	src->est = est;
	src->owns_est = owns_est;
	return src;
}

void
sim_webcam_source_destroy(struct sim_webcam_source **src_ptr)
{
	if (src_ptr == NULL || *src_ptr == NULL) {
		return;
	}
	struct sim_webcam_source *src = *src_ptr;
	sim_webcam_tracker_destroy(&src->tracker); // joins: closes the camera
	if (src->owns_est) {
		sim_face_estimator_destroy(&src->est);
	}
	free(src);
	*src_ptr = NULL;
}

struct sim_webcam_tracker *
sim_webcam_source_tracker(struct sim_webcam_source *src)
{
	return src != NULL ? src->tracker : NULL;
}

uint32_t
sim_webcam_selected_camera(const struct sim_webcam_settings *s,
                           const struct u_stereo_uvc_backend *backend,
                           struct sim_webcam_camera_info *out)
{
	struct u_stereo_uvc_backend os_backend;
	if (backend == NULL) {
		if (!os_uvc_capture_backend(&os_backend)) {
			return 0;
		}
		backend = &os_backend;
	}
	if (backend->enumerate == NULL) {
		return 0;
	}
	struct u_stereo_uvc_device *devs =
	    (struct u_stereo_uvc_device *)calloc(U_STEREO_UVC_MAX_DEVICES, sizeof(*devs));
	if (devs == NULL) {
		return 0;
	}
	uint32_t n = backend->enumerate(backend->ctx, devs, U_STEREO_UVC_MAX_DEVICES); // lists, opens nothing
	n = n > U_STEREO_UVC_MAX_DEVICES ? U_STEREO_UVC_MAX_DEVICES : n;
	char device[256];
	const char *sel = sim_webcam_setting(s, "WEBCAM_DEVICE", device, sizeof(device)) ? device : NULL;
	const int32_t idx = sim_webcam_select_device(devs, n, sel);
	uint32_t count = 0;
	if (idx >= 0 && devs[idx].id[0] != '\0') {
		if (out != NULL) {
			snprintf(out->id, sizeof(out->id), "%s", devs[idx].id);
			sim_webcam_camera_key(&devs[idx], out->key, sizeof(out->key));
			snprintf(out->name, sizeof(out->name), "%s", devs[idx].name);
		}
		count = 1;
	}
	free(devs);
	return count;
}
