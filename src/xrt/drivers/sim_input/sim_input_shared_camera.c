// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_input `SOURCE=shared-camera` (see the header).
 * @ingroup drv_sim_input
 */

#include "sim_input_shared_camera.h"
#include "sim_input_settings.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

//! Acquire timeout: short, so stop is prompt and the "no frames" clock is accurate.
#define SHCAM_ACQUIRE_TIMEOUT_MS 100
//! No frame for longer than this = not flowing (ABSENT).
#define SHCAM_STALE_NS (500ll * 1000 * 1000)
//! Retry period for an open that failed / a stream that ended.
#define SHCAM_RETRY_NS (1000ll * 1000 * 1000)

enum shcam_state
{
	SHCAM_NONE = 0, //!< nothing reported yet
	SHCAM_FLOWING,
	SHCAM_WAITING,
	SHCAM_UNAVAILABLE,
};

static struct
{
	bool initialized;
	struct os_thread_helper oth;
	struct os_mutex lock; //!< guards presence
	const struct xrt_input_plugin_host_iface *host;
	enum xrt_input_provider_presence presence;

	// Poll-thread only.
	enum shcam_state state;
	int32_t unavailable_code; //!< the xrt_result_t / state the last "unavailable" line named
	uint64_t frames;
} g_shcam;

static bool
host_has_slots(const struct xrt_input_plugin_host_iface *h)
{
	return h != NULL &&
	       h->struct_size >=
	           offsetof(struct xrt_input_plugin_host_iface, close_shared_camera) + sizeof(h->close_shared_camera) &&
	       h->open_shared_camera != NULL && h->camera_acquire != NULL && h->camera_release != NULL &&
	       h->close_shared_camera != NULL;
}

static const char *
format_str(uint32_t f)
{
	switch (f) {
	case XRT_STEREO_CAMERA_FORMAT_GRAY8: return "GRAY8";
	case XRT_STEREO_CAMERA_FORMAT_NV12: return "NV12";
	case XRT_STEREO_CAMERA_FORMAT_BGRA8: return "BGRA8";
	default: return "?";
	}
}

static const char *
open_error_str(xrt_result_t r)
{
	switch (r) {
	case XRT_ERROR_FEATURE_NOT_SUPPORTED: return "no camera manager or no rectifiable camera in this process";
	case XRT_ERROR_STEREO_CAMERA_DISABLED: return "camera sharing is off";
	case XRT_ERROR_STEREO_CAMERA_CONSENT_REFUSED: return "consent refused";
	case XRT_ERROR_NOT_AUTHORIZED: return "not authorized";
	case XRT_ERROR_CLIENT_LIMIT_REACHED: return "camera-consumer quota exhausted";
	case XRT_ERROR_STEREO_CAMERA_BUSY: return "camera busy";
	case XRT_ERROR_STEREO_CAMERA_STREAM_ENDED: return "the stream ended";
	default: return "open failed";
	}
}

static void
set_presence(enum xrt_input_provider_presence p)
{
	os_mutex_lock(&g_shcam.lock);
	g_shcam.presence = p;
	os_mutex_unlock(&g_shcam.lock);
}

static void
to_flowing(uint32_t w, uint32_t h, uint32_t format)
{
	if (g_shcam.state != SHCAM_FLOWING) {
		g_shcam.state = SHCAM_FLOWING;
		U_LOG_W("sim_input shared-camera: %s frames FLOWING (%ux%u %s, %llu frames)", SIM_INPUT_ID, w, h,
		        format_str(format), (unsigned long long)g_shcam.frames);
	}
	set_presence(XRT_INPUT_PROVIDER_PRESENCE_PRESENT);
}

static void
to_waiting(void)
{
	if (g_shcam.state != SHCAM_WAITING) {
		g_shcam.state = SHCAM_WAITING;
		U_LOG_W("sim_input shared-camera: %s WAITING (no 3D party holds the tracker)", SIM_INPUT_ID);
	}
	set_presence(XRT_INPUT_PROVIDER_PRESENCE_ABSENT);
}

static void
to_unavailable(int32_t code, const char *reason)
{
	if (g_shcam.state != SHCAM_UNAVAILABLE || g_shcam.unavailable_code != code) {
		g_shcam.state = SHCAM_UNAVAILABLE;
		g_shcam.unavailable_code = code;
		U_LOG_W("sim_input shared-camera: %s unavailable: %s", SIM_INPUT_ID, reason);
	}
	set_presence(XRT_INPUT_PROVIDER_PRESENCE_ABSENT);
}

//! Sleep up to @p ns in short steps; false once the thread is asked to stop.
static bool
nap(int64_t ns)
{
	const int64_t step = 50ll * 1000 * 1000;
	while (ns > 0) {
		if (!os_thread_helper_is_running(&g_shcam.oth)) {
			return false;
		}
		os_nanosleep(ns < step ? ns : step);
		ns -= step;
	}
	return os_thread_helper_is_running(&g_shcam.oth);
}

static void *
shcam_thread(void *ptr)
{
	(void)ptr;
	os_thread_helper_name(&g_shcam.oth, "sim_input shcam");
	const struct xrt_input_plugin_host_iface *host = g_shcam.host;
	struct xrt_input_host_camera *cam = NULL;
	int64_t opened_ns = 0;
	int64_t last_frame_ns = 0;

	while (os_thread_helper_is_running(&g_shcam.oth)) {
		if (cam == NULL) {
			struct xrt_input_host_camera_open req;
			memset(&req, 0, sizeof(req));
			req.struct_size = (uint32_t)sizeof(req);
			req.output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
			req.camera_id = 0; // the active display's own camera
			req.max_frame_rate = 0.0f;
			xrt_result_t r = host->open_shared_camera(SIM_INPUT_ID, &req, &cam);
			if (r != XRT_SUCCESS || cam == NULL) {
				cam = NULL;
				char why[160];
				snprintf(why, sizeof(why), "%s (%d)", open_error_str(r), (int)r);
				to_unavailable((int32_t)r, why);
				if (!nap(SHCAM_RETRY_NS)) {
					break;
				}
				continue;
			}
			U_LOG_W(
			    "sim_input shared-camera: %s opened camera %llu \"%s\" — %ux%u, %.1f Hz cap, baseline %.2f "
			    "mm",
			    SIM_INPUT_ID, (unsigned long long)req.props.camera_id, req.props.display_name,
			    req.layout.width, req.layout.height, (double)req.layout.max_frame_rate,
			    (double)req.calib.baseline_mm);
			opened_ns = (int64_t)os_monotonic_get_ns();
			last_frame_ns = 0;
		}

		struct xrt_input_host_camera_frame f;
		memset(&f, 0, sizeof(f));
		f.struct_size = (uint32_t)sizeof(f);
		uint32_t state = 0;
		xrt_result_t r = host->camera_acquire(cam, SHCAM_ACQUIRE_TIMEOUT_MS, &f, &state);
		const int64_t now = (int64_t)os_monotonic_get_ns();
		if (r == XRT_SUCCESS) {
			g_shcam.frames++;
			last_frame_ns = now;
			host->camera_release(cam,
			                     f.frame_index); // nothing reads the pixels (yet): the hands stay scripted
			to_flowing(f.width, f.height, f.format);
			continue;
		}
		if (r == XRT_TIMEOUT) {
			const int64_t since = last_frame_ns != 0 ? now - last_frame_ns : now - opened_ns;
			if (since <= SHCAM_STALE_NS) {
				continue; // still within the grace: keep the last verdict
			}
			if (state == XRT_STEREO_CAMERA_STATE_WAITING || state == XRT_STEREO_CAMERA_STATE_AVAILABLE) {
				// AVAILABLE with no frame: the source is open and silent — the
				// display delivers only while a 3D session holds its tracker.
				to_waiting();
			} else if (state == XRT_STEREO_CAMERA_STATE_SUSPENDED) {
				to_unavailable(-1000 - (int32_t)state,
				               "camera SUSPENDED (session locked / sharing off / frames stopped)");
			} else {
				to_unavailable(-1000 - (int32_t)state, "camera UNAVAILABLE");
			}
			continue;
		}
		// Ended (sharing stopped, source lost, shutdown) or a hard error: close, retry later.
		host->close_shared_camera(cam);
		cam = NULL;
		char why[160];
		snprintf(why, sizeof(why), "%s (%d)", open_error_str(r), (int)r);
		to_unavailable((int32_t)r, why);
		if (!nap(SHCAM_RETRY_NS)) {
			break;
		}
	}
	if (cam != NULL) {
		host->close_shared_camera(cam);
	}
	set_presence(XRT_INPUT_PROVIDER_PRESENCE_ABSENT);
	return NULL;
}

void
sim_input_shared_camera_start(const struct xrt_input_plugin_host_iface *host)
{
	if (!g_shcam.initialized) {
		os_mutex_init(&g_shcam.lock);
		os_thread_helper_init(&g_shcam.oth);
		g_shcam.presence = XRT_INPUT_PROVIDER_PRESENCE_ABSENT;
		g_shcam.initialized = true;
	}
	if (os_thread_helper_is_running(&g_shcam.oth)) {
		return;
	}
	if (!host_has_slots(host)) {
		to_unavailable(-1, "the runtime has no shared-camera host slot (older runtime)");
		return;
	}
	g_shcam.host = host;
	g_shcam.state = SHCAM_NONE;
	g_shcam.frames = 0;
	if (os_thread_helper_start(&g_shcam.oth, shcam_thread, NULL) != 0) {
		to_unavailable(-2, "could not start the poll thread");
	}
}

void
sim_input_shared_camera_stop(void)
{
	if (!g_shcam.initialized) {
		return;
	}
	os_thread_helper_stop_and_wait(&g_shcam.oth);
	set_presence(XRT_INPUT_PROVIDER_PRESENCE_ABSENT);
}

enum xrt_input_provider_presence
sim_input_shared_camera_presence(void)
{
	if (!g_shcam.initialized) {
		return XRT_INPUT_PROVIDER_PRESENCE_ABSENT;
	}
	os_mutex_lock(&g_shcam.lock);
	enum xrt_input_provider_presence p = g_shcam.presence;
	os_mutex_unlock(&g_shcam.lock);
	return p;
}
