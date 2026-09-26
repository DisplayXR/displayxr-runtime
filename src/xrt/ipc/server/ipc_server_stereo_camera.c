// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043): the service's camera manager and its
 *         IPC handlers. See ipc_server_stereo_camera.h.
 *
 * Locking: ONE manager mutex guards cameras, streams and rings. Plug-in calls
 * that may block (open, wait_frame, close) are made with it RELEASED; the
 * per-frame fan-out (conversion into each stream's slot) runs under it — a
 * 1280x480 NV12 copy is well under a millisecond, and it keeps an acquire from
 * ever observing a half-written slot. Only the camera thread closes a source.
 *
 * Privacy (spec §7) — R1 ships the HOOK POINTS, deny-by-default:
 *  - authorise_locked(): every start and every calibration read. Allowed in R1
 *    only with DXR_STEREO_CAMERA_DEV_ALLOW=1 in the SERVICE environment;
 *    otherwise XRT_ERROR_NOT_AUTHORIZED (XR_ERROR_PERMISSION_INSUFFICIENT).
 *    R3 replaces the env check with OS consent + the DisplayXR consent store +
 *    delegating-client registration.
 *  - client_visible_locked(): the foreground rule, evaluated per published
 *    frame. R1 always true; R3 wires session visibility / OS lock.
 *  - output_allowed(): RAW is refused to PRESENT_OWNER clients (the browser,
 *    which exposes cameras to web pages) — maintainer decision for R1.
 *  - DXR_STEREO_CAMERA=0: kill switch, zero cameras enumerated.
 *  - the in-use indicator (tray badge naming consumers) is R3; R1 logs one
 *    WARN per stream start/stop naming the peer executable.
 *
 * Transports — R1 implements SHARED_MEMORY only. The GPU transports are
 * designed to reuse this ring's state machine unchanged, only the slot storage
 * differs:
 *  - D3D11_TEXTURE (Windows): TODO(L1/B1). Three D3D11 textures on the
 *    service's lift/weave device created SHARED_NTHANDLE|SHARED_KEYEDMUTEX-free
 *    with one ID3D11Fence; the publish step becomes an UpdateSubresource into
 *    the free slot + Signal(fence, seq); acquire returns (slot, fence value);
 *    the NT handles + fence are handed out on the first acquire and on
 *    reallocation, the XR_DXR_weave output-export pattern (legacy DXGI handles
 *    low-bit tagged for Low-IL callers). The browser's capture device imports
 *    them and waits on the fence instead of reading the section.
 *  - Vulkan / AHARDWAREBUFFER (Android, and desktop Linux via dma-buf): TODO(A1).
 *    Three AHardwareBuffers (or VkImages exported as dma-buf / opaque fd) per
 *    stream, sent once over the IPC socket like weave's dma-buf outputs
 *    (#1699), with a sync_file per publish instead of a fence value. NV12 per
 *    eye maps to AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420; the tablet's per-lens
 *    1280x720 NV12 pair makes this transport the default there.
 *
 * @ingroup ipc_server
 */

#include "server/ipc_server.h"
#include "server/ipc_server_peer_creds.h"
#include "server/ipc_server_stereo_camera.h"
#include "ipc_server_generated.h"

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_stereo_camera.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "shared/ipc_shmem.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_stereo_camera.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

DEBUG_GET_ONCE_BOOL_OPTION(stereo_camera_enabled, "DXR_STEREO_CAMERA", true)
DEBUG_GET_ONCE_BOOL_OPTION(stereo_camera_dev_allow, "DXR_STEREO_CAMERA_DEV_ALLOW", false)

#define MAX_STREAMS (XRT_STEREO_CAMERA_MAX_CAMERAS * XRT_STEREO_CAMERA_MAX_STREAMS_PER_CAMERA)
#define LINGER_NS (2000ll * 1000 * 1000)
#define WAIT_TIMEOUT_NS (100ll * 1000 * 1000)
#define SUSPEND_AFTER_NS (1000ll * 1000 * 1000)
#define REOPEN_BACKOFF_NS (1000ll * 1000 * 1000)
#define STATS_LOG_NS (5000ll * 1000 * 1000)


/*
 *
 * Wake handle: an auto-reset event (Windows) / a non-blocking pipe (POSIX).
 *
 */

struct scam_wake
{
#ifdef XRT_OS_WINDOWS
	HANDLE event;
#else
	int fds[2]; //!< [0] = read end (sent to the client), [1] = write end
#endif
};

static bool
wake_create(struct scam_wake *w)
{
#ifdef XRT_OS_WINDOWS
	w->event = CreateEventW(NULL, FALSE, FALSE, NULL);
	return w->event != NULL;
#else
	w->fds[0] = w->fds[1] = -1;
	if (pipe(w->fds) != 0) {
		return false;
	}
	for (int i = 0; i < 2; i++) {
		int fl = fcntl(w->fds[i], F_GETFL, 0);
		fcntl(w->fds[i], F_SETFL, fl | O_NONBLOCK);
		fcntl(w->fds[i], F_SETFD, FD_CLOEXEC);
	}
	return true;
#endif
}

static void
wake_signal(struct scam_wake *w)
{
#ifdef XRT_OS_WINDOWS
	if (w->event != NULL) {
		SetEvent(w->event);
	}
#else
	if (w->fds[1] >= 0) {
		const char b = 1;
		// EAGAIN = the consumer is not draining: it already has a pending wake.
		ssize_t r = write(w->fds[1], &b, 1);
		(void)r;
	}
#endif
}

static void
wake_destroy(struct scam_wake *w)
{
#ifdef XRT_OS_WINDOWS
	if (w->event != NULL) {
		CloseHandle(w->event);
		w->event = NULL;
	}
#else
	for (int i = 0; i < 2; i++) {
		if (w->fds[i] >= 0) {
			close(w->fds[i]);
			w->fds[i] = -1;
		}
	}
#endif
}


/*
 *
 * Structs.
 *
 */

struct scam_stream
{
	bool used;
	uint64_t id;
	uint64_t owner; //!< ics->stereo_camera_owner of the creating connection
	volatile struct ipc_client_state *ics;
	uint32_t camera; //!< index into mgr->cams
	struct xrt_stereo_camera_stream_request req;
	uint32_t output; //!< what frames actually are (RAW when rectification is unavailable)
	char consumer[260];

	bool started;
	bool allocated;
	uint32_t width, height;
	struct u_stereo_camera_planes layout;
	uint64_t slot_stride;
	uint64_t section_size;
	xrt_shmem_handle_t section;
	uint8_t *map;
	struct scam_wake wake;
	struct u_stereo_camera_ring ring;
	struct u_stereo_camera_decimator dec;
	struct xrt_stereo_camera_frame_info slot_info[U_STEREO_CAMERA_RING_SLOTS];

#ifdef XRT_OS_WINDOWS
	//! Restricted duplicates handed to the last get_section / get_wake reply.
	//! The generated dispatch duplicates them into the peer AFTER the handler
	//! returns, so they are closed on the next call / stream destroy.
	HANDLE sent_section_dup;
	HANDLE sent_wake_dup;
#endif

	// stats
	float delivered_rate;
	int64_t last_publish_ns;
	uint64_t latency_sum_ns;
	uint64_t latency_count;
};

struct scam_camera
{
	uint32_t index; //!< plug-in index
	uint64_t camera_id;
	struct xrt_plugin_stereo_camera_info info;
	bool have_calib;
	struct xrt_plugin_stereo_camera_calibration calib;
	uint32_t state;
	uint32_t calibration_generation;

	struct xrt_plugin_stereo_camera *src; //!< owned by the camera thread
	uint32_t started_count;
	int64_t linger_deadline_ns;
	int64_t last_frame_ns;
	int64_t retry_after_ns;
	float source_rate;
	int64_t last_stats_log_ns;

	bool thread_started;
	struct os_thread thread;
	struct os_cond cond;
	struct ipc_server_stereo_camera *mgr;
};

struct ipc_server_stereo_camera
{
	struct os_mutex lock;
	bool shutting_down;
	const struct xrt_plugin_iface *iface;
	struct xrt_plugin_instance *inst;
	uint32_t camera_count;
	struct scam_camera cams[XRT_STEREO_CAMERA_MAX_CAMERAS];
	struct scam_stream streams[MAX_STREAMS];
	uint64_t next_stream_id;
	uint64_t next_owner;
};


/*
 *
 * Helpers.
 *
 */

static const char *
state_str(uint32_t s)
{
	switch (s) {
	case XRT_STEREO_CAMERA_STATE_AVAILABLE: return "AVAILABLE";
	case XRT_STEREO_CAMERA_STATE_WAITING: return "WAITING";
	case XRT_STEREO_CAMERA_STATE_SUSPENDED: return "SUSPENDED";
	case XRT_STEREO_CAMERA_STATE_UNAVAILABLE: return "UNAVAILABLE";
	default: return "?";
	}
}

static struct ipc_server_stereo_camera *
mgr_of(volatile struct ipc_client_state *ics)
{
	return (ics != NULL && ics->server != NULL) ? ics->server->stereo_camera : NULL;
}

static uint64_t
owner_of(struct ipc_server_stereo_camera *m, volatile struct ipc_client_state *ics)
{
	if (ics->stereo_camera_owner == 0) {
		ics->stereo_camera_owner = ++m->next_owner;
	}
	return ics->stereo_camera_owner;
}

static struct scam_camera *
camera_by_id_locked(struct ipc_server_stereo_camera *m, uint64_t id)
{
	for (uint32_t i = 0; i < m->camera_count; i++) {
		if (m->cams[i].camera_id == id) {
			return &m->cams[i];
		}
	}
	return NULL;
}

static struct scam_stream *
stream_by_id_locked(struct ipc_server_stereo_camera *m, volatile struct ipc_client_state *ics, uint64_t id)
{
	if (ics->stereo_camera_owner == 0) {
		return NULL;
	}
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		struct scam_stream *s = &m->streams[i];
		if (s->used && s->id == id && s->owner == ics->stereo_camera_owner) {
			return s;
		}
	}
	return NULL;
}

//! Who may use cameras at all: apps, the browser, and the diagnostic CLI.
static xrt_result_t
require_camera_client(volatile struct ipc_client_state *ics, const char *what)
{
	uint32_t cls = ics->client_state.client_class;
	if (cls != XRT_CLIENT_CLASS_APP && cls != XRT_CLIENT_CLASS_PRESENT_OWNER && cls != XRT_CLIENT_CLASS_DIAG) {
		U_LOG_W("%s: denied — pid %ld is class %s (stereo camera: APP / PRESENT_OWNER / DIAG).", what,
		        ics->peer_pid, ipc_server_client_class_str(cls));
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	return XRT_SUCCESS;
}

/*!
 * Spec §7.1 authorisation HOOK. R1: deny by default; the dev override is the
 * only way in. R3 replaces the body: OS camera consent of the OS-derived peer
 * (Windows CapabilityAccessManager ConsentStore\webcam, Android CAMERA of the
 * peer uid), then the DisplayXR per-executable consent (HKCU allow list / tray
 * prompt; a registered consent-delegating browser passes per executable and
 * prompts per origin itself).
 */
static xrt_result_t
authorise_locked(volatile struct ipc_client_state *ics, const struct scam_camera *cam, const char *what)
{
	(void)cam;
	if (debug_get_bool_option_stereo_camera_dev_allow()) {
		return XRT_SUCCESS;
	}
	U_LOG_W("%s: refused for pid %ld — stereo camera consent is not implemented yet (R3); set "
	        "DXR_STEREO_CAMERA_DEV_ALLOW=1 in the SERVICE environment for development.",
	        what, ics->peer_pid);
	return XRT_ERROR_NOT_AUTHORIZED;
}

/*!
 * Spec §7.2 foreground-rule HOOK, evaluated per published frame. R1: always
 * visible. R3: an app's session must be VISIBLE/FOCUSED; a delegating client
 * follows its own rule; every stream is suspended while the OS session is
 * locked / switched away (Android: android_package_is_visible of the peer).
 */
static bool
client_visible_locked(volatile struct ipc_client_state *ics)
{
	(void)ics;
	return true;
}

//! Maintainer decision (R1): raw frames never reach web pages — the browser
//! (PRESENT_OWNER) gets RECTIFIED only; native clients may ask for RAW.
static bool
output_allowed(volatile struct ipc_client_state *ics, uint32_t output)
{
	return output != XRT_STEREO_CAMERA_OUTPUT_RAW ||
	       ics->client_state.client_class != XRT_CLIENT_CLASS_PRESENT_OWNER;
}

static bool
camera_can_rectify(const struct scam_camera *cam)
{
	// R1 has no rectifier (u_stereo_rectify is R2): only a natively rectified
	// source yields RECTIFIED frames; a CALIBRATED one is delivered RAW-flagged.
	return (cam->info.flags & XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED) != 0;
}

static void
set_state_locked(struct scam_camera *cam, uint32_t state)
{
	if (cam->state == state) {
		return;
	}
	U_LOG_W("stereo camera %llu \"%s\": %s -> %s", (unsigned long long)cam->camera_id, cam->info.display_name,
	        state_str(cam->state), state_str(state));
	cam->state = state;
	if (state == XRT_STEREO_CAMERA_STATE_SUSPENDED || state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE) {
		// Spec §7.2: the last image must not linger for a suspended consumer.
		struct ipc_server_stereo_camera *m = cam->mgr;
		for (uint32_t i = 0; i < MAX_STREAMS; i++) {
			struct scam_stream *s = &m->streams[i];
			if (s->used && s->camera == cam->index) {
				u_stereo_camera_ring_clear(&s->ring);
			}
		}
	}
	// TODO(R1 follow-up): queue XrEventDataStereoCameraStateChangedDXR to the
	// owning connections (needs a server->client instance-event channel).
}

static void
mat3_to_quat(const double r[3][3], float out[4])
{
	double tr = r[0][0] + r[1][1] + r[2][2];
	double x, y, z, w;
	if (tr > 0.0) {
		double s = sqrt(tr + 1.0) * 2.0;
		w = 0.25 * s;
		x = (r[2][1] - r[1][2]) / s;
		y = (r[0][2] - r[2][0]) / s;
		z = (r[1][0] - r[0][1]) / s;
	} else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
		double s = sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0;
		w = (r[2][1] - r[1][2]) / s;
		x = 0.25 * s;
		y = (r[0][1] + r[1][0]) / s;
		z = (r[0][2] + r[2][0]) / s;
	} else if (r[1][1] > r[2][2]) {
		double s = sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0;
		w = (r[0][2] - r[2][0]) / s;
		x = (r[0][1] + r[1][0]) / s;
		y = 0.25 * s;
		z = (r[1][2] + r[2][1]) / s;
	} else {
		double s = sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0;
		w = (r[1][0] - r[0][1]) / s;
		x = (r[0][2] + r[2][0]) / s;
		y = (r[1][2] + r[2][1]) / s;
		z = 0.25 * s;
	}
	out[0] = (float)x;
	out[1] = (float)y;
	out[2] = (float)z;
	out[3] = (float)w;
}

static double
baseline_mm(const struct xrt_plugin_stereo_camera_calibration *c)
{
	const double *t = c->translation_right_from_left_mm;
	return sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
}


/*
 *
 * Stream storage.
 *
 */

static void
stream_free_storage(struct scam_stream *s)
{
	if (s->map != NULL || s->section != XRT_SHMEM_HANDLE_INVALID) {
		void *map = s->map;
		ipc_shmem_destroy(&s->section, &map, (size_t)s->section_size);
		s->map = NULL;
	}
	wake_destroy(&s->wake);
#ifdef XRT_OS_WINDOWS
	if (s->sent_section_dup != NULL) {
		CloseHandle(s->sent_section_dup);
		s->sent_section_dup = NULL;
	}
	if (s->sent_wake_dup != NULL) {
		CloseHandle(s->sent_wake_dup);
		s->sent_wake_dup = NULL;
	}
#endif
	s->allocated = false;
}

static xrt_result_t
stream_allocate_locked(struct scam_stream *s, const struct scam_camera *cam)
{
	if (s->allocated) {
		return XRT_SUCCESS;
	}
	s->width = 2 * cam->info.eye_width;
	s->height = cam->info.eye_height;
	if (!u_stereo_camera_layout(s->req.format, s->width, s->height, &s->layout)) {
		return XRT_ERROR_ALLOCATION;
	}
	s->slot_stride = (s->layout.size + 4095u) & ~(uint64_t)4095u;
	s->section_size = s->slot_stride * U_STEREO_CAMERA_RING_SLOTS;
	void *map = NULL;
	s->section = XRT_SHMEM_HANDLE_INVALID;
	xrt_result_t xret = ipc_shmem_create((size_t)s->section_size, &s->section, &map);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	s->map = map;
	memset(s->map, 0, (size_t)s->section_size);
	// TODO(R3): hand POSIX consumers a read-only fd (Linux: reopen
	// /proc/self/fd/N O_RDONLY; macOS: shm_open the name O_RDONLY before unlink;
	// Android: an ASharedMemory with PROT_READ set via ASharedMemory_setProt).
	if (!wake_create(&s->wake)) {
		stream_free_storage(s);
		return XRT_ERROR_ALLOCATION;
	}
	u_stereo_camera_ring_init(&s->ring);
	s->allocated = true;
	return XRT_SUCCESS;
}


/*
 *
 * Camera thread.
 *
 */

static void
publish_locked(struct ipc_server_stereo_camera *m,
               struct scam_camera *cam,
               const struct xrt_plugin_stereo_camera_frame *f,
               int64_t now)
{
	if (cam->last_frame_ns > 0) {
		double dt = (double)(now - cam->last_frame_ns) * 1e-9;
		if (dt > 0.0) {
			float inst = (float)(1.0 / dt);
			cam->source_rate = cam->source_rate <= 0.0f ? inst : cam->source_rate * 0.9f + inst * 0.1f;
		}
	}
	cam->last_frame_ns = now;
	set_state_locked(cam, XRT_STEREO_CAMERA_STATE_AVAILABLE);

	if (f->width != 2 * cam->info.eye_width || f->height != cam->info.eye_height) {
		U_LOG_W("stereo camera %llu: frame %ux%u does not match the advertised %ux%u SBS — dropped",
		        (unsigned long long)cam->camera_id, f->width, f->height, 2 * cam->info.eye_width,
		        cam->info.eye_height);
		return;
	}

	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		struct scam_stream *s = &m->streams[i];
		if (!s->used || !s->started || !s->allocated || s->camera != cam->index) {
			continue;
		}
		if (!client_visible_locked(s->ics)) {
			u_stereo_camera_ring_clear(&s->ring);
			continue;
		}
		if (!u_stereo_camera_decimator_accept(&s->dec, f->time_ns)) {
			continue;
		}
		int32_t slot = u_stereo_camera_ring_begin_write(&s->ring);
		if (slot < 0) {
			continue;
		}
		uint8_t *dst = s->map + (uint64_t)slot * s->slot_stride;
		if (!u_stereo_camera_convert(f->format, f->planes, f->pitches, s->req.format, dst, &s->layout, f->width,
		                             f->height)) {
			u_stereo_camera_ring_abort_write(&s->ring);
			continue;
		}
		struct xrt_stereo_camera_frame_info *fi = &s->slot_info[slot];
		U_ZERO(fi);
		fi->frame_index = f->sequence;
		fi->slot = (uint32_t)slot;
		fi->width = f->width;
		fi->height = f->height;
		fi->format = s->req.format;
		fi->row_pitch[0] = s->layout.pitch[0];
		fi->row_pitch[1] = s->layout.pitch[1];
		fi->plane_offset[0] = s->layout.offset[0];
		fi->plane_offset[1] = s->layout.offset[1];
		fi->capture_time_ns = f->time_ns;
		fi->time_is_exposure = f->time_is_exposure ? 1u : 0u;
		fi->output = s->output;
		fi->calibration_generation = cam->calibration_generation;
		u_stereo_camera_ring_publish(&s->ring, slot, f->sequence);

		int64_t pub = os_monotonic_get_ns();
		if (pub > f->time_ns) {
			s->latency_sum_ns += (uint64_t)(pub - f->time_ns);
			s->latency_count++;
		}
		if (s->last_publish_ns > 0 && pub > s->last_publish_ns) {
			float inst = (float)(1e9 / (double)(pub - s->last_publish_ns));
			s->delivered_rate = s->delivered_rate <= 0.0f ? inst : s->delivered_rate * 0.9f + inst * 0.1f;
		}
		s->last_publish_ns = pub;
		wake_signal(&s->wake);
	}

	if (now - cam->last_stats_log_ns >= STATS_LOG_NS) {
		cam->last_stats_log_ns = now;
		U_LOG_I("stereo camera %llu: source %.1f Hz, %u started stream(s)", (unsigned long long)cam->camera_id,
		        cam->source_rate, cam->started_count);
	}
}

static void *
camera_thread(void *ptr)
{
	struct scam_camera *cam = (struct scam_camera *)ptr;
	struct ipc_server_stereo_camera *m = cam->mgr;
	const struct xrt_plugin_iface *iface = m->iface;

	os_mutex_lock(&m->lock);
	while (!m->shutting_down) {
		int64_t now = os_monotonic_get_ns();

		// Idle: close the source once the linger expires, then sleep until a
		// stream starts (or the manager shuts down).
		if (cam->started_count == 0 && (cam->src == NULL || now >= cam->linger_deadline_ns)) {
			if (cam->src != NULL) {
				struct xrt_plugin_stereo_camera *src = cam->src;
				cam->src = NULL;
				os_mutex_unlock(&m->lock);
				iface->stereo_camera_close(src);
				os_mutex_lock(&m->lock);
				U_LOG_W("stereo camera %llu: source closed (no started stream for %lld ms)",
				        (unsigned long long)cam->camera_id, (long long)(LINGER_NS / 1000000));
				cam->last_frame_ns = 0;
				cam->source_rate = 0.0f;
				if (cam->state != XRT_STEREO_CAMERA_STATE_UNAVAILABLE) {
					cam->state = XRT_STEREO_CAMERA_STATE_AVAILABLE; // idle: "will flow on start"
				}
				continue;
			}
			os_cond_wait(&cam->cond, &m->lock);
			continue;
		}

		if (cam->src == NULL) {
			if (now < cam->retry_after_ns) {
				os_cond_wait_timeout_ns(&cam->cond, &m->lock, (uint64_t)(cam->retry_after_ns - now));
				continue;
			}
			struct xrt_plugin_stereo_camera *src = NULL;
			os_mutex_unlock(&m->lock);
			xrt_result_t xret = iface->stereo_camera_open(m->inst, cam->index, &src);
			os_mutex_lock(&m->lock);
			if (xret != XRT_SUCCESS || src == NULL) {
				U_LOG_W("stereo camera %llu: plug-in open failed (%d); retrying in 1 s",
				        (unsigned long long)cam->camera_id, (int)xret);
				set_state_locked(cam, XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
				cam->retry_after_ns = os_monotonic_get_ns() + REOPEN_BACKOFF_NS;
				continue;
			}
			if (m->shutting_down) {
				os_mutex_unlock(&m->lock);
				iface->stereo_camera_close(src);
				os_mutex_lock(&m->lock);
				break;
			}
			cam->src = src;
			cam->last_frame_ns = 0;
			U_LOG_W("stereo camera %llu: source opened", (unsigned long long)cam->camera_id);
			set_state_locked(cam, XRT_STEREO_CAMERA_STATE_WAITING);
		}

		struct xrt_plugin_stereo_camera *src = cam->src;
		struct xrt_plugin_stereo_camera_frame f;
		memset(&f, 0, sizeof(f));
		os_mutex_unlock(&m->lock);
		uint32_t w = iface->stereo_camera_wait_frame(src, WAIT_TIMEOUT_NS, &f);
		os_mutex_lock(&m->lock);
		now = os_monotonic_get_ns();

		switch (w) {
		case XRT_PLUGIN_STEREO_CAMERA_WAIT_OK:
			publish_locked(m, cam, &f, now);
			iface->stereo_camera_release_frame(src);
			break;
		case XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT:
			if (cam->last_frame_ns == 0) {
				set_state_locked(cam, XRT_STEREO_CAMERA_STATE_WAITING);
			} else if (now - cam->last_frame_ns > SUSPEND_AFTER_NS) {
				set_state_locked(cam, XRT_STEREO_CAMERA_STATE_SUSPENDED);
			}
			break;
		case XRT_PLUGIN_STEREO_CAMERA_WAIT_SUSPENDED:
			set_state_locked(cam, XRT_STEREO_CAMERA_STATE_SUSPENDED);
			cam->last_frame_ns = 0;
			break;
		default:
			U_LOG_W("stereo camera %llu: plug-in reported a source error; closing, retry in 1 s",
			        (unsigned long long)cam->camera_id);
			set_state_locked(cam, XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
			cam->src = NULL;
			os_mutex_unlock(&m->lock);
			iface->stereo_camera_close(src);
			os_mutex_lock(&m->lock);
			cam->retry_after_ns = os_monotonic_get_ns() + REOPEN_BACKOFF_NS;
			break;
		}
	}
	if (cam->src != NULL) {
		struct xrt_plugin_stereo_camera *src = cam->src;
		cam->src = NULL;
		os_mutex_unlock(&m->lock);
		iface->stereo_camera_close(src);
		os_mutex_lock(&m->lock);
	}
	os_mutex_unlock(&m->lock);
	return NULL;
}


/*
 *
 * Manager lifecycle.
 *
 */

struct ipc_server_stereo_camera *
ipc_server_stereo_camera_create(struct xrt_instance *xinst)
{
	struct ipc_server_stereo_camera *m = U_TYPED_CALLOC(struct ipc_server_stereo_camera);
	os_mutex_init(&m->lock);
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		m->streams[i].section = XRT_SHMEM_HANDLE_INVALID;
	}

	if (!debug_get_bool_option_stereo_camera_enabled()) {
		U_LOG_W("stereo camera: disabled by DXR_STEREO_CAMERA=0 (kill switch) — zero cameras");
		return m;
	}
	const struct xrt_plugin_iface *iface = NULL;
	struct xrt_plugin_instance *inst = NULL;
	if (xinst == NULL || xinst->get_active_plugin == NULL || !xinst->get_active_plugin(xinst, &iface, &inst) ||
	    !xrt_plugin_iface_has_stereo_camera(iface)) {
		U_LOG_I("stereo camera: the active plug-in provides no camera source");
		return m;
	}
	m->iface = iface;
	m->inst = inst;

	struct xrt_plugin_stereo_camera_info infos[XRT_STEREO_CAMERA_MAX_CAMERAS];
	memset(infos, 0, sizeof(infos));
	for (uint32_t i = 0; i < XRT_STEREO_CAMERA_MAX_CAMERAS; i++) {
		infos[i].struct_size = (uint32_t)sizeof(infos[i]);
	}
	uint32_t n = iface->stereo_camera_enumerate(inst, XRT_STEREO_CAMERA_MAX_CAMERAS, infos);
	if (n > XRT_STEREO_CAMERA_MAX_CAMERAS) {
		n = XRT_STEREO_CAMERA_MAX_CAMERAS;
	}
	for (uint32_t i = 0; i < n; i++) {
		struct scam_camera *cam = &m->cams[m->camera_count];
		struct xrt_plugin_stereo_camera_info *info = &infos[i];
		info->display_name[sizeof(info->display_name) - 1] = '\0';
		info->device_identity[sizeof(info->device_identity) - 1] = '\0';
		info->platform_device_hint[sizeof(info->platform_device_hint) - 1] = '\0';
		if (info->eye_width == 0 || info->eye_height == 0 || (info->eye_width & 1u) || (info->eye_height & 1u) ||
		    info->native_format < XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8 ||
		    info->native_format > XRT_PLUGIN_STEREO_CAMERA_FORMAT_BGRA8) {
			U_LOG_W("stereo camera: plug-in camera %u has a malformed description (%ux%u, format %u) — "
			        "skipped",
			        i, info->eye_width, info->eye_height, info->native_format);
			continue;
		}
		cam->index = i;
		cam->camera_id = (uint64_t)m->camera_count + 1; // never reused in this service lifetime
		cam->info = *info;
		cam->mgr = m;
		cam->state = XRT_STEREO_CAMERA_STATE_AVAILABLE;
		os_cond_init(&cam->cond);
		if (info->flags & XRT_PLUGIN_STEREO_CAMERA_CALIBRATED) {
			cam->calib.struct_size = (uint32_t)sizeof(cam->calib);
			if (iface->stereo_camera_get_calibration(inst, i, &cam->calib) == XRT_SUCCESS) {
				cam->have_calib = true;
			} else {
				U_LOG_W("stereo camera: \"%s\" claims CALIBRATED but returned no calibration — RAW only",
				        info->display_name);
				cam->info.flags &= ~XRT_PLUGIN_STEREO_CAMERA_CALIBRATED;
			}
		}
		U_LOG_W("stereo camera %llu: \"%s\" %ux%u per eye @ %.1f Hz, flags 0x%x, native format %u",
		        (unsigned long long)cam->camera_id, info->display_name, info->eye_width, info->eye_height,
		        info->max_frame_rate, info->flags, info->native_format);
		m->camera_count++;
	}
	return m;
}

void
ipc_server_stereo_camera_destroy(struct ipc_server_stereo_camera **mgr_ptr)
{
	struct ipc_server_stereo_camera *m = *mgr_ptr;
	if (m == NULL) {
		return;
	}
	os_mutex_lock(&m->lock);
	m->shutting_down = true;
	for (uint32_t i = 0; i < m->camera_count; i++) {
		os_cond_broadcast(&m->cams[i].cond);
	}
	os_mutex_unlock(&m->lock);
	for (uint32_t i = 0; i < m->camera_count; i++) {
		if (m->cams[i].thread_started) {
			os_thread_join(&m->cams[i].thread);
			os_thread_destroy(&m->cams[i].thread);
		}
		os_cond_destroy(&m->cams[i].cond);
	}
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		if (m->streams[i].used) {
			stream_free_storage(&m->streams[i]);
		}
	}
	os_mutex_destroy(&m->lock);
	free(m);
	*mgr_ptr = NULL;
}

static void
stream_stop_locked(struct scam_stream *s, struct scam_camera *cam)
{
	if (!s->started) {
		return;
	}
	s->started = false;
	u_stereo_camera_ring_clear(&s->ring);
	if (cam->started_count > 0) {
		cam->started_count--;
	}
	if (cam->started_count == 0) {
		cam->linger_deadline_ns = os_monotonic_get_ns() + LINGER_NS;
	}
	U_LOG_W("stereo camera %llu: stream %llu stopped (%s); %u stream(s) still started",
	        (unsigned long long)cam->camera_id, (unsigned long long)s->id, s->consumer, cam->started_count);
}

static void
stream_destroy_locked(struct ipc_server_stereo_camera *m, struct scam_stream *s)
{
	stream_stop_locked(s, &m->cams[s->camera]);
	stream_free_storage(s);
	memset(s, 0, sizeof(*s));
	s->section = XRT_SHMEM_HANDLE_INVALID;
#ifndef XRT_OS_WINDOWS
	s->wake.fds[0] = s->wake.fds[1] = -1;
#endif
}

void
ipc_server_client_stereo_camera_release(volatile struct ipc_client_state *ics)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL || ics->stereo_camera_owner == 0) {
		return;
	}
	os_mutex_lock(&m->lock);
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		struct scam_stream *s = &m->streams[i];
		if (s->used && s->owner == ics->stereo_camera_owner) {
			stream_destroy_locked(m, s);
		}
	}
	os_mutex_unlock(&m->lock);
	ics->stereo_camera_owner = 0;
}


/*
 *
 * IPC handlers.
 *
 */

xrt_result_t
ipc_handle_stereo_camera_count(volatile struct ipc_client_state *ics, uint32_t *out_count)
{
	*out_count = 0;
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (require_camera_client(ics, "stereo_camera_count") != XRT_SUCCESS || m == NULL) {
		return XRT_SUCCESS; // zero cameras, indistinguishable from "none" on purpose
	}
	os_mutex_lock(&m->lock);
	*out_count = m->camera_count;
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_stereo_camera_get_properties(volatile struct ipc_client_state *ics,
                                        uint32_t index,
                                        struct xrt_stereo_camera_properties *out_props)
{
	U_ZERO(out_props);
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	xrt_result_t auth = require_camera_client(ics, "stereo_camera_get_properties");
	if (auth != XRT_SUCCESS) {
		return auth;
	}
	if (m == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	char exe[260] = {0};
	ipc_server_peer_exe_path(ics->peer_pid, exe, sizeof(exe));

	os_mutex_lock(&m->lock);
	if (index >= m->camera_count) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const struct scam_camera *cam = &m->cams[index];
	out_props->camera_id = cam->camera_id;
	u_stereo_camera_persistent_id(cam->info.device_identity, exe, out_props->persistent_id);
	snprintf(out_props->display_name, sizeof(out_props->display_name), "%s", cam->info.display_name);
	snprintf(out_props->platform_device_hint, sizeof(out_props->platform_device_hint), "%s",
	         cam->info.platform_device_hint);
	out_props->flags = cam->info.flags;
	out_props->state = cam->state;
	out_props->view_count = 2;
	out_props->eye_width = cam->info.eye_width;
	out_props->eye_height = cam->info.eye_height;
	out_props->max_frame_rate = cam->info.max_frame_rate;
	if (cam->have_calib) {
		out_props->baseline_mm = (float)baseline_mm(&cam->calib);
		double fx = cam->calib.k[0][0];
		if (fx > 0.0) {
			out_props->horizontal_fov_deg =
			    (float)(2.0 * atan(cam->info.eye_width / (2.0 * fx)) * 180.0 / 3.14159265358979323846);
		}
	}
	out_props->supported_formats = XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_GRAY8) |
	                               XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_NV12) |
	                               XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_BGRA8);
	out_props->supported_transports = XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY);
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_stereo_camera_get_calibration(volatile struct ipc_client_state *ics,
                                         uint64_t camera_id,
                                         uint32_t output,
                                         struct xrt_stereo_camera_calibration *out_calib)
{
	U_ZERO(out_calib);
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	xrt_result_t auth = require_camera_client(ics, "stereo_camera_get_calibration");
	if (auth != XRT_SUCCESS) {
		return auth;
	}
	if (m == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&m->lock);
	struct scam_camera *cam = camera_by_id_locked(m, camera_id);
	if (cam == NULL || (output != XRT_STEREO_CAMERA_OUTPUT_RAW && output != XRT_STEREO_CAMERA_OUTPUT_RECTIFIED)) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// Spec §7.5: calibration identifies the device — only after §7.1 passes.
	xrt_result_t xret = authorise_locked(ics, cam, "stereo_camera_get_calibration");
	if (xret == XRT_SUCCESS && !output_allowed(ics, output)) {
		xret = XRT_ERROR_NOT_AUTHORIZED;
	}
	if (xret == XRT_SUCCESS && !cam->have_calib) {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (xret == XRT_SUCCESS && output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && !camera_can_rectify(cam)) {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED; // R2 computes post-rectification numbers
	}
	if (xret != XRT_SUCCESS) {
		os_mutex_unlock(&m->lock);
		return xret;
	}
	const struct xrt_plugin_stereo_camera_calibration *c = &cam->calib;
	out_calib->output = output;
	for (int e = 0; e < 2; e++) {
		struct xrt_stereo_camera_intrinsics *in = &out_calib->eye[e];
		in->width = c->image_width;
		in->height = c->image_height;
		in->fx = (float)c->k[e][0];
		in->fy = (float)c->k[e][1];
		in->cx = (float)c->k[e][2];
		in->cy = (float)c->k[e][3];
		in->model = output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED ? XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE
		                                                         : c->distortion_model;
		if (in->model != XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE) {
			for (int k = 0; k < 8; k++) {
				in->coefficients[k] = (float)c->distortion[e][k];
			}
		}
	}
	// Plug-in: OpenCV x_R = R x_L + T. Client: the pose of the right camera in
	// the left camera's frame = (R^T, -R^T T), metres.
	double rt[3][3];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			rt[i][j] = c->rotation_right_from_left[j][i];
		}
	}
	mat3_to_quat(rt, out_calib->orientation);
	for (int i = 0; i < 3; i++) {
		double p = 0.0;
		for (int j = 0; j < 3; j++) {
			p -= rt[i][j] * c->translation_right_from_left_mm[j];
		}
		out_calib->position[i] = (float)(p / 1000.0);
	}
	out_calib->baseline_mm = (float)baseline_mm(c);
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_stereo_camera_stream_create(volatile struct ipc_client_state *ics,
                                       const struct xrt_stereo_camera_stream_request *req,
                                       uint64_t *out_stream_id)
{
	*out_stream_id = 0;
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	xrt_result_t auth = require_camera_client(ics, "stereo_camera_stream_create");
	if (auth != XRT_SUCCESS) {
		return auth;
	}
	if (m == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (req->transport != XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED; // GPU transports: see the file comment
	}
	if (req->format < XRT_STEREO_CAMERA_FORMAT_GRAY8 || req->format > XRT_STEREO_CAMERA_FORMAT_BGRA8 ||
	    (req->output != XRT_STEREO_CAMERA_OUTPUT_RAW && req->output != XRT_STEREO_CAMERA_OUTPUT_RECTIFIED)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (!output_allowed(ics, req->output)) {
		U_LOG_W("stereo_camera_stream_create: RAW refused for pid %ld (PRESENT_OWNER: rectified only)",
		        ics->peer_pid);
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	char exe[260] = {0};
	ipc_server_peer_exe_path(ics->peer_pid, exe, sizeof(exe));

	os_mutex_lock(&m->lock);
	struct scam_camera *cam = camera_by_id_locked(m, req->camera_id);
	if (cam == NULL) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	bool calibrated = (cam->info.flags & (XRT_PLUGIN_STEREO_CAMERA_CALIBRATED |
	                                      XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED)) != 0;
	if (req->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && !calibrated) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	uint32_t per_camera = 0;
	struct scam_stream *slot = NULL;
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		struct scam_stream *s = &m->streams[i];
		if (s->used && s->camera == cam->index) {
			per_camera++;
		} else if (!s->used && slot == NULL) {
			slot = s;
		}
	}
	if (per_camera >= XRT_STEREO_CAMERA_MAX_STREAMS_PER_CAMERA || slot == NULL) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_CLIENT_LIMIT_REACHED;
	}
	memset(slot, 0, sizeof(*slot));
	slot->section = XRT_SHMEM_HANDLE_INVALID;
#ifndef XRT_OS_WINDOWS
	slot->wake.fds[0] = slot->wake.fds[1] = -1;
#endif
	slot->used = true;
	slot->id = ++m->next_stream_id;
	slot->owner = owner_of(m, ics);
	slot->ics = ics;
	slot->camera = cam->index;
	slot->req = *req;
	// R1: RECTIFIED only from a natively rectified source; a calibrated one is
	// delivered RAW and FLAGGED (frame.output) until u_stereo_rectify lands (R2).
	slot->output = (req->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && camera_can_rectify(cam))
	                   ? XRT_STEREO_CAMERA_OUTPUT_RECTIFIED
	                   : XRT_STEREO_CAMERA_OUTPUT_RAW;
	snprintf(slot->consumer, sizeof(slot->consumer), "%s (pid %ld)", exe[0] ? exe : "?", ics->peer_pid);
	*out_stream_id = slot->id;
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_stereo_camera_stream_destroy(volatile struct ipc_client_state *ics, uint64_t stream_id)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL) {
		return XRT_SUCCESS;
	}
	os_mutex_lock(&m->lock);
	struct scam_stream *s = stream_by_id_locked(m, ics, stream_id);
	if (s != NULL) {
		stream_destroy_locked(m, s);
	}
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_stereo_camera_stream_start(volatile struct ipc_client_state *ics, uint64_t stream_id)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	xrt_result_t auth = require_camera_client(ics, "stereo_camera_stream_start");
	if (auth != XRT_SUCCESS) {
		return auth;
	}
	if (m == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&m->lock);
	struct scam_stream *s = stream_by_id_locked(m, ics, stream_id);
	if (s == NULL) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (s->started) {
		os_mutex_unlock(&m->lock);
		return XRT_SUCCESS;
	}
	struct scam_camera *cam = &m->cams[s->camera];
	// Spec §7.1: start is THE authorisation point.
	xrt_result_t xret = authorise_locked(ics, cam, "stereo_camera_stream_start");
	if (xret == XRT_SUCCESS) {
		xret = stream_allocate_locked(s, cam);
	}
	if (xret != XRT_SUCCESS) {
		os_mutex_unlock(&m->lock);
		return xret;
	}
	u_stereo_camera_decimator_init(&s->dec, s->req.max_frame_rate, cam->info.max_frame_rate);
	u_stereo_camera_ring_clear(&s->ring);
	s->started = true;
	cam->started_count++;
	if (!cam->thread_started) {
		os_thread_init(&cam->thread);
		if (os_thread_start(&cam->thread, camera_thread, cam) != 0) {
			s->started = false;
			cam->started_count--;
			os_mutex_unlock(&m->lock);
			return XRT_ERROR_THREADING_INIT_FAILURE;
		}
		cam->thread_started = true;
	}
	os_cond_signal(&cam->cond);
	// TODO(R3): runtime-owned in-use indicator naming s->consumer.
	U_LOG_W("stereo camera %llu: stream %llu started by %s — output %s, format %u, max %.1f Hz; %u started",
	        (unsigned long long)cam->camera_id, (unsigned long long)s->id, s->consumer,
	        s->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED ? "RECTIFIED" : "RAW", s->req.format,
	        s->req.max_frame_rate, cam->started_count);
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_stereo_camera_stream_stop(volatile struct ipc_client_state *ics, uint64_t stream_id)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL) {
		return XRT_SUCCESS;
	}
	os_mutex_lock(&m->lock);
	struct scam_stream *s = stream_by_id_locked(m, ics, stream_id);
	if (s != NULL) {
		stream_stop_locked(s, &m->cams[s->camera]);
	}
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_stereo_camera_stream_get_section(volatile struct ipc_client_state *ics,
                                            uint64_t stream_id,
                                            struct xrt_stereo_camera_stream_layout *out_layout,
                                            uint32_t max_handle_count,
                                            xrt_shmem_handle_t *out_handles,
                                            uint32_t *out_handle_count)
{
	U_ZERO(out_layout);
	*out_handle_count = 0;
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL || max_handle_count < 1) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&m->lock);
	struct scam_stream *s = stream_by_id_locked(m, ics, stream_id);
	if (s == NULL || !s->allocated) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED; // not started yet
	}
	out_layout->width = s->width;
	out_layout->height = s->height;
	out_layout->format = s->req.format;
	out_layout->output = s->output;
	out_layout->transport = s->req.transport;
	out_layout->max_frame_rate = s->dec.period_ns > 0 ? (float)(1e9 / (double)s->dec.period_ns)
	                                                  : m->cams[s->camera].info.max_frame_rate;
	out_layout->section_size = s->section_size;
	out_layout->slot_stride = s->slot_stride;
	out_layout->slot_count = U_STEREO_CAMERA_RING_SLOTS;
#ifdef XRT_OS_WINDOWS
	// A READ-ONLY duplicate: the consumer can map it but never write the ring.
	if (s->sent_section_dup != NULL) {
		CloseHandle(s->sent_section_dup);
		s->sent_section_dup = NULL;
	}
	if (!DuplicateHandle(GetCurrentProcess(), s->section, GetCurrentProcess(), &s->sent_section_dup,
	                     FILE_MAP_READ, FALSE, 0)) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_IPC_FAILURE;
	}
	out_handles[0] = s->sent_section_dup;
#else
	out_handles[0] = s->section; // SCM_RIGHTS installs a copy; ours stays open
#endif
	*out_handle_count = 1;
	ics->handles_sent = true; // browser#103 RC-1
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_stereo_camera_stream_get_wake(volatile struct ipc_client_state *ics,
                                         uint64_t stream_id,
                                         uint32_t max_handle_count,
                                         xrt_graphics_sync_handle_t *out_handles,
                                         uint32_t *out_handle_count)
{
	*out_handle_count = 0;
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL || max_handle_count < 1) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&m->lock);
	struct scam_stream *s = stream_by_id_locked(m, ics, stream_id);
	if (s == NULL || !s->allocated) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
#ifdef XRT_OS_WINDOWS
	// SYNCHRONIZE only: the consumer can wait, never signal / reset it.
	if (s->sent_wake_dup != NULL) {
		CloseHandle(s->sent_wake_dup);
		s->sent_wake_dup = NULL;
	}
	if (!DuplicateHandle(GetCurrentProcess(), s->wake.event, GetCurrentProcess(), &s->sent_wake_dup, SYNCHRONIZE,
	                     FALSE, 0)) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_IPC_FAILURE;
	}
	out_handles[0] = s->sent_wake_dup;
#else
	out_handles[0] = s->wake.fds[0];
#endif
	*out_handle_count = 1;
	ics->handles_sent = true;
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_stereo_camera_acquire(volatile struct ipc_client_state *ics,
                                 uint64_t stream_id,
                                 bool *out_ready,
                                 struct xrt_stereo_camera_frame_info *out_frame)
{
	*out_ready = false;
	U_ZERO(out_frame);
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&m->lock);
	struct scam_stream *s = stream_by_id_locked(m, ics, stream_id);
	if (s == NULL) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	int32_t slot = -1;
	uint64_t seq = 0;
	if (s->started && s->allocated && u_stereo_camera_ring_acquire(&s->ring, &slot, &seq)) {
		*out_frame = s->slot_info[slot];
		*out_ready = true;
	}
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_stereo_camera_stream_stats(volatile struct ipc_client_state *ics,
                                      uint64_t stream_id,
                                      struct xrt_stereo_camera_stream_stats *out_stats)
{
	U_ZERO(out_stats);
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&m->lock);
	struct scam_stream *s = stream_by_id_locked(m, ics, stream_id);
	if (s == NULL) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	out_stats->source_frame_rate = m->cams[s->camera].source_rate;
	out_stats->delivered_frame_rate = s->delivered_rate;
	out_stats->frames_published = s->ring.published;
	out_stats->frames_skipped = s->ring.skipped;
	out_stats->frames_acquired = s->ring.acquired;
	out_stats->mean_latency_ns = s->latency_count > 0 ? s->latency_sum_ns / s->latency_count : 0;
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}
