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
 * Privacy (spec §7) — R3, as built. The plug-in never sees any of it:
 *  - consent_evaluate(): every stream start and every calibration read runs
 *    the u_camera_consent policy (aux util, unit-tested with fakes) against
 *    the OS-derived peer executable, in the order of spec §7.1 (stated once
 *    there and in u_camera_consent.h): sharing off (DXR_STEREO_CAMERA=0 or the
 *    user's tray toggle) -> DISABLED; DXR_STEREO_CAMERA_DEV_ALLOW=1 -> allowed
 *    (one WARN); OS camera privacy switch -> PERMISSION_INSUFFICIENT and a
 *    stored Deny -> CONSENT_REFUSED, both for EVERY client; only then a
 *    registered consent-DELEGATING client (the browser, by installer /
 *    `displayxr-cli camera trust`) -> allowed, its own prompt is the consent
 *    (not when the client set DECLINE_DELEGATION, nor for an entry whose
 *    user-writable executable lacks a matching Authenticode signer; a
 *    USER-level entry raises the notice provider once per exe per run);
 *    a stored Allow; else the tray prompt ("<app> wants to use the
 *    3D camera — Allow / Allow once / Deny"), which the host installs with
 *    ipc_server_stereo_camera_set_prompt_provider(). The prompt BLOCKS the
 *    calling client thread (manager lock released, one prompt at a time) for
 *    up to 60 s; unanswered = refused, retryable. Each verdict has its own
 *    result: CONSENT_REFUSED / DISABLED / BUSY / STREAM_ENDED, so a browser
 *    can map consent to NotAllowedError and the rest to NotReadableError.
 *  - client_visible_locked(): the foreground rule, per published frame, cached
 *    250 ms per stream. It applies to window-bearing classes (APP,
 *    PRESENT_OWNER): the peer pid must own a visible top-level window
 *    (Windows EnumWindows; macOS NSRunningApplication not hidden; Linux: no
 *    display connection in the service, always visible). A delegating client
 *    follows its own rule; CAMERA_CONSUMER / DIAG have no window by contract
 *    and are exempt (they got explicit consent instead). Every stream is
 *    suspended while the OS session is locked (ipc_server_stereo_camera_
 *    set_session_locked from the platform notification: WTS session change on
 *    Windows, screenIsLocked / sessionDidResignActive on macOS).
 *  - output_allowed(): RAW is refused to PRESENT_OWNER and to any delegating
 *    client (both expose cameras to third-party content).
 *  - events: XrEventDataStereoCameraStateChangedDXR (effective state: SUSPENDED
 *    while locked or sharing is off) and XrEventDataStereoCameraStreamEndedDXR
 *    are queued per connection (16 deep, oldest dropped) and drained by the
 *    client from xrPollEvent through stereo_camera_poll_event.
 *  - indicator + user kill switch: ipc_server_stereo_camera_get_status() feeds
 *    the tray / menu-bar item ("3D camera in use by <app>"); "Stop camera
 *    sharing" = ipc_server_stereo_camera_stop_all() ends every started stream
 *    (STREAM_ENDED event, acquire -> STREAM_ENDED); the persistent "Share the
 *    3D camera with apps" toggle = ipc_server_stereo_camera_set_sharing()
 *    (off: zero cameras enumerated, DISABLED on start).
 *  - persistentId is HMAC-SHA-256(per-user secret, device | consumer) — the
 *    secret lives in the consent store (§7.5).
 *
 * Rectification (R2) — when the plug-in reports CALIBRATED but not
 * NATIVELY_RECTIFIED, the manager builds a per-camera rectifier at create time
 * from the plug-in's RAW calibration (u_stereo_rectify: Bouguet, zero
 * disparity at infinity, alpha = 0 crop, no convergence shear). It runs ONCE
 * per source frame, on the camera thread, with the manager lock RELEASED, and
 * only while at least one started stream wants RECTIFIED; every RECTIFIED
 * stream then converts from the rectified image, RAW streams from the
 * original. The seam is `struct scam_rectifier`: the geometry and the float
 * maps are backend-neutral; `apply` is the backend. CPU today (fixed-point
 * bilinear LUT: ~0.9 ms GRAY8 / ~1.4 ms NV12 per 1280x480 frame on an M1 Pro,
 * -O2). A GPU backend uploads u_stereo_rectify_build_map() as an RG32F texture
 * and remaps straight into the GPU transports' slots below. Rectified
 * calibration (common f / principal point, rightFromLeft = pure +x baseline)
 * comes from the same geometry, so frames and numbers cannot disagree.
 *
 * Online vertical-alignment refinement (R2) — a device's stored calibration
 * can be slightly off for its frames (a Leia SR laptop: +1.7 px of row
 * residual that OpenCV's own stereoRectify reproduces on the same frame). A
 * per-camera WORKER thread measures the residual dy(y) = a + b (y - cy) on
 * rectified frames the camera thread hands it (a copy of the luma, only when
 * u_stereo_vrefine_due(): every 250 ms for 5 s after an open or an update,
 * then one 3-frame window every 30 s; ~2.7 ms per measurement at -O2, i.e.
 * < 0.4 ms per frame amortised at 30 Hz). When the fitted correction moves by
 * more than 0.2 px it rebuilds the geometry + LUTs with the correction folded
 * in (u_stereo_rectify_input::v_offset / v_slope; ~8 ms, on the worker) and
 * parks them; the camera thread swaps them in between two frames under the
 * lock and bumps calibration_generation, so the rectified calibration a
 * consumer re-reads describes exactly the frames it gets. Clamped (|a| <= 6
 * px, |b| <= 2 px / 100 px), gated (>= 50 matches over >= 3 frames),
 * re-verified on every re-open, and DXR_STEREO_CAMERA_REFINE=0 turns it off.
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
#ifdef XRT_OS_MACOS
#include "server/ipc_server_macos_appkit.h"
#endif

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_stereo_camera.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "shared/ipc_shmem.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_camera_consent.h"
#include "util/u_misc.h"
#include "util/u_stereo_camera.h"
#include "util/u_stereo_rectify.h"
#include "util/u_stereo_vrefine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
DEBUG_GET_ONCE_BOOL_OPTION(stereo_camera_prompt, "DXR_STEREO_CAMERA_PROMPT", true)
DEBUG_GET_ONCE_BOOL_OPTION(stereo_camera_refine, "DXR_STEREO_CAMERA_REFINE", true)
// DEV ONLY (#1842 hardware repro): shorten / lengthen the consent prompt's
// timeout. Read once at service start; 0 / unset = PROMPT_TIMEOUT_MS.
DEBUG_GET_ONCE_NUM_OPTION(stereo_camera_prompt_timeout_ms, "DXR_STEREO_CAMERA_PROMPT_TIMEOUT_MS", 0)

#define MAX_STREAMS (XRT_STEREO_CAMERA_MAX_CAMERAS * XRT_STEREO_CAMERA_MAX_STREAMS_PER_CAMERA)
#define LINGER_NS (2000ll * 1000 * 1000)
#define WAIT_TIMEOUT_NS (100ll * 1000 * 1000)
#define SUSPEND_AFTER_NS (1000ll * 1000 * 1000)
#define REOPEN_BACKOFF_NS (1000ll * 1000 * 1000)
#define STATS_LOG_NS (5000ll * 1000 * 1000)
#define REFINE_SAMPLES 512 //!< matches one measurement may return
#define VISIBILITY_TTL_NS (250ll * 1000 * 1000) //!< foreground-rule cache per stream
#define PROMPT_TIMEOUT_MS 60000u                //!< an unanswered consent prompt = refused
#define PROMPT_TIMEOUT_MIN_MS 1000u             //!< DXR_STEREO_CAMERA_PROMPT_TIMEOUT_MS clamp (dev only)
#define PROMPT_TIMEOUT_MAX_MS 600000u
#define EVQ_CAP 16                              //!< queued events per connection


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

	// R3 privacy facts, settled at create from the OS-derived peer.
	char exe[512];         //!< verified peer executable ("" = unknown)
	long peer_pid;         //!< the pid the foreground rule looks at
	uint32_t client_class; //!< verified class of the creating connection
	bool delegating;       //!< registered consent-delegating client (RAW refusal keys on it)
	//! Its start was allowed BY delegation (trusted, not declined): only such
	//! a stream follows its client's own visibility rule (§7.1.1, §7.2).
	bool delegated;
	int64_t vis_checked_ns;
	bool visible;
	uint32_t consent_why; //!< enum u_camera_consent_why of the start that allowed it
	//! The SERVICE ended this stream (user stop / sharing off / source lost):
	//! stays until destroy; acquire and start report STREAM_ENDED.
	bool ended;
	uint32_t end_reason; //!< enum xrt_stereo_camera_end_reason

	bool started;
	bool allocated;
	uint32_t width, height;
	struct u_stereo_camera_planes layout;
	uint64_t slot_stride;
	uint64_t section_size;
	xrt_shmem_handle_t section;
	//! READ-ONLY handle to the same memory — what every consumer receives
	//! (R3, spec §5.3): a client can map the ring but never write it.
	xrt_shmem_handle_t section_ro;
	uint8_t *map;
	struct scam_wake wake;
	struct u_stereo_camera_ring ring;
	struct u_stereo_camera_decimator dec;
	struct xrt_stereo_camera_frame_info slot_info[U_STEREO_CAMERA_RING_SLOTS];

#ifdef XRT_OS_WINDOWS
	//! SYNCHRONIZE-only duplicate handed to the last get_wake reply. The
	//! generated dispatch duplicates it into the peer AFTER the handler
	//! returns, so it is closed on the next call / stream destroy.
	HANDLE sent_wake_dup;
#endif

	// stats
	float delivered_rate;
	int64_t last_publish_ns;
	uint64_t latency_sum_ns;
	uint64_t latency_count;
};

/*!
 * The rectifier seam (R2). Geometry + maps are backend-neutral; @ref apply is
 * the backend. Owned by one camera, used only on its camera thread.
 */
struct scam_rectifier
{
	struct u_stereo_rectify_result geo;
	//! [0] full resolution (GRAY8, NV12 Y, BGRA8), [1] NV12 UV (half-res, 2 ch).
	struct u_stereo_rectify_lut lut[2];
	//! Rectified output in the SOURCE frame's format (tight layout).
	uint8_t *buf;
	uint64_t buf_size;
	uint32_t buf_format;
	struct u_stereo_camera_planes buf_layout;
	//! Backend: rectify @p in into @p out (planes point into buf). CPU LUT today.
	bool (*apply)(struct scam_rectifier *r,
	              const struct xrt_plugin_stereo_camera_frame *in,
	              struct xrt_plugin_stereo_camera_frame *out);
	// stats
	uint64_t ns_sum;
	uint64_t count;

	/*
	 * Online vertical-alignment refinement. Everything below is guarded by the
	 * manager lock except where noted; the worker never touches geo / lut.
	 */
	bool refine;                      //!< enabled (DXR_STEREO_CAMERA_REFINE)
	struct u_stereo_rectify_input in; //!< the RAW calibration, rebuilt with a correction
	struct u_stereo_vrefine ref;      //!< the controller
	struct u_stereo_vrefine_measure_params mp;
	struct ipc_server_stereo_camera *mgr;
	uint64_t camera_id;
	//! Luma of one rectified SBS frame. Written by the camera thread while
	//! !job_pending, read by the worker while job_pending.
	uint8_t *job_gray;
	bool job_pending;
	int64_t job_ns;
	struct u_stereo_vrefine_sample *samples; //!< worker-only
	//! A rebuilt geometry + LUTs the camera thread swaps in before its next frame.
	bool pending_ready;
	struct u_stereo_rectify_result pending_geo;
	struct u_stereo_rectify_lut pending_lut[2];
	bool quit;
	bool worker_started;
	struct os_thread worker;
	struct os_cond worker_cond;
	uint64_t measure_ns_sum;
	uint32_t measure_count;
	bool stable_logged;
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
	//! R2: set when CALIBRATED and not NATIVELY_RECTIFIED and the geometry is
	//! sane; NULL = no service-side rectification (RAW-flagged fallback).
	struct scam_rectifier *rect;

	struct xrt_plugin_stereo_camera *src; //!< owned by the camera thread
	uint32_t started_count;
	int64_t linger_deadline_ns;
	int64_t last_frame_ns;
	int64_t retry_after_ns;
	float source_rate;
	//! Measured over the first frames after each open; what the descriptor
	//! reports once known (the plug-in's enumerate-time rate is a guess, and
	//! was 2x wrong on the Leia SR tracking camera).
	struct u_stereo_camera_rate_meter rate_meter;
	bool rate_logged;
	int64_t last_stats_log_ns;

	bool thread_started;
	struct os_thread thread;
	struct os_cond cond;
	struct ipc_server_stereo_camera *mgr;
};

//! Per-connection event queue (R3), keyed by the connection's owner token.
struct scam_evq
{
	uint64_t owner;
	struct xrt_stereo_camera_event ev[EVQ_CAP];
	uint32_t head;
	uint32_t count;
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

	// R3 privacy.
	struct u_camera_consent consent;
	//! Serialises consent evaluations (one prompt at a time); never held
	//! together with @ref lock.
	struct os_mutex consent_lock;
	bool sharing_enabled; //!< cached user toggle (the store is the truth)
	bool locked;          //!< OS session locked / switched away
	struct scam_evq evq[IPC_MAX_CLIENTS];
};

//! The one manager of this process, for the service's UI (tray / menu bar).
static struct ipc_server_stereo_camera *g_mgr = NULL;
//! UI providers. Process-wide, not per manager: the tray registers them before
//! ipc_server_main() has created the manager.
static ipc_server_stereo_camera_prompt_fn g_prompt_fn = NULL;
static void *g_prompt_ctx = NULL;
static ipc_server_stereo_camera_indicator_fn g_indicator_fn = NULL;
static void *g_indicator_ctx = NULL;
static ipc_server_stereo_camera_notice_fn g_notice_fn = NULL;
static void *g_notice_ctx = NULL;


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
	if (cls != XRT_CLIENT_CLASS_APP && cls != XRT_CLIENT_CLASS_PRESENT_OWNER && cls != XRT_CLIENT_CLASS_DIAG &&
	    cls != XRT_CLIENT_CLASS_CAMERA_CONSUMER) {
		U_LOG_W("%s: denied — pid %ld is class %s (stereo camera: APP / PRESENT_OWNER / CAMERA_CONSUMER / DIAG).",
		        what, ics->peer_pid, ipc_server_client_class_str(cls));
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	return XRT_SUCCESS;
}

//! The control ops are for the service's own UI and the diagnostic CLI.
static xrt_result_t
require_diag_client(volatile struct ipc_client_state *ics, const char *what)
{
	if (ics->client_state.client_class != XRT_CLIENT_CLASS_DIAG) {
		U_LOG_W("%s: denied — pid %ld is class %s, not DIAG.", what, ics->peer_pid,
		        ipc_server_client_class_str(ics->client_state.client_class));
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	return XRT_SUCCESS;
}

/*
 *
 * R3 privacy: consent, foreground rule, events.
 *
 */

//! The service's view of the OS camera switch (u_camera_consent_store.c).
static bool
env_os_camera_allowed(void *ctx, const char *exe)
{
	(void)ctx;
	return u_camera_consent_os_camera_allowed(exe);
}

//! The tray prompt, if the host installed one.
static enum u_camera_consent_prompt_answer
env_prompt(void *ctx, const char *exe, const char *app_name, long pid, uint32_t timeout_ms)
{
	(void)ctx;
	ipc_server_stereo_camera_prompt_fn fn = g_prompt_fn;
	if (fn == NULL) {
		return U_CAMERA_CONSENT_PROMPT_UNAVAILABLE;
	}
	return fn(g_prompt_ctx, exe, app_name, pid, timeout_ms);
}

//! §7.1.1: may a non-admin process replace this executable? (POSIX: no — path-only for now.)
static bool
env_path_user_writable(void *ctx, const char *exe)
{
	(void)ctx;
	return u_camera_consent_path_user_writable(exe);
}

//! §7.1.1: the executable's valid code signer (Windows Authenticode; none on POSIX).
static bool
env_exe_signer(void *ctx, const char *exe, char *out, size_t cap)
{
	(void)ctx;
	return u_camera_consent_exe_signer(exe, out, cap);
}

//! §7.1.1: first use of a USER-level delegation — the host's notice (tray balloon), if any.
static void
env_user_delegation_notice(void *ctx, const char *exe, const char *app_name)
{
	(void)ctx;
	ipc_server_stereo_camera_notice_fn fn = g_notice_fn;
	if (fn != NULL) {
		fn(g_notice_ctx, exe, app_name);
	}
}

static const struct u_camera_consent_env_ops env_ops = {
    .os_camera_allowed = env_os_camera_allowed,
    .prompt = env_prompt,
    .path_user_writable = env_path_user_writable,
    .exe_signer = env_exe_signer,
    .user_delegation_notice = env_user_delegation_notice,
};

static xrt_result_t
verdict_to_xret(enum u_camera_consent_verdict v)
{
	switch (v) {
	case U_CAMERA_CONSENT_ALLOWED: return XRT_SUCCESS;
	case U_CAMERA_CONSENT_DISABLED: return XRT_ERROR_STEREO_CAMERA_DISABLED;
	case U_CAMERA_CONSENT_OS_DENIED: return XRT_ERROR_NOT_AUTHORIZED;
	case U_CAMERA_CONSENT_REFUSED:
	default: return XRT_ERROR_STEREO_CAMERA_CONSENT_REFUSED;
	}
}

/*!
 * Spec §7.1: the authorisation point for a stream start and a calibration
 * read. Manager lock NOT held (the prompt may block for up to a minute);
 * evaluations are serialised on consent_lock so two apps never race two
 * dialogs. One WARN per decision names the peer and the rule.
 */
static xrt_result_t
consent_evaluate(struct ipc_server_stereo_camera *m,
                 volatile struct ipc_client_state *ics,
                 const char *exe,
                 const char *what,
                 struct u_camera_consent_decision *out)
{
	char app_name[XRT_MAX_APPLICATION_NAME_SIZE];
	snprintf(app_name, sizeof(app_name), "%s", (const char *)ics->client_state.info.application_name);
	// Spec v3: XR_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION_BIT_DXR, as declared
	// at xrCreateInstance (it only ever restricts, so the claim is taken as is).
	uint32_t flags = 0;
	if ((ics->client_state.info.stereo_camera_client_flags & XRT_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION) != 0) {
		flags |= U_CAMERA_CONSENT_FLAG_DECLINE_DELEGATION;
	}
	os_mutex_lock(&m->consent_lock);
	u_camera_consent_evaluate(&m->consent, exe, app_name, ics->peer_pid, flags, out);
	os_mutex_unlock(&m->consent_lock);
	xrt_result_t xret = verdict_to_xret(out->verdict);
	// When a registered entry was not applied, say why (declined / untrusted).
	const char *skip = u_camera_consent_delegation_skip_str(out->delegation_skip);
	U_LOG_W("%s: %s for %s (pid %ld, \"%s\") — %s%s%s%s", what, xret == XRT_SUCCESS ? "ALLOWED" : "REFUSED",
	        exe[0] ? exe : "?", ics->peer_pid, app_name, u_camera_consent_why_str(out->why),
	        skip[0] ? " (delegation not applied: " : "", skip, skip[0] ? ")" : "");
	return xret;
}

//! Is @p exe a registered consent-delegating client (cheap store read)?
static bool
exe_is_delegating(struct ipc_server_stereo_camera *m, const char *exe)
{
	return u_camera_consent_is_registered_delegating(m->consent.store, m->consent.store_ctx, exe);
}

//! Does @p pid own a visible top-level window right now (platform query)?
#ifdef XRT_OS_WINDOWS
struct vis_probe
{
	DWORD pid;
	bool visible;
};

static BOOL CALLBACK
vis_enum_cb(HWND hwnd, LPARAM lp)
{
	struct vis_probe *vp = (struct vis_probe *)lp;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == vp->pid && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
		vp->visible = true;
		return FALSE;
	}
	return TRUE;
}
#endif

static bool
pid_has_visible_window(long pid)
{
#if defined(XRT_OS_WINDOWS)
	struct vis_probe vp = {.pid = (DWORD)pid, .visible = false};
	EnumWindows(vis_enum_cb, (LPARAM)&vp);
	return vp.visible;
#elif defined(XRT_OS_MACOS)
	return ipc_server_macos_pid_app_visible(pid);
#else
	(void)pid;
	return true; // no display connection in the service: the OS lock is the only gate
#endif
}

/*!
 * Spec §7.2 foreground rule, evaluated per published frame with a 250 ms
 * cache. Window-bearing classes must own a visible top-level window; a
 * client allowed BY delegation follows its own rule (a registered client
 * that declined delegation, or whose entry was not trusted, is an ordinary
 * app here too); CAMERA_CONSUMER / DIAG have no window by contract and are
 * exempt. The OS-lock gate is separate (publish checks m->locked first).
 */
static bool
client_visible_locked(struct scam_stream *s, int64_t now)
{
	if (s->delegated || s->client_class == XRT_CLIENT_CLASS_CAMERA_CONSUMER ||
	    s->client_class == XRT_CLIENT_CLASS_DIAG || s->peer_pid <= 0) {
		return true;
	}
	if (s->vis_checked_ns != 0 && now - s->vis_checked_ns < VISIBILITY_TTL_NS) {
		return s->visible;
	}
	bool v = pid_has_visible_window(s->peer_pid);
	if (s->vis_checked_ns != 0 && v != s->visible) {
		U_LOG_W("stereo camera: stream %llu (%s) %s — foreground rule", (unsigned long long)s->id, s->consumer,
		        v ? "visible again, frames resume" : "has no visible window, frames suspended");
	}
	s->vis_checked_ns = now;
	s->visible = v;
	return v;
}

//! RAW frames never reach third-party content: refused to PRESENT_OWNER and
//! to every consent-delegating client (a browser exposes cameras to pages).
static bool
output_allowed(volatile struct ipc_client_state *ics, bool delegating, uint32_t output)
{
	return output != XRT_STEREO_CAMERA_OUTPUT_RAW ||
	       (ics->client_state.client_class != XRT_CLIENT_CLASS_PRESENT_OWNER && !delegating);
}

//! The state a client is told: SUSPENDED while the session is locked or sharing is off.
static uint32_t
effective_state_locked(const struct ipc_server_stereo_camera *m, const struct scam_camera *cam)
{
	if (m->locked || !m->sharing_enabled) {
		return XRT_STEREO_CAMERA_STATE_SUSPENDED;
	}
	return cam->state;
}

static struct scam_evq *
evq_for_owner_locked(struct ipc_server_stereo_camera *m, uint64_t owner, bool create)
{
	struct scam_evq *free_slot = NULL;
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		if (m->evq[i].owner == owner) {
			return &m->evq[i];
		}
		if (m->evq[i].owner == 0 && free_slot == NULL) {
			free_slot = &m->evq[i];
		}
	}
	if (!create || free_slot == NULL) {
		return NULL;
	}
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->owner = owner;
	return free_slot;
}

static void
evq_push_locked(struct ipc_server_stereo_camera *m, uint64_t owner, const struct xrt_stereo_camera_event *ev)
{
	struct scam_evq *q = evq_for_owner_locked(m, owner, true);
	if (q == NULL) {
		return;
	}
	if (q->count == EVQ_CAP) {
		q->head = (q->head + 1) % EVQ_CAP; // drop the oldest
		q->count--;
	}
	q->ev[(q->head + q->count) % EVQ_CAP] = *ev;
	q->count++;
}

//! Queue a CAMERA_STATE event (effective state) to every connection with a stream on @p cam.
static void
notify_camera_state_locked(struct ipc_server_stereo_camera *m, const struct scam_camera *cam)
{
	struct xrt_stereo_camera_event ev = {
	    .kind = XRT_STEREO_CAMERA_EVENT_CAMERA_STATE,
	    .value = effective_state_locked(m, cam),
	    .camera_id = cam->camera_id,
	};
	uint64_t done[MAX_STREAMS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		const struct scam_stream *s = &m->streams[i];
		if (!s->used || s->camera != cam->index) {
			continue;
		}
		bool dup = false;
		for (uint32_t k = 0; k < n; k++) {
			dup |= done[k] == s->owner;
		}
		if (!dup) {
			done[n++] = s->owner;
			evq_push_locked(m, s->owner, &ev);
		}
	}
}

static void
notify_all_cameras_locked(struct ipc_server_stereo_camera *m)
{
	for (uint32_t i = 0; i < m->camera_count; i++) {
		notify_camera_state_locked(m, &m->cams[i]);
	}
}

static void
indicator_changed(struct ipc_server_stereo_camera *m)
{
	(void)m;
	ipc_server_stereo_camera_indicator_fn fn = g_indicator_fn;
	if (fn != NULL) {
		fn(g_indicator_ctx);
	}
}

static bool
camera_can_rectify(const struct scam_camera *cam)
{
	// A natively rectified source, or a calibrated one the service rectifies
	// (R2). A calibrated camera whose geometry was rejected stays RAW-only.
	return (cam->info.flags & XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED) != 0 || cam->rect != NULL;
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
	notify_camera_state_locked(cam->mgr, cam);
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
 * Rectifier (R2).
 *
 */

//! CPU backend: LUT remap per plane into the rectifier's buffer.
static bool
rectify_apply_cpu(struct scam_rectifier *r,
                  const struct xrt_plugin_stereo_camera_frame *in,
                  struct xrt_plugin_stereo_camera_frame *out)
{
	if (in->width != 2 * r->geo.width || in->height != r->geo.height || in->planes[0] == NULL) {
		return false;
	}
	if (r->buf == NULL || r->buf_format != in->format) {
		struct u_stereo_camera_planes lay;
		if (!u_stereo_camera_layout(in->format, in->width, in->height, &lay)) {
			return false;
		}
		if (lay.size > r->buf_size) {
			uint8_t *nb = realloc(r->buf, (size_t)lay.size);
			if (nb == NULL) {
				return false;
			}
			r->buf = nb;
			r->buf_size = lay.size;
		}
		r->buf_layout = lay;
		r->buf_format = in->format;
	}
	int64_t t0 = os_monotonic_get_ns();
	const struct u_stereo_camera_planes *lay = &r->buf_layout;
	switch (in->format) {
	case XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8:
		u_stereo_rectify_lut_apply(&r->lut[0], 1, in->planes[0], in->pitches[0], r->buf + lay->offset[0],
		                           lay->pitch[0]);
		break;
	case XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12:
		if (in->planes[1] == NULL) {
			return false;
		}
		u_stereo_rectify_lut_apply(&r->lut[0], 1, in->planes[0], in->pitches[0], r->buf + lay->offset[0],
		                           lay->pitch[0]);
		u_stereo_rectify_lut_apply(&r->lut[1], 2, in->planes[1], in->pitches[1], r->buf + lay->offset[1],
		                           lay->pitch[1]);
		break;
	case XRT_PLUGIN_STEREO_CAMERA_FORMAT_BGRA8:
		u_stereo_rectify_lut_apply(&r->lut[0], 4, in->planes[0], in->pitches[0], r->buf + lay->offset[0],
		                           lay->pitch[0]);
		break;
	default: return false;
	}
	r->ns_sum += (uint64_t)(os_monotonic_get_ns() - t0);
	r->count++;
	*out = *in;
	out->planes[0] = r->buf + lay->offset[0];
	out->pitches[0] = lay->pitch[0];
	out->planes[1] = lay->plane_count > 1 ? r->buf + lay->offset[1] : NULL;
	out->pitches[1] = lay->plane_count > 1 ? lay->pitch[1] : 0;
	return true;
}

static void
rectifier_destroy(struct scam_rectifier **rp)
{
	struct scam_rectifier *r = *rp;
	if (r == NULL) {
		return;
	}
	u_stereo_rectify_lut_fini(&r->lut[0]);
	u_stereo_rectify_lut_fini(&r->lut[1]);
	if (r->pending_ready) {
		u_stereo_rectify_lut_fini(&r->pending_lut[0]);
		u_stereo_rectify_lut_fini(&r->pending_lut[1]);
	}
	if (r->refine) {
		os_cond_destroy(&r->worker_cond);
	}
	free(r->job_gray);
	free(r->samples);
	free(r->buf);
	free(r);
	*rp = NULL;
}

//! Stop and join the refinement worker (manager lock NOT held).
static void
rectifier_stop_worker(struct ipc_server_stereo_camera *m, struct scam_rectifier *r)
{
	if (r == NULL || !r->worker_started) {
		return;
	}
	os_mutex_lock(&m->lock);
	r->quit = true;
	os_cond_signal(&r->worker_cond);
	os_mutex_unlock(&m->lock);
	os_thread_join(&r->worker);
	os_thread_destroy(&r->worker);
	r->worker_started = false;
}

/*!
 * Build the rectifier from the plug-in's RAW calibration: per-eye K +
 * distortion at image_width x image_height (rescaled to the frame's eye size
 * when they differ), OpenCV extrinsics x_R = R x_L + T.
 */
static struct scam_rectifier *
rectifier_create(const struct scam_camera *cam)
{
	const struct xrt_plugin_stereo_camera_calibration *c = &cam->calib;
	struct u_stereo_rectify_input in;
	memset(&in, 0, sizeof(in));
	in.width = cam->info.eye_width;
	in.height = cam->info.eye_height;
	in.calib_width = c->image_width;
	in.calib_height = c->image_height;
	for (int e = 0; e < 2; e++) {
		in.eye[e].fx = c->k[e][0];
		in.eye[e].fy = c->k[e][1];
		in.eye[e].cx = c->k[e][2];
		in.eye[e].cy = c->k[e][3];
		in.eye[e].model = c->distortion_model;
		for (int k = 0; k < 8; k++) {
			in.eye[e].d[k] = c->distortion[e][k];
		}
	}
	memcpy(in.R, c->rotation_right_from_left, sizeof(in.R));
	memcpy(in.T, c->translation_right_from_left_mm, sizeof(in.T));

	int64_t t0 = os_monotonic_get_ns();
	struct scam_rectifier *r = U_TYPED_CALLOC(struct scam_rectifier);
	if (!u_stereo_rectify_compute(&in, &r->geo)) {
		U_LOG_W(
		    "stereo camera %llu: calibration rejected by the rectifier (degenerate or vertical pair) — "
		    "RAW only",
		    (unsigned long long)cam->camera_id);
		free(r);
		return NULL;
	}
	if (!u_stereo_rectify_lut_init(&r->lut[0], &r->geo, 1) || !u_stereo_rectify_lut_init(&r->lut[1], &r->geo, 2)) {
		U_LOG_W("stereo camera %llu: rectifier LUT allocation failed — RAW only",
		        (unsigned long long)cam->camera_id);
		rectifier_destroy(&r);
		return NULL;
	}
	r->apply = rectify_apply_cpu;
	r->in = in;
	r->mgr = cam->mgr;
	r->camera_id = cam->camera_id;
	r->refine = debug_get_bool_option_stereo_camera_refine();
	if (r->refine) {
		r->job_gray = malloc((size_t)2 * in.width * in.height);
		r->samples = U_TYPED_ARRAY_CALLOC(struct u_stereo_vrefine_sample, REFINE_SAMPLES);
		if (r->job_gray == NULL || r->samples == NULL) {
			free(r->job_gray);
			free(r->samples);
			r->job_gray = NULL;
			r->samples = NULL;
			r->refine = false;
		}
	}
	if (r->refine) {
		os_cond_init(&r->worker_cond);
		struct u_stereo_vrefine_config cfg;
		u_stereo_vrefine_config_defaults(&cfg, in.height, r->geo.cy);
		u_stereo_vrefine_init(&r->ref, &cfg);
		// The disparity window: a subject at 0.3 m (the probe uses 0.4 m).
		double dmax = r->geo.f * r->geo.baseline / 300.0;
		dmax = dmax < 24.0 ? 24.0 : dmax > in.width / 2.0 ? in.width / 2.0 : dmax;
		u_stereo_vrefine_measure_defaults(&r->mp, (uint32_t)dmax);
	} else {
		U_LOG_W("stereo camera %llu: vertical-alignment refinement OFF (DXR_STEREO_CAMERA_REFINE=0)",
		        (unsigned long long)cam->camera_id);
	}
	if (r->geo.t_rect[0] > 0.0) {
		// Contract: SBS left half = the camera's LEFT lens, T.x < 0 (xrt_plugin.h).
		U_LOG_W(
		    "stereo camera %llu: calibration puts the right camera at -x (T.x > 0) — the plug-in's SBS halves "
		    "look swapped; rectified disparities will be NEGATIVE",
		    (unsigned long long)cam->camera_id);
	}
	U_LOG_W(
	    "stereo camera %llu: rectifier ready (CPU) — f %.2f px, principal point (%.2f, %.2f), baseline %.2f mm, "
	    "valid-region zoom x%.4f, %u/%u valid taps, built in %.1f ms",
	    (unsigned long long)cam->camera_id, r->geo.f, r->geo.cx, r->geo.cy, r->geo.baseline, r->geo.crop_scale,
	    r->lut[0].valid, r->lut[0].width * r->lut[0].height, (double)(os_monotonic_get_ns() - t0) * 1e-6);
	return r;
}

/*!
 * The refinement worker: measures the frame the camera thread parked, feeds
 * the controller and, when the correction moved, rebuilds geometry + LUTs off
 * the frame path and parks them for the camera thread to swap in.
 */
static void *
refine_worker(void *ptr)
{
	struct scam_rectifier *r = (struct scam_rectifier *)ptr;
	struct ipc_server_stereo_camera *m = r->mgr;
	const uint32_t w = r->in.width, h = r->in.height;

	os_mutex_lock(&m->lock);
	while (!r->quit && !m->shutting_down) {
		if (!r->job_pending) {
			os_cond_wait(&r->worker_cond, &m->lock);
			continue;
		}
		const int64_t t_job = r->job_ns;
		const struct u_stereo_vrefine_measure_params mp = r->mp;
		os_mutex_unlock(&m->lock);

		int64_t t0 = os_monotonic_get_ns();
		uint32_t n = u_stereo_vrefine_measure(r->job_gray, 2 * w, w, h, &mp, r->samples, REFINE_SAMPLES);
		int64_t t1 = os_monotonic_get_ns();

		os_mutex_lock(&m->lock);
		r->measure_ns_sum += (uint64_t)(t1 - t0);
		r->measure_count++;
		enum u_stereo_vrefine_result res = u_stereo_vrefine_push(&r->ref, t_job, r->samples, n);
		if (res != U_STEREO_VREFINE_UPDATED) {
			// One INFO when a correction is confirmed, then one per slow-cadence
			// window (every 30 s) — never one per settling window.
			bool settling = t_job - r->ref.phase_start_ns < r->ref.cfg.fast_phase_ns;
			if (res == U_STEREO_VREFINE_STABLE && (!r->stable_logged || !settling)) {
				r->stable_logged = true;
				U_LOG_I(
				    "stereo camera %llu: row alignment check — residual %+.2f px over %u matches, "
				    "correction a %+.2f px b %+.3f px/100 px unchanged",
				    (unsigned long long)r->camera_id, r->ref.last_residual_dy, r->ref.last_matches,
				    r->ref.a, r->ref.b * 100.0);
			}
			r->job_pending = false;
			continue;
		}
		r->stable_logged = false;
		const double a = r->ref.a, b = r->ref.b, residual = r->ref.last_residual_dy;
		const uint32_t matches = r->ref.last_matches, updates = r->ref.updates;
		struct u_stereo_rectify_input in = r->in;
		in.v_offset = a / r->geo.f; // a is in pixels of the frames currently delivered
		in.v_slope = b;
		os_mutex_unlock(&m->lock);

		int64_t b0 = os_monotonic_get_ns();
		struct u_stereo_rectify_result g;
		struct u_stereo_rectify_lut lut[2];
		memset(lut, 0, sizeof(lut));
		bool ok = u_stereo_rectify_compute(&in, &g) && u_stereo_rectify_lut_init(&lut[0], &g, 1) &&
		          u_stereo_rectify_lut_init(&lut[1], &g, 2);
		double build_ms = (double)(os_monotonic_get_ns() - b0) * 1e-6;

		os_mutex_lock(&m->lock);
		if (!ok) {
			u_stereo_rectify_lut_fini(&lut[0]);
			u_stereo_rectify_lut_fini(&lut[1]);
			u_stereo_vrefine_revert(&r->ref);
			U_LOG_W(
			    "stereo camera %llu: vertical correction a %+.2f px b %+.3f px/100 px rejected by the "
			    "rectifier — kept the previous maps",
			    (unsigned long long)r->camera_id, a, b * 100.0);
		} else {
			if (r->pending_ready) {
				u_stereo_rectify_lut_fini(&r->pending_lut[0]);
				u_stereo_rectify_lut_fini(&r->pending_lut[1]);
			}
			r->pending_geo = g;
			r->pending_lut[0] = lut[0];
			r->pending_lut[1] = lut[1];
			r->pending_ready = true;
			if (updates == 1) {
				U_LOG_W(
				    "stereo camera %llu: vertical-alignment refinement APPLIED — the calibration left "
				    "the rows %+.2f px off (fit %+.2f px %+.3f px/100 px about the centre row, %u "
				    "matches); correction a %+.2f px, b %+.3f px/100 px, maps rebuilt in %.1f ms",
				    (unsigned long long)r->camera_id, r->ref.initial_dy, r->ref.initial_a,
				    r->ref.initial_b * 100.0, matches, a, b * 100.0, build_ms);
			} else {
				U_LOG_I(
				    "stereo camera %llu: vertical correction updated — residual %+.2f px over %u "
				    "matches; now a %+.2f px, b %+.3f px/100 px (update %u, %.1f ms)",
				    (unsigned long long)r->camera_id, residual, matches, a, b * 100.0, updates,
				    build_ms);
			}
		}
		r->job_pending = false;
	}
	os_mutex_unlock(&m->lock);
	return NULL;
}

/*!
 * Camera thread, lock held, before rectifying a frame: swap in maps the worker
 * rebuilt. The old LUTs are only ever read by this thread, so they can go now.
 */
static void
rectifier_swap_pending_locked(struct scam_camera *cam)
{
	struct scam_rectifier *r = cam->rect;
	if (r == NULL || !r->pending_ready) {
		return;
	}
	u_stereo_rectify_lut_fini(&r->lut[0]);
	u_stereo_rectify_lut_fini(&r->lut[1]);
	r->geo = r->pending_geo;
	r->lut[0] = r->pending_lut[0];
	r->lut[1] = r->pending_lut[1];
	r->pending_ready = false;
	// Consumers re-read the rectified calibration (f / principal point move
	// by the alpha = 0 re-crop) on the next frame's generation change.
	cam->calibration_generation++;
}

/*!
 * Camera thread, lock held: should this rectified frame be measured? Starts
 * the worker on first use.
 */
static bool
rectifier_want_sample_locked(struct scam_rectifier *r, int64_t now)
{
	if (r == NULL || !r->refine || r->job_pending || r->pending_ready || r->quit) {
		return false;
	}
	if (!u_stereo_vrefine_due(&r->ref, now)) {
		return false;
	}
	if (!r->worker_started) {
		if (os_thread_init(&r->worker) != 0 || os_thread_start(&r->worker, refine_worker, r) != 0) {
			U_LOG_W("stereo camera %llu: refinement worker failed to start — refinement OFF",
			        (unsigned long long)r->camera_id);
			r->refine = false;
			return false;
		}
		r->worker_started = true;
	}
	return true;
}

//! Camera thread, lock NOT held: copy the luma of rectified frame @p rf.
static bool
rectifier_copy_luma(struct scam_rectifier *r, const struct xrt_plugin_stereo_camera_frame *rf)
{
	const uint32_t W = 2 * r->in.width, H = r->in.height;
	if (rf->width != W || rf->height != H || rf->planes[0] == NULL) {
		return false;
	}
	for (uint32_t y = 0; y < H; y++) {
		const uint8_t *src = rf->planes[0] + (size_t)y * rf->pitches[0];
		uint8_t *dst = r->job_gray + (size_t)y * W;
		if (rf->format == XRT_PLUGIN_STEREO_CAMERA_FORMAT_BGRA8) {
			for (uint32_t x = 0; x < W; x++) {
				const uint8_t *p = src + 4 * (size_t)x;
				dst[x] = (uint8_t)((p[0] + 2 * p[1] + p[2] + 2) >> 2); // B + 2G + R: luma enough
			}
		} else {
			memcpy(dst, src, W); // GRAY8, or NV12's Y plane
		}
	}
	return true;
}

//! Does any started stream on @p cam want the service's rectified image?
static bool
camera_wants_rectified_locked(struct ipc_server_stereo_camera *m, const struct scam_camera *cam)
{
	if (cam->rect == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		const struct scam_stream *s = &m->streams[i];
		if (s->used && s->started && s->camera == cam->index &&
		    s->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED) {
			return true;
		}
	}
	return false;
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
	ipc_shmem_close_handle(&s->section_ro);
	wake_destroy(&s->wake);
#ifdef XRT_OS_WINDOWS
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
	s->section_ro = XRT_SHMEM_HANDLE_INVALID;
	xrt_result_t xret = ipc_shmem_create_with_readonly((size_t)s->section_size, &s->section, &map, &s->section_ro);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	s->map = map;
	memset(s->map, 0, (size_t)s->section_size);
	if (!wake_create(&s->wake)) {
		stream_free_storage(s);
		return XRT_ERROR_ALLOCATION;
	}
	u_stereo_camera_ring_init(&s->ring);
	s->allocated = true;
	return XRT_SUCCESS;
}


//! The descriptor's rate: measured once known, else the plug-in's (0 = unknown).
static float
source_rate_hz(const struct scam_camera *cam)
{
	return cam->rate_meter.rate > 0.0f ? cam->rate_meter.rate : cam->info.max_frame_rate;
}


/*
 *
 * Camera thread.
 *
 */

/*!
 * Fan one source frame out. @p rf is the service-rectified image of @p f (NULL
 * when the camera has no rectifier or nobody wanted it this frame).
 */
static void
publish_locked(struct ipc_server_stereo_camera *m,
               struct scam_camera *cam,
               const struct xrt_plugin_stereo_camera_frame *f,
               const struct xrt_plugin_stereo_camera_frame *rf,
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

	if (u_stereo_camera_rate_meter_push(&cam->rate_meter, f->time_ns > 0 ? f->time_ns : now)) {
		float adv = cam->info.max_frame_rate;
		bool off = adv <= 0.0f || fabsf(cam->rate_meter.rate - adv) > 0.1f * adv;
		if (!cam->rate_logged) {
			cam->rate_logged = true;
			U_LOG_W(
			    "stereo camera %llu: measured source rate %.1f Hz over %d frames (plug-in advertised %.1f "
			    "Hz%s)%s",
			    (unsigned long long)cam->camera_id, cam->rate_meter.rate, U_STEREO_CAMERA_RATE_WINDOW, adv,
			    adv > 0.0f ? "" : " = unknown",
			    off ? " — the descriptor now reports the measured rate" : "");
		} else {
			U_LOG_I("stereo camera %llu: measured source rate %.1f Hz", (unsigned long long)cam->camera_id,
			        cam->rate_meter.rate);
		}
	}

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
		// Spec §7.2: nothing while the OS session is locked / switched away,
		// and nothing to a window-bearing client with no visible window. The
		// pinned slot is cleared so the last image does not linger.
		if (m->locked || !m->sharing_enabled || !client_visible_locked(s, now)) {
			u_stereo_camera_ring_clear(&s->ring);
			continue;
		}
		// A RECTIFIED stream on a service-rectified camera takes the rectified
		// image; if it was not produced this frame (the stream started while the
		// camera thread was rectifying without it), skip — never RAW pixels
		// labelled RECTIFIED.
		const struct xrt_plugin_stereo_camera_frame *src = f;
		if (s->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && cam->rect != NULL) {
			if (rf == NULL) {
				continue;
			}
			src = rf;
		}
		if (!u_stereo_camera_decimator_accept(&s->dec, f->time_ns)) {
			continue;
		}
		int32_t slot = u_stereo_camera_ring_begin_write(&s->ring);
		if (slot < 0) {
			continue;
		}
		uint8_t *dst = s->map + (uint64_t)slot * s->slot_stride;
		if (!u_stereo_camera_convert(src->format, src->planes, src->pitches, s->req.format, dst, &s->layout,
		                             src->width, src->height)) {
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
		double rect_ms = (cam->rect != NULL && cam->rect->count > 0)
		                     ? (double)cam->rect->ns_sum / (double)cam->rect->count * 1e-6
		                     : 0.0;
		U_LOG_I("stereo camera %llu: source %.1f Hz, %u started stream(s), rectify %.3f ms/frame (%llu frames)",
		        (unsigned long long)cam->camera_id, cam->source_rate, cam->started_count, rect_ms,
		        (unsigned long long)(cam->rect != NULL ? cam->rect->count : 0));
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
			u_stereo_camera_rate_meter_restart(&cam->rate_meter);
			if (cam->rect != NULL && cam->rect->refine) {
				// Keep the correction, re-verify it at the settling cadence.
				u_stereo_vrefine_restart(&cam->rect->ref, os_monotonic_get_ns());
			}
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
		case XRT_PLUGIN_STEREO_CAMERA_WAIT_OK: {
			// R2: rectify once per source frame, outside the lock (only this
			// thread touches cam->rect after create), only if someone wants it.
			struct xrt_plugin_stereo_camera_frame rf;
			const struct xrt_plugin_stereo_camera_frame *rfp = NULL;
			if (camera_wants_rectified_locked(m, cam)) {
				struct scam_rectifier *rect = cam->rect;
				rectifier_swap_pending_locked(cam);
				bool sample = rectifier_want_sample_locked(rect, now);
				os_mutex_unlock(&m->lock);
				bool ok = rect->apply(rect, &f, &rf);
				bool copied = ok && sample && rectifier_copy_luma(rect, &rf);
				os_mutex_lock(&m->lock);
				if (copied) {
					rect->job_ns = f.time_ns > 0 ? f.time_ns : now;
					rect->job_pending = true;
					os_cond_signal(&rect->worker_cond);
				}
				now = os_monotonic_get_ns();
				rfp = ok ? &rf : NULL;
			}
			publish_locked(m, cam, &f, rfp, now);
			iface->stereo_camera_release_frame(src);
			break;
		}
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
	os_mutex_init(&m->consent_lock);
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		m->streams[i].section = XRT_SHMEM_HANDLE_INVALID;
		m->streams[i].section_ro = XRT_SHMEM_HANDLE_INVALID;
	}

	// R3: the consent policy over the real store; the prompt arrives later
	// from the host (tray / menu bar) through set_prompt_provider.
	u_camera_consent_init(&m->consent, u_camera_consent_store_default(), NULL, &env_ops, m);
	m->consent.kill_switch = !debug_get_bool_option_stereo_camera_enabled();
	m->consent.dev_override = debug_get_bool_option_stereo_camera_dev_allow();
	m->consent.prompt_enabled = debug_get_bool_option_stereo_camera_prompt();
	m->consent.prompt_timeout_ms = PROMPT_TIMEOUT_MS;
	long dev_timeout = debug_get_num_option_stereo_camera_prompt_timeout_ms();
	if (dev_timeout > 0) {
		// Clamped so a typo can neither make the prompt unanswerable nor
		// park a client thread for hours.
		uint32_t t = dev_timeout > (long)PROMPT_TIMEOUT_MAX_MS ? PROMPT_TIMEOUT_MAX_MS : (uint32_t)dev_timeout;
		if (t < PROMPT_TIMEOUT_MIN_MS) {
			t = PROMPT_TIMEOUT_MIN_MS;
		}
		m->consent.prompt_timeout_ms = t;
		U_LOG_W(
		    "stereo camera: consent prompt timeout %u ms (DXR_STEREO_CAMERA_PROMPT_TIMEOUT_MS — development "
		    "override; default %u ms)",
		    t, PROMPT_TIMEOUT_MS);
	}
	m->sharing_enabled = u_camera_consent_sharing_enabled(&m->consent);
	g_mgr = m;

	if (!debug_get_bool_option_stereo_camera_enabled()) {
		U_LOG_W("stereo camera: disabled by DXR_STEREO_CAMERA=0 (kill switch) — zero cameras");
		return m;
	}
	if (!m->sharing_enabled) {
		U_LOG_W("stereo camera: camera sharing is OFF (user toggle) — zero cameras until it is switched on");
	}
	if (!m->consent.prompt_enabled) {
		U_LOG_W("stereo camera: consent prompt disabled (DXR_STEREO_CAMERA_PROMPT=0) — an app without a stored "
		        "or delegated consent is refused");
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
		if (info->eye_width == 0 || info->eye_height == 0 || (info->eye_width & 1u) ||
		    (info->eye_height & 1u) || info->native_format < XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8 ||
		    info->native_format > XRT_PLUGIN_STEREO_CAMERA_FORMAT_BGRA8) {
			U_LOG_W(
			    "stereo camera: plug-in camera %u has a malformed description (%ux%u, format %u) — "
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
				U_LOG_W(
				    "stereo camera: \"%s\" claims CALIBRATED but returned no calibration — RAW only",
				    info->display_name);
				cam->info.flags &= ~XRT_PLUGIN_STEREO_CAMERA_CALIBRATED;
			}
		}
		if (cam->have_calib && (cam->info.flags & XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED) == 0) {
			cam->rect = rectifier_create(cam);
		}
		if (!(info->max_frame_rate >= 0.0f && info->max_frame_rate <= 1000.0f)) {
			cam->info.max_frame_rate = 0.0f; // NaN / negative / absurd = unknown
		}
		char rate[48];
		if (cam->info.max_frame_rate > 0.0f) {
			snprintf(rate, sizeof(rate), "advertised %.1f Hz", cam->info.max_frame_rate);
		} else {
			snprintf(rate, sizeof(rate), "rate unknown");
		}
		U_LOG_W(
		    "stereo camera %llu: \"%s\" %ux%u per eye, %s (measured on first open), flags 0x%x, native "
		    "format %u",
		    (unsigned long long)cam->camera_id, info->display_name, info->eye_width, info->eye_height, rate,
		    info->flags, info->native_format);
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
		rectifier_stop_worker(m, m->cams[i].rect);
		rectifier_destroy(&m->cams[i].rect);
	}
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		if (m->streams[i].used) {
			stream_free_storage(&m->streams[i]);
		}
	}
	if (g_mgr == m) {
		g_mgr = NULL;
	}
	os_mutex_destroy(&m->consent_lock);
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

//! The SERVICE ends a started stream: stopped for good, the owner is told.
static void
stream_end_locked(struct ipc_server_stereo_camera *m, struct scam_stream *s, uint32_t reason)
{
	if (!s->used || s->ended) {
		return;
	}
	stream_stop_locked(s, &m->cams[s->camera]);
	s->ended = true;
	s->end_reason = reason;
	struct xrt_stereo_camera_event ev = {
	    .kind = XRT_STEREO_CAMERA_EVENT_STREAM_ENDED,
	    .value = reason,
	    .camera_id = m->cams[s->camera].camera_id,
	    .stream_id = s->id,
	};
	evq_push_locked(m, s->owner, &ev);
	wake_signal(&s->wake); // a consumer blocked on the wake handle re-polls and sees ENDED
}

static void
stream_destroy_locked(struct ipc_server_stereo_camera *m, struct scam_stream *s)
{
	stream_stop_locked(s, &m->cams[s->camera]);
	stream_free_storage(s);
	memset(s, 0, sizeof(*s));
	s->section = XRT_SHMEM_HANDLE_INVALID;
	s->section_ro = XRT_SHMEM_HANDLE_INVALID;
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
	struct scam_evq *q = evq_for_owner_locked(m, ics->stereo_camera_owner, false);
	if (q != NULL) {
		memset(q, 0, sizeof(*q));
	}
	os_mutex_unlock(&m->lock);
	ics->stereo_camera_owner = 0;
	indicator_changed(m);
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
	// Spec §2 / §7.4: sharing off = zero cameras, indistinguishable from none.
	*out_count = m->sharing_enabled ? m->camera_count : 0;
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
	// Spec §7.5: keyed with the per-user secret — a page cannot derive the
	// device from the id, and two installs never share one.
	u_camera_consent_persistent_id(&m->consent, cam->info.device_identity, exe, out_props->persistent_id);
	snprintf(out_props->display_name, sizeof(out_props->display_name), "%s", cam->info.display_name);
	snprintf(out_props->platform_device_hint, sizeof(out_props->platform_device_hint), "%s",
	         cam->info.platform_device_hint);
	out_props->flags = cam->info.flags;
	out_props->state = effective_state_locked(m, cam);
	out_props->view_count = 2;
	out_props->eye_width = cam->info.eye_width;
	out_props->eye_height = cam->info.eye_height;
	out_props->max_frame_rate = source_rate_hz(cam);
	if (cam->have_calib) {
		out_props->baseline_mm = (float)baseline_mm(&cam->calib);
		// Spec §3: per eye, RECTIFIED (the service's rectified focal when it
		// rectifies; the plug-in's own for a natively rectified source).
		double fx = cam->rect != NULL ? cam->rect->geo.f : cam->calib.k[0][0];
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
	if (output != XRT_STEREO_CAMERA_OUTPUT_RAW && output != XRT_STEREO_CAMERA_OUTPUT_RECTIFIED) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	char exe[512] = {0};
	ipc_server_peer_exe_path(ics->peer_pid, exe, sizeof(exe));
	os_mutex_lock(&m->lock);
	bool known = camera_by_id_locked(m, camera_id) != NULL && m->sharing_enabled;
	os_mutex_unlock(&m->lock);
	if (!known) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// Spec §7.5: calibration identifies the device — only after §7.1 passes.
	// Evaluated with the manager lock released (it may prompt).
	struct u_camera_consent_decision dec;
	xrt_result_t xret = consent_evaluate(m, ics, exe, "stereo_camera_get_calibration", &dec);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	os_mutex_lock(&m->lock);
	struct scam_camera *cam = camera_by_id_locked(m, camera_id);
	if (cam == NULL) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (!output_allowed(ics, dec.delegating, output)) {
		xret = XRT_ERROR_NOT_AUTHORIZED;
	}
	if (xret == XRT_SUCCESS && !cam->have_calib) {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (xret == XRT_SUCCESS && output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && !camera_can_rectify(cam)) {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (xret != XRT_SUCCESS) {
		os_mutex_unlock(&m->lock);
		return xret;
	}
	if (output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && cam->rect != NULL) {
		// R2: the numbers of the frames the service rectifies — one pinhole for
		// both eyes (fx = fy = f, one principal point: zero disparity at
		// infinity), no distortion, and the right camera a pure +x translation
		// of the baseline (OpenCV P1/P2 with P2[0][3] = f * Tx, Tx = -baseline).
		const struct u_stereo_rectify_result *g = &cam->rect->geo;
		out_calib->output = output;
		for (int e = 0; e < 2; e++) {
			struct xrt_stereo_camera_intrinsics *in = &out_calib->eye[e];
			in->width = g->width;
			in->height = g->height;
			in->fx = (float)g->f;
			in->fy = (float)g->f;
			in->cx = (float)g->cx;
			in->cy = (float)g->cy;
			in->model = XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE;
		}
		out_calib->orientation[3] = 1.0f; // identity
		// Right camera centre in the rectified left frame = -t_rect (mm -> m).
		for (int i = 0; i < 3; i++) {
			out_calib->position[i] = (float)(-g->t_rect[i] / 1000.0);
		}
		out_calib->baseline_mm = (float)g->baseline;
		os_mutex_unlock(&m->lock);
		return XRT_SUCCESS;
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
	char exe[512] = {0};
	ipc_server_peer_exe_path(ics->peer_pid, exe, sizeof(exe));
	const bool delegating = exe_is_delegating(m, exe);
	if (!output_allowed(ics, delegating, req->output)) {
		U_LOG_W("stereo_camera_stream_create: RAW refused for pid %ld (%s: rectified only)", ics->peer_pid,
		        delegating ? "consent-delegating client" : "PRESENT_OWNER");
		return XRT_ERROR_NOT_AUTHORIZED;
	}

	os_mutex_lock(&m->lock);
	struct scam_camera *cam = m->sharing_enabled ? camera_by_id_locked(m, req->camera_id) : NULL;
	if (cam == NULL) {
		os_mutex_unlock(&m->lock);
		return m->sharing_enabled ? XRT_ERROR_INPUT_UNSUPPORTED : XRT_ERROR_STEREO_CAMERA_DISABLED;
	}
	bool calibrated = (cam->info.flags &
	                   (XRT_PLUGIN_STEREO_CAMERA_CALIBRATED | XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED)) != 0;
	if (req->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && !calibrated) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	// Raw frames never reach web pages: the browser gets truly rectified
	// frames or nothing, never the RAW-flagged fallback.
	if (req->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && !camera_can_rectify(cam) &&
	    !output_allowed(ics, delegating, XRT_STEREO_CAMERA_OUTPUT_RAW)) {
		os_mutex_unlock(&m->lock);
		U_LOG_W(
		    "stereo_camera_stream_create: camera %llu cannot be rectified; refused for pid %ld "
		    "(PRESENT_OWNER: rectified only)",
		    (unsigned long long)cam->camera_id, ics->peer_pid);
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
	slot->section_ro = XRT_SHMEM_HANDLE_INVALID;
#ifndef XRT_OS_WINDOWS
	slot->wake.fds[0] = slot->wake.fds[1] = -1;
#endif
	slot->used = true;
	slot->id = ++m->next_stream_id;
	slot->owner = owner_of(m, ics);
	slot->ics = ics;
	slot->camera = cam->index;
	slot->req = *req;
	// RECTIFIED from a natively rectified source or the service's rectifier
	// (R2); a calibrated camera whose geometry the rectifier rejected is
	// delivered RAW and FLAGGED (frame.output) — to native clients only.
	slot->output = (req->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && camera_can_rectify(cam))
	                   ? XRT_STEREO_CAMERA_OUTPUT_RECTIFIED
	                   : XRT_STEREO_CAMERA_OUTPUT_RAW;
	snprintf(slot->consumer, sizeof(slot->consumer), "%s (pid %ld)", exe[0] ? exe : "?", ics->peer_pid);
	snprintf(slot->exe, sizeof(slot->exe), "%s", exe);
	slot->peer_pid = ics->peer_pid;
	slot->client_class = ics->client_state.client_class;
	slot->delegating = delegating;
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
	indicator_changed(m);
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
	// Pass 1 (locked): find the stream, copy what consent needs.
	char exe[512];
	os_mutex_lock(&m->lock);
	struct scam_stream *s = stream_by_id_locked(m, ics, stream_id);
	if (s == NULL) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (s->ended) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_STEREO_CAMERA_STREAM_ENDED;
	}
	if (s->started) {
		os_mutex_unlock(&m->lock);
		return XRT_SUCCESS;
	}
	snprintf(exe, sizeof(exe), "%s", s->exe);
	os_mutex_unlock(&m->lock);

	// Spec §7.1: start is THE authorisation point. Lock released — the tray
	// prompt may block for up to a minute.
	struct u_camera_consent_decision dec;
	xrt_result_t xret = consent_evaluate(m, ics, exe, "stereo_camera_stream_start", &dec);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	// Pass 2 (locked): the stream may have gone away while we asked.
	os_mutex_lock(&m->lock);
	s = stream_by_id_locked(m, ics, stream_id);
	if (s == NULL) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (s->ended) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_STEREO_CAMERA_STREAM_ENDED;
	}
	if (s->started) {
		os_mutex_unlock(&m->lock);
		return XRT_SUCCESS;
	}
	if (!m->sharing_enabled) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_STEREO_CAMERA_DISABLED;
	}
	struct scam_camera *cam = &m->cams[s->camera];
	// BUSY: the plug-in could not open the source and its retry is still
	// pending — distinct from "refused", so a browser maps it to NotReadableError.
	if (cam->state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE && cam->src == NULL &&
	    os_monotonic_get_ns() < cam->retry_after_ns) {
		os_mutex_unlock(&m->lock);
		U_LOG_W("stereo camera %llu: stream %llu start refused — source unavailable (BUSY), retry later",
		        (unsigned long long)cam->camera_id, (unsigned long long)s->id);
		return XRT_ERROR_STEREO_CAMERA_BUSY;
	}
	s->delegating = dec.delegating || s->delegating;
	s->delegated = dec.delegated;
	s->consent_why = (uint32_t)dec.why;
	xret = stream_allocate_locked(s, cam);
	if (xret != XRT_SUCCESS) {
		os_mutex_unlock(&m->lock);
		return xret;
	}
	u_stereo_camera_decimator_init(&s->dec, s->req.max_frame_rate, source_rate_hz(cam));
	u_stereo_camera_ring_clear(&s->ring);
	s->vis_checked_ns = 0;
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
	U_LOG_W("stereo camera %llu: stream %llu started by %s — output %s, format %u, max %.1f Hz; %u started%s",
	        (unsigned long long)cam->camera_id, (unsigned long long)s->id, s->consumer,
	        s->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED ? "RECTIFIED" : "RAW", s->req.format,
	        s->req.max_frame_rate, cam->started_count, m->locked ? " (session locked: suspended)" : "");
	os_mutex_unlock(&m->lock);
	indicator_changed(m); // the tray shows "3D camera in use by <app>"
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
	indicator_changed(m);
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
	out_layout->max_frame_rate =
	    s->dec.period_ns > 0 ? (float)(1e9 / (double)s->dec.period_ns) : source_rate_hz(&m->cams[s->camera]);
	out_layout->section_size = s->section_size;
	out_layout->slot_stride = s->slot_stride;
	out_layout->slot_count = U_STEREO_CAMERA_RING_SLOTS;
	// The READ-ONLY handle (FILE_MAP_READ duplicate on Windows, an O_RDONLY
	// shm fd / PROT_READ ashmem on POSIX): the consumer can map the ring but
	// never write it. The dispatch duplicates it into the peer; ours stays open.
	out_handles[0] = s->section_ro;
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
	if (s->ended) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_STEREO_CAMERA_STREAM_ENDED;
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
	const struct scam_rectifier *r = m->cams[s->camera].rect;
	if (r != NULL && s->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED) {
		const struct u_stereo_vrefine *ref = &r->ref;
		out_stats->refine_state = !r->refine         ? XRT_STEREO_CAMERA_REFINE_OFF
		                          : ref->applied     ? XRT_STEREO_CAMERA_REFINE_APPLIED
		                          : ref->windows > 0 ? XRT_STEREO_CAMERA_REFINE_ALIGNED
		                                             : XRT_STEREO_CAMERA_REFINE_MEASURING;
		out_stats->refine_updates = ref->updates;
		out_stats->refine_windows = ref->windows;
		out_stats->refine_matches = ref->last_matches;
		out_stats->refine_offset_px = (float)ref->a;
		out_stats->refine_slope_per_100px = (float)(ref->b * 100.0);
		out_stats->refine_initial_dy_px = (float)ref->initial_dy;
		out_stats->refine_residual_dy_px = (float)ref->last_residual_dy;
		out_stats->refine_measure_ms =
		    r->measure_count > 0 ? (float)((double)r->measure_ns_sum / r->measure_count * 1e-6) : 0.0f;
	}
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}


/*
 *
 * R3: events, control, and the service-UI entry points.
 *
 */

xrt_result_t
ipc_handle_stereo_camera_poll_event(volatile struct ipc_client_state *ics,
                                    bool *out_has,
                                    struct xrt_stereo_camera_event *out_event)
{
	*out_has = false;
	U_ZERO(out_event);
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL || ics->stereo_camera_owner == 0) {
		return XRT_SUCCESS; // never used a camera: nothing can be queued
	}
	os_mutex_lock(&m->lock);
	struct scam_evq *q = evq_for_owner_locked(m, ics->stereo_camera_owner, false);
	if (q != NULL && q->count > 0) {
		*out_event = q->ev[q->head];
		q->head = (q->head + 1) % EVQ_CAP;
		q->count--;
		*out_has = true;
	}
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

//! End every started stream (lock held). Returns how many were ended.
static uint32_t
stop_all_locked(struct ipc_server_stereo_camera *m, uint32_t reason)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		struct scam_stream *s = &m->streams[i];
		if (s->used && s->started) {
			stream_end_locked(m, s, reason);
			n++;
		}
	}
	return n;
}

static uint32_t
status_bits_locked(const struct ipc_server_stereo_camera *m)
{
	uint32_t started = 0;
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		started += m->streams[i].used && m->streams[i].started ? 1u : 0u;
	}
	return (m->sharing_enabled ? 1u : 0u) | (m->locked ? 2u : 0u) | ((started & 0xffu) << 8);
}

xrt_result_t
ipc_handle_stereo_camera_control(volatile struct ipc_client_state *ics, uint32_t op, uint32_t arg, uint32_t *out_value)
{
	*out_value = 0;
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	xrt_result_t auth = require_diag_client(ics, "stereo_camera_control");
	if (auth != XRT_SUCCESS) {
		return auth;
	}
	if (m == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	char who[64];
	snprintf(who, sizeof(who), "diag client pid %ld", ics->peer_pid);
	switch (op) {
	case XRT_STEREO_CAMERA_CONTROL_STOP_ALL: *out_value = ipc_server_stereo_camera_stop_all(who); return XRT_SUCCESS;
	case XRT_STEREO_CAMERA_CONTROL_SET_SHARING:
		return ipc_server_stereo_camera_set_sharing(arg != 0, who) ? XRT_SUCCESS : XRT_ERROR_IPC_FAILURE;
	case XRT_STEREO_CAMERA_CONTROL_SET_LOCKED:
		ipc_server_stereo_camera_set_session_locked(arg != 0, who);
		return XRT_SUCCESS;
	case XRT_STEREO_CAMERA_CONTROL_STATUS:
		os_mutex_lock(&m->lock);
		*out_value = status_bits_locked(m);
		os_mutex_unlock(&m->lock);
		return XRT_SUCCESS;
	default: return XRT_ERROR_INPUT_UNSUPPORTED;
	}
}

void
ipc_server_stereo_camera_set_prompt_provider(ipc_server_stereo_camera_prompt_fn fn, void *ctx)
{
	g_prompt_ctx = ctx;
	g_prompt_fn = fn;
}

void
ipc_server_stereo_camera_set_indicator_provider(ipc_server_stereo_camera_indicator_fn fn, void *ctx)
{
	g_indicator_ctx = ctx;
	g_indicator_fn = fn;
}

void
ipc_server_stereo_camera_set_notice_provider(ipc_server_stereo_camera_notice_fn fn, void *ctx)
{
	g_notice_ctx = ctx;
	g_notice_fn = fn;
}

bool
ipc_server_stereo_camera_get_status(struct ipc_server_stereo_camera_status *out)
{
	memset(out, 0, sizeof(*out));
	struct ipc_server_stereo_camera *m = g_mgr;
	if (m == NULL) {
		return false;
	}
	os_mutex_lock(&m->lock);
	out->camera_count = m->camera_count;
	out->sharing_enabled = m->sharing_enabled;
	out->locked = m->locked;
	size_t n = 0;
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		const struct scam_stream *s = &m->streams[i];
		if (!s->used || !s->started) {
			continue;
		}
		out->started_streams++;
		// Base name of the executable, de-duplicated, comma separated.
		const char *base = s->exe;
		for (const char *p = s->exe; *p != '\0'; p++) {
			if (*p == '\\' || *p == '/') {
				base = p + 1;
			}
		}
		if (base[0] == '\0') {
			base = "unknown app";
		}
		if (strstr(out->consumers, base) == NULL && n < sizeof(out->consumers) - 1) {
			n += (size_t)snprintf(out->consumers + n, sizeof(out->consumers) - n, "%s%s", n ? ", " : "", base);
			if (n >= sizeof(out->consumers)) {
				n = sizeof(out->consumers) - 1;
			}
		}
	}
	os_mutex_unlock(&m->lock);
	return true;
}

uint32_t
ipc_server_stereo_camera_stop_all(const char *why)
{
	struct ipc_server_stereo_camera *m = g_mgr;
	if (m == NULL) {
		return 0;
	}
	os_mutex_lock(&m->lock);
	uint32_t n = stop_all_locked(m, XRT_STEREO_CAMERA_END_USER_STOPPED);
	os_mutex_unlock(&m->lock);
	os_mutex_lock(&m->consent_lock);
	u_camera_consent_forget_once(&m->consent); // "Allow once" grants die with the stop
	os_mutex_unlock(&m->consent_lock);
	U_LOG_W("stereo camera: STOP camera sharing (%s) — %u stream(s) ended", why != NULL ? why : "?", n);
	indicator_changed(m);
	return n;
}

bool
ipc_server_stereo_camera_set_sharing(bool enabled, const char *why)
{
	struct ipc_server_stereo_camera *m = g_mgr;
	if (m == NULL) {
		return false;
	}
	const struct u_camera_consent_store_ops *st = m->consent.store;
	bool persisted = st != NULL && st->set_sharing_enabled != NULL && st->set_sharing_enabled(m->consent.store_ctx, enabled);
	if (!persisted) {
		U_LOG_W("stereo camera: could not persist the sharing toggle (%s) — applied for this run only",
		        enabled ? "on" : "off");
	}
	os_mutex_lock(&m->lock);
	bool changed = m->sharing_enabled != enabled;
	m->sharing_enabled = enabled;
	if (changed) {
		if (!enabled) {
			(void)stop_all_locked(m, XRT_STEREO_CAMERA_END_DISABLED);
		}
		notify_all_cameras_locked(m);
	}
	os_mutex_unlock(&m->lock);
	if (changed) {
		U_LOG_W("stereo camera: sharing switched %s (%s)%s", enabled ? "ON" : "OFF", why != NULL ? why : "?",
		        enabled ? "" : " — every started stream ended, zero cameras enumerated");
		indicator_changed(m);
	}
	return true;
}

void
ipc_server_stereo_camera_set_session_locked(bool locked, const char *why)
{
	struct ipc_server_stereo_camera *m = g_mgr;
	if (m == NULL) {
		return;
	}
	os_mutex_lock(&m->lock);
	bool changed = m->locked != locked;
	m->locked = locked;
	if (changed) {
		if (locked) {
			// Spec §7.2: the last image must not linger on a locked desktop.
			for (uint32_t i = 0; i < MAX_STREAMS; i++) {
				if (m->streams[i].used) {
					u_stereo_camera_ring_clear(&m->streams[i].ring);
				}
			}
		}
		notify_all_cameras_locked(m);
	}
	os_mutex_unlock(&m->lock);
	if (changed) {
		U_LOG_W("stereo camera: OS session %s (%s) — streams %s", locked ? "LOCKED" : "unlocked",
		        why != NULL ? why : "?", locked ? "suspended" : "resume");
		indicator_changed(m);
	}
}

bool
ipc_server_stereo_camera_is_session_locked(void)
{
	struct ipc_server_stereo_camera *m = g_mgr;
	if (m == NULL) {
		return false;
	}
	os_mutex_lock(&m->lock);
	bool l = m->locked;
	os_mutex_unlock(&m->lock);
	return l;
}
