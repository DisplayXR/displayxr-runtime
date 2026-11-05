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
 *    XrEventDataStereoCamerasChangedDXR (#1961) goes to every connection that
 *    has enumerated or created a stream, whenever a re-enumeration (see
 *    "Camera discovery" below) or the sharing toggle changes the list.
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
 * Sources (Amendment 4) — a camera comes from the active plug-in's
 * stereo_camera_* slots or from the service's own vendor-neutral UVC
 * side-by-side source (util/u_stereo_uvc.h over os/os_uvc_capture.h: Media
 * Foundation on Windows, a V4L2 TODO elsewhere), opted in per device by the
 * user's stereo-cameras.json. Both have the same six-operation shape; the
 * src_* dispatch is the only place that tells them apart, so consent, the
 * foreground rule, the indicator, linger, rates, rings and rectification are
 * one code path. Plug-in cameras are enumerated first and their
 * platform_device_hints are never claimed by the UVC source. Both sources are
 * RE-enumerated after create (#1961, "Camera discovery" below) with stable
 * camera ids: a camera that appears later is appended, one that disappears
 * turns UNAVAILABLE in place. An uncalibrated
 * UVC pair gets a NOMINAL model (pinhole from its configured HFOV + baseline,
 * parallel, undistorted): the camera reports no CALIBRATED, RAW calibration is
 * FEATURE_NOT_SUPPORTED, and RECTIFIED is a PASS-THROUGH (no remap, nominal
 * numbers) until the online refinement below folds a vertical correction in —
 * from then on the ordinary LUT path with the corrected geometry.
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
#include "util/u_camera_consent_android.h"
#include "util/u_misc.h"
#include "util/u_stereo_camera.h"
#include "util/u_stereo_rectify.h"
#include "util/u_stereo_uvc.h"
#include "util/u_stereo_vrefine.h"
#include "util/u_app_share_fake.h"
#include "util/u_handles.h"
#include "os/os_uvc_capture.h"

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
// ADR-061 app share (service environment).
DEBUG_GET_ONCE_BOOL_OPTION(app_share_enabled, "DXR_APP_SHARE", true)
DEBUG_GET_ONCE_BOOL_OPTION(app_share_dev, "DXR_APP_SHARE_DEV", false)
// DXR_APP_SHARE_FAKE: "1" (truthy) = the synthetic service-composited app,
// "link" = a synthetic in-process registrant over the share-link core (ADR-061
// arm 2), "both" = both.
DEBUG_GET_ONCE_OPTION(app_share_fake, "DXR_APP_SHARE_FAKE", NULL)

/*!
 * ADR-061: app entries live in their OWN table (mgr->apps), never in cams[]
 * (four slots, never reused). A stream's `camera` index addresses both:
 * [0, MAX_CAMERAS) = cams[], [APP_SLOT_BASE, APP_SLOT_BASE + APP_MAX) =
 * apps[]. App slots ARE reused, but only once nothing references them.
 */
#define APP_MAX 8
#define APP_SLOT_BASE XRT_STEREO_CAMERA_MAX_CAMERAS
#define TOTAL_SLOTS (XRT_STEREO_CAMERA_MAX_CAMERAS + APP_MAX)
#define APP_LINGER_NS_DEFAULT (10000ll * 1000 * 1000) //!< an exited app stays listed UNAVAILABLE this long
#define APP_TICK_NS (1000ll * 1000 * 1000)            //!< the app rescan tick
#define APP_CLOSE_WAIT_NS (1000ll * 1000 * 1000)      //!< app_session_ending: bounded wait for the detach
#define APP_ID_MEMORY 32                              //!< (exe, ordinal) -> camera_id, per service run
//! ADR-061 arm 2: registered in-process service links (one per connection, plus the fake).
#define LINK_MAX (IPC_MAX_CLIENTS + 1)
//! A ring that declares a fence is imported once it arrives; past this, the share fails loudly.
#define LINK_FENCE_WAIT_NS (2000ll * 1000 * 1000)
//! The synthetic link registrant's connection id (never a real client id: those count up from 1).
#define FAKE_LINK_CLIENT_ID 0xFA4E0001u
#define MAX_STREAMS (TOTAL_SLOTS * XRT_STEREO_CAMERA_MAX_STREAMS_PER_CAMERA)
#define LINGER_NS (2000ll * 1000 * 1000)
#define WAIT_TIMEOUT_NS (100ll * 1000 * 1000)
#define SUSPEND_AFTER_NS (1000ll * 1000 * 1000)
#define REOPEN_BACKOFF_NS (1000ll * 1000 * 1000)
/*!
 * #2221: a UVC source that opened but delivered nothing for this long is treated as
 * one that could not start (the device is really someone else's). UVC only: a
 * plug-in camera may legitimately take seconds after a tracker unpause.
 */
#define FIRST_FRAME_DEADLINE_NS (5000ll * 1000 * 1000)
#define STATS_LOG_NS (5000ll * 1000 * 1000)
#define REFINE_SAMPLES 512 //!< matches one measurement may return
#define VISIBILITY_TTL_NS (250ll * 1000 * 1000) //!< foreground-rule cache per stream
//! A2 front-camera gate: a weave submit this recent means "presents woven 3D".
#define PRESENTING_WINDOW_NS (1000ll * 1000 * 1000)
/*!
 * A2 front-camera gate at start: how long a start waits for the app's first
 * weave before it is refused. A browser asks for the camera while the page
 * that will show it is still loading (or right as it returns from the
 * background), ~100 ms before its first weave submit; a one-shot check
 * refused it and the page fell back to another camera. An app that never
 * weaves still gets BUSY, after this long.
 */
#define PRESENTING_GRACE_NS (3000ll * 1000 * 1000)
#define PRESENTING_POLL_NS (50ll * 1000 * 1000)
#define PROMPT_TIMEOUT_MS 60000u                //!< an unanswered consent prompt = refused
#define PROMPT_TIMEOUT_MIN_MS 1000u             //!< DXR_STEREO_CAMERA_PROMPT_TIMEOUT_MS clamp (dev only)
#define PROMPT_TIMEOUT_MAX_MS 600000u
#define EVQ_CAP 16                              //!< queued events per connection
#define RESCAN_MIN_INTERVAL_NS (1000ll * 1000 * 1000) //!< #1961: at most one source re-enumeration per second
#define RESCAN_PERIOD_DEFAULT_MS 2000                 //!< #1961: background re-enumeration while a client listens
//! In-process consumers at most (they also count against the CAMERA_CONSUMER quota).
#define INTERNAL_MAX 4
//! Owner tokens of in-process consumers carry this bit (no event queue, no connection).
#define INTERNAL_OWNER_BIT (1ull << 63)
#define UVC_OBJS_MAX                                                                                                   \
	(XRT_STEREO_CAMERA_MAX_CAMERAS + 2) //!< live u_stereo_uvc resolutions (one per rescan that changed)


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
	//! A2 (Android, option R): a USER_FACING camera — frames only while the
	//! peer's uid presents woven 3D in this process.
	bool needs_presenting;
	long peer_uid;
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
	//! ADR-061: the app metadata of each slot's frame (valid = 0 on a camera stream).
	struct xrt_app_share_frame_info slot_app[U_STEREO_CAMERA_RING_SLOTS];

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

	//! The in-process consumer owning this stream (input host iface shared-camera
	//! slot), NULL for an IPC connection's. Its cond is signalled with the wake.
	struct ipc_server_stereo_camera_internal *internal;
	//! What the tray names the consumer by ("" = the executable's base name).
	char label[128];
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
	/*!
	 * Amendment 4: the camera's model is a NOMINAL parallel pinhole pair (an
	 * uncalibrated UVC stereo webcam) and no vertical correction has been
	 * folded in yet — the rectified frames ARE the source frames (no remap,
	 * no zoom), described by the nominal intrinsics below. The first applied
	 * refinement swaps in real maps and clears it.
	 */
	bool passthrough;
	double pass_f, pass_cx, pass_cy;
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

/*!
 * ADR-061: what an app entry adds to a camera. Guarded by the manager lock
 * except @ref frame (camera thread only).
 */
struct scam_app
{
	bool listed; //!< in the enumeration (live, or exited and lingering)
	bool live;   //!< its connection + session are still there
	bool fake;   //!< DXR_APP_SHARE_FAKE: the service's synthetic source
	uint32_t client_id;
	long pid;
	char exe_path[U_CAMERA_CONSENT_EXE_MAX];
	uint32_t ordinal; //!< 0 = the first running copy of exe_path
	int64_t exited_ns;
	//! The description as the enumeration reports it (availability, window, views, ...).
	struct xrt_app_share_source_info desc;
	int64_t last_commit_ns; //!< from share_tap_query; 0 = unknown
	//! Metadata of the frame src_wait_frame just returned (camera thread only).
	struct xrt_app_share_frame_info frame;
};

/*!
 * ADR-061 arm 2: one registered in-process service link (decision 9, 16).
 * Guarded by the manager's link_lock.
 */
struct scam_link
{
	bool used;
	bool fake; //!< the DXR_APP_SHARE_FAKE=link registrant (CPU ring, no connection)
	uint32_t client_id;
	long pid;                                //!< OS-derived (the connection's peer)
	char exe_path[U_CAMERA_CONSENT_EXE_MAX]; //!< OS-derived at registration
	struct xrt_share_link_registration reg;
	bool identity_mismatch_logged;

	// What the service asks of the app (the poll's answer).
	uint32_t state; //!< enum xrt_share_link_state
	uint32_t stream_generation;
	uint32_t eye_w, eye_h;
	float fps;

	// The newest published ring the source has not taken yet (handles owned).
	bool ring_pending;
	struct xrt_share_link_ring_desc ring;
	xrt_graphics_buffer_handle_t tex[XRT_SHARE_LINK_RING_SLOTS];
	uint32_t tex_count;
	bool fence_pending;
	uint32_t fence_generation;
	xrt_graphics_sync_handle_t fence;
	int64_t ring_ns; //!< when the pending ring arrived (the fence wait is time-boxed from here)
	//! The content size of the last ring (the entry's content_eye_width/height).
	uint32_t content_w, content_h;

	// The newest frame_ready (latest-wins).
	uint64_t frame_seq;
	struct xrt_share_link_frame frame;
	int64_t last_frame_ns; //!< when frame_ready last arrived (os_monotonic)

	//! The fake's CPU ring: XRT_SHARE_LINK_RING_SLOTS SBS NV12 frames of the ring's slot size.
	uint8_t *cpu_slots[XRT_SHARE_LINK_RING_SLOTS];
	size_t cpu_slot_size;
};

struct scam_link_src;
struct scam_fake_link;

struct scam_camera
{
	uint32_t index; //!< stream key: position in mgr->cams, or APP_SLOT_BASE + position in mgr->apps
	//! enum xrt_stereo_camera_source: the plug-in's slots or the service's UVC source.
	uint32_t source;
	uint32_t source_index; //!< the camera's index inside its source
	//! UVC only: the source resolution this camera opens from (one of mgr->uvc_objs).
	struct u_stereo_uvc *uvc;
	uint64_t camera_id;
	/*!
	 * #1961: the camera's source stopped listing it at a re-enumeration. It
	 * keeps its slot, index and camera_id (nothing is renumbered), reports
	 * UNAVAILABLE, and is never opened; it comes back, same id, if its
	 * device_identity is listed again.
	 */
	bool gone;
	//! The camera thread is inside src_open (lock released): a rescan must not repoint the source.
	bool opening;
	//! A re-enumeration reported different geometry / format / flags (logged once).
	bool description_drift_logged;
	struct xrt_plugin_stereo_camera_info info;
	bool have_calib;
	//! Amendment 4: @ref calib is a NOMINAL model (an uncalibrated UVC pair):
	//! it drives the rectifier, but is never handed out as RAW calibration.
	bool calib_nominal;
	bool baseline_known, hfov_known; //!< nominal model: did the user configure them
	struct xrt_plugin_stereo_camera_calibration calib;
	uint32_t state;
	uint32_t calibration_generation;
	//! R2: set when CALIBRATED and not NATIVELY_RECTIFIED and the geometry is
	//! sane; NULL = no service-side rectification (RAW-flagged fallback).
	struct scam_rectifier *rect;

	//! Owned by the camera thread: a struct xrt_plugin_stereo_camera (PLUGIN)
	//! or a struct u_stereo_uvc_stream (UVC); see the src_* dispatch below.
	void *src;
	uint32_t started_count;
	int64_t linger_deadline_ns;
	int64_t last_frame_ns;
	//! A2: the source delivered at least once since a stream started it (reset
	//! when it closes idle) — tells "the OS took it away" from "never opened".
	bool streamed;
	int64_t retry_after_ns;
	//! When the current source opened (#2221: the UVC first-frame deadline counts from here).
	int64_t opened_ns;
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

	//! ADR-061: set for an app entry (source APP / APP_LINK).
	struct scam_app app;
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
	/*!
	 * Amendment 5: the active plug-in when it declares the cameras its display
	 * owns (get_claimed_cameras) — independent of whether it has camera slots.
	 */
	const struct xrt_plugin_iface *claims_iface;
	struct xrt_plugin_instance *claims_inst;
	/*!
	 * Amendment 4: the service's own UVC side-by-side source. Each rescan that
	 * finds the UVC inputs changed resolves a NEW u_stereo_uvc (an open stream
	 * keeps the one it was opened from); every object some camera still
	 * references stays here, the rest are freed (uvc_gc_locked).
	 */
	struct u_stereo_uvc *uvc_objs[UVC_OBJS_MAX];
	//! Hash of what the last UVC resolution saw (config, plug-in hints, device ids).
	uint64_t uvc_fingerprint;
	bool uvc_scanned_once;
	//! Test seams, copied from the create options (NULL fields = the real ones).
	bool have_uvc_config;
	struct u_stereo_uvc_config uvc_config;
	bool have_uvc_backend;
	struct u_stereo_uvc_backend uvc_backend;

	uint32_t camera_count;
	struct scam_camera cams[XRT_STEREO_CAMERA_MAX_CAMERAS];
	struct scam_stream streams[MAX_STREAMS];
	uint64_t next_stream_id;
	uint64_t next_owner;
	uint64_t next_camera_id; //!< camera_ids are never reused within a service lifetime

	/*
	 * #1961 re-enumeration. rescan_lock serialises rescans (and is the only
	 * thing that adds cameras); it is taken BEFORE lock, never under it, and
	 * source enumerate / calibration calls run with only rescan_lock held, so
	 * a camera thread is never blocked behind a plug-in's enumerate.
	 */
	struct os_mutex rescan_lock;
	int64_t last_rescan_ns;
	uint64_t rescan_period_ns; //!< 0 = no background rescan
	bool watch_started;
	struct os_thread watch;
	struct os_cond watch_cond;
	//! ipc_server_stereo_camera_rescan_soon asked; the next watch pass is unthrottled.
	bool rescan_soon;

	// R3 privacy.
	struct u_camera_consent consent;
	//! Serialises consent evaluations (one prompt at a time); never held
	//! together with @ref lock.
	struct os_mutex consent_lock;
	bool sharing_enabled; //!< cached user toggle (the store is the truth)
	bool locked;          //!< OS session locked / switched away
	struct scam_evq evq[IPC_MAX_CLIENTS];
	//! A2 (Android): decisions + secret from the private-storage store, the
	//! delegating list from the runtime's built-in one.
	struct u_camera_consent_store_android android_store;

	/*
	 * In-process consumers (ipc_server_stereo_camera_internal_*): the input
	 * host iface's shared-camera slot. @ref server (may be NULL) counts the
	 * CAMERA_CONSUMER connections the internal ones share a quota with.
	 * @ref internal is guarded by @ref lock; @ref internal_busy by
	 * g_internal_lock (it pins the manager against destroy).
	 */
	struct ipc_server *server;
	struct ipc_server_stereo_camera_internal *internal[INTERNAL_MAX];
	uint32_t internal_busy;

	/*
	 * ADR-061 app share. apps[] is guarded by @ref lock; app_rescan_lock
	 * serialises app rescans (taken before lock, never under it).
	 */
	struct scam_camera apps[APP_MAX];
	bool apps_enabled; //!< DXR_APP_SHARE (default on)
	bool app_dev;      //!< DXR_APP_SHARE_DEV: listed to every camera client, no prompt
	int64_t app_linger_ns;
	bool apps_dirty; //!< apps_changed() asked; the watch thread rescans now
	int64_t last_app_rescan_ns;
	struct os_mutex app_rescan_lock;
	struct os_cond app_closed_cond; //!< broadcast when an app source closes
	struct
	{
		char exe[U_CAMERA_CONSENT_EXE_MAX];
		uint32_t ordinal;
		uint64_t camera_id;
	} app_ids[APP_ID_MEMORY];
	uint32_t app_id_next;
	//! The share policy (its own decisions; shares the camera's kill switch / toggle).
	struct u_app_share_consent share;
	/*!
	 * App keys (camera ids) whose app went: their per-app-run grants are
	 * forgotten under consent_lock once the manager lock is released
	 * (share_forget_drain) — the two locks are never held together.
	 */
	uint64_t share_forget[APP_MAX * 2];
	uint32_t share_forget_count;
	int32_t share_prompt_override; //!< options: 0 = store, 1 = always, 2 = per-app-run
	int32_t share_resolution_override;
	bool have_share_prompt_seam; //!< options supplied a share prompt

	/*
	 * ADR-061 arm 2: the in-process service links that declared SHARE.
	 * link_lock is a LEAF: nothing else is ever taken while it is held (the
	 * manager lock may be held when it is taken, never the reverse).
	 * link_cond is broadcast on every frame_ready / ring / unregister.
	 */
	struct os_mutex link_lock;
	struct os_cond link_cond;
	struct scam_link *links; //!< [LINK_MAX]
	struct scam_fake_link *fake_link;
};

/*!
 * One in-process consumer. Allocated by internal_open, freed by
 * internal_close (the consumer's thread); @ref m is cleared when the manager
 * is destroyed first (service shutdown destroys the manager before the input
 * providers' devices), after which every call reports the stream ended.
 */
struct ipc_server_stereo_camera_internal
{
	struct ipc_server_stereo_camera *m; //!< guarded by g_internal_lock
	uint64_t owner;                     //!< the stream's owner token (no connection)
	uint64_t stream_id;
	struct os_cond cond; //!< signalled (manager lock held) on publish / end / shutdown
	char label[128];
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
static u_app_share_prompt_fn g_share_prompt_fn = NULL;
static void *g_share_prompt_ctx = NULL;
//! A2: platform identity / permission hooks (Android), copied at set time.
static struct ipc_server_stereo_camera_platform g_platform;
static bool g_platform_set = false;
/*!
 * In-process consumers: pins the manager (internal_busy) against destroy and
 * guards every handle's `m` and @ref g_internal_count. Static-initialised (a
 * consumer may call before the manager exists); never taken under the
 * manager lock — the order is g_internal_lock -> manager lock.
 */
static pthread_mutex_t g_internal_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_internal_idle = PTHREAD_COND_INITIALIZER;
//! Open in-process consumers, for the CAMERA_CONSUMER connection quota.
static uint32_t g_internal_count = 0;

void
ipc_server_stereo_camera_set_platform(const struct ipc_server_stereo_camera_platform *p)
{
	if (p == NULL) {
		memset(&g_platform, 0, sizeof(g_platform));
		g_platform_set = false;
		return;
	}
	g_platform = *p;
	// ADR-061 arm 2: the ring hooks come as a set (an import without an
	// unimport would leak every ring). A partial table is refused whole.
	const bool any = p->share_link_import != NULL || p->share_link_convert != NULL ||
	                 p->share_link_unimport != NULL || p->share_link_release_frame != NULL;
	const bool all =
	    p->share_link_import != NULL && p->share_link_convert != NULL && p->share_link_unimport != NULL;
	if (any && !all) {
		U_LOG_E(
		    "app share: the platform's share_link_* hooks are incomplete (import %s, convert %s, unimport %s) "
		    "— "
		    "refused; in-process app entries are listed \"no tap\"",
		    p->share_link_import != NULL ? "set" : "NULL", p->share_link_convert != NULL ? "set" : "NULL",
		    p->share_link_unimport != NULL ? "set" : "NULL");
		g_platform.share_link_import = NULL;
		g_platform.share_link_convert = NULL;
		g_platform.share_link_unimport = NULL;
		g_platform.share_link_release_frame = NULL;
	}
	g_platform_set = true;
}


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

//! A stream's camera index -> the camera or app entry (see APP_SLOT_BASE).
static inline struct scam_camera *
slot_cam(struct ipc_server_stereo_camera *m, uint32_t index)
{
	return index < APP_SLOT_BASE ? &m->cams[index] : &m->apps[index - APP_SLOT_BASE];
}

static inline bool
cam_is_app(const struct scam_camera *cam)
{
	return cam->source == XRT_STEREO_CAMERA_SOURCE_APP || cam->source == XRT_STEREO_CAMERA_SOURCE_APP_LINK;
}

//! ADR-061 decision 3: does this connection see app entries at all?
static bool
client_sees_apps(const struct ipc_server_stereo_camera *m, volatile struct ipc_client_state *ics)
{
	return m->apps_enabled && (m->app_dev || (ics->client_state.info.stereo_camera_client_flags &
	                                          XRT_STEREO_CAMERA_CLIENT_INTERNAL_APP_SHARE) != 0);
}

//! Is app entry @p cam listed to @p ics (never to the shared app's own process)?
static bool
app_listed_for_locked(const struct scam_camera *cam, volatile struct ipc_client_state *ics)
{
	return cam->app.listed && (cam->app.pid <= 0 || cam->app.pid != ics->peer_pid);
}

//! A listed app entry @p ics may see, by camera id (NULL = none).
static struct scam_camera *
app_by_id_for_locked(struct ipc_server_stereo_camera *m, volatile struct ipc_client_state *ics, uint64_t id)
{
	if (!client_sees_apps(m, ics)) {
		return NULL;
	}
	for (uint32_t i = 0; i < APP_MAX; i++) {
		struct scam_camera *a = &m->apps[i];
		if (a->camera_id == id && app_listed_for_locked(a, ics)) {
			return a;
		}
	}
	return NULL;
}

//! How many app entries @p ics sees (0 when it does not see apps).
static uint32_t
apps_listed_count_locked(struct ipc_server_stereo_camera *m, volatile struct ipc_client_state *ics)
{
	if (!client_sees_apps(m, ics)) {
		return 0;
	}
	uint32_t n = 0;
	for (uint32_t i = 0; i < APP_MAX; i++) {
		n += app_listed_for_locked(&m->apps[i], ics) ? 1u : 0u;
	}
	return n;
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
	if (g_platform_set && g_platform.os_camera_allowed != NULL) {
		return g_platform.os_camera_allowed(g_platform.ctx, exe);
	}
	return u_camera_consent_os_camera_allowed(exe);
}

//! The tray prompt, if the host installed one.
static enum u_camera_consent_prompt_answer
env_prompt(void *ctx, const char *exe, const char *app_name, long pid, uint32_t timeout_ms)
{
	(void)ctx;
	if (g_platform_set && g_platform.prompt != NULL) {
		return g_platform.prompt(g_platform.ctx, exe, app_name, pid, timeout_ms);
	}
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
                 const char *exe,
                 const char *app_name,
                 long pid,
                 uint32_t flags,
                 const char *what,
                 struct u_camera_consent_decision *out)
{
	os_mutex_lock(&m->consent_lock);
	u_camera_consent_evaluate(&m->consent, exe, app_name, pid, flags, out);
	os_mutex_unlock(&m->consent_lock);
	xrt_result_t xret = verdict_to_xret(out->verdict);
	// When a registered entry was not applied, say why (declined / untrusted).
	const char *skip = u_camera_consent_delegation_skip_str(out->delegation_skip);
	U_LOG_W("%s: %s for %s (pid %ld, \"%s\") — %s%s%s%s", what, xret == XRT_SUCCESS ? "ALLOWED" : "REFUSED",
	        exe[0] ? exe : "?", pid, app_name, u_camera_consent_why_str(out->why),
	        skip[0] ? " (delegation not applied: " : "", skip, skip[0] ? ")" : "");
	return xret;
}

static xrt_result_t
consent_evaluate_client(struct ipc_server_stereo_camera *m,
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
	return consent_evaluate(m, exe, app_name, ics->peer_pid, flags, what, out);
}

/*!
 * The identity consent, the delegating check and the persistentId key on:
 * the verified executable path on the desktop; on a platform with a
 * peer_subject hook (Android) the subject it resolves from the connection's
 * uid. Android without the hook fails closed ("" = refused, spec §7.1 step 3):
 * its executable is app_process for every app.
 */
static void
peer_identity(volatile struct ipc_client_state *ics, char *out, size_t cap)
{
	out[0] = '\0';
	if (g_platform_set && g_platform.peer_subject != NULL) {
		if (!g_platform.peer_subject(g_platform.ctx, ics->peer_pid, ics->peer_uid, out, cap)) {
			out[0] = '\0';
		}
		return;
	}
#ifdef XRT_OS_ANDROID
	U_LOG_W("stereo camera: no Android identity provider registered — pid %ld has no verifiable identity",
	        ics->peer_pid);
	return;
#else
	ipc_server_peer_exe_path(ics->peer_pid, out, cap);
#endif
}

/*!
 * A2 front-camera gate (§1.2 option R): does any connection of @p uid in this
 * process present woven 3D — a weave submit within PRESENTING_WINDOW_NS, a
 * visible session, or a weave present-owner connection of an app that is on
 * top? A present-owner whose only 3D content is the camera itself (a call
 * page's preview) cannot submit a weave before it has frames, so it would
 * never pass a weave-only rule.
 *
 * @p fg says whether the app is on top: -1 = ask the platform now (a start;
 * no manager lock held), 1 = treat as on top (per frame: a stream in the
 * background is already suspended by the platform's own foreground rule).
 * Reads other connections' fields racily (the per-frame pattern of input
 * focus); a stale read errs for one frame at most.
 */
static bool
uid_presenting(volatile struct ipc_client_state *ics, long uid, int64_t now, int fg)
{
	struct ipc_server *vs = ics != NULL ? ics->server : NULL;
	if (vs == NULL || uid < 0) {
		return false;
	}
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		volatile struct ipc_client_state *o = &vs->threads[i].ics;
		if (o->server_thread_index < 0 || o->peer_uid != uid) {
			continue;
		}
		int64_t w = o->last_weave_ns;
		if (w != 0 && now - w < PRESENTING_WINDOW_NS) {
			return true;
		}
		if (o->xc != NULL && o->client_state.session_active && o->client_state.session_visible) {
			return true;
		}
		if (o->client_state.client_class == XRT_CLIENT_CLASS_PRESENT_OWNER) {
			if (fg < 0) {
				fg = (g_platform_set && g_platform.consumer_foreground != NULL &&
				      g_platform.consumer_foreground(g_platform.ctx))
				         ? 1
				         : 0;
			}
			if (fg > 0) {
				return true;
			}
		}
	}
	return false;
}

/*!
 * uid_presenting(), waiting up to PRESENTING_GRACE_NS for it to become true.
 * Called without the manager lock, on the requesting connection's own thread
 * (a start already blocks there for the consent prompt).
 */
static bool
uid_presenting_within_grace(volatile struct ipc_client_state *ics, long uid)
{
	const int64_t t0 = (int64_t)os_monotonic_get_ns();
	int64_t now = t0;
	while (!uid_presenting(ics, uid, now, -1)) {
		if (now - t0 >= PRESENTING_GRACE_NS) {
			return false;
		}
		os_nanosleep(PRESENTING_POLL_NS);
		now = (int64_t)os_monotonic_get_ns();
	}
	if (now != t0) {
		U_LOG_W("stereo_camera_stream_start: the app started presenting %.0f ms after asking for the front camera",
		        (double)(now - t0) / 1e6);
	}
	return true;
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
static void
evq_push_locked(struct ipc_server_stereo_camera *m, uint64_t owner, const struct xrt_stereo_camera_event *ev);

static bool
client_visible_locked(struct ipc_server_stereo_camera *m, struct scam_stream *s, int64_t now)
{
	if (s->needs_presenting) {
		// A2: no frames while the peer's package stops presenting (it went to
		// the background, its weave ended). Told as SUSPENDED, per connection.
		if (s->vis_checked_ns != 0 && now - s->vis_checked_ns < VISIBILITY_TTL_NS) {
			return s->visible;
		}
		bool v = uid_presenting(s->ics, s->peer_uid, now, 1);
		if (s->vis_checked_ns == 0 || v != s->visible) {
			if (s->vis_checked_ns != 0 || !v) {
				U_LOG_W("stereo camera: stream %llu (%s) %s — front camera follows the live weave",
				        (unsigned long long)s->id, s->consumer,
				        v ? "presents again, frames resume" : "no live weave, frames suspended");
			}
			struct xrt_stereo_camera_event ev = {
			    .kind = XRT_STEREO_CAMERA_EVENT_CAMERA_STATE,
			    .value = v ? slot_cam(m, s->camera)->state : XRT_STEREO_CAMERA_STATE_SUSPENDED,
			    .camera_id = slot_cam(m, s->camera)->camera_id,
			};
			if (s->vis_checked_ns != 0 || !v) {
				evq_push_locked(m, s->owner, &ev);
			}
		}
		s->vis_checked_ns = now;
		s->visible = v;
		return v;
	}
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
	if ((owner & INTERNAL_OWNER_BIT) != 0) {
		// An in-process consumer has no connection to queue for: it reads the
		// state (and STREAM_ENDED) from every acquire instead.
		return;
	}
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
	for (uint32_t i = 0; i < APP_MAX; i++) {
		if (m->apps[i].app.listed) {
			notify_camera_state_locked(m, &m->apps[i]);
		}
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

/*!
 * Spec v7: the plug-in's frame orientation (0 = unknown) as the degrees apps
 * see. Anything outside the enum is a plug-in bug and reads as unknown rather
 * than as a turn a consumer would apply.
 */
static uint32_t
frame_orientation_deg(uint32_t plugin_value)
{
	switch (plugin_value) {
	case XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_0: return 0;
	case XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_90: return 90;
	case XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_180: return 180;
	case XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_270: return 270;
	default: return XRT_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN;
	}
}


/*
 *
 * Source dispatch (Amendment 4): a camera is the plug-in's (its stereo_camera_*
 * slots) or the service's own UVC source (u_stereo_uvc). Both have the same
 * six-operation shape; everything above and below this block is shared.
 *
 */

static const char *
source_str(uint32_t source)
{
	switch (source) {
	case XRT_STEREO_CAMERA_SOURCE_UVC: return "uvc";
	case XRT_STEREO_CAMERA_SOURCE_APP: return "app";
	case XRT_STEREO_CAMERA_SOURCE_APP_LINK: return "app-link";
	default: return "plug-in";
	}
}


/*
 *
 * ADR-061 arm 2: the share-link core. An in-process app's service link
 * (ipc_client_service_link) registers here; its app entry (SOURCE_APP_LINK)
 * asks for frames by queueing SHARE_REQUESTED, the app publishes a ring and
 * posts frame_ready, the entry's camera thread imports the ring and converts
 * through the platform's share_link_* hooks. The IPC handlers below and the
 * service's own synthetic registrant (DXR_APP_SHARE_FAKE=link) both drive
 * these functions, so the fake exercises the exact path a real link takes.
 *
 */

static bool
app_mark_gone_locked(
    struct ipc_server_stereo_camera *m, struct scam_camera *cam, int64_t now, bool delist, const char *why);
static void
notify_cameras_changed_locked(struct ipc_server_stereo_camera *m);
static void
share_forget_drain(struct ipc_server_stereo_camera *m);
static uint32_t
app_end_streams_locked(struct ipc_server_stereo_camera *m, struct scam_camera *cam);
static void
fake_link_start(struct ipc_server_stereo_camera *m);
static void
fake_link_stop(struct ipc_server_stereo_camera *m);

//! An app entry's open source: one SHARE_REQUESTED stream of one link.
struct scam_link_src
{
	uint32_t client_id;
	uint32_t stream_generation;
	uint32_t eye_w, eye_h;
	bool fake;
	uint64_t last_seq; //!< the last frame_ready consumed
	bool imported;
	void *import; //!< the platform's (NULL for the fake)
	struct xrt_share_link_ring_desc ring;
	//! The fake: the frame copied out of its CPU ring under link_lock.
	uint8_t *cpu_frame;
	size_t cpu_size;
	/*!
	 * The ring cannot be used (import failed, its fence never came): the
	 * stream ends SOURCE_LOST and the app hears SHARE_STOPPED instead of an
	 * endless wait. Camera thread only.
	 */
	bool fatal;
};

//! Caller holds link_lock.
static struct scam_link *
link_find_locked(struct ipc_server_stereo_camera *m, uint32_t client_id)
{
	if (m->links == NULL) {
		return NULL;
	}
	for (uint32_t i = 0; i < LINK_MAX; i++) {
		if (m->links[i].used && m->links[i].client_id == client_id) {
			return &m->links[i];
		}
	}
	return NULL;
}

static void
link_close_handles(xrt_graphics_buffer_handle_t *tex, uint32_t count, xrt_graphics_sync_handle_t *fence)
{
	for (uint32_t i = 0; i < count; i++) {
		u_graphics_buffer_unref(&tex[i]);
	}
	if (fence != NULL) {
		u_graphics_sync_unref(fence);
	}
}

//! Caller holds link_lock: drop a ring / fence nobody took.
static void
link_drop_pending_locked(struct scam_link *l)
{
	if (l->ring_pending) {
		link_close_handles(l->tex, l->tex_count, NULL);
		l->tex_count = 0;
		l->ring_pending = false;
	}
	if (l->fence_pending) {
		u_graphics_sync_unref(&l->fence);
		l->fence_pending = false;
	}
}

//! Caller holds link_lock.
static void
link_free_locked(struct scam_link *l)
{
	link_drop_pending_locked(l);
	for (uint32_t i = 0; i < XRT_SHARE_LINK_RING_SLOTS; i++) {
		free(l->cpu_slots[i]);
		l->cpu_slots[i] = NULL;
	}
	memset(l, 0, sizeof(*l));
}

static void
link_poll_from_locked(struct scam_link *l, struct xrt_share_link_poll *out)
{
	memset(out, 0, sizeof(*out));
	out->state = l->state;
	out->eye_w = l->eye_w;
	out->eye_h = l->eye_h;
	out->fps = l->fps;
	out->stream_generation = l->stream_generation;
	if (l->state == XRT_SHARE_LINK_STATE_SHARE_STOPPED) {
		l->state = XRT_SHARE_LINK_STATE_NONE; // reported once
	}
}

/*!
 * Register (or update) the link of connection @p client_id. @p exe_path /
 * @p pid: OS-derived. Changes that alter the listing ask for an app rescan.
 */
static xrt_result_t
link_register(struct ipc_server_stereo_camera *m,
              uint32_t client_id,
              long pid,
              const char *exe_path,
              bool fake,
              const struct xrt_share_link_registration *reg)
{
	bool first = false, relist = false;
	os_mutex_lock(&m->link_lock);
	struct scam_link *l = link_find_locked(m, client_id);
	if (l == NULL) {
		for (uint32_t i = 0; m->links != NULL && i < LINK_MAX && l == NULL; i++) {
			if (!m->links[i].used) {
				l = &m->links[i];
			}
		}
		if (l == NULL) {
			os_mutex_unlock(&m->link_lock);
			U_LOG_W("app share: more than %d service links — connection %u not registered", LINK_MAX,
			        client_id);
			return XRT_ERROR_CLIENT_LIMIT_REACHED; // a refusal, never "the pipe died"
		}
		memset(l, 0, sizeof(*l));
		l->used = true;
		l->fake = fake;
		l->client_id = client_id;
		l->pid = pid;
		l->fence = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		snprintf(l->exe_path, sizeof(l->exe_path), "%s", exe_path != NULL ? exe_path : "");
		first = true;
	}
	const struct xrt_share_link_registration was = l->reg;
	l->reg = *reg;
	l->reg.exe[sizeof(l->reg.exe) - 1] = '\0';
	l->reg.app_name[sizeof(l->reg.app_name) - 1] = '\0';
	relist = first || was.caps != l->reg.caps || was.opt_out != l->reg.opt_out ||
	         was.session_running != l->reg.session_running || was.tap_available != l->reg.tap_available ||
	         was.tap_unavailable_reason != l->reg.tap_unavailable_reason || was.tier != l->reg.tier ||
	         was.luid_valid != l->reg.luid_valid || memcmp(was.luid, l->reg.luid, sizeof(was.luid)) != 0 ||
	         strcmp(was.app_name, l->reg.app_name) != 0;
	bool log_mismatch = false;
	if (!fake && !l->identity_mismatch_logged && l->reg.pid != 0 && (long)l->reg.pid != pid) {
		l->identity_mismatch_logged = true;
		log_mismatch = true;
	}
	const struct xrt_share_link_registration r = l->reg;
	os_mutex_unlock(&m->link_lock);

	if (log_mismatch) {
		U_LOG_W("app share: service link %u says pid %u, the OS says %ld — the OS-derived one is listed",
		        client_id, r.pid, pid);
	}
	if (first) {
		U_LOG_W(
		    "app share: in-process service link %u registered — \"%s\" (%s, pid %ld), caps 0x%x, tier %u, "
		    "tap %s, session %s%s",
		    client_id, r.app_name, exe_path != NULL ? exe_path : "?", pid, r.caps, r.tier,
		    r.tap_available ? "available" : "none", r.session_running ? "running" : "not running",
		    r.opt_out ? ", OPTED OUT" : "");
	}
	if (relist) {
		ipc_server_stereo_camera_apps_changed(m);
	}
	return XRT_SUCCESS;
}

static void
link_poll(struct ipc_server_stereo_camera *m, uint32_t client_id, struct xrt_share_link_poll *out)
{
	memset(out, 0, sizeof(*out));
	os_mutex_lock(&m->link_lock);
	struct scam_link *l = link_find_locked(m, client_id);
	if (l != NULL) {
		link_poll_from_locked(l, out);
	}
	os_mutex_unlock(&m->link_lock);
}

//! Takes ownership of @p tex. A ring for a stream that is no longer asked for is dropped (not an error).
static xrt_result_t
link_publish_ring(struct ipc_server_stereo_camera *m,
                  uint32_t client_id,
                  const struct xrt_share_link_ring_desc *desc,
                  const xrt_graphics_buffer_handle_t *tex_in,
                  uint32_t tex_count)
{
	xrt_graphics_buffer_handle_t tex[XRT_SHARE_LINK_RING_SLOTS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < tex_count; i++) {
		if (n < XRT_SHARE_LINK_RING_SLOTS) {
			tex[n++] = tex_in[i];
		} else {
			xrt_graphics_buffer_handle_t h = tex_in[i];
			u_graphics_buffer_unref(&h); // the wire is not trusted
		}
	}
	const bool shape_ok = desc->slot_count <= XRT_SHARE_LINK_RING_SLOTS && (n == 0 || n == desc->slot_count) &&
	                      (desc->slot_count == 0 || (desc->slot_width >= 2 && desc->slot_height >= 1 &&
	                                                 desc->slot_width <= 16384 && desc->slot_height <= 16384));
	os_mutex_lock(&m->link_lock);
	struct scam_link *l = link_find_locked(m, client_id);
	// A real app's ring has one texture per slot; only the service's own fake
	// (DXR_APP_SHARE_FAKE=link) publishes a CPU ring without handles.
	const bool handles_ok = l != NULL && (l->fake ? n == 0 : n == desc->slot_count);
	if (l == NULL || !shape_ok || !handles_ok) {
		os_mutex_unlock(&m->link_lock);
		if (l != NULL && shape_ok && !handles_ok) {
			U_LOG_W(
			    "app share: service link %u published a ring with %u texture(s) for %u slot(s) — refused",
			    client_id, n, desc->slot_count);
		}
		link_close_handles(tex, n, NULL);
		// Logical refusals are never XRT_ERROR_IPC_FAILURE: the client reads that
		// as "the pipe died" and would drop its live lift streams.
		return l == NULL ? XRT_ERROR_OUTPUT_REQUEST_FAILURE : XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (l->state != XRT_SHARE_LINK_STATE_SHARE_REQUESTED || desc->stream_generation != l->stream_generation) {
		os_mutex_unlock(&m->link_lock);
		link_close_handles(tex, n, NULL); // a stale ring: nobody asks for it any more
		return XRT_SUCCESS;
	}
	if (l->ring_pending) {
		link_close_handles(l->tex, l->tex_count, NULL);
	}
	l->ring = *desc;
	memcpy(l->tex, tex, sizeof(tex[0]) * n);
	l->tex_count = n;
	l->ring_pending = true;
	l->ring_ns = os_monotonic_get_ns();
	l->content_w = desc->content_width;
	l->content_h = desc->content_height;
	if (l->fence_pending && l->fence_generation != desc->generation) {
		u_graphics_sync_unref(&l->fence);
		l->fence_pending = false;
	}
	os_cond_broadcast(&m->link_cond);
	os_mutex_unlock(&m->link_lock);
	return XRT_SUCCESS;
}

//! Takes ownership of @p fence.
static xrt_result_t
link_publish_fence(struct ipc_server_stereo_camera *m,
                   uint32_t client_id,
                   uint32_t generation,
                   xrt_graphics_sync_handle_t fence)
{
	os_mutex_lock(&m->link_lock);
	struct scam_link *l = link_find_locked(m, client_id);
	if (l == NULL) {
		os_mutex_unlock(&m->link_lock);
		u_graphics_sync_unref(&fence);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown link: a refusal, not a dead pipe
	}
	if (l->fence_pending) {
		u_graphics_sync_unref(&l->fence);
	}
	l->fence = fence;
	l->fence_generation = generation;
	l->fence_pending = true;
	os_cond_broadcast(&m->link_cond);
	os_mutex_unlock(&m->link_lock);
	return XRT_SUCCESS;
}

static xrt_result_t
link_frame_ready(struct ipc_server_stereo_camera *m,
                 uint32_t client_id,
                 const struct xrt_share_link_frame *frame,
                 struct xrt_share_link_poll *out)
{
	memset(out, 0, sizeof(*out));
	os_mutex_lock(&m->link_lock);
	struct scam_link *l = link_find_locked(m, client_id);
	if (l == NULL) {
		os_mutex_unlock(&m->link_lock);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown link: a refusal, not a dead pipe
	}
	if (l->state == XRT_SHARE_LINK_STATE_SHARE_REQUESTED) {
		l->frame = *frame; // latest-wins
		l->frame_seq++;
		l->last_frame_ns = os_monotonic_get_ns();
		os_cond_broadcast(&m->link_cond);
	}
	link_poll_from_locked(l, out);
	os_mutex_unlock(&m->link_lock);
	return XRT_SUCCESS;
}

/*!
 * The link of connection @p client_id went away (its connection closed): its
 * entry ends SOURCE_LOST (the app is gone, as far as sharing knows), then the
 * link and every handle it still held are freed. No lock held.
 */
static void
link_unregister(struct ipc_server_stereo_camera *m, uint32_t client_id)
{
	os_mutex_lock(&m->link_lock);
	const bool known = link_find_locked(m, client_id) != NULL;
	os_mutex_unlock(&m->link_lock);
	if (!known) {
		return;
	}
	bool any = false;
	os_mutex_lock(&m->lock);
	const int64_t now = os_monotonic_get_ns();
	for (uint32_t i = 0; i < APP_MAX; i++) {
		struct scam_camera *a = &m->apps[i];
		if (a->source == XRT_STEREO_CAMERA_SOURCE_APP_LINK && a->app.live && a->app.client_id == client_id) {
			any |= app_mark_gone_locked(m, a, now, false, "app exited (its service link closed)");
		}
	}
	if (any) {
		notify_cameras_changed_locked(m);
	}
	os_mutex_unlock(&m->lock);

	os_mutex_lock(&m->link_lock);
	struct scam_link *l = link_find_locked(m, client_id);
	if (l != NULL) {
		link_free_locked(l);
	}
	os_cond_broadcast(&m->link_cond); // a waiting source sees its link gone
	os_mutex_unlock(&m->link_lock);
	share_forget_drain(m);
	if (any) {
		indicator_changed(m);
	}
}

static xrt_result_t
link_share_open(
    struct ipc_server_stereo_camera *m, uint32_t client_id, uint32_t eye_w, uint32_t eye_h, float fps, void **out)
{
	struct scam_link_src *src = U_TYPED_CALLOC(struct scam_link_src);
	if (src == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	os_mutex_lock(&m->link_lock);
	struct scam_link *l = link_find_locked(m, client_id);
	if (l == NULL) {
		os_mutex_unlock(&m->link_lock);
		free(src);
		return XRT_ERROR_IPC_FAILURE; // its app went between the listing and the start
	}
	if (!l->fake &&
	    (!g_platform_set || g_platform.share_link_import == NULL || g_platform.share_link_convert == NULL)) {
		os_mutex_unlock(&m->link_lock);
		free(src);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED; // no importer here (listed UNAVAILABLE, no tap)
	}
	link_drop_pending_locked(l); // rings of an earlier stream
	l->stream_generation++;
	l->state = XRT_SHARE_LINK_STATE_SHARE_REQUESTED;
	l->eye_w = eye_w;
	l->eye_h = eye_h;
	l->fps = fps;
	src->client_id = client_id;
	src->stream_generation = l->stream_generation;
	src->eye_w = eye_w;
	src->eye_h = eye_h;
	src->fake = l->fake;
	src->last_seq = l->frame_seq;
	const uint32_t sg = l->stream_generation;
	os_cond_broadcast(&m->link_cond);
	os_mutex_unlock(&m->link_lock);
	U_LOG_W("app share: SHARE_REQUESTED on service link %u (stream generation %u, %ux%u per eye @ %.0f Hz)",
	        client_id, sg, eye_w, eye_h, fps);
	*out = src;
	return XRT_SUCCESS;
}

static void
link_share_close(struct ipc_server_stereo_camera *m, struct scam_link_src *src)
{
	// Release the service's references to the app's ring FIRST, then tell the
	// app SHARE_STOPPED: once it hears that it may free the ring.
	if (src->imported && !src->fake && g_platform_set && g_platform.share_link_unimport != NULL) {
		g_platform.share_link_unimport(g_platform.ctx, src->import); // validated with import at install
	}
	src->imported = false;
	src->import = NULL;
	os_mutex_lock(&m->link_lock);
	struct scam_link *l = link_find_locked(m, src->client_id);
	if (l != NULL && l->stream_generation == src->stream_generation &&
	    l->state == XRT_SHARE_LINK_STATE_SHARE_REQUESTED) {
		l->state = XRT_SHARE_LINK_STATE_SHARE_STOPPED;
		link_drop_pending_locked(l);
	}
	os_cond_broadcast(&m->link_cond);
	os_mutex_unlock(&m->link_lock);
	U_LOG_W("app share: SHARE_STOPPED on service link %u (stream generation %u)", src->client_id,
	        src->stream_generation);
	free(src->cpu_frame);
	free(src);
}

//! Camera thread only. No lock held on entry or exit.
static uint32_t
link_share_wait_frame(struct ipc_server_stereo_camera *m,
                      struct scam_camera *cam,
                      struct scam_link_src *src,
                      int64_t timeout_ns,
                      struct xrt_plugin_stereo_camera_frame *out)
{
	const int64_t deadline = os_monotonic_get_ns() + timeout_ns;
	struct xrt_share_link_frame frame;
	os_mutex_lock(&m->link_lock);
	for (;;) {
		struct scam_link *l = link_find_locked(m, src->client_id);
		if (l == NULL || l->stream_generation != src->stream_generation ||
		    l->state != XRT_SHARE_LINK_STATE_SHARE_REQUESTED) {
			os_mutex_unlock(&m->link_lock);
			return XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR; // the link or the request is gone
		}
		// A new ring for this stream, complete with its fence when it declares
		// one (has_fence). The fence wait is time-boxed: a fence that never
		// comes (a failed publish_fence) fails the stream loudly.
		const bool fence_ok =
		    l->ring.has_fence == 0 || (l->fence_pending && l->fence_generation == l->ring.generation);
		if (l->ring_pending && !fence_ok && os_monotonic_get_ns() - l->ring_ns > LINK_FENCE_WAIT_NS) {
			const uint32_t g = l->ring.generation;
			link_drop_pending_locked(l);
			os_mutex_unlock(&m->link_lock);
			U_LOG_W(
			    "app share: link %u ring generation %u declared a fence that never arrived within %lld ms "
			    "— the share ends (SOURCE_LOST)",
			    src->client_id, g, (long long)(LINK_FENCE_WAIT_NS / 1000000));
			src->fatal = true;
			return XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR;
		}
		if (l->ring_pending && fence_ok) {
			struct xrt_share_link_ring_desc desc = l->ring;
			xrt_graphics_buffer_handle_t tex[XRT_SHARE_LINK_RING_SLOTS];
			const uint32_t n = l->tex_count;
			memcpy(tex, l->tex, sizeof(tex[0]) * n);
			xrt_graphics_sync_handle_t fence = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
			if (l->fence_pending) {
				fence = l->fence;
				l->fence_pending = false;
			}
			l->ring_pending = false;
			l->tex_count = 0;
			os_mutex_unlock(&m->link_lock);

			if (src->imported && !src->fake) {
				// The hook table is validated whole at install: unimport comes with import.
				g_platform.share_link_unimport(g_platform.ctx, src->import);
			}
			src->imported = false;
			src->import = NULL;
			if (desc.slot_count == 0) {
				link_close_handles(tex, n, &fence); // the app withdrew its ring
			} else if (src->fake) {
				link_close_handles(tex, n, &fence);
				src->imported = true;
			} else {
				void *imp = NULL;
				xrt_result_t xret = g_platform.share_link_import(g_platform.ctx, &desc, tex, n, fence,
				                                                 src->eye_w, src->eye_h, &imp);
				src->imported = xret == XRT_SUCCESS && imp != NULL;
				src->import = imp;
				if (!src->imported) {
					U_LOG_W(
					    "app share: link %u ring generation %u (%ux%u, format 0x%x) import FAILED "
					    "(%d) — "
					    "the share ends (SOURCE_LOST)",
					    src->client_id, desc.generation, desc.slot_width, desc.slot_height,
					    desc.format, (int)xret);
					if (xret == XRT_SUCCESS && imp != NULL) {
						g_platform.share_link_unimport(g_platform.ctx, imp);
					}
					src->import = NULL;
					src->fatal = true;
					return XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR;
				}
				U_LOG_W("app share: link %u ring generation %u (%ux%u, format 0x%x) %s (%d)",
				        src->client_id, desc.generation, desc.slot_width, desc.slot_height, desc.format,
				        src->imported ? "imported" : "import FAILED", (int)xret);
			}
			src->ring = desc;
			os_mutex_lock(&m->link_lock);
			continue;
		}
		if (src->imported && l->frame_seq != src->last_seq) {
			src->last_seq = l->frame_seq;
			if (l->frame.generation == src->ring.generation && l->frame.slot < src->ring.slot_count) {
				frame = l->frame;
				if (src->fake) {
					const uint8_t *slot = l->cpu_slots[frame.slot];
					if (slot == NULL || l->cpu_slot_size == 0) {
						continue;
					}
					if (src->cpu_size != l->cpu_slot_size) {
						free(src->cpu_frame);
						src->cpu_frame = (uint8_t *)malloc(l->cpu_slot_size);
						src->cpu_size = src->cpu_frame != NULL ? l->cpu_slot_size : 0;
					}
					if (src->cpu_frame == NULL) {
						continue;
					}
					memcpy(src->cpu_frame, slot, src->cpu_size);
				}
				break;
			}
			continue; // a frame of an older ring: dropped
		}
		const int64_t now = os_monotonic_get_ns();
		if (now >= deadline) {
			os_mutex_unlock(&m->link_lock);
			return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT;
		}
		os_cond_wait_timeout_ns(&m->link_cond, &m->link_lock, (uint64_t)(deadline - now));
	}
	os_mutex_unlock(&m->link_lock);

	memset(&cam->app.frame, 0, sizeof(cam->app.frame));
	if (src->fake) {
		// The fake's CPU ring is SBS NV12 at the stream extent by construction.
		const uint32_t w = src->ring.slot_width, h = src->ring.slot_height;
		if (w != 2 * src->eye_w || h != src->eye_h || src->cpu_size < (size_t)w * h * 3 / 2) {
			return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT; // a ring of an older size
		}
		out->sequence = src->last_seq;
		out->time_ns = frame.commit_ns;
		out->time_is_exposure = false;
		out->width = w;
		out->height = h;
		out->format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
		out->planes[0] = src->cpu_frame;
		out->planes[1] = src->cpu_frame + (size_t)w * h;
		out->pitches[0] = w;
		out->pitches[1] = w;
		cam->app.frame.content_x = 0;
		cam->app.frame.content_y = 0;
		cam->app.frame.content_width = src->eye_w;
		cam->app.frame.content_height = src->eye_h;
		cam->app.frame.mono = frame.mono;
		cam->app.frame.content_generation = frame.content_generation;
		cam->app.frame.zero_disparity_shift_frac = frame.zero_disparity_shift_frac;
		cam->app.frame.commit_time_ns = frame.commit_ns;
		cam->app.frame.valid = 1;
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_OK;
	}
	uint32_t w = g_platform.share_link_convert(g_platform.ctx, src->import, &frame, out, &cam->app.frame);
	cam->app.frame.valid = w == XRT_PLUGIN_STEREO_CAMERA_WAIT_OK ? 1u : 0u;
	return w;
}

static void
link_share_release_frame(struct scam_link_src *src)
{
	if (!src->fake && src->imported && g_platform_set && g_platform.share_link_release_frame != NULL) {
		g_platform.share_link_release_frame(g_platform.ctx, src->import);
	}
}

/*!
 * What src_open needs of a camera, captured under the manager lock by the
 * camera thread (a rescan may update the camera while the open runs unlocked).
 */
struct src_open_args
{
	uint32_t source;
	struct u_stereo_uvc *uvc;
	uint32_t index;
	// ADR-061 app entries.
	bool app_fake;
	uint32_t app_client_id;
	uint32_t eye_w, eye_h;
	float fps;
};

static xrt_result_t
src_open(struct ipc_server_stereo_camera *m, const struct src_open_args *a, void **out)
{
	*out = NULL;
	if (a->source == XRT_STEREO_CAMERA_SOURCE_UVC) {
		struct u_stereo_uvc_stream *s = NULL;
		xrt_result_t xret = u_stereo_uvc_open(a->uvc, a->index, &s);
		*out = s;
		return xret;
	}
	if (a->source == XRT_STEREO_CAMERA_SOURCE_APP) {
		if (a->app_fake) {
			struct u_app_share_fake *f = u_app_share_fake_create(a->eye_w, a->eye_h, a->fps);
			*out = f;
			return f != NULL ? XRT_SUCCESS : XRT_ERROR_ALLOCATION;
		}
		if (!g_platform_set || g_platform.share_tap_attach == NULL) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED; // no tap on this platform (listed UNAVAILABLE)
		}
		return g_platform.share_tap_attach(g_platform.ctx, a->app_client_id, a->eye_w, a->eye_h, a->fps, out);
	}
	if (a->source == XRT_STEREO_CAMERA_SOURCE_APP_LINK) {
		// ADR-061 decision 9: queue SHARE_REQUESTED on the app's service link.
		return link_share_open(m, a->app_client_id, a->eye_w, a->eye_h, a->fps, out);
	}
	struct xrt_plugin_stereo_camera *pc = NULL;
	xrt_result_t xret = m->iface->stereo_camera_open(m->inst, a->index, &pc);
	*out = pc;
	return xret;
}

//! Camera thread only (writes cam->app.frame, which only this thread reads).
static uint32_t
src_wait_frame(struct ipc_server_stereo_camera *m,
               struct scam_camera *cam,
               void *src,
               int64_t timeout_ns,
               struct xrt_plugin_stereo_camera_frame *out)
{
	if (cam->source == XRT_STEREO_CAMERA_SOURCE_UVC) {
		return u_stereo_uvc_wait_frame((struct u_stereo_uvc_stream *)src, timeout_ns, out);
	}
	if (cam->source == XRT_STEREO_CAMERA_SOURCE_APP) {
		memset(&cam->app.frame, 0, sizeof(cam->app.frame));
		if (cam->app.fake) {
			struct u_app_share_fake_frame ff;
			if (!u_app_share_fake_next((struct u_app_share_fake *)src, timeout_ns, &ff)) {
				return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT;
			}
			out->sequence = ff.sequence;
			out->time_ns = ff.time_ns;
			out->time_is_exposure = false;
			out->width = ff.width;
			out->height = ff.height;
			out->format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
			out->planes[0] = ff.planes[0];
			out->planes[1] = ff.planes[1];
			out->pitches[0] = ff.pitches[0];
			out->pitches[1] = ff.pitches[1];
			// Display-referred and full-frame by construction (spec v1: shift 0).
			cam->app.frame.content_x = 0;
			cam->app.frame.content_y = 0;
			cam->app.frame.content_width = ff.width / 2;
			cam->app.frame.content_height = ff.height;
			cam->app.frame.mono = 0;
			cam->app.frame.content_generation = 1;
			cam->app.frame.zero_disparity_shift_frac = 0.0f;
			cam->app.frame.commit_time_ns = ff.time_ns;
			cam->app.frame.valid = 1;
			return XRT_PLUGIN_STEREO_CAMERA_WAIT_OK;
		}
		if (!g_platform_set || g_platform.share_tap_wait_frame == NULL) {
			return XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR;
		}
		uint32_t w = g_platform.share_tap_wait_frame(g_platform.ctx, src, timeout_ns, out, &cam->app.frame);
		cam->app.frame.valid = w == XRT_PLUGIN_STEREO_CAMERA_WAIT_OK ? 1u : 0u;
		return w;
	}
	if (cam->source == XRT_STEREO_CAMERA_SOURCE_APP_LINK) {
		return link_share_wait_frame(m, cam, (struct scam_link_src *)src, timeout_ns, out);
	}
	return m->iface->stereo_camera_wait_frame((struct xrt_plugin_stereo_camera *)src, timeout_ns, out);
}

static void
src_release_frame(struct ipc_server_stereo_camera *m, const struct scam_camera *cam, void *src)
{
	if (cam->source == XRT_STEREO_CAMERA_SOURCE_UVC) {
		u_stereo_uvc_release_frame((struct u_stereo_uvc_stream *)src);
		return;
	}
	if (cam->source == XRT_STEREO_CAMERA_SOURCE_APP) {
		if (!cam->app.fake && g_platform_set && g_platform.share_tap_release_frame != NULL) {
			g_platform.share_tap_release_frame(g_platform.ctx, src);
		}
		return;
	}
	if (cam->source == XRT_STEREO_CAMERA_SOURCE_APP_LINK) {
		link_share_release_frame((struct scam_link_src *)src);
		return;
	}
	m->iface->stereo_camera_release_frame((struct xrt_plugin_stereo_camera *)src);
}

static void
src_close(struct ipc_server_stereo_camera *m, const struct scam_camera *cam, void *src)
{
	if (cam->source == XRT_STEREO_CAMERA_SOURCE_UVC) {
		u_stereo_uvc_close((struct u_stereo_uvc_stream *)src);
		return;
	}
	if (cam->source == XRT_STEREO_CAMERA_SOURCE_APP) {
		if (cam->app.fake) {
			struct u_app_share_fake *f = (struct u_app_share_fake *)src;
			u_app_share_fake_destroy(&f);
		} else if (g_platform_set && g_platform.share_tap_detach != NULL) {
			g_platform.share_tap_detach(g_platform.ctx, src);
		}
		return;
	}
	if (cam->source == XRT_STEREO_CAMERA_SOURCE_APP_LINK) {
		link_share_close(m, (struct scam_link_src *)src); // queues SHARE_STOPPED
		return;
	}
	m->iface->stereo_camera_close((struct xrt_plugin_stereo_camera *)src);
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

//! Pass-through backend (Amendment 4): the nominal parallel pair needs no remap.
static bool
rectify_apply_passthrough(struct scam_rectifier *r,
                          const struct xrt_plugin_stereo_camera_frame *in,
                          struct xrt_plugin_stereo_camera_frame *out)
{
	if (in->width != 2 * r->geo.width || in->height != r->geo.height || in->planes[0] == NULL) {
		return false;
	}
	*out = *in;
	return true;
}

/*!
 * Is @p c an exactly parallel, undistorted pair with one pinhole for both
 * eyes at the frame size — i.e. are the frames already rectified, so that
 * the nominal intrinsics describe them with no remap? (A nominal model is
 * built that way; a real calibration practically never is.)
 */
static bool
calibration_is_parallel_pinhole(const struct xrt_plugin_stereo_camera_calibration *c, uint32_t w, uint32_t h)
{
	if (c->distortion_model != XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE || c->image_width != w ||
	    c->image_height != h) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		if (c->k[0][i] != c->k[1][i]) {
			return false;
		}
	}
	if (c->k[0][0] != c->k[0][1]) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			if (c->rotation_right_from_left[i][j] != (i == j ? 1.0 : 0.0)) {
				return false;
			}
		}
	}
	const double *t = c->translation_right_from_left_mm;
	return t[0] < 0.0 && t[1] == 0.0 && t[2] == 0.0;
}

/*!
 * The rectified intrinsics clients are told: the pass-through's nominal ones,
 * else the rectifier's geometry.
 */
static void
rectified_intrinsics(const struct scam_rectifier *r, double *f, double *cx, double *cy)
{
	if (r->passthrough) {
		*f = r->pass_f;
		*cx = r->pass_cx;
		*cy = r->pass_cy;
	} else {
		*f = r->geo.f;
		*cx = r->geo.cx;
		*cy = r->geo.cy;
	}
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
	if (cam->calib_nominal && calibration_is_parallel_pinhole(c, in.width, in.height)) {
		// Amendment 4: an uncalibrated pair modelled as an ideal parallel rig.
		// Bouguet of that is the identity up to the alpha = 0 border zoom, so
		// deliver the frames untouched (no remap, no zoom) with the nominal
		// numbers; the refinement below aligns the rows from the frames.
		r->passthrough = true;
		r->pass_f = c->k[0][0];
		r->pass_cx = c->k[0][2];
		r->pass_cy = c->k[0][3];
		r->apply = rectify_apply_passthrough;
	}
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
	if (r->passthrough) {
		U_LOG_W(
		    "stereo camera %llu: NOMINAL model (uncalibrated) — RECTIFIED = the frames as delivered "
		    "(f %.2f px, principal point (%.2f, %.2f), nominal baseline %.2f mm) until the online "
		    "refinement aligns the rows%s",
		    (unsigned long long)cam->camera_id, r->pass_f, r->pass_cx, r->pass_cy, r->geo.baseline,
		    r->refine ? "" : " (refinement OFF: rows are as the device delivers them)");
		return r;
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
	// A folded-in correction needs real maps: a nominal pass-through ends here.
	r->passthrough = false;
	r->apply = rectify_apply_cpu;
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
		if (m->locked || !m->sharing_enabled || !client_visible_locked(m, s, now)) {
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
		// ADR-061: the per-frame app metadata rides the acquire reply, not the ring.
		s->slot_app[slot] = cam_is_app(cam) ? cam->app.frame : (struct xrt_app_share_frame_info){0};
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
		if (s->internal != NULL) {
			os_cond_signal(&s->internal->cond);
		}
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

static void
end_started_streams_busy_locked(struct ipc_server_stereo_camera *m, struct scam_camera *cam, const char *why);

static void *
camera_thread(void *ptr)
{
	struct scam_camera *cam = (struct scam_camera *)ptr;
	struct ipc_server_stereo_camera *m = cam->mgr;

	os_mutex_lock(&m->lock);
	while (!m->shutting_down) {
		int64_t now = os_monotonic_get_ns();

		// Idle: close the source once the linger expires, then sleep until a
		// stream starts (or the manager shuts down). ADR-061: an app entry
		// whose app is gone closes (detaches its tap) at once — its
		// compositor is being torn down.
		const bool app_gone = cam_is_app(cam) && !cam->app.live;
		if (cam->started_count == 0 && (cam->src == NULL || now >= cam->linger_deadline_ns || app_gone)) {
			if (cam->src != NULL) {
				void *src = cam->src;
				cam->src = NULL;
				cam->opening = true; // app_session_ending waits until the close is done
				os_mutex_unlock(&m->lock);
				src_close(m, cam, src);
				os_mutex_lock(&m->lock);
				cam->opening = false;
				if (cam_is_app(cam)) {
					os_cond_broadcast(&m->app_closed_cond);
				}
				U_LOG_W("stereo camera %llu: source closed (no started stream for %lld ms)",
				        (unsigned long long)cam->camera_id, (long long)(LINGER_NS / 1000000));
				cam->last_frame_ns = 0;
				cam->streamed = false;
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
			if (cam->gone || (cam_is_app(cam) && !cam->app.live)) {
				// #1961: its source no longer lists it — never open a stale
				// index (it may name another device now). Start refuses BUSY.
				os_cond_wait_timeout_ns(&cam->cond, &m->lock, (uint64_t)REOPEN_BACKOFF_NS);
				continue;
			}
			if (now < cam->retry_after_ns) {
				os_cond_wait_timeout_ns(&cam->cond, &m->lock, (uint64_t)(cam->retry_after_ns - now));
				continue;
			}
			void *src = NULL;
			const struct src_open_args oa = {
			    .source = cam->source,
			    .uvc = cam->uvc,
			    .index = cam->source_index,
			    .app_fake = cam->app.fake,
			    .app_client_id = cam->app.client_id,
			    .eye_w = cam->info.eye_width,
			    .eye_h = cam->info.eye_height,
			    .fps = cam->info.max_frame_rate,
			};
			cam->opening = true; // a rescan leaves uvc / source_index alone meanwhile
			os_mutex_unlock(&m->lock);
			xrt_result_t xret = src_open(m, &oa, &src);
			os_mutex_lock(&m->lock);
			cam->opening = false;
			if (xret != XRT_SUCCESS || src == NULL) {
				U_LOG_W("stereo camera %llu: %s source open failed (%d); retrying in 1 s",
				        (unsigned long long)cam->camera_id, source_str(cam->source), (int)xret);
				if (g_platform.gave_up_is_suspended && cam->streamed) {
					// A2: the OS still holds the camera back (background); the
					// started streams wait SUSPENDED for it to come back.
					set_state_locked(cam, XRT_STEREO_CAMERA_STATE_SUSPENDED);
				} else {
					set_state_locked(cam, XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
					end_started_streams_busy_locked(m, cam, "source open failed");
				}
				cam->retry_after_ns = os_monotonic_get_ns() + REOPEN_BACKOFF_NS;
				continue;
			}
			if (m->shutting_down) {
				os_mutex_unlock(&m->lock);
				src_close(m, cam, src);
				os_mutex_lock(&m->lock);
				break;
			}
			cam->src = src;
			cam->last_frame_ns = 0;
			cam->opened_ns = os_monotonic_get_ns();
			u_stereo_camera_rate_meter_restart(&cam->rate_meter);
			if (cam->rect != NULL && cam->rect->refine) {
				// Keep the correction, re-verify it at the settling cadence.
				u_stereo_vrefine_restart(&cam->rect->ref, os_monotonic_get_ns());
			}
			U_LOG_W("stereo camera %llu: source opened", (unsigned long long)cam->camera_id);
			// A2: a reopen while the OS still holds the camera back stays SUSPENDED
			// (no WAITING <-> SUSPENDED flapping every retry); frames move it on.
			set_state_locked(cam, g_platform.gave_up_is_suspended && cam->streamed
			                          ? XRT_STEREO_CAMERA_STATE_SUSPENDED
			                          : XRT_STEREO_CAMERA_STATE_WAITING);
		}

		void *src = cam->src;
		struct xrt_plugin_stereo_camera_frame f;
		memset(&f, 0, sizeof(f));
		os_mutex_unlock(&m->lock);
		uint32_t w = src_wait_frame(m, cam, src, WAIT_TIMEOUT_NS, &f);
		os_mutex_lock(&m->lock);
		now = os_monotonic_get_ns();

		switch (w) {
		case XRT_PLUGIN_STEREO_CAMERA_WAIT_OK: {
			if (cam_is_app(cam) && !cam->app.live) {
				// ADR-061: a frame that was in flight when the app went: never
				// published (it would flip the entry back to AVAILABLE). The tap's
				// release hook is never called with the manager lock held.
				os_mutex_unlock(&m->lock);
				src_release_frame(m, cam, src);
				os_mutex_lock(&m->lock);
				break;
			}
			cam->streamed = true;
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
			if (cam_is_app(cam)) {
				// ADR-061: platform share-tap hooks never run under the manager lock.
				os_mutex_unlock(&m->lock);
				src_release_frame(m, cam, src);
				os_mutex_lock(&m->lock);
			} else {
				src_release_frame(m, cam, src);
			}
			break;
		}
		case XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT:
			if (cam->last_frame_ns == 0 && cam->source == XRT_STEREO_CAMERA_SOURCE_UVC &&
			    now - cam->opened_ns > FIRST_FRAME_DEADLINE_NS) {
				// #2221: opened, never delivered — the device is not really ours.
				U_LOG_W(
				    "stereo camera %llu: %s source delivered nothing for %lld ms after open; closing, "
				    "retry in 1 s",
				    (unsigned long long)cam->camera_id, source_str(cam->source),
				    (long long)(FIRST_FRAME_DEADLINE_NS / 1000000));
				set_state_locked(cam, XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
				end_started_streams_busy_locked(m, cam, "no first frame");
				cam->src = NULL;
				os_mutex_unlock(&m->lock);
				src_close(m, cam, src);
				os_mutex_lock(&m->lock);
				cam->retry_after_ns = os_monotonic_get_ns() + REOPEN_BACKOFF_NS;
			} else if (cam->last_frame_ns == 0) {
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
			U_LOG_W("stereo camera %llu: %s source reported an error; closing, retry in 1 s",
			        (unsigned long long)cam->camera_id, source_str(cam->source));
			if (cam_is_app(cam)) {
				// ADR-061: the tap lost its compositor; the rescan decides whether the
				// app is gone (SOURCE_LOST) or the next start may try again.
				m->apps_dirty = true;
				os_cond_broadcast(&m->watch_cond);
			}
			if (cam->source == XRT_STEREO_CAMERA_SOURCE_APP_LINK && ((struct scam_link_src *)src)->fatal) {
				// Arm 2: the app's ring is unusable (import failed / no fence): end
				// the share instead of reopening into the same failure. The close
				// below queues SHARE_STOPPED on the link.
				const uint32_t ended = app_end_streams_locked(m, cam);
				U_LOG_W(
				    "app share %llu \"%s\": the app's ring could not be used — %u stream(s) ended "
				    "SOURCE_LOST",
				    (unsigned long long)cam->camera_id, cam->info.display_name, ended);
			}
			if (g_platform.gave_up_is_suspended && cam->streamed) {
				// A2 (Android): a source that delivered and then lost the device
				// lost it to the OS (app in the background, sensor privacy):
				// SUSPENDED, the started streams kept, the retry runs on.
				set_state_locked(cam, XRT_STEREO_CAMERA_STATE_SUSPENDED);
			} else {
				set_state_locked(cam, XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
			}
			if (cam->last_frame_ns == 0 && !(g_platform.gave_up_is_suspended && cam->streamed)) {
				// #2221: it never delivered — a device another process holds errors on its first read.
				end_started_streams_busy_locked(
				    m, cam, "source failed before its first frame (held by another process?)");
			}
			cam->src = NULL;
			os_mutex_unlock(&m->lock);
			src_close(m, cam, src);
			os_mutex_lock(&m->lock);
			cam->retry_after_ns = os_monotonic_get_ns() + REOPEN_BACKOFF_NS;
			break;
		}
	}
	if (cam->src != NULL) {
		void *src = cam->src;
		cam->src = NULL;
		os_mutex_unlock(&m->lock);
		src_close(m, cam, src);
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

/*
 *
 * Camera discovery (#1961): ONE routine runs the first enumeration at create
 * and every later re-enumeration — on a client's enumerate (at most once per
 * RESCAN_MIN_INTERVAL_NS) and from the background watch thread while some
 * connection listens for camera events.
 *
 * Stability rules, so a re-enumeration never disturbs what clients hold:
 *  - a camera is matched by (source, device_identity) — the key persistentId
 *    is derived from — so an unchanged camera keeps its slot, its index in
 *    the enumeration and its camera_id;
 *  - slots are never removed or reordered: a camera its source stops listing
 *    is marked gone and reported UNAVAILABLE in place (its streams end with
 *    SOURCE_LOST), and comes back with the same id if it is listed again;
 *  - a new camera is appended with a fresh camera_id (never reused);
 *  - every source call (enumerate, calibration, UVC resolution) runs with the
 *    manager lock RELEASED, so a streaming camera thread never waits on it.
 * Any change queues XrEventDataStereoCamerasChangedDXR to every listening
 * connection.
 *
 */

static void
stream_end_locked(struct ipc_server_stereo_camera *m, struct scam_stream *s, uint32_t reason);

/*!
 * #2221: a source that cannot START (held by another process, unplugged between
 * enumerate and open, a driver that errors on its first read, or one that never
 * delivers) must not leave a started stream waiting at 0 fps. Every started stream
 * on the camera ends with SOURCE_BUSY (the browser maps it to NotReadableError),
 * the camera reads UNAVAILABLE, and the retry keeps running so a later start
 * succeeds once the device is free. A source that stops AFTER it delivered keeps
 * the old behaviour (streams wait through the retry).
 */
static void
end_started_streams_busy_locked(struct ipc_server_stereo_camera *m, struct scam_camera *cam, const char *why)
{
	uint32_t ended = 0;
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		struct scam_stream *s = &m->streams[i];
		if (s->used && s->camera == cam->index && s->started && !s->ended) {
			stream_end_locked(m, s, XRT_STEREO_CAMERA_END_SOURCE_BUSY);
			ended++;
		}
	}
	if (ended > 0) {
		U_LOG_W("stereo camera %llu \"%s\": %s — %u started stream(s) ended with SOURCE_BUSY",
		        (unsigned long long)cam->camera_id, cam->info.display_name, why, ended);
	}
}

//! One camera as its source lists it, staged outside the manager lock.
struct scam_candidate
{
	uint32_t source;
	uint32_t source_index;
	struct u_stereo_uvc *uvc;
	struct xrt_plugin_stereo_camera_info info;
	bool skip;                 //!< malformed, a duplicate, or no room
	struct scam_camera *match; //!< the existing camera it is, NULL = new
	//! New cameras: id, calibration and rectifier, built before insertion.
	uint64_t camera_id;
	bool have_calib, calib_nominal, baseline_known, hfov_known;
	struct xrt_plugin_stereo_camera_calibration calib;
	struct scam_rectifier *rect;
};

static bool
info_valid(struct xrt_plugin_stereo_camera_info *info)
{
	info->display_name[sizeof(info->display_name) - 1] = '\0';
	info->device_identity[sizeof(info->device_identity) - 1] = '\0';
	info->platform_device_hint[sizeof(info->platform_device_hint) - 1] = '\0';
	return info->eye_width != 0 && info->eye_height != 0 && (info->eye_width & 1u) == 0 &&
	       (info->eye_height & 1u) == 0 && info->native_format >= XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8 &&
	       info->native_format <= XRT_PLUGIN_STEREO_CAMERA_FORMAT_BGRA8;
}

/*!
 * Is the listed camera (@p source, @p info at @p source_index) the camera
 * @p cam? By device_identity — what persistentId is derived from; a camera
 * whose source gives no identity can only be keyed by its source index.
 */
static bool
same_camera(const struct scam_camera *cam,
            uint32_t source,
            uint32_t source_index,
            const struct xrt_plugin_stereo_camera_info *info)
{
	if (cam->source != source) {
		return false;
	}
	if (info->device_identity[0] != '\0') {
		return strcmp(cam->info.device_identity, info->device_identity) == 0;
	}
	return cam->info.device_identity[0] == '\0' && cam->source_index == source_index;
}

//! Shared tail of camera registration: rectifier, rate sanity, the WARN line.
static void
finish_camera(struct scam_camera *cam)
{
	if (cam->have_calib && (cam->info.flags & XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED) == 0) {
		cam->rect = rectifier_create(cam);
	}
	if (!(cam->info.max_frame_rate >= 0.0f && cam->info.max_frame_rate <= 1000.0f)) {
		cam->info.max_frame_rate = 0.0f; // NaN / negative / absurd = unknown
	}
	char rate[48];
	if (cam->info.max_frame_rate > 0.0f) {
		snprintf(rate, sizeof(rate), "advertised %.1f Hz", cam->info.max_frame_rate);
	} else {
		snprintf(rate, sizeof(rate), "rate unknown");
	}
	U_LOG_W(
	    "stereo camera %llu: \"%s\" (%s source) %ux%u per eye, %s (measured on first open), flags 0x%x, native "
	    "format %u%s",
	    (unsigned long long)cam->camera_id, cam->info.display_name, source_str(cam->source), cam->info.eye_width,
	    cam->info.eye_height, rate, cam->info.flags, cam->info.native_format,
	    cam->calib_nominal ? ", NOMINAL model (uncalibrated)" : "");
}

//! The active plug-in's cameras (rescan_lock held, manager lock NOT held).
static uint32_t
scan_plugin(struct ipc_server_stereo_camera *m, struct scam_candidate *out, uint32_t cap)
{
	if (m->iface == NULL) {
		return 0;
	}
	struct xrt_plugin_stereo_camera_info infos[XRT_STEREO_CAMERA_MAX_CAMERAS];
	memset(infos, 0, sizeof(infos));
	for (uint32_t i = 0; i < XRT_STEREO_CAMERA_MAX_CAMERAS; i++) {
		infos[i].struct_size = (uint32_t)sizeof(infos[i]);
	}
	uint32_t n = m->iface->stereo_camera_enumerate(m->inst, XRT_STEREO_CAMERA_MAX_CAMERAS, infos);
	n = n > XRT_STEREO_CAMERA_MAX_CAMERAS ? XRT_STEREO_CAMERA_MAX_CAMERAS : n;
	n = n > cap ? cap : n;
	for (uint32_t i = 0; i < n; i++) {
		out[i].source = XRT_STEREO_CAMERA_SOURCE_PLUGIN;
		out[i].source_index = i;
		out[i].info = infos[i];
	}
	return n;
}

static uint64_t
fnv1a(uint64_t h, const void *data, size_t size)
{
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < size; i++) {
		h = (h ^ p[i]) * 0x100000001b3ull;
	}
	return h;
}

static uint64_t
fnv1a_str(uint64_t h, const char *s)
{
	return fnv1a(h, s, strlen(s) + 1);
}

//! Field by field (struct padding is not hashed): what a UVC resolution depends on.
static uint64_t
uvc_config_hash(uint64_t h, const struct u_stereo_uvc_config *cfg)
{
	h = fnv1a(h, &cfg->count, sizeof(cfg->count));
	h = fnv1a(h, &cfg->fake, sizeof(cfg->fake));
	if (cfg->fake) {
		const struct u_stereo_uvc_fake_params *f = &cfg->fake_params;
		h = fnv1a(h, &f->disparity, sizeof(f->disparity));
		h = fnv1a(h, &f->dy, sizeof(f->dy));
		h = fnv1a(h, &f->pixel, sizeof(f->pixel));
		h = fnv1a(h, &f->fps, sizeof(f->fps));
	}
	for (uint32_t i = 0; i < cfg->count && i < U_STEREO_UVC_MAX_ENTRIES; i++) {
		const struct u_stereo_uvc_entry *e = &cfg->entries[i];
		h = fnv1a_str(h, e->name_contains);
		h = fnv1a(h, &e->match_vid_pid, sizeof(e->match_vid_pid));
		h = fnv1a(h, &e->vid, sizeof(e->vid));
		h = fnv1a(h, &e->pid, sizeof(e->pid));
		h = fnv1a_str(h, e->display_name);
		h = fnv1a(h, &e->layout, sizeof(e->layout));
		h = fnv1a(h, &e->swap_eyes, sizeof(e->swap_eyes));
		h = fnv1a(h, &e->mode_width, sizeof(e->mode_width));
		h = fnv1a(h, &e->mode_height, sizeof(e->mode_height));
		h = fnv1a(h, &e->mode_fps, sizeof(e->mode_fps));
		h = fnv1a(h, &e->eye_width, sizeof(e->eye_width));
		h = fnv1a(h, &e->eye_height, sizeof(e->eye_height));
		h = fnv1a(h, &e->baseline_mm, sizeof(e->baseline_mm));
		h = fnv1a(h, &e->hfov_deg, sizeof(e->hfov_deg));
		h = fnv1a_str(h, e->calibration);
	}
	return h;
}

/*!
 * Amendment 4: the service's own UVC side-by-side source. Opt-in only — with
 * no config (stereo-cameras.json / DXR_STEREO_CAMERA_UVC_CONFIG) nothing is
 * enumerated and no device is touched. Never claims a device a plug-in camera
 * names as its platform_device_hint (the tracker camera is the plug-in's).
 *
 * #1961: the config, the plug-in hints and the OS device list (ids only —
 * listing never opens a device) are hashed; only when the hash moved is a NEW
 * u_stereo_uvc resolved (@p out_obj), so an unchanged setup costs one config
 * read + one device listing per rescan and logs nothing.
 *
 * @return false = nothing changed since the last resolution (leave the UVC
 *         cameras as they are); true = @p out / @p out_obj are the truth.
 */
static bool
scan_uvc(struct ipc_server_stereo_camera *m,
         bool initial,
         bool room,
         const struct scam_candidate *plugin,
         uint32_t plugin_count,
         struct scam_candidate *out,
         uint32_t cap,
         uint32_t *out_count,
         struct u_stereo_uvc **out_obj)
{
	*out_count = 0;
	*out_obj = NULL;

	struct u_stereo_uvc_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	char path[1024] = {0}, err[256] = {0};
	enum u_stereo_uvc_config_load_result r = U_STEREO_UVC_CONFIG_OK;
	if (m->have_uvc_config) {
		cfg = m->uvc_config;
	} else {
		r = u_stereo_uvc_config_load(&cfg, path, sizeof(path), err, sizeof(err));
	}

	const char *hints[XRT_STEREO_CAMERA_MAX_CAMERAS];
	uint32_t nh = 0;
	for (uint32_t i = 0; i < plugin_count && nh < XRT_STEREO_CAMERA_MAX_CAMERAS; i++) {
		if (plugin[i].info.platform_device_hint[0] != '\0') {
			hints[nh++] = plugin[i].info.platform_device_hint;
		}
	}
	// Amendment 5: the cameras the active plug-in declares as its display's
	// (the eye tracker's) — refused like the plug-in cameras' hints, and part
	// of the fingerprint so a declaration that appears later re-resolves.
	char declared_buf[XRT_PLUGIN_CLAIMED_CAMERAS_MAX][XRT_PLUGIN_CLAIMED_CAMERA_ID_MAX];
	const char *declared[XRT_PLUGIN_CLAIMED_CAMERAS_MAX];
	uint32_t nd_claims = 0;
	if (m->claims_iface != NULL) {
		memset(declared_buf, 0, sizeof(declared_buf));
		uint32_t n =
		    m->claims_iface->get_claimed_cameras(m->claims_inst, XRT_PLUGIN_CLAIMED_CAMERAS_MAX, declared_buf);
		n = n > XRT_PLUGIN_CLAIMED_CAMERAS_MAX ? XRT_PLUGIN_CLAIMED_CAMERAS_MAX : n;
		for (uint32_t i = 0; i < n; i++) {
			declared_buf[i][XRT_PLUGIN_CLAIMED_CAMERA_ID_MAX - 1] = '\0';
			if (declared_buf[i][0] != '\0') {
				declared[nd_claims++] = declared_buf[i];
			}
		}
	}

	struct u_stereo_uvc_backend os_backend;
	const struct u_stereo_uvc_backend *backend = NULL;
	if (m->have_uvc_backend) {
		backend = &m->uvc_backend;
	} else if (os_uvc_capture_backend(&os_backend)) {
		backend = &os_backend;
	}

	uint64_t fp = 0xcbf29ce484222325ull;
	fp = fnv1a(fp, &r, sizeof(r));
	const bool configured = r == U_STEREO_UVC_CONFIG_OK && cfg.count > 0;
	if (configured) {
		fp = uvc_config_hash(fp, &cfg);
		for (uint32_t i = 0; i < nh; i++) {
			fp = fnv1a_str(fp, hints[i]);
		}
		for (uint32_t i = 0; i < nd_claims; i++) {
			fp = fnv1a_str(fp, declared[i]);
		}
		if (!cfg.fake && backend != NULL && backend->enumerate != NULL) {
			struct u_stereo_uvc_device devs[U_STEREO_UVC_MAX_DEVICES];
			memset(devs, 0, sizeof(devs));
			uint32_t nd = backend->enumerate(backend->ctx, devs, U_STEREO_UVC_MAX_DEVICES);
			nd = nd > U_STEREO_UVC_MAX_DEVICES ? U_STEREO_UVC_MAX_DEVICES : nd;
			for (uint32_t i = 0; i < nd; i++) {
				devs[i].id[sizeof(devs[i].id) - 1] = '\0';
				fp = fnv1a_str(fp, devs[i].id);
			}
		}
	}
	if (m->uvc_scanned_once && fp == m->uvc_fingerprint) {
		return false;
	}
	if (configured && !room) {
		// Every resolution slot is held by a camera: try again on a later rescan.
		return false;
	}
	m->uvc_fingerprint = fp;
	m->uvc_scanned_once = true;

	if (!m->have_uvc_config) {
		if (r == U_STEREO_UVC_CONFIG_INVALID) {
			U_LOG_W("stereo camera (uvc): config \"%s\" is INVALID (%s) — no UVC camera", path, err);
		} else if (r != U_STEREO_UVC_CONFIG_OK) {
			U_LOG_I("stereo camera (uvc): no UVC stereo camera configured (%s: %s)", path[0] ? path : "-",
			        err);
		} else {
			U_LOG_W("stereo camera (uvc): %sconfig \"%s\": %u entr%s%s", initial ? "" : "re-resolving, ",
			        path, cfg.count, cfg.count == 1 ? "y" : "ies",
			        cfg.fake ? ", FAKE synthetic device (no real camera is considered)" : "");
		}
	}
	if (!configured) {
		return true;
	}

	if (nd_claims > 0) {
		U_LOG_I("stereo camera (uvc): the display plug-in declares %u tracker camera id(s) — never claimed",
		        nd_claims);
	}
	const struct u_stereo_uvc_denylist dl = {hints, nh, declared, nd_claims};
	struct u_stereo_uvc *obj = u_stereo_uvc_create_ex(&cfg, backend, &dl);
	struct xrt_plugin_stereo_camera_info infos[XRT_STEREO_CAMERA_MAX_CAMERAS];
	memset(infos, 0, sizeof(infos));
	for (uint32_t i = 0; i < XRT_STEREO_CAMERA_MAX_CAMERAS; i++) {
		infos[i].struct_size = (uint32_t)sizeof(infos[i]);
	}
	uint32_t n = u_stereo_uvc_enumerate(obj, XRT_STEREO_CAMERA_MAX_CAMERAS, infos);
	n = n > XRT_STEREO_CAMERA_MAX_CAMERAS ? XRT_STEREO_CAMERA_MAX_CAMERAS : n;
	n = n > cap ? cap : n;
	for (uint32_t i = 0; i < n; i++) {
		out[i].source = XRT_STEREO_CAMERA_SOURCE_UVC;
		out[i].source_index = i;
		out[i].uvc = obj;
		out[i].info = infos[i];
	}
	*out_count = n;
	*out_obj = obj;
	return true;
}

//! Free every UVC resolution no camera references (an open stream's camera does).
static void
uvc_gc_locked(struct ipc_server_stereo_camera *m)
{
	for (uint32_t k = 0; k < UVC_OBJS_MAX; k++) {
		if (m->uvc_objs[k] == NULL) {
			continue;
		}
		bool used = false;
		for (uint32_t i = 0; i < m->camera_count; i++) {
			used |= m->cams[i].uvc == m->uvc_objs[k];
		}
		if (!used) {
			u_stereo_uvc_destroy(&m->uvc_objs[k]);
		}
	}
}

static bool
uvc_has_room_locked(struct ipc_server_stereo_camera *m)
{
	uvc_gc_locked(m);
	for (uint32_t k = 0; k < UVC_OBJS_MAX; k++) {
		if (m->uvc_objs[k] == NULL) {
			return true;
		}
	}
	return false;
}

//! Is any connection listening for camera events (it enumerated or created a stream)?
static bool
has_listener_locked(const struct ipc_server_stereo_camera *m)
{
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		if (m->evq[i].owner != 0) {
			return true;
		}
	}
	return false;
}

//! Queue XrEventDataStereoCamerasChangedDXR to every listening connection (coalesced).
static void
notify_cameras_changed_locked(struct ipc_server_stereo_camera *m)
{
	const struct xrt_stereo_camera_event ev = {.kind = XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED};
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		struct scam_evq *q = &m->evq[i];
		if (q->owner == 0) {
			continue;
		}
		if (q->count > 0 &&
		    q->ev[(q->head + q->count - 1) % EVQ_CAP].kind == XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) {
			continue; // one pending "re-enumerate" is enough
		}
		evq_push_locked(m, q->owner, &ev);
	}
}

//! A known camera listed again: revive it, follow its source index, refresh its labels.
static bool
refresh_camera_locked(struct ipc_server_stereo_camera *m, struct scam_camera *cam, const struct scam_candidate *c)
{
	(void)m;
	bool changed = false;
	if (cam->src == NULL && !cam->opening) {
		// Where to open it next time. An open (or opening) camera keeps the
		// source it was opened from; it is repointed after it closes.
		cam->source_index = c->source_index;
		if (c->uvc != NULL) {
			cam->uvc = c->uvc;
		}
	}
	if (cam->gone) {
		cam->gone = false;
		cam->retry_after_ns = 0;
		U_LOG_W("stereo camera %llu \"%s\": listed again by its %s source — back, same id",
		        (unsigned long long)cam->camera_id, c->info.display_name, source_str(cam->source));
		if (cam->src == NULL) {
			set_state_locked(cam, XRT_STEREO_CAMERA_STATE_AVAILABLE);
		}
		changed = true;
	}
	if (strcmp(cam->info.display_name, c->info.display_name) != 0) {
		U_LOG_W("stereo camera %llu: display name \"%s\" -> \"%s\"", (unsigned long long)cam->camera_id,
		        cam->info.display_name, c->info.display_name);
		snprintf(cam->info.display_name, sizeof(cam->info.display_name), "%s", c->info.display_name);
		changed = true;
	}
	if (strcmp(cam->info.platform_device_hint, c->info.platform_device_hint) != 0) {
		snprintf(cam->info.platform_device_hint, sizeof(cam->info.platform_device_hint), "%s",
		         c->info.platform_device_hint);
		changed = true;
	}
	if (cam->info.monitor_id != c->info.monitor_id) {
		// The screen it belongs to moved (display plugged in, claim resolved
		// late, monitor re-keyed): a label, like the name.
		U_LOG_W("stereo camera %llu \"%s\": screen 0x%016llx -> 0x%016llx", (unsigned long long)cam->camera_id,
		        cam->info.display_name, (unsigned long long)cam->info.monitor_id,
		        (unsigned long long)c->info.monitor_id);
		cam->info.monitor_id = c->info.monitor_id;
		changed = true;
	}
	if (cam->info.frame_orientation != c->info.frame_orientation) {
		// Spec v7: a property the plug-in may only learn late (e.g. once it
		// read the sensor's characteristics); reported like the screen.
		U_LOG_W("stereo camera %llu \"%s\": frame orientation %u -> %u deg cw",
		        (unsigned long long)cam->camera_id, cam->info.display_name,
		        frame_orientation_deg(cam->info.frame_orientation),
		        frame_orientation_deg(c->info.frame_orientation));
		cam->info.frame_orientation = c->info.frame_orientation;
		changed = true;
	}
	if (c->info.max_frame_rate > 0.0f && c->info.max_frame_rate <= 1000.0f) {
		cam->info.max_frame_rate = c->info.max_frame_rate; // a rate the source learned since
	}
	if ((cam->info.eye_width != c->info.eye_width || cam->info.eye_height != c->info.eye_height ||
	     cam->info.native_format != c->info.native_format || cam->info.flags != c->info.flags) &&
	    !cam->description_drift_logged) {
		// Streams, rings and the rectifier are sized from the first
		// description; swapping it under them is not safe. Kept until the
		// service restarts.
		cam->description_drift_logged = true;
		U_LOG_W(
		    "stereo camera %llu \"%s\": its source now describes it as %ux%u format %u flags 0x%x (was %ux%u "
		    "format %u flags 0x%x) — the first description is kept until the service restarts",
		    (unsigned long long)cam->camera_id, cam->info.display_name, c->info.eye_width, c->info.eye_height,
		    c->info.native_format, c->info.flags, cam->info.eye_width, cam->info.eye_height,
		    cam->info.native_format, cam->info.flags);
	}
	return changed;
}

//! A known camera its source no longer lists: UNAVAILABLE in place, its streams end.
static void
vanish_camera_locked(struct ipc_server_stereo_camera *m, struct scam_camera *cam)
{
	cam->gone = true;
	uint32_t ended = 0;
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		struct scam_stream *s = &m->streams[i];
		if (s->used && s->camera == cam->index && !s->ended) {
			stream_end_locked(m, s, XRT_STEREO_CAMERA_END_SOURCE_LOST);
			ended++;
		}
	}
	U_LOG_W(
	    "stereo camera %llu \"%s\": no longer listed by its %s source — UNAVAILABLE (id and index kept), %u "
	    "stream(s) ended",
	    (unsigned long long)cam->camera_id, cam->info.display_name, source_str(cam->source), ended);
	set_state_locked(cam, XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
}

static void
insert_camera_locked(struct ipc_server_stereo_camera *m, struct scam_candidate *c)
{
	struct scam_camera *cam = &m->cams[m->camera_count];
	cam->index = m->camera_count;
	cam->source = c->source;
	cam->source_index = c->source_index;
	cam->uvc = c->uvc;
	cam->camera_id = c->camera_id;
	cam->info = c->info;
	cam->have_calib = c->have_calib;
	cam->calib_nominal = c->calib_nominal;
	cam->baseline_known = c->baseline_known;
	cam->hfov_known = c->hfov_known;
	cam->calib = c->calib;
	cam->rect = c->rect;
	c->rect = NULL;
	cam->mgr = m;
	cam->state = XRT_STEREO_CAMERA_STATE_AVAILABLE;
	os_cond_init(&cam->cond);
	m->camera_count++;
}

/*!
 * Re-query every camera source and merge the result into the camera list
 * (see the block comment above). @p force skips the once-per-second throttle;
 * @p initial is the create-time enumeration (no event, nobody listens yet).
 * Takes rescan_lock then the manager lock itself; call with neither held.
 * @return true if the camera set changed.
 */
static bool
rescan(struct ipc_server_stereo_camera *m, bool force, bool initial)
{
	os_mutex_lock(&m->rescan_lock);
	os_mutex_lock(&m->lock);
	const int64_t now = os_monotonic_get_ns();
	bool skip = m->shutting_down || m->consent.kill_switch || (!initial && !m->sharing_enabled) ||
	            (!force && m->last_rescan_ns != 0 && now - m->last_rescan_ns < RESCAN_MIN_INTERVAL_NS);
	if (!skip) {
		m->last_rescan_ns = now;
	}
	const bool uvc_room = !skip && uvc_has_room_locked(m);
	os_mutex_unlock(&m->lock);
	if (skip) {
		os_mutex_unlock(&m->rescan_lock);
		return false;
	}

	// 1. List (no manager lock): the plug-in, then UVC (it avoids the plug-in's devices).
	const uint32_t cap = 2 * XRT_STEREO_CAMERA_MAX_CAMERAS;
	struct scam_candidate *cands = U_TYPED_ARRAY_CALLOC(struct scam_candidate, cap);
	if (cands == NULL) {
		os_mutex_unlock(&m->rescan_lock);
		return false;
	}
	const bool plugin_scanned = m->iface != NULL;
	const uint32_t np = scan_plugin(m, cands, XRT_STEREO_CAMERA_MAX_CAMERAS);
	uint32_t nu = 0;
	struct u_stereo_uvc *uvc_obj = NULL;
	const bool uvc_scanned =
	    scan_uvc(m, initial, uvc_room, cands, np, cands + np, XRT_STEREO_CAMERA_MAX_CAMERAS, &nu, &uvc_obj);
	const uint32_t n = np + nu;

	// 2. Match against the known cameras; reserve ids for new ones (locked).
	os_mutex_lock(&m->lock);
	uint32_t room = XRT_STEREO_CAMERA_MAX_CAMERAS - m->camera_count;
	for (uint32_t i = 0; i < n; i++) {
		struct scam_candidate *c = &cands[i];
		const bool valid = info_valid(&c->info); // also NUL-terminates the strings
		for (uint32_t k = 0; k < m->camera_count && c->match == NULL; k++) {
			if (same_camera(&m->cams[k], c->source, c->source_index, &c->info)) {
				c->match = &m->cams[k];
			}
		}
		if (!valid) {
			// A known camera listed with a malformed description is still
			// listed (not gone); it is just not refreshed from this one.
			c->skip = true;
			if (initial) {
				U_LOG_W(
				    "stereo camera: %s camera %u has a malformed description (%ux%u, format %u) — "
				    "skipped",
				    source_str(c->source), c->source_index, c->info.eye_width, c->info.eye_height,
				    c->info.native_format);
			}
			continue;
		}
		for (uint32_t k = 0; k < i && !c->skip; k++) {
			// A source listing one device twice: the first entry wins.
			const struct scam_candidate *o = &cands[k];
			c->skip = !o->skip && o->source == c->source && c->info.device_identity[0] != '\0' &&
			          strcmp(o->info.device_identity, c->info.device_identity) == 0;
		}
		if (c->skip || c->match != NULL) {
			continue;
		}
		if (room == 0) {
			c->skip = true;
			if (initial) {
				U_LOG_W("stereo camera: more than %d cameras — \"%s\" (%s) skipped",
				        XRT_STEREO_CAMERA_MAX_CAMERAS, c->info.display_name, source_str(c->source));
			}
			continue;
		}
		room--;
		c->camera_id = ++m->next_camera_id; // never reused in this service lifetime
	}
	os_mutex_unlock(&m->lock);

	// 3. Build each NEW camera (calibration, rectifier) — no manager lock.
	for (uint32_t i = 0; i < n; i++) {
		struct scam_candidate *c = &cands[i];
		if (c->skip || c->match != NULL) {
			continue;
		}
		c->calib.struct_size = (uint32_t)sizeof(c->calib);
		if (c->source == XRT_STEREO_CAMERA_SOURCE_UVC) {
			bool nominal = true;
			if (u_stereo_uvc_get_calibration(c->uvc, c->source_index, &c->calib, &nominal,
			                                 &c->baseline_known, &c->hfov_known) == XRT_SUCCESS) {
				c->have_calib = true;
				c->calib_nominal = nominal;
			}
		} else if (c->info.flags & XRT_PLUGIN_STEREO_CAMERA_CALIBRATED) {
			if (m->iface->stereo_camera_get_calibration(m->inst, c->source_index, &c->calib) ==
			    XRT_SUCCESS) {
				c->have_calib = true;
			} else {
				U_LOG_W(
				    "stereo camera: \"%s\" claims CALIBRATED but returned no calibration — RAW only",
				    c->info.display_name);
				c->info.flags &= ~XRT_PLUGIN_STEREO_CAMERA_CALIBRATED;
			}
		}
		// finish_camera works on a camera: run it on a scratch one, keep the results.
		struct scam_camera *tmp = U_TYPED_CALLOC(struct scam_camera);
		if (tmp == NULL) {
			c->skip = true;
			continue;
		}
		tmp->source = c->source;
		tmp->camera_id = c->camera_id;
		tmp->info = c->info;
		tmp->have_calib = c->have_calib;
		tmp->calib_nominal = c->calib_nominal;
		tmp->calib = c->calib;
		tmp->mgr = m;
		finish_camera(tmp);
		c->info = tmp->info;
		c->rect = tmp->rect;
		free(tmp);
	}

	// 4. Merge (locked).
	os_mutex_lock(&m->lock);
	bool changed = false;
	bool seen[XRT_STEREO_CAMERA_MAX_CAMERAS] = {false};
	for (uint32_t i = 0; i < n; i++) {
		struct scam_candidate *c = &cands[i];
		if (c->match != NULL && !c->skip) {
			seen[c->match->index] = true;
			changed |= refresh_camera_locked(m, c->match, c);
		} else if (c->match != NULL) {
			// A malformed re-listing of a known camera still says "it is there".
			seen[c->match->index] = true;
		}
	}
	const uint32_t known = m->camera_count;
	for (uint32_t i = 0; i < n; i++) {
		struct scam_candidate *c = &cands[i];
		if (c->skip || c->match != NULL || m->camera_count >= XRT_STEREO_CAMERA_MAX_CAMERAS) {
			continue;
		}
		if (!initial) {
			U_LOG_W("stereo camera %llu \"%s\": NEW — listed by the %s source since the last enumeration",
			        (unsigned long long)c->camera_id, c->info.display_name, source_str(c->source));
		}
		insert_camera_locked(m, c);
		changed = true;
	}
	for (uint32_t i = 0; i < known; i++) {
		struct scam_camera *cam = &m->cams[i];
		const bool scanned = cam->source == XRT_STEREO_CAMERA_SOURCE_UVC ? uvc_scanned : plugin_scanned;
		if (!seen[i] && !cam->gone && scanned) {
			vanish_camera_locked(m, cam);
			changed = true;
		}
	}
	if (uvc_obj != NULL) {
		for (uint32_t k = 0; k < UVC_OBJS_MAX; k++) {
			if (m->uvc_objs[k] == NULL) {
				m->uvc_objs[k] = uvc_obj; // uvc_room said one is free
				uvc_obj = NULL;
				break;
			}
		}
	}
	uvc_gc_locked(m);
	if (changed && !initial) {
		notify_cameras_changed_locked(m);
	}
	os_mutex_unlock(&m->lock);

	u_stereo_uvc_destroy(&uvc_obj); // only if no slot took it (cannot happen; defensive)
	for (uint32_t i = 0; i < n; i++) {
		if (cands[i].rect != NULL) {
			rectifier_destroy(&cands[i].rect); // built, then out of room
		}
	}
	free(cands);
	os_mutex_unlock(&m->rescan_lock);
	if (changed && !initial) {
		indicator_changed(m); // a vanished camera may have ended streams
	}
	return changed;
}

/*
 *
 * ADR-061 app entries: the app table, rebuilt from the live connections.
 *
 * Rules (spec §2 "Which apps are listed" / "Lifetime of an entry"):
 *  - a candidate is a connection with a session AND a projection compositor,
 *    except CONTROLLER, DIAG, CAMERA_CONSUMER and PRESENT_OWNER clients (a
 *    present-owner is a browser: its pages carry other origins' content) and
 *    sessions that opted out (XrSessionShareabilityDXR). The consumer's own
 *    process is filtered per consumer at enumeration time;
 *  - a candidate the platform cannot tap is LISTED UNAVAILABLE with a reason
 *    (no tap hooks = "no tap on this platform": the macOS / Linux service);
 *  - an entry keeps its camera_id while its app runs, and the same
 *    (executable, instance ordinal) gets the same id back within this service
 *    run; an exited app's entry stays UNAVAILABLE for app_linger_ns (its
 *    streams end SOURCE_LOST at once), then leaves the list; an opted-out
 *    session leaves the list at once;
 *  - app entries come after every camera, so no camera index moves.
 * Every change queues XrEventDataStereoCamerasChangedDXR.
 *
 */

//! One candidate connection, staged without the manager lock.
struct app_cand
{
	uint32_t client_id;
	long pid;
	bool opted_out;
	char exe_path[U_CAMERA_CONSENT_EXE_MAX];
	char app_name[XRT_APP_SHARE_APPLICATION_NAME_SIZE];
	struct xrt_app_share_source_info q; //!< the platform's description
	int64_t last_commit_ns;
	bool matched;
	//! XRT_STEREO_CAMERA_SOURCE_APP (a session on its IPC connection) or _APP_LINK (an in-process service link).
	uint32_t source;
	struct xrt_share_link_registration reg; //!< APP_LINK: the link's registration
	uint32_t link_content_w, link_content_h;
};

static const char *
path_base(const char *path)
{
	const char *b = path;
	for (const char *p = path; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			b = p + 1;
		}
	}
	return b;
}

//! The share stream size + rate of a new app entry (`share.resolution`).
static void
app_stream_extent(struct ipc_server_stereo_camera *m, uint32_t *w, uint32_t *h, float *fps)
{
	enum u_app_share_resolution r = U_APP_SHARE_RESOLUTION_720P;
	if (m->share_resolution_override == 1) {
		r = U_APP_SHARE_RESOLUTION_720P;
	} else if (m->share_resolution_override == 2) {
		r = U_APP_SHARE_RESOLUTION_1080P;
	} else {
		struct u_app_share_settings st;
		u_app_share_settings_load(&st);
		r = st.resolution;
	}
	u_app_share_resolution_extent(r, w, h, fps);
}

//! The camera_id (exe, ordinal) had earlier in this run, or a fresh one remembered for it.
static uint64_t
app_id_for_locked(struct ipc_server_stereo_camera *m, const char *exe, uint32_t ordinal)
{
	for (uint32_t i = 0; i < APP_ID_MEMORY; i++) {
		if (m->app_ids[i].camera_id != 0 && m->app_ids[i].ordinal == ordinal &&
		    strcmp(m->app_ids[i].exe, exe) == 0) {
			return m->app_ids[i].camera_id;
		}
	}
	uint64_t id = ++m->next_camera_id; // shared with cameras: ids never collide, never reused
	uint32_t k = m->app_id_next++ % APP_ID_MEMORY;
	snprintf(m->app_ids[k].exe, sizeof(m->app_ids[k].exe), "%s", exe);
	m->app_ids[k].ordinal = ordinal;
	m->app_ids[k].camera_id = id;
	return id;
}

//! Is any stream still pointing at app slot @p cam (an ended one waiting for destroy, too)?
static bool
app_slot_referenced_locked(struct ipc_server_stereo_camera *m, const struct scam_camera *cam)
{
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		if (m->streams[i].used && m->streams[i].camera == cam->index) {
			return true;
		}
	}
	return cam->src != NULL || cam->opening || cam->started_count > 0;
}

//! The state an idle app entry reports from its description (a running stream's thread overrides it).
static uint32_t
app_idle_state(const struct scam_camera *cam, int64_t now)
{
	if (!cam->app.live || cam->app.desc.availability != XRT_APP_SHARE_AVAILABLE) {
		return XRT_STEREO_CAMERA_STATE_UNAVAILABLE;
	}
	if (cam->app.last_commit_ns > 0 && now - cam->app.last_commit_ns > SUSPEND_AFTER_NS) {
		return XRT_STEREO_CAMERA_STATE_SUSPENDED;
	}
	return XRT_STEREO_CAMERA_STATE_AVAILABLE;
}

//! End every stream of app entry @p cam with SOURCE_LOST; returns how many.
static uint32_t
app_end_streams_locked(struct ipc_server_stereo_camera *m, struct scam_camera *cam)
{
	uint32_t ended = 0;
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		struct scam_stream *s = &m->streams[i];
		if (s->used && s->camera == cam->index && !s->ended) {
			stream_end_locked(m, s, XRT_STEREO_CAMERA_END_SOURCE_LOST);
			ended++;
		}
	}
	return ended;
}

/*!
 * The app of entry @p cam is gone (exited, session destroyed, or opted out):
 * UNAVAILABLE, streams SOURCE_LOST, its per-app-run grants die, and the camera
 * thread detaches the tap. @p delist: leave the list now (opt-out) instead of
 * after the linger.
 */
static bool
app_mark_gone_locked(
    struct ipc_server_stereo_camera *m, struct scam_camera *cam, int64_t now, bool delist, const char *why)
{
	if (!cam->app.live && (!delist || !cam->app.listed)) {
		return false;
	}
	cam->app.live = false;
	cam->app.exited_ns = now;
	cam->app.desc.availability = XRT_APP_SHARE_UNAVAILABLE_EXITED;
	// Its per-app-run grants die with it (applied by share_forget_drain).
	if (m->share_forget_count < sizeof(m->share_forget) / sizeof(m->share_forget[0])) {
		m->share_forget[m->share_forget_count++] = cam->camera_id;
	} else {
		m->share_forget[0] = UINT64_MAX; // overflow: forget everything (a grant only saves a prompt)
	}
	uint32_t ended = app_end_streams_locked(m, cam);
	set_state_locked(cam, XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
	if (delist) {
		cam->app.listed = false;
	}
	os_cond_signal(&cam->cond); // the camera thread closes the source now (no linger)
	U_LOG_W("app share %llu \"%s\" (pid %ld): %s — UNAVAILABLE%s, %u stream(s) ended SOURCE_LOST",
	        (unsigned long long)cam->camera_id, cam->info.display_name, cam->app.pid, why,
	        delist ? " and unlisted" : " (listed while it lingers)", ended);
	return true;
}

/*!
 * Apply the pending per-app-run grant forgets (ADR-061 spec §6: a grant
 * lasts until the shared app exits). Call with NO lock held.
 */
static void
share_forget_drain(struct ipc_server_stereo_camera *m)
{
	uint64_t keys[APP_MAX * 2];
	os_mutex_lock(&m->lock);
	const uint32_t n = m->share_forget_count;
	memcpy(keys, m->share_forget, sizeof(keys[0]) * n);
	m->share_forget_count = 0;
	os_mutex_unlock(&m->lock);
	if (n == 0) {
		return;
	}
	os_mutex_lock(&m->consent_lock);
	for (uint32_t i = 0; i < n; i++) {
		if (keys[i] == UINT64_MAX) {
			u_app_share_consent_forget_all(&m->share);
		} else {
			u_app_share_consent_forget_app(&m->share, keys[i]);
		}
	}
	os_mutex_unlock(&m->consent_lock);
}

//! Describe a candidate through the platform tap (no manager lock held).
static void
app_query(struct app_cand *c)
{
	memset(&c->q, 0, sizeof(c->q));
	c->q.pid = (uint64_t)c->pid;
	snprintf(c->q.executable_name, sizeof(c->q.executable_name), "%s", path_base(c->exe_path));
	snprintf(c->q.application_name, sizeof(c->q.application_name), "%s", c->app_name);
	c->q.source = XRT_STEREO_CAMERA_SOURCE_APP;
	c->q.view_count = 2;
	c->q.active_view_count = 2;
	c->last_commit_ns = 0;
	if (!g_platform_set || g_platform.share_tap_query == NULL) {
		c->q.availability = XRT_APP_SHARE_UNAVAILABLE_NO_TAP;
		return;
	}
	c->q.availability = XRT_APP_SHARE_AVAILABLE;
	if (!g_platform.share_tap_query(g_platform.ctx, c->client_id, &c->q, &c->last_commit_ns)) {
		if (c->q.availability == XRT_APP_SHARE_AVAILABLE) {
			c->q.availability = XRT_APP_SHARE_UNAVAILABLE_NO_COMPOSITOR;
		}
	}
	// The manager's facts win over anything the hook wrote.
	c->q.pid = (uint64_t)c->pid;
	snprintf(c->q.executable_name, sizeof(c->q.executable_name), "%s", path_base(c->exe_path));
	snprintf(c->q.application_name, sizeof(c->q.application_name), "%s", c->app_name);
	c->q.source = XRT_STEREO_CAMERA_SOURCE_APP;
}

/*!
 * Describe an in-process service link (ADR-061 decision 9, spec §3): AVAILABLE
 * only when the app's compositor can tap (Windows fill arm), its ring lives on
 * the adapter the service publishes, and this service can import a ring;
 * otherwise UNAVAILABLE with the reason. No manager lock held.
 */
static void
app_query_link(struct app_cand *c, bool fake)
{
	memset(&c->q, 0, sizeof(c->q));
	c->q.pid = (uint64_t)c->pid;
	snprintf(c->q.executable_name, sizeof(c->q.executable_name), "%s", path_base(c->exe_path));
	snprintf(c->q.application_name, sizeof(c->q.application_name), "%s", c->app_name);
	c->q.source = XRT_STEREO_CAMERA_SOURCE_APP_LINK;
	c->q.view_count = 2;
	c->q.active_view_count = 2;
	c->q.content_eye_width = c->link_content_w;
	c->q.content_eye_height = c->link_content_h;
	c->last_commit_ns = 0; // a link reports commits only while it streams
	const struct xrt_share_link_registration *r = &c->reg;
	uint8_t svc_luid[XRT_LUID_SIZE] = {0};
	const bool svc_luid_known = g_platform_set && g_platform.share_link_adapter_luid != NULL &&
	                            g_platform.share_link_adapter_luid(g_platform.ctx, svc_luid);
	const bool importer =
	    fake || (g_platform_set && g_platform.share_link_import != NULL && g_platform.share_link_convert != NULL);
	if (r->tier == XRT_SHARE_LINK_TIER_GL) {
		c->q.availability = XRT_APP_SHARE_UNAVAILABLE_UNSUPPORTED_API;
	} else if (r->tap_available == 0) {
		c->q.availability =
		    r->tap_unavailable_reason != 0 ? r->tap_unavailable_reason : XRT_APP_SHARE_UNAVAILABLE_NO_TAP;
	} else if (r->luid_valid != 0 && svc_luid_known && memcmp(r->luid, svc_luid, XRT_LUID_SIZE) != 0) {
		c->q.availability = XRT_APP_SHARE_UNAVAILABLE_OTHER_ADAPTER;
	} else if (!importer) {
		c->q.availability = XRT_APP_SHARE_UNAVAILABLE_NO_TAP;
	} else {
		c->q.availability = XRT_APP_SHARE_AVAILABLE;
	}
}

//! Fill a fresh app slot from @p c (manager lock held; the slot is unreferenced).
/*!
 * Fill a fresh app slot from @p c (manager lock held; the slot is
 * unreferenced). @p w / @p h / @p fps: the share stream size, resolved from
 * `share.resolution` BEFORE the lock was taken (it may read the user store).
 */
static void
app_insert_locked(struct ipc_server_stereo_camera *m,
                  struct scam_camera *cam,
                  const struct app_cand *c,
                  int64_t now,
                  uint32_t w,
                  uint32_t h,
                  float fps)
{
	uint32_t ordinal = 0;
	for (bool clash = true; clash; ordinal++) {
		clash = false;
		for (uint32_t i = 0; i < APP_MAX; i++) {
			const struct scam_camera *o = &m->apps[i];
			clash |= o != cam && o->app.listed && o->app.live && o->app.ordinal == ordinal &&
			         strcmp(o->app.exe_path, c->exe_path) == 0;
		}
		if (!clash) {
			break;
		}
	}
	// ADR-061 spec §2: one cameraId per running app. The same (exe, ordinal)
	// relaunching while its old entry still lingers takes the id over (via
	// app_id_for_locked) and the lingering entry leaves the list now — never
	// two listed rows with one cameraId.
	for (uint32_t i = 0; i < APP_MAX; i++) {
		struct scam_camera *o = &m->apps[i];
		if (o != cam && o->app.listed && !o->app.live && o->app.ordinal == ordinal &&
		    strcmp(o->app.exe_path, c->exe_path) == 0) {
			o->app.listed = false;
			U_LOG_W("app share %llu \"%s\": relaunched while its exited entry lingered — the new instance "
			        "takes the id, the old entry leaves the list",
			        (unsigned long long)o->camera_id, o->info.display_name);
		}
	}

	cam->source = c->q.source == XRT_STEREO_CAMERA_SOURCE_APP_LINK ? XRT_STEREO_CAMERA_SOURCE_APP_LINK
	                                                               : XRT_STEREO_CAMERA_SOURCE_APP;
	cam->source_index = 0;
	cam->uvc = NULL;
	cam->gone = false;
	cam->have_calib = false;
	cam->calib_nominal = false;
	cam->rect = NULL;
	cam->calibration_generation = 0;
	cam->source_rate = 0.0f;
	cam->last_frame_ns = 0;
	cam->streamed = false;
	cam->retry_after_ns = 0;
	cam->rate_logged = false;
	u_stereo_camera_rate_meter_restart(&cam->rate_meter);
	memset(&cam->info, 0, sizeof(cam->info));
	cam->info.struct_size = (uint32_t)sizeof(cam->info);
	snprintf(cam->info.display_name, sizeof(cam->info.display_name), "%s",
	         c->app_name[0] != '\0' ? c->app_name : path_base(c->exe_path));
	// What persistentId is keyed on (spec §2): executable path + instance ordinal.
	snprintf(cam->info.device_identity, sizeof(cam->info.device_identity), "app:%s#%u", c->exe_path, ordinal);
	cam->info.flags = XRT_STEREO_CAMERA_APP_CONTENT | XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED |
	                  XRT_PLUGIN_STEREO_CAMERA_RECTIFIABLE;
	cam->info.eye_width = w;
	cam->info.eye_height = h;
	cam->info.max_frame_rate = fps;
	cam->info.native_format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;

	memset(&cam->app, 0, sizeof(cam->app));
	cam->app.listed = true;
	cam->app.live = true;
	cam->app.client_id = c->client_id;
	cam->app.pid = c->pid;
	snprintf(cam->app.exe_path, sizeof(cam->app.exe_path), "%s", c->exe_path);
	cam->app.ordinal = ordinal;
	cam->app.desc = c->q;
	cam->app.last_commit_ns = c->last_commit_ns;
	cam->camera_id = app_id_for_locked(m, c->exe_path, ordinal);
	cam->app.desc.camera_id = cam->camera_id;
	cam->app.desc.instance_ordinal = ordinal;
	cam->mgr = m;
	cam->state = app_idle_state(cam, now);
	U_LOG_W("app share %llu: \"%s\" (%s, pid %ld, copy #%u) listed %s%s — stream %ux%u per eye @ %.0f Hz",
	        (unsigned long long)cam->camera_id, cam->info.display_name, path_base(c->exe_path), c->pid, ordinal,
	        state_str(cam->state),
	        cam->app.desc.availability == XRT_APP_SHARE_AVAILABLE            ? ""
	        : cam->app.desc.availability == XRT_APP_SHARE_UNAVAILABLE_NO_TAP ? " (no share tap on this platform)"
	                                                                         : " (cannot be tapped)",
	        w, h, fps);
}

//! The synthetic DXR_APP_SHARE_FAKE entry (create time, manager lock held).
static void
app_insert_fake_locked(struct ipc_server_stereo_camera *m)
{
	struct app_cand c;
	memset(&c, 0, sizeof(c));
	c.pid = 0;
	snprintf(c.exe_path, sizeof(c.exe_path), "%s", "displayxr-app-share-fake");
	snprintf(c.app_name, sizeof(c.app_name), "%s", "DisplayXR app-share fake");
	c.q.source = XRT_STEREO_CAMERA_SOURCE_APP;
	snprintf(c.q.executable_name, sizeof(c.q.executable_name), "%s", c.exe_path);
	snprintf(c.q.application_name, sizeof(c.q.application_name), "%s", c.app_name);
	c.q.availability = XRT_APP_SHARE_AVAILABLE;
	c.q.view_count = 2;
	c.q.active_view_count = 2;
	struct scam_camera *cam = &m->apps[0];
	cam->index = APP_SLOT_BASE;
	uint32_t w = 0, h = 0;
	float fps = 0.0f;
	app_stream_extent(m, &w, &h, &fps); // create time: no manager lock is held yet
	app_insert_locked(m, cam, &c, os_monotonic_get_ns(), w, h, fps);
	cam->app.fake = true;
	cam->app.desc.content_eye_width = cam->info.eye_width;
	cam->app.desc.content_eye_height = cam->info.eye_height;
	U_LOG_W(
	    "app share: DXR_APP_SHARE_FAKE=1 — a SYNTHETIC app source (SBS NV12, disparity ramp, zero-disparity band) "
	    "is listed as app share %llu (diagnostics / CI only)",
	    (unsigned long long)cam->camera_id);
}

static bool
app_class_shareable(uint32_t cls)
{
	return cls != XRT_CLIENT_CLASS_CONTROLLER && cls != XRT_CLIENT_CLASS_DIAG &&
	       cls != XRT_CLIENT_CLASS_CAMERA_CONSUMER && cls != XRT_CLIENT_CLASS_PRESENT_OWNER;
}

/*!
 * Rebuild the app table (ADR-061 decision 4). Takes app_rescan_lock then the
 * manager lock; call with neither held. Exe paths and the platform query run
 * with only app_rescan_lock held. Returns true if anything a client sees changed.
 */
static bool
rescan_apps(struct ipc_server_stereo_camera *m, bool force)
{
	if (!m->apps_enabled) {
		return false;
	}
	os_mutex_lock(&m->app_rescan_lock);
	if (!force) {
		// An enumerate-driven rescan runs at most once a second (like rescan());
		// a pending apps_changed() is not held back by it.
		os_mutex_lock(&m->lock);
		const int64_t t = os_monotonic_get_ns();
		const bool skip =
		    !m->apps_dirty && m->last_app_rescan_ns != 0 && t - m->last_app_rescan_ns < RESCAN_MIN_INTERVAL_NS;
		os_mutex_unlock(&m->lock);
		if (skip) {
			os_mutex_unlock(&m->app_rescan_lock);
			return false;
		}
	}
	// The share stream size of any NEW entry: resolved before the manager lock
	// (`share.resolution` may read the registry / the user's JSON store).
	uint32_t ext_w = 0, ext_h = 0;
	float ext_fps = 0.0f;
	app_stream_extent(m, &ext_w, &ext_h, &ext_fps);

	// 1. Candidates (no manager lock). Other connections' fields are read
	//    racily, the uid_presenting pattern; the next tick corrects a stale read.
	struct app_cand *cands = U_TYPED_ARRAY_CALLOC(struct app_cand, IPC_MAX_CLIENTS + LINK_MAX);
	uint32_t n = 0;
	struct ipc_server *vs = m->server;
	if (cands != NULL && vs != NULL) {
		for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
			volatile struct ipc_client_state *o = &vs->threads[i].ics;
			if (o->server_thread_index < 0 || o->xs == NULL || o->xc == NULL || o->peer_pid <= 0 ||
			    !app_class_shareable(o->client_state.client_class)) {
				continue;
			}
			struct app_cand *c = &cands[n++];
			c->client_id = o->client_state.id;
			c->pid = o->peer_pid;
			c->opted_out = o->app_share_opt_out;
			c->source = XRT_STEREO_CAMERA_SOURCE_APP;
			snprintf(c->app_name, sizeof(c->app_name), "%s",
			         (const char *)o->client_state.info.application_name);
		}
	}
	// ADR-061 arm 2: in-process sessions over their service links. A link is
	// sessionless on the service side (no xs / xc), so the loop above never
	// sees it; its registration says whether its session runs.
	if (cands != NULL && m->links != NULL) {
		os_mutex_lock(&m->link_lock);
		for (uint32_t i = 0; i < LINK_MAX; i++) {
			const struct scam_link *l = &m->links[i];
			if (!l->used || (l->reg.caps & XRT_SHARE_LINK_CAP_SHARE) == 0 || l->reg.session_running == 0) {
				continue;
			}
			struct app_cand *c = &cands[n++];
			c->client_id = l->client_id;
			c->pid = l->pid;
			c->opted_out = l->reg.opt_out != 0;
			c->source = XRT_STEREO_CAMERA_SOURCE_APP_LINK;
			c->reg = l->reg;
			c->link_content_w = l->content_w;
			c->link_content_h = l->content_h;
			snprintf(c->exe_path, sizeof(c->exe_path), "%s", l->exe_path);
			snprintf(c->app_name, sizeof(c->app_name), "%s", l->reg.app_name);
		}
		os_mutex_unlock(&m->link_lock);
	}
	os_mutex_lock(&m->lock);
	// Who is already known needs no exe lookup or query beyond a refresh.
	for (uint32_t k = 0; k < n; k++) {
		cands[k].matched = false;
		for (uint32_t i = 0; i < APP_MAX; i++) {
			const struct scam_camera *a = &m->apps[i];
			if (!a->app.fake && a->app.live && a->app.client_id == cands[k].client_id &&
			    a->app.pid == cands[k].pid) {
				snprintf(cands[k].exe_path, sizeof(cands[k].exe_path), "%s", a->app.exe_path);
				cands[k].matched = true;
			}
		}
	}
	os_mutex_unlock(&m->lock);
	for (uint32_t k = 0; k < n; k++) {
		if (!cands[k].matched && !cands[k].opted_out && cands[k].exe_path[0] == '\0') {
			ipc_server_peer_exe_path(cands[k].pid, cands[k].exe_path, sizeof(cands[k].exe_path));
			if (cands[k].exe_path[0] == '\0') {
				snprintf(cands[k].exe_path, sizeof(cands[k].exe_path), "pid-%ld", cands[k].pid);
			}
		}
		if (cands[k].opted_out) {
			continue;
		}
		if (cands[k].source == XRT_STEREO_CAMERA_SOURCE_APP_LINK) {
			app_query_link(&cands[k], cands[k].client_id == FAKE_LINK_CLIENT_ID);
		} else {
			app_query(&cands[k]);
		}
	}

	// 2. Merge (locked).
	os_mutex_lock(&m->lock);
	const int64_t now = os_monotonic_get_ns();
	bool changed = false, ended_any = false;
	for (uint32_t i = 0; i < APP_MAX; i++) {
		struct scam_camera *a = &m->apps[i];
		if (a->app.fake || !a->app.listed) {
			continue;
		}
		if (a->app.live) {
			struct app_cand *hit = NULL;
			for (uint32_t k = 0; k < n && hit == NULL; k++) {
				if (cands[k].client_id == a->app.client_id && cands[k].pid == a->app.pid) {
					hit = &cands[k];
				}
			}
			if (hit == NULL || hit->opted_out) {
				changed |= app_mark_gone_locked(m, a, now, hit != NULL,
				                                hit != NULL ? "opted out (XrSessionShareabilityDXR)"
				                                            : "app exited");
				ended_any = true;
				if (hit != NULL) {
					hit->matched = true; // consumed: not re-listed
				}
				continue;
			}
			hit->matched = true;
			const uint32_t was = a->app.desc.availability;
			const bool renamed = strcmp(a->app.desc.application_name, hit->q.application_name) != 0;
			const uint64_t id = a->camera_id;
			const uint32_t ord = a->app.ordinal;
			a->app.desc = hit->q;
			a->app.desc.camera_id = id;
			a->app.desc.instance_ordinal = ord;
			a->app.last_commit_ns = hit->last_commit_ns;
			if (renamed && hit->app_name[0] != '\0') {
				snprintf(a->info.display_name, sizeof(a->info.display_name), "%s", hit->app_name);
			}
			if (was != a->app.desc.availability && a->app.desc.availability != XRT_APP_SHARE_AVAILABLE) {
				// It can no longer be tapped (another adapter, a kill-switched split): streams end.
				ended_any |= app_end_streams_locked(m, a) > 0;
			}
			if (a->src == NULL && a->started_count == 0) {
				set_state_locked(a, app_idle_state(a, now));
			}
			changed |= renamed || was != a->app.desc.availability;
		} else if (now - a->app.exited_ns >= m->app_linger_ns) {
			a->app.listed = false; // the linger is over: it leaves the list
			changed = true;
			U_LOG_W("app share %llu \"%s\": left the list (%lld ms after its app exited)",
			        (unsigned long long)a->camera_id, a->info.display_name,
			        (long long)(m->app_linger_ns / 1000000));
		}
	}
	for (uint32_t k = 0; k < n; k++) {
		struct app_cand *c = &cands[k];
		if (c->matched || c->opted_out) {
			continue;
		}
		struct scam_camera *slot = NULL;
		for (uint32_t i = 0; i < APP_MAX && slot == NULL; i++) {
			struct scam_camera *a = &m->apps[i];
			if (!a->app.listed && !a->app.fake && !app_slot_referenced_locked(m, a)) {
				slot = a;
			}
		}
		if (slot == NULL) {
			U_LOG_W("app share: more than %d app entries — \"%s\" (pid %ld) not listed", APP_MAX,
			        c->app_name, c->pid);
			break;
		}
		app_insert_locked(m, slot, c, now, ext_w, ext_h, ext_fps);
		changed = true;
	}
	m->last_app_rescan_ns = now;
	if (changed) {
		notify_cameras_changed_locked(m);
	}
	os_mutex_unlock(&m->lock);
	free(cands);
	os_mutex_unlock(&m->app_rescan_lock);
	share_forget_drain(m);
	if (ended_any) {
		indicator_changed(m);
	}
	return changed;
}

bool
ipc_server_stereo_camera_rescan_apps(struct ipc_server_stereo_camera *m)
{
	return m != NULL && rescan_apps(m, true);
}

void
ipc_server_stereo_camera_apps_changed(struct ipc_server_stereo_camera *m)
{
	if (m == NULL || !m->apps_enabled) {
		return;
	}
	os_mutex_lock(&m->lock);
	m->apps_dirty = true;
	os_cond_broadcast(&m->watch_cond);
	os_mutex_unlock(&m->lock);
}

void
ipc_server_stereo_camera_app_session_ending(volatile struct ipc_client_state *ics)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL || !m->apps_enabled) {
		return;
	}
	bool any = false;
	os_mutex_lock(&m->lock);
	const int64_t now = os_monotonic_get_ns();
	for (uint32_t i = 0; i < APP_MAX; i++) {
		struct scam_camera *a = &m->apps[i];
		if (a->app.fake || !a->app.live || a->app.client_id != ics->client_state.id ||
		    a->app.pid != ics->peer_pid) {
			continue;
		}
		any |= app_mark_gone_locked(m, a, now, false, "session ending");
		// Bounded: the camera thread detaches the tap between two waits.
		const int64_t deadline = os_monotonic_get_ns() + APP_CLOSE_WAIT_NS;
		while ((a->src != NULL || a->opening) && !m->shutting_down) {
			const int64_t t = os_monotonic_get_ns();
			if (t >= deadline) {
				U_LOG_W("app share %llu: the tap did not detach within %lld ms of session end",
				        (unsigned long long)a->camera_id, (long long)(APP_CLOSE_WAIT_NS / 1000000));
				break;
			}
			os_cond_wait_timeout_ns(&m->app_closed_cond, &m->lock, (uint64_t)(deadline - t));
		}
	}
	if (any) {
		notify_cameras_changed_locked(m);
	}
	os_mutex_unlock(&m->lock);
	share_forget_drain(m);
	if (any) {
		indicator_changed(m);
	}
}

/*!
 * #1961: the background half of re-enumeration. While at least one
 * connection listens for camera events, re-query the sources every
 * rescan_period_ns so a camera that appears (a tracker that starts late, a
 * display plugged in) or disappears reaches clients as
 * XrEventDataStereoCamerasChangedDXR without anyone polling enumerate.
 */
static void *
watch_thread(void *ptr)
{
	struct ipc_server_stereo_camera *m = (struct ipc_server_stereo_camera *)ptr;
	os_mutex_lock(&m->lock);
	int64_t next_camera_ns = os_monotonic_get_ns() + (int64_t)m->rescan_period_ns;
	while (!m->shutting_down) {
		// ADR-061: the app table ticks at 1 Hz (and at once on apps_changed);
		// the camera sources keep their own period.
		// The 1 Hz app tick only while it has something to do: a pending
		// apps_changed(), a listening consumer, or a listed live app (its exit
		// must end streams / start the linger). Otherwise the camera period.
		uint64_t wait_ns = m->rescan_period_ns;
		bool apps_live_now = false;
		for (uint32_t i = 0; i < APP_MAX; i++) {
			apps_live_now |= m->apps[i].app.listed && !m->apps[i].app.fake;
		}
		if (m->apps_enabled && (m->apps_dirty || has_listener_locked(m) || apps_live_now) &&
		    wait_ns > (uint64_t)APP_TICK_NS) {
			wait_ns = (uint64_t)APP_TICK_NS;
		}
		if (!m->apps_dirty && !m->rescan_soon) {
			os_cond_wait_timeout_ns(&m->watch_cond, &m->lock, wait_ns);
		}
		if (m->shutting_down) {
			break;
		}
		const int64_t now = os_monotonic_get_ns();
		const bool listening = has_listener_locked(m);
		const bool soon = m->rescan_soon;
		const bool cameras_due = soon || now >= next_camera_ns;
		m->rescan_soon = false;
		bool apps_live = false;
		for (uint32_t i = 0; i < APP_MAX; i++) {
			apps_live |= m->apps[i].app.listed && !m->apps[i].app.fake;
		}
		const bool apps_due = m->apps_enabled && (m->apps_dirty || listening || apps_live);
		m->apps_dirty = false;
		if (cameras_due) {
			next_camera_ns = now + (int64_t)m->rescan_period_ns;
		}
		os_mutex_unlock(&m->lock);
		if (listening && cameras_due) {
			(void)rescan(m, soon, false);
		}
		if (apps_due) {
			(void)rescan_apps(m, true);
		}
		os_mutex_lock(&m->lock);
	}
	os_mutex_unlock(&m->lock);
	return NULL;
}

struct ipc_server_stereo_camera *
ipc_server_stereo_camera_create(struct xrt_instance *xinst)
{
	return ipc_server_stereo_camera_create_ex(xinst, NULL);
}

struct ipc_server_stereo_camera *
ipc_server_stereo_camera_create_ex(struct xrt_instance *xinst, const struct ipc_server_stereo_camera_options *opts)
{
	struct ipc_server_stereo_camera *m = U_TYPED_CALLOC(struct ipc_server_stereo_camera);
	os_mutex_init(&m->lock);
	os_mutex_init(&m->consent_lock);
	os_mutex_init(&m->rescan_lock);
	os_cond_init(&m->watch_cond);
	os_mutex_init(&m->app_rescan_lock);
	os_cond_init(&m->app_closed_cond);
	os_mutex_init(&m->link_lock);
	os_cond_init(&m->link_cond);
	m->links = U_TYPED_ARRAY_CALLOC(struct scam_link, LINK_MAX);
	for (uint32_t i = 0; i < APP_MAX; i++) {
		m->apps[i].index = APP_SLOT_BASE + i;
		m->apps[i].mgr = m;
		m->apps[i].source = XRT_STEREO_CAMERA_SOURCE_APP;
		os_cond_init(&m->apps[i].cond);
	}
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		m->streams[i].section = XRT_SHMEM_HANDLE_INVALID;
		m->streams[i].section_ro = XRT_SHMEM_HANDLE_INVALID;
	}
	if (opts != NULL && opts->uvc_config != NULL) {
		m->have_uvc_config = true;
		m->uvc_config = *opts->uvc_config;
	}
	if (opts != NULL && opts->uvc_backend != NULL) {
		m->have_uvc_backend = true;
		m->uvc_backend = *opts->uvc_backend;
	}
	int32_t period_ms =
	    opts != NULL && opts->rescan_period_ms != 0 ? opts->rescan_period_ms : RESCAN_PERIOD_DEFAULT_MS;
	m->rescan_period_ns = period_ms > 0 ? (uint64_t)period_ms * 1000000ull : 0;

	// R3: the consent policy over the real store; the prompt arrives later
	// from the host (tray / menu bar) through set_prompt_provider.
	const struct u_camera_consent_store_ops *store = u_camera_consent_store_default();
	void *store_ctx = NULL;
	const struct u_camera_consent_env_ops *env = &env_ops;
	void *env_ctx = m;
	if (opts != NULL && opts->consent_store != NULL) {
		store = opts->consent_store;
		store_ctx = opts->consent_store_ctx;
	}
	if (opts != NULL && opts->consent_env != NULL) {
		env = opts->consent_env;
		env_ctx = opts->consent_env_ctx;
	}
#ifdef XRT_OS_ANDROID
	if (opts == NULL || opts->consent_store == NULL) {
		// A2: the delegating list is the runtime's built-in one (package +
		// certificate); decisions / sharing / secret stay in the JSON store.
		uint32_t n = 0;
		const struct u_camera_consent_android_trusted *list = u_camera_consent_android_builtin_trusted(&n);
		u_camera_consent_store_android_init(&m->android_store, store, store_ctx, list, n);
		store = &m->android_store.ops;
		store_ctx = &m->android_store;
	}
#endif
	u_camera_consent_init(&m->consent, store, store_ctx, env, env_ctx);
	m->consent.kill_switch = !debug_get_bool_option_stereo_camera_enabled();
	m->consent.dev_override = opts != NULL && opts->dev_override != 0
	                              ? opts->dev_override > 0
	                              : debug_get_bool_option_stereo_camera_dev_allow();
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

	// ADR-061 app share: its own policy over the camera's switches.
	m->apps_enabled = debug_get_bool_option_app_share_enabled();
	m->app_dev =
	    opts != NULL && opts->app_share_dev != 0 ? opts->app_share_dev > 0 : debug_get_bool_option_app_share_dev();
	// DXR_APP_SHARE_FAKE: truthy = the service-composited fake; "link" = the
	// in-process (service link) fake; "both" = both.
	const char *fake_env = debug_get_option_app_share_fake();
	bool env_fake = false, env_fake_link = false;
	if (fake_env != NULL && fake_env[0] != '\0') {
		if (strcmp(fake_env, "link") == 0) {
			env_fake_link = true;
		} else if (strcmp(fake_env, "both") == 0) {
			env_fake = env_fake_link = true;
		} else {
			env_fake = strcmp(fake_env, "0") != 0 && strcmp(fake_env, "false") != 0 &&
			           strcmp(fake_env, "off") != 0 && strcmp(fake_env, "no") != 0;
		}
	}
	const bool app_fake = opts != NULL && opts->app_share_fake != 0 ? opts->app_share_fake > 0 : env_fake;
	const bool app_fake_link =
	    opts != NULL && opts->app_share_fake_link != 0 ? opts->app_share_fake_link > 0 : env_fake_link;
	m->app_linger_ns =
	    opts != NULL && opts->app_linger_ms > 0 ? (int64_t)opts->app_linger_ms * 1000000ll : APP_LINGER_NS_DEFAULT;
	m->share_prompt_override = opts != NULL ? opts->share_prompt_mode : 0;
	m->share_resolution_override = opts != NULL ? opts->share_resolution : 0;
	m->share.camera = &m->consent;
	m->share.app_share_off = !m->apps_enabled;
	m->share.dev_gate = m->app_dev;
	m->share.prompt_enabled = m->consent.prompt_enabled;
	m->share.prompt_timeout_ms = m->consent.prompt_timeout_ms;
	if (opts != NULL && opts->share_prompt != NULL) {
		m->have_share_prompt_seam = true;
		m->share.prompt = opts->share_prompt;
		m->share.prompt_ctx = opts->share_prompt_ctx;
	}
	g_mgr = m;
	if (m->consent.dev_override) {
		U_LOG_W(
		    "stereo camera: ******** DXR_STEREO_CAMERA_DEV_ALLOW=1 — DEVELOPER OVERRIDE: every peer gets the "
		    "camera WITHOUT consent, identity or OS-permission checks (Android: "
		    "debug.xrt.DXR_STEREO_CAMERA_DEV_ALLOW). The front-camera rule (a live weave) still applies. "
		    "Never on a user's device. ********");
	}

	if (!m->apps_enabled) {
		U_LOG_W("app share: disabled by DXR_APP_SHARE=0 — no app is ever listed as a stereo source");
	} else if (m->app_dev) {
		U_LOG_W(
		    "app share: ******** DXR_APP_SHARE_DEV=1 — DEVELOPMENT GATE: running apps are listed to EVERY "
		    "camera consumer (no XR_DXR_app_share needed) and app shares start WITHOUT the share prompt. "
		    "Never on a user's machine. ********");
	}
	if (m->apps_enabled && app_fake) {
		app_insert_fake_locked(m);
	}
	if (m->apps_enabled && app_fake_link) {
		fake_link_start(m); // registers itself; the next app rescan lists it
	}

	if (!debug_get_bool_option_stereo_camera_enabled()) {
		U_LOG_W("stereo camera: disabled by DXR_STEREO_CAMERA=0 (kill switch) — zero cameras");
		return m;
	}
	if (!m->sharing_enabled) {
		U_LOG_W("stereo camera: camera sharing is OFF (user toggle) — zero cameras until it is switched on");
	}
	if (!m->consent.prompt_enabled) {
		U_LOG_W(
		    "stereo camera: consent prompt disabled (DXR_STEREO_CAMERA_PROMPT=0) — an app without a stored "
		    "or delegated consent is refused");
	}
	const struct xrt_plugin_iface *iface = NULL;
	struct xrt_plugin_instance *inst = NULL;
	if (xinst != NULL && xinst->get_active_plugin != NULL && xinst->get_active_plugin(xinst, &iface, &inst) &&
	    xrt_plugin_iface_has_stereo_camera(iface)) {
		m->iface = iface;
		m->inst = inst;
	} else {
		U_LOG_I("stereo camera: the active plug-in provides no camera source");
	}
	// Amendment 5: a plug-in declares its display's cameras with or without
	// camera slots of its own.
	const struct xrt_plugin_iface *ci = NULL;
	struct xrt_plugin_instance *cinst = NULL;
	if (xinst != NULL && xinst->get_active_plugin != NULL && xinst->get_active_plugin(xinst, &ci, &cinst) &&
	    xrt_plugin_iface_has_claimed_cameras(ci)) {
		m->claims_iface = ci;
		m->claims_inst = cinst;
	}
	// Plug-in cameras first: their platform_device_hints keep the UVC source
	// off the devices they own.
	(void)rescan(m, true, true);
	if (m->rescan_period_ns > 0) {
		os_thread_init(&m->watch);
		if (os_thread_start(&m->watch, watch_thread, m) == 0) {
			m->watch_started = true;
		} else {
			U_LOG_W(
			    "stereo camera: re-enumeration thread failed to start — cameras refresh on enumerate only");
		}
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
	fake_link_stop(m); // its thread drives the link core: gone before anything else
	os_mutex_lock(&m->lock);
	m->shutting_down = true;
	os_cond_broadcast(&m->watch_cond);
	for (uint32_t i = 0; i < m->camera_count; i++) {
		os_cond_broadcast(&m->cams[i].cond);
	}
	for (uint32_t i = 0; i < APP_MAX; i++) {
		os_cond_broadcast(&m->apps[i].cond);
	}
	os_cond_broadcast(&m->app_closed_cond);
	os_mutex_lock(&m->link_lock); // a leaf: allowed under the manager lock
	os_cond_broadcast(&m->link_cond);
	os_mutex_unlock(&m->link_lock);
	for (uint32_t i = 0; i < INTERNAL_MAX; i++) {
		if (m->internal[i] != NULL) {
			os_cond_broadcast(&m->internal[i]->cond); // a blocked acquire returns ENDED
		}
	}
	os_mutex_unlock(&m->lock);
	// In-process consumers: no new call may pin this manager, wait out the ones
	// inside, then cut every open handle loose (its owner frees it at close).
	pthread_mutex_lock(&g_internal_lock);
	if (g_mgr == m) {
		g_mgr = NULL;
	}
	while (m->internal_busy > 0) {
		pthread_cond_wait(&g_internal_idle, &g_internal_lock);
	}
	for (uint32_t i = 0; i < INTERNAL_MAX; i++) {
		if (m->internal[i] != NULL) {
			m->internal[i]->m = NULL;
			m->internal[i] = NULL;
			if (g_internal_count > 0) {
				g_internal_count--;
			}
		}
	}
	pthread_mutex_unlock(&g_internal_lock);
	if (m->watch_started) {
		os_thread_join(&m->watch);
		os_thread_destroy(&m->watch);
	}
	// No rescan can run past this point (shutting_down) or be running (it
	// holds rescan_lock until it returns).
	os_mutex_lock(&m->rescan_lock);
	os_mutex_unlock(&m->rescan_lock);
	os_mutex_lock(&m->app_rescan_lock);
	os_mutex_unlock(&m->app_rescan_lock);
	for (uint32_t i = 0; i < APP_MAX; i++) {
		if (m->apps[i].thread_started) {
			os_thread_join(&m->apps[i].thread);
			os_thread_destroy(&m->apps[i].thread);
		}
		os_cond_destroy(&m->apps[i].cond);
	}
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
	// Every camera thread has closed its source above: the UVC resolutions can go.
	for (uint32_t k = 0; k < UVC_OBJS_MAX; k++) {
		u_stereo_uvc_destroy(&m->uvc_objs[k]);
	}
	if (m->links != NULL) {
		for (uint32_t i = 0; i < LINK_MAX; i++) {
			if (m->links[i].used) {
				link_free_locked(&m->links[i]); // no thread left: no lock needed
			}
		}
		free(m->links);
		m->links = NULL;
	}
	os_cond_destroy(&m->link_cond);
	os_mutex_destroy(&m->link_lock);
	os_cond_destroy(&m->watch_cond);
	os_cond_destroy(&m->app_closed_cond);
	os_mutex_destroy(&m->app_rescan_lock);
	os_mutex_destroy(&m->rescan_lock);
	os_mutex_destroy(&m->consent_lock);
	os_mutex_destroy(&m->lock);
	free(m);
	*mgr_ptr = NULL;
}

bool
ipc_server_stereo_camera_rescan(struct ipc_server_stereo_camera *m)
{
	return m != NULL && rescan(m, true, false);
}

void
ipc_server_stereo_camera_rescan_soon(struct ipc_server_stereo_camera *m)
{
	if (m == NULL) {
		return;
	}
	os_mutex_lock(&m->lock);
	m->rescan_soon = true;
	os_cond_broadcast(&m->watch_cond); // the watch thread rescans if anyone listens
	os_mutex_unlock(&m->lock);
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
	stream_stop_locked(s, slot_cam(m, s->camera));
	s->ended = true;
	s->end_reason = reason;
	struct xrt_stereo_camera_event ev = {
	    .kind = XRT_STEREO_CAMERA_EVENT_STREAM_ENDED,
	    .value = reason,
	    .camera_id = slot_cam(m, s->camera)->camera_id,
	    .stream_id = s->id,
	};
	evq_push_locked(m, s->owner, &ev);
	wake_signal(&s->wake); // a consumer blocked on the wake handle re-polls and sees ENDED
	if (s->internal != NULL) {
		os_cond_signal(&s->internal->cond);
	}
}

static void
stream_destroy_locked(struct ipc_server_stereo_camera *m, struct scam_stream *s)
{
	stream_stop_locked(s, slot_cam(m, s->camera));
	stream_free_storage(s);
	memset(s, 0, sizeof(*s));
	s->section = XRT_SHMEM_HANDLE_INVALID;
	s->section_ro = XRT_SHMEM_HANDLE_INVALID;
#ifndef XRT_OS_WINDOWS
	s->wake.fds[0] = s->wake.fds[1] = -1;
#endif
}

void
ipc_server_client_stereo_camera_link_release(volatile struct ipc_client_state *ics)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m != NULL) {
		// ADR-061 arm 2: a closing in-process service link takes its share with it.
		link_unregister(m, ics->client_state.id);
	}
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
 * ADR-061 arm 2: the service-link IPC handlers (ipc_client_service_link).
 *
 */

xrt_result_t
ipc_handle_service_link_register(volatile struct ipc_client_state *ics, const struct xrt_share_link_registration *reg)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL || !m->apps_enabled || (reg->caps & XRT_SHARE_LINK_CAP_SHARE) == 0) {
		// Sharing off service-side (DXR_APP_SHARE=0), or a link that only lifts:
		// accepted and ignored — the poll answers NONE forever.
		return XRT_SUCCESS;
	}
	if (!app_class_shareable(ics->client_state.client_class)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED; // a browser / controller / consumer is never a share source
	}
	char exe[U_CAMERA_CONSENT_EXE_MAX] = {0};
	ipc_server_peer_exe_path(ics->peer_pid, exe, sizeof(exe));
	if (exe[0] == '\0') {
		snprintf(exe, sizeof(exe), "pid-%ld", ics->peer_pid);
	}
	return link_register(m, ics->client_state.id, ics->peer_pid, exe, false, reg);
}

xrt_result_t
ipc_handle_service_link_share_poll(volatile struct ipc_client_state *ics, struct xrt_share_link_poll *out_poll)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	memset(out_poll, 0, sizeof(*out_poll));
	if (m != NULL) {
		link_poll(m, ics->client_state.id, out_poll);
	}
	return XRT_SUCCESS;
}

xrt_result_t
ipc_handle_service_link_share_publish_ring(volatile struct ipc_client_state *ics,
                                           const struct xrt_share_link_ring_desc *desc,
                                           const xrt_graphics_buffer_handle_t *handles,
                                           uint32_t handle_count)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	if (m == NULL) {
		for (uint32_t i = 0; i < handle_count; i++) {
			xrt_graphics_buffer_handle_t h = handles[i];
			u_graphics_buffer_unref(&h);
		}
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	return link_publish_ring(m, ics->client_state.id, desc, handles, handle_count);
}

xrt_result_t
ipc_handle_service_link_share_publish_fence(volatile struct ipc_client_state *ics,
                                            uint32_t generation,
                                            const xrt_graphics_sync_handle_t *handles,
                                            uint32_t handle_count)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	for (uint32_t i = 1; i < handle_count; i++) {
		xrt_graphics_sync_handle_t h = handles[i];
		u_graphics_sync_unref(&h); // one fence per ring
	}
	if (handle_count == 0) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // malformed, refused (not a dead pipe)
	}
	if (m == NULL) {
		xrt_graphics_sync_handle_t h = handles[0];
		u_graphics_sync_unref(&h);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	return link_publish_fence(m, ics->client_state.id, generation, handles[0]);
}

xrt_result_t
ipc_handle_service_link_share_frame_ready(volatile struct ipc_client_state *ics,
                                          const struct xrt_share_link_frame *frame,
                                          struct xrt_share_link_poll *out_poll)
{
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	memset(out_poll, 0, sizeof(*out_poll));
	if (m == NULL) {
		return XRT_SUCCESS;
	}
	// An unregistered link (sharing off service-side) gets the refusal and a NONE poll.
	return link_frame_ready(m, ics->client_state.id, frame, out_poll);
}


/*
 *
 * ADR-061 arm 2: DXR_APP_SHARE_FAKE=link — a synthetic in-process registrant.
 * It drives the share-link core exactly as an app's service link does
 * (register, poll, publish a ring, frame_ready at the stream rate), with a
 * CPU NV12 ring in place of the app's GPU textures, so the whole arm runs
 * headless: enumerate -> consent -> start -> SHARE_REQUESTED -> ring ->
 * frames -> acquire -> stop -> SHARE_STOPPED.
 *
 */

struct scam_fake_link
{
	struct ipc_server_stereo_camera *m;
	struct os_thread thread;
	bool started;
	bool stop; //!< guarded by the manager's link_lock
};

//! Copy one fake frame into ring slot @p slot (link_lock taken here).
static bool
fake_link_write_slot(struct ipc_server_stereo_camera *m, uint32_t slot, const struct u_app_share_fake_frame *ff)
{
	bool ok = false;
	os_mutex_lock(&m->link_lock);
	struct scam_link *l = link_find_locked(m, FAKE_LINK_CLIENT_ID);
	const size_t y = (size_t)ff->width * ff->height;
	const size_t size = y + y / 2;
	if (l != NULL && slot < XRT_SHARE_LINK_RING_SLOTS) {
		if (l->cpu_slot_size != size) {
			for (uint32_t i = 0; i < XRT_SHARE_LINK_RING_SLOTS; i++) {
				free(l->cpu_slots[i]);
				l->cpu_slots[i] = (uint8_t *)calloc(1, size);
			}
			l->cpu_slot_size = size;
		}
		uint8_t *dst = l->cpu_slots[slot];
		if (dst != NULL) {
			for (uint32_t r = 0; r < ff->height; r++) {
				memcpy(dst + (size_t)r * ff->width, ff->planes[0] + (size_t)r * ff->pitches[0],
				       ff->width);
			}
			for (uint32_t r = 0; r < ff->height / 2; r++) {
				memcpy(dst + y + (size_t)r * ff->width, ff->planes[1] + (size_t)r * ff->pitches[1],
				       ff->width);
			}
			ok = true;
		}
	}
	os_mutex_unlock(&m->link_lock);
	return ok;
}

static bool
fake_link_should_stop(struct scam_fake_link *fl)
{
	os_mutex_lock(&fl->m->link_lock);
	const bool stop = fl->stop;
	os_mutex_unlock(&fl->m->link_lock);
	return stop;
}

static void *
fake_link_thread(void *ptr)
{
	struct scam_fake_link *fl = (struct scam_fake_link *)ptr;
	struct ipc_server_stereo_camera *m = fl->m;

	struct xrt_share_link_registration reg;
	memset(&reg, 0, sizeof(reg));
	reg.caps = XRT_SHARE_LINK_CAP_SHARE;
	snprintf(reg.exe, sizeof(reg.exe), "%s", "displayxr-app-share-fake-link");
	snprintf(reg.app_name, sizeof(reg.app_name), "%s", "DisplayXR app-share fake (in-process)");
	reg.tap_available = 1;
	reg.session_running = 1;
	(void)link_register(m, FAKE_LINK_CLIENT_ID, 0, "displayxr-app-share-fake-link", true, &reg);

	struct u_app_share_fake *gen = NULL;
	uint32_t ring_generation = 0, ring_stream = 0, ring_w = 0, ring_h = 0, slot = 0;
	bool have_ring = false;
	while (!fake_link_should_stop(fl)) {
		struct xrt_share_link_poll p;
		link_poll(m, FAKE_LINK_CLIENT_ID, &p);
		if (p.state != XRT_SHARE_LINK_STATE_SHARE_REQUESTED) {
			if (gen != NULL) {
				u_app_share_fake_destroy(&gen);
				have_ring = false;
			}
			os_nanosleep(20 * 1000 * 1000);
			continue;
		}
		if (!have_ring || ring_stream != p.stream_generation || ring_w != p.eye_w || ring_h != p.eye_h) {
			u_app_share_fake_destroy(&gen);
			gen = u_app_share_fake_create(p.eye_w, p.eye_h, p.fps > 0.0f ? p.fps : 30.0f);
			if (gen == NULL) {
				os_nanosleep(100 * 1000 * 1000);
				continue;
			}
			struct xrt_share_link_ring_desc d;
			memset(&d, 0, sizeof(d));
			d.generation = ++ring_generation;
			d.stream_generation = p.stream_generation;
			d.format = XRT_SHARE_LINK_FORMAT_CPU_NV12;
			d.slot_width = 2 * p.eye_w;
			d.slot_height = p.eye_h;
			d.content_width = p.eye_w;
			d.content_height = p.eye_h;
			d.slot_count = XRT_SHARE_LINK_RING_SLOTS;
			(void)link_publish_ring(m, FAKE_LINK_CLIENT_ID, &d, NULL, 0);
			have_ring = true;
			ring_stream = p.stream_generation;
			ring_w = p.eye_w;
			ring_h = p.eye_h;
		}
		struct u_app_share_fake_frame ff;
		if (!u_app_share_fake_next(gen, 50ll * 1000 * 1000, &ff)) {
			continue;
		}
		if (!fake_link_write_slot(m, slot, &ff)) {
			continue;
		}
		struct xrt_share_link_frame f;
		memset(&f, 0, sizeof(f));
		f.generation = ring_generation;
		f.slot = slot;
		f.fence_value = ff.sequence;
		f.commit_ns = ff.time_ns;
		f.content_generation = ring_generation;
		struct xrt_share_link_poll r;
		(void)link_frame_ready(m, FAKE_LINK_CLIENT_ID, &f, &r);
		slot = (slot + 1) % XRT_SHARE_LINK_RING_SLOTS;
	}
	u_app_share_fake_destroy(&gen);
	return NULL;
}

static void
fake_link_start(struct ipc_server_stereo_camera *m)
{
	struct scam_fake_link *fl = U_TYPED_CALLOC(struct scam_fake_link);
	if (fl == NULL) {
		return;
	}
	fl->m = m;
	os_thread_init(&fl->thread);
	if (os_thread_start(&fl->thread, fake_link_thread, fl) != 0) {
		U_LOG_W("app share: DXR_APP_SHARE_FAKE=link — the registrant thread failed to start");
		os_thread_destroy(&fl->thread);
		free(fl);
		return;
	}
	fl->started = true;
	m->fake_link = fl;
	U_LOG_W(
	    "app share: DXR_APP_SHARE_FAKE=link — a SYNTHETIC in-process registrant (CPU ring over the share-link "
	    "core) is listed as an app-link source (diagnostics / CI only)");
}

static void
fake_link_stop(struct ipc_server_stereo_camera *m)
{
	struct scam_fake_link *fl = m->fake_link;
	if (fl == NULL) {
		return;
	}
	os_mutex_lock(&m->link_lock);
	fl->stop = true;
	os_mutex_unlock(&m->link_lock);
	if (fl->started) {
		os_thread_join(&fl->thread);
		os_thread_destroy(&fl->thread);
	}
	free(fl);
	m->fake_link = NULL;
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
	// #1961: an enumerate re-queries the sources (throttled to once a second;
	// source calls run without the manager lock, so streams never wait).
	(void)rescan(m, false, false);
	// ADR-061: a consumer that sees apps gets the current app set (cheap: the
	// connection table plus one platform query per app).
	if (client_sees_apps(m, ics)) {
		(void)rescan_apps(m, false); // throttled: at most once a second per enumerate burst
	}
	os_mutex_lock(&m->lock);
	// #1961: a connection that enumerated LISTENS from now on — it is told
	// XrEventDataStereoCamerasChangedDXR whenever the list changes, and keeps
	// the background re-enumeration running while it is connected.
	(void)evq_for_owner_locked(m, owner_of(m, ics), true);
	// Spec §2 / §7.4: sharing off = zero cameras, indistinguishable from none.
	// ADR-061: app entries follow every camera.
	*out_count = m->sharing_enabled ? m->camera_count + apps_listed_count_locked(m, ics) : 0;
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

static void
fill_properties_locked(struct ipc_server_stereo_camera *m,
                       const struct scam_camera *cam,
                       const char *exe,
                       struct xrt_stereo_camera_properties *out_props);

static void
fill_calibration_locked(const struct scam_camera *cam,
                        uint32_t output,
                        struct xrt_stereo_camera_calibration *out_calib);

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
	char exe[512] = {0};
	peer_identity(ics, exe, sizeof(exe));

	os_mutex_lock(&m->lock);
	if (index < m->camera_count) {
		fill_properties_locked(m, &m->cams[index], exe, out_props);
		os_mutex_unlock(&m->lock);
		return XRT_SUCCESS;
	}
	// ADR-061: the (index - camera_count)-th app entry this connection sees.
	uint32_t k = index - m->camera_count;
	if (client_sees_apps(m, ics)) {
		for (uint32_t i = 0; i < APP_MAX; i++) {
			struct scam_camera *a = &m->apps[i];
			if (!app_listed_for_locked(a, ics)) {
				continue;
			}
			if (k-- == 0) {
				fill_properties_locked(m, a, exe, out_props);
				os_mutex_unlock(&m->lock);
				return XRT_SUCCESS;
			}
		}
	}
	os_mutex_unlock(&m->lock);
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

xrt_result_t
ipc_handle_stereo_camera_get_app_source(volatile struct ipc_client_state *ics,
                                        uint64_t camera_id,
                                        struct xrt_app_share_source_info *out_info)
{
	U_ZERO(out_info);
	struct ipc_server_stereo_camera *m = mgr_of(ics);
	xrt_result_t auth = require_camera_client(ics, "stereo_camera_get_app_source");
	if (auth != XRT_SUCCESS) {
		return auth;
	}
	if (m == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&m->lock);
	const struct scam_camera *a = m->sharing_enabled ? app_by_id_for_locked(m, ics, camera_id) : NULL;
	if (a == NULL) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_INPUT_UNSUPPORTED; // a camera, or not an app this connection sees
	}
	*out_info = a->app.desc;
	out_info->camera_id = a->camera_id;
	out_info->pid = a->app.pid > 0 ? (uint64_t)a->app.pid : 0;
	out_info->source = a->source;
	out_info->instance_ordinal = a->app.ordinal;
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

/*!
 * One camera's properties as consumer @p exe sees them (its keyed
 * persistentId). Manager lock held.
 */
static void
fill_properties_locked(struct ipc_server_stereo_camera *m,
                       const struct scam_camera *cam,
                       const char *exe,
                       struct xrt_stereo_camera_properties *out_props)
{
	out_props->camera_id = cam->camera_id;
	if (cam_is_app(cam)) {
		// ADR-061 spec §2: not a camera — display-referred, no geometry, NV12 over shared memory.
		u_camera_consent_persistent_id(&m->consent, cam->info.device_identity, exe, out_props->persistent_id);
		snprintf(out_props->display_name, sizeof(out_props->display_name), "%s", cam->info.display_name);
		out_props->platform_device_hint[0] = '\0';
		out_props->flags = XRT_STEREO_CAMERA_APP_CONTENT | XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED |
		                   XRT_PLUGIN_STEREO_CAMERA_RECTIFIABLE;
		out_props->state = cam->app.live ? effective_state_locked(m, cam) : XRT_STEREO_CAMERA_STATE_UNAVAILABLE;
		out_props->view_count = 2;
		out_props->eye_width = cam->info.eye_width;
		out_props->eye_height = cam->info.eye_height;
		out_props->max_frame_rate = cam->info.max_frame_rate;
		out_props->source = cam->source;
		out_props->display_id = 0;
		out_props->frame_orientation_deg = XRT_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN;
		out_props->supported_formats = XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_NV12);
		out_props->supported_transports = XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY);
		return;
	}
	// Spec §7.5: keyed with the per-user secret — a page cannot derive the
	// device from the id, and two installs never share one.
	u_camera_consent_persistent_id(&m->consent, cam->info.device_identity, exe, out_props->persistent_id);
	snprintf(out_props->display_name, sizeof(out_props->display_name), "%s", cam->info.display_name);
	snprintf(out_props->platform_device_hint, sizeof(out_props->platform_device_hint), "%s",
	         cam->info.platform_device_hint);
	// Spec v7: RECTIFIABLE is the service's promise, never the plug-in's claim.
	out_props->flags = (cam->info.flags & ~(XRT_PLUGIN_STEREO_CAMERA_RECTIFIABLE | XRT_STEREO_CAMERA_APP_CONTENT)) |
	                   (camera_can_rectify(cam) ? XRT_PLUGIN_STEREO_CAMERA_RECTIFIABLE : 0u);
	out_props->state = effective_state_locked(m, cam);
	out_props->view_count = 2;
	out_props->eye_width = cam->info.eye_width;
	out_props->eye_height = cam->info.eye_height;
	out_props->max_frame_rate = source_rate_hz(cam);
	out_props->source = cam->source;
	out_props->display_id = cam->info.monitor_id;
	out_props->frame_orientation_deg = frame_orientation_deg(cam->info.frame_orientation);
	if (cam->have_calib) {
		// Spec §3: per eye, RECTIFIED (the service's rectified focal when it
		// rectifies; the plug-in's own for a natively rectified source).
		double fx = cam->calib.k[0][0], cx = 0.0, cy = 0.0;
		if (cam->rect != NULL) {
			rectified_intrinsics(cam->rect, &fx, &cx, &cy);
		}
		// Amendment 4: a NOMINAL model reports only what the user configured;
		// a default it fell back to is not a fact about the device (0 = unknown).
		if (!cam->calib_nominal || cam->baseline_known) {
			out_props->baseline_mm = (float)baseline_mm(&cam->calib);
		}
		if (fx > 0.0 && (!cam->calib_nominal || cam->hfov_known)) {
			out_props->horizontal_fov_deg =
			    (float)(2.0 * atan(cam->info.eye_width / (2.0 * fx)) * 180.0 / 3.14159265358979323846);
		}
	}
	out_props->supported_formats = XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_GRAY8) |
	                               XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_NV12) |
	                               XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_BGRA8);
	out_props->supported_transports = XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY);
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
	peer_identity(ics, exe, sizeof(exe));
	os_mutex_lock(&m->lock);
	// ADR-061 spec §6: an app entry has no calibration (its geometry is the
	// display-referred frame metadata) — refused before any consent prompt.
	bool known = camera_by_id_locked(m, camera_id) != NULL && m->sharing_enabled;
	os_mutex_unlock(&m->lock);
	if (!known) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// Spec §7.5: calibration identifies the device — only after §7.1 passes.
	// Evaluated with the manager lock released (it may prompt).
	struct u_camera_consent_decision dec;
	xrt_result_t xret = consent_evaluate_client(m, ics, exe, "stereo_camera_get_calibration", &dec);
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
	// Amendment 4: a nominal model is not a RAW calibration of the device —
	// only the rectified numbers (which describe the frames delivered) are given.
	if (xret == XRT_SUCCESS && cam->calib_nominal && output == XRT_STEREO_CAMERA_OUTPUT_RAW) {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (xret == XRT_SUCCESS && output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && !camera_can_rectify(cam)) {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (xret != XRT_SUCCESS) {
		os_mutex_unlock(&m->lock);
		return xret;
	}
	fill_calibration_locked(cam, output, out_calib);
	os_mutex_unlock(&m->lock);
	return XRT_SUCCESS;
}

//! The calibration of @p cam for @p output (checked by the caller). Manager lock held.
static void
fill_calibration_locked(const struct scam_camera *cam, uint32_t output, struct xrt_stereo_camera_calibration *out_calib)
{
	if (output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED && cam->rect != NULL) {
		// R2: the numbers of the frames the service rectifies — one pinhole for
		// both eyes (fx = fy = f, one principal point: zero disparity at
		// infinity), no distortion, and the right camera a pure +x translation
		// of the baseline (OpenCV P1/P2 with P2[0][3] = f * Tx, Tx = -baseline).
		const struct u_stereo_rectify_result *g = &cam->rect->geo;
		double f = 0.0, cx = 0.0, cy = 0.0;
		rectified_intrinsics(cam->rect, &f, &cx, &cy); // a nominal pass-through: no zoom
		out_calib->output = output;
		for (int e = 0; e < 2; e++) {
			struct xrt_stereo_camera_intrinsics *in = &out_calib->eye[e];
			in->width = g->width;
			in->height = g->height;
			in->fx = (float)f;
			in->fy = (float)f;
			in->cx = (float)cx;
			in->cy = (float)cy;
			in->model = XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE;
		}
		out_calib->orientation[3] = 1.0f; // identity
		// Right camera centre in the rectified left frame = -t_rect (mm -> m).
		for (int i = 0; i < 3; i++) {
			out_calib->position[i] = (float)(-g->t_rect[i] / 1000.0);
		}
		out_calib->baseline_mm = (float)g->baseline;
		return;
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
}

/*!
 * A free stream slot on @p cam, cleared, marked used and given a fresh id —
 * or NULL when the camera's per-camera limit or the table is full. Manager
 * lock held. Shared by the IPC create and the in-process open.
 */
static struct scam_stream *
stream_claim_locked(struct ipc_server_stereo_camera *m, const struct scam_camera *cam)
{
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
		return NULL;
	}
	memset(slot, 0, sizeof(*slot));
	slot->section = XRT_SHMEM_HANDLE_INVALID;
	slot->section_ro = XRT_SHMEM_HANDLE_INVALID;
#ifndef XRT_OS_WINDOWS
	slot->wake.fds[0] = slot->wake.fds[1] = -1;
#endif
	slot->used = true;
	slot->id = ++m->next_stream_id;
	return slot;
}

/*!
 * Allocate and start @p s on @p cam (consent already ALLOWED, BUSY already
 * checked): ring + wake, decimator, the camera thread, one WARN. Manager lock
 * held. Shared by the IPC start and the in-process open.
 */
static xrt_result_t
stream_start_locked(struct ipc_server_stereo_camera *m, struct scam_stream *s, struct scam_camera *cam)
{
	xrt_result_t xret = stream_allocate_locked(s, cam);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	u_stereo_camera_decimator_init(&s->dec, s->req.max_frame_rate, source_rate_hz(cam));
	u_stereo_camera_ring_clear(&s->ring);
	s->vis_checked_ns = 0;
	s->started = true;
	cam->started_count++;
	// A camera that just lost its last stream keeps its source open for
	// LINGER_NS so a quick restart is cheap. Another camera starting ends that
	// grace at once: two cameras of one device often cannot be open together
	// (a tablet's front and rear logical pairs: the HAL refuses the second
	// with "too many cameras already open", measured on a Lume Pad 2), so a
	// front -> rear switch otherwise waits out the linger plus a reopen retry.
	for (uint32_t k = 0; k < m->camera_count; k++) {
		struct scam_camera *other = &m->cams[k];
		if (other != cam && other->started_count == 0 && other->src != NULL) {
			other->linger_deadline_ns = 0;
			os_cond_signal(&other->cond);
		}
	}
	if (!cam->thread_started) {
		os_thread_init(&cam->thread);
		if (os_thread_start(&cam->thread, camera_thread, cam) != 0) {
			s->started = false;
			cam->started_count--;
			return XRT_ERROR_THREADING_INIT_FAILURE;
		}
		cam->thread_started = true;
	}
	os_cond_signal(&cam->cond);
	U_LOG_W("stereo camera %llu: stream %llu started by %s — output %s, format %u, max %.1f Hz; %u started%s",
	        (unsigned long long)cam->camera_id, (unsigned long long)s->id, s->consumer,
	        s->output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED ? "RECTIFIED" : "RAW", s->req.format,
	        s->req.max_frame_rate, cam->started_count, m->locked ? " (session locked: suspended)" : "");
	return XRT_SUCCESS;
}

//! The started stream's layout, exactly as get_section reports it. Manager lock held.
static void
stream_layout_locked(struct ipc_server_stereo_camera *m,
                     const struct scam_stream *s,
                     struct xrt_stereo_camera_stream_layout *out_layout)
{
	out_layout->width = s->width;
	out_layout->height = s->height;
	out_layout->format = s->req.format;
	out_layout->output = s->output;
	out_layout->transport = s->req.transport;
	out_layout->max_frame_rate =
	    s->dec.period_ns > 0 ? (float)(1e9 / (double)s->dec.period_ns) : source_rate_hz(slot_cam(m, s->camera));
	out_layout->section_size = s->section_size;
	out_layout->slot_stride = s->slot_stride;
	out_layout->slot_count = U_STEREO_CAMERA_RING_SLOTS;
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
	peer_identity(ics, exe, sizeof(exe));
	const bool delegating = exe_is_delegating(m, exe);
	if (!output_allowed(ics, delegating, req->output)) {
		U_LOG_W("stereo_camera_stream_create: RAW refused for pid %ld (%s: rectified only)", ics->peer_pid,
		        delegating ? "consent-delegating client" : "PRESENT_OWNER");
		return XRT_ERROR_NOT_AUTHORIZED;
	}

	os_mutex_lock(&m->lock);
	struct scam_camera *cam = m->sharing_enabled ? camera_by_id_locked(m, req->camera_id) : NULL;
	if (cam == NULL && m->sharing_enabled) {
		cam = app_by_id_for_locked(m, ics, req->camera_id);
	}
	if (cam == NULL) {
		os_mutex_unlock(&m->lock);
		return m->sharing_enabled ? XRT_ERROR_INPUT_UNSUPPORTED : XRT_ERROR_STEREO_CAMERA_DISABLED;
	}
	// ADR-061 spec §9: an app entry is RECTIFIED (display-referred) NV12 over
	// shared memory, nothing else.
	if (cam_is_app(cam) &&
	    (req->output != XRT_STEREO_CAMERA_OUTPUT_RECTIFIED || req->format != XRT_STEREO_CAMERA_FORMAT_NV12)) {
		os_mutex_unlock(&m->lock);
		U_LOG_W(
		    "stereo_camera_stream_create: app share %llu is RECTIFIED NV12 only (asked output %u, format %u)",
		    (unsigned long long)cam->camera_id, req->output, req->format);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// RECTIFIED needs a calibration (CALIBRATED / NATIVELY_RECTIFIED) — or,
	// Amendment 4, a nominal model the service rectifies (an uncalibrated UVC
	// pair: frames as delivered + the online row alignment).
	bool calibrated = (cam->info.flags &
	                   (XRT_PLUGIN_STEREO_CAMERA_CALIBRATED | XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED)) != 0 ||
	                  (cam->calib_nominal && cam->rect != NULL);
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
	struct scam_stream *slot = stream_claim_locked(m, cam);
	if (slot == NULL) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_CLIENT_LIMIT_REACHED;
	}
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
	slot->peer_uid = ics->peer_uid;
	// The developer override does NOT lift this gate (decided 2026-10-31): the
	// front pair feeds the display's head tracking, so even a dev box never
	// lets an app without a live weave take it.
	slot->needs_presenting =
	    g_platform.user_facing_requires_presenting && (cam->info.flags & XRT_PLUGIN_STEREO_CAMERA_USER_FACING) != 0;
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

/*!
 * ADR-061: the start of an app-share stream. The share policy decides (the
 * prompt may block: no manager lock), then the stream starts on the app
 * entry exactly like a camera stream.
 */
static xrt_result_t
share_stream_start(struct ipc_server_stereo_camera *m,
                   volatile struct ipc_client_state *ics,
                   uint64_t stream_id,
                   const struct u_app_share_request *req)
{
	struct u_app_share_decision dec;
	os_mutex_lock(&m->consent_lock);
	// The user's `share.prompt` is read per start (a CLI / Dashboard change applies to the next one).
	if (m->share_prompt_override == 1) {
		m->share.mode = U_APP_SHARE_PROMPT_ALWAYS;
	} else if (m->share_prompt_override == 2) {
		m->share.mode = U_APP_SHARE_PROMPT_PER_APP_RUN;
	} else {
		struct u_app_share_settings st;
		u_app_share_settings_load(&st);
		m->share.mode = st.prompt;
	}
	if (!m->have_share_prompt_seam) {
		m->share.prompt = g_share_prompt_fn;
		m->share.prompt_ctx = g_share_prompt_ctx;
	}
	u_app_share_consent_evaluate(&m->share, req, &dec);
	os_mutex_unlock(&m->consent_lock);
	U_LOG_W("stereo_camera_stream_start: app share \"%s\" for %s (pid %ld) %s — %s", req->app_name,
	        req->consumer_exe[0] ? req->consumer_exe : "?", req->consumer_pid,
	        dec.verdict == U_APP_SHARE_ALLOWED ? "ALLOWED" : "REFUSED", u_app_share_why_str(dec.why));
	switch (dec.verdict) {
	case U_APP_SHARE_ALLOWED: break;
	case U_APP_SHARE_DISABLED: return XRT_ERROR_STEREO_CAMERA_DISABLED;
	case U_APP_SHARE_BUSY: return XRT_ERROR_STEREO_CAMERA_BUSY;
	case U_APP_SHARE_REFUSED:
	default: return XRT_ERROR_STEREO_CAMERA_CONSENT_REFUSED;
	}

	// Pass 2 (locked): the stream or the app may have gone while the prompt was up.
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
	if (!m->sharing_enabled) {
		os_mutex_unlock(&m->lock);
		return XRT_ERROR_STEREO_CAMERA_DISABLED;
	}
	struct scam_camera *cam = slot_cam(m, s->camera);
	if (!cam->app.live || !cam->app.listed || cam->camera_id != req->app_key ||
	    cam->app.desc.availability != XRT_APP_SHARE_AVAILABLE) {
		os_mutex_unlock(&m->lock);
		U_LOG_W("app share %llu: stream %llu start refused — the app left while the prompt was up (BUSY)",
		        (unsigned long long)req->app_key, (unsigned long long)stream_id);
		return XRT_ERROR_STEREO_CAMERA_BUSY;
	}
	xrt_result_t xret = stream_start_locked(m, s, cam);
	os_mutex_unlock(&m->lock);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	indicator_changed(m); // the tray shows "Sharing <app>"
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
	const bool needs_presenting = s->needs_presenting;
	const long peer_uid = s->peer_uid;
	struct scam_camera *cam0 = slot_cam(m, s->camera);
	if (cam_is_app(cam0)) {
		// ADR-061: an app share has its own consent (spec §6) and its own start.
		struct u_app_share_request sreq = {0};
		char app_name[sizeof(cam0->info.display_name)];
		char app_exe[U_CAMERA_CONSENT_EXE_MAX];
		snprintf(app_name, sizeof(app_name), "%s", cam0->info.display_name);
		snprintf(app_exe, sizeof(app_exe), "%s", cam0->app.fake ? "" : cam0->app.exe_path);
		sreq.consumer_exe = exe;
		sreq.consumer_pid = ics->peer_pid;
		sreq.app_name = app_name;
		sreq.app_exe = app_exe;
		sreq.app_pid = cam0->app.pid;
		sreq.app_key = cam0->camera_id;
		sreq.source_available =
		    cam0->app.live && cam0->app.listed && cam0->app.desc.availability == XRT_APP_SHARE_AVAILABLE;
		os_mutex_unlock(&m->lock);
		return share_stream_start(m, ics, stream_id, &sreq);
	}
	os_mutex_unlock(&m->lock);

	// A2 front gate first: an app that may not take the front camera now is
	// not asked for consent it could not use (checked again below). The app
	// gets PRESENTING_GRACE_NS to start weaving: it typically asks just before
	// its first weave submit.
	if (!u_camera_consent_user_facing_gate(needs_presenting, true,
	                                       !needs_presenting || uid_presenting_within_grace(ics, peer_uid))) {
		U_LOG_W(
		    "stereo_camera_stream_start: stream %llu refused for pid %ld — a user-facing camera needs a live "
		    "weave from the same app in this process, none within %lld ms (BUSY)",
		    (unsigned long long)stream_id, ics->peer_pid, PRESENTING_GRACE_NS / (1000ll * 1000));
		return XRT_ERROR_STEREO_CAMERA_BUSY;
	}

	// Spec §7.1: start is THE authorisation point. Lock released — the tray
	// prompt may block for up to a minute.
	struct u_camera_consent_decision dec;
	xrt_result_t xret = consent_evaluate_client(m, ics, exe, "stereo_camera_stream_start", &dec);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	// A2 (Android): the runtime is the OS-visible camera client, so it needs
	// the camera permission itself; the platform asks the user if it must.
	if (g_platform_set && g_platform.ensure_service_camera != NULL &&
	    !g_platform.ensure_service_camera(g_platform.ctx, m->consent.prompt_timeout_ms)) {
		U_LOG_W(
		    "stereo_camera_stream_start: the runtime itself holds no camera permission — refused "
		    "(PERMISSION_INSUFFICIENT); grant it from the DisplayXR app");
		return XRT_ERROR_NOT_AUTHORIZED;
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
	struct scam_camera *cam = slot_cam(m, s->camera);
	// A2 (§1.2 option R): the front camera feeds the display's head tracking
	// while it is open, so only an app that presents woven 3D here may take
	// it — never one in the background. BUSY (the browser: NotReadableError).
	// Under the lock the platform is not asked whether the app is on top (pass
	// 1 did, just before consent); one that left meanwhile is suspended by the
	// platform's foreground rule once its stream starts.
	if (!u_camera_consent_user_facing_gate(s->needs_presenting, true,
	                                       !s->needs_presenting ||
	                                           uid_presenting(ics, s->peer_uid, (int64_t)os_monotonic_get_ns(), 1))) {
		os_mutex_unlock(&m->lock);
		U_LOG_W(
		    "stereo camera %llu: stream %llu start refused for %s — a user-facing camera needs a live "
		    "weave from the same app in this process (BUSY)",
		    (unsigned long long)cam->camera_id, (unsigned long long)s->id, s->consumer);
		return XRT_ERROR_STEREO_CAMERA_BUSY;
	}
	// BUSY: the plug-in could not open the source and its retry is still
	// pending — distinct from "refused", so a browser maps it to NotReadableError.
	// #1961: so is a camera its source no longer lists (gone, UNAVAILABLE).
	if (cam->gone || (cam->state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE && cam->src == NULL &&
	                  os_monotonic_get_ns() < cam->retry_after_ns)) {
		os_mutex_unlock(&m->lock);
		U_LOG_W("stereo camera %llu: stream %llu start refused — source unavailable (BUSY), retry later",
		        (unsigned long long)cam->camera_id, (unsigned long long)s->id);
		return XRT_ERROR_STEREO_CAMERA_BUSY;
	}
	s->delegating = dec.delegating || s->delegating;
	s->delegated = dec.delegated;
	s->consent_why = (uint32_t)dec.why;
	xret = stream_start_locked(m, s, cam);
	os_mutex_unlock(&m->lock);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
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
		stream_stop_locked(s, slot_cam(m, s->camera));
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
	stream_layout_locked(m, s, out_layout);
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
                                 struct xrt_stereo_camera_frame_info *out_frame,
                                 struct xrt_app_share_frame_info *out_app)
{
	*out_ready = false;
	U_ZERO(out_frame);
	U_ZERO(out_app);
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
		*out_app = s->slot_app[slot];
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
	out_stats->source_frame_rate = slot_cam(m, s->camera)->source_rate;
	out_stats->delivered_frame_rate = s->delivered_rate;
	out_stats->frames_published = s->ring.published;
	out_stats->frames_skipped = s->ring.skipped;
	out_stats->frames_acquired = s->ring.acquired;
	out_stats->mean_latency_ns = s->latency_count > 0 ? s->latency_sum_ns / s->latency_count : 0;
	const struct scam_rectifier *r = slot_cam(m, s->camera)->rect;
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
ipc_server_stereo_camera_set_share_prompt_provider(u_app_share_prompt_fn fn, void *ctx)
{
	g_share_prompt_ctx = ctx;
	g_share_prompt_fn = fn;
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
		const struct scam_camera *sc = slot_cam(m, s->camera);
		if (cam_is_app(sc)) {
			// ADR-061: "Sharing <app>" — the shared app, not the consumer, names the share.
			out->share_streams++;
			size_t sn = strlen(out->shares);
			if (strstr(out->shares, sc->info.display_name) == NULL && sn < sizeof(out->shares) - 1) {
				snprintf(out->shares + sn, sizeof(out->shares) - sn, "%s%s", sn ? ", " : "",
				         sc->info.display_name);
			}
		}
		// Base name of the executable (Android: the package), de-duplicated, comma
		// separated; an in-process component by its display name.
		char label[256];
		if (s->label[0] != '\0') {
			out->component_streams++;
			snprintf(label, sizeof(label), "%s", s->label);
		} else {
			u_camera_consent_subject_label(s->exe, label, sizeof(label));
		}
		const char *base = label[0] != '\0' ? label : "unknown app";
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
	u_app_share_consent_forget_all(&m->share); // so do per-app-run share grants
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
		// The enumerated set goes N -> 0 (off) or 0 -> N (on).
		notify_cameras_changed_locked(m);
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


/*
 *
 * In-process consumers: the input host iface's shared-camera slot (ADR-043 /
 * tracking-provider-discovery.md §10 item 2). A consumer here is a
 * CAMERA_CONSUMER stream with no connection: the same RECTIFIED GRAY8
 * shared-memory ring, the same wake handle, the same start path (consent,
 * kill switch, BUSY, indicator) — only the transport of the handles differs,
 * and the events a connection would poll surface through every acquire.
 *
 */

//! Pin the manager of @p h (or, for an open, the process's one) against destroy.
static struct ipc_server_stereo_camera *
internal_pin(struct ipc_server_stereo_camera_internal *h)
{
	pthread_mutex_lock(&g_internal_lock);
	struct ipc_server_stereo_camera *m = h != NULL ? h->m : g_mgr;
	if (m != NULL) {
		m->internal_busy++;
	}
	pthread_mutex_unlock(&g_internal_lock);
	return m;
}

static void
internal_unpin(struct ipc_server_stereo_camera *m)
{
	pthread_mutex_lock(&g_internal_lock);
	if (m->internal_busy > 0 && --m->internal_busy == 0) {
		pthread_cond_broadcast(&g_internal_idle);
	}
	pthread_mutex_unlock(&g_internal_lock);
}

static void
internal_release_quota(void)
{
	pthread_mutex_lock(&g_internal_lock);
	if (g_internal_count > 0) {
		g_internal_count--;
	}
	pthread_mutex_unlock(&g_internal_lock);
}

static struct scam_stream *
internal_stream_locked(struct ipc_server_stereo_camera *m, const struct ipc_server_stereo_camera_internal *h)
{
	for (uint32_t i = 0; i < MAX_STREAMS; i++) {
		struct scam_stream *s = &m->streams[i];
		if (s->used && s->owner == h->owner && s->id == h->stream_id) {
			return s;
		}
	}
	return NULL;
}

//! camera_id 0 = the active display's own camera (the plug-in's), else any rectifiable one.
static struct scam_camera *
internal_pick_camera_locked(struct ipc_server_stereo_camera *m, uint64_t camera_id)
{
	if (camera_id != 0) {
		return camera_by_id_locked(m, camera_id);
	}
	struct scam_camera *any = NULL;
	for (uint32_t i = 0; i < m->camera_count; i++) {
		struct scam_camera *c = &m->cams[i];
		if (c->gone || !camera_can_rectify(c)) {
			continue;
		}
		if (c->source == XRT_STEREO_CAMERA_SOURCE_PLUGIN) {
			return c;
		}
		if (any == NULL) {
			any = c;
		}
	}
	return any;
}

static long
own_pid(void)
{
#ifdef XRT_OS_WINDOWS
	return (long)GetCurrentProcessId();
#else
	return (long)getpid();
#endif
}

void
ipc_server_stereo_camera_attach_server(struct ipc_server_stereo_camera *m, struct ipc_server *s)
{
	if (m != NULL) {
		m->server = s;
	}
}

uint32_t
ipc_server_stereo_camera_internal_consumer_count(void)
{
	pthread_mutex_lock(&g_internal_lock);
	uint32_t n = g_internal_count;
	pthread_mutex_unlock(&g_internal_lock);
	return n;
}

xrt_result_t
ipc_server_stereo_camera_internal_open(const struct ipc_server_stereo_camera_internal_request *req,
                                       struct ipc_server_stereo_camera_internal_info *out_info,
                                       struct ipc_server_stereo_camera_internal **out_h)
{
	*out_h = NULL;
	memset(out_info, 0, sizeof(*out_info));
#ifdef XRT_OS_WINDOWS
	out_info->wake = NULL;
#else
	out_info->wake = -1;
#endif
	if (req == NULL || req->provider_id == NULL || req->provider_id[0] == '\0') {
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	const char *name =
	    req->display_name != NULL && req->display_name[0] != '\0' ? req->display_name : req->provider_id;
	if (req->output != XRT_STEREO_CAMERA_OUTPUT_RECTIFIED) {
		U_LOG_W("stereo camera: in-process consumer %s asked for output %u — refused (RECTIFIED only)",
		        req->provider_id, req->output);
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	struct ipc_server_stereo_camera *m = internal_pin(NULL);
	if (m == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED; // no manager in this process (yet)
	}

	// The CAMERA_CONSUMER quota, shared with the connections of that class.
	const uint32_t conns =
	    m->server != NULL ? ipc_server_client_class_count(m->server, XRT_CLIENT_CLASS_CAMERA_CONSUMER) : 0;
	const uint32_t quota = ipc_server_client_class_quota(m->server, XRT_CLIENT_CLASS_CAMERA_CONSUMER);
	pthread_mutex_lock(&g_internal_lock);
	const bool full = (quota != 0 && conns + g_internal_count >= quota) || g_internal_count >= INTERNAL_MAX;
	if (!full) {
		g_internal_count++;
	}
	const uint32_t in_use = conns + g_internal_count;
	pthread_mutex_unlock(&g_internal_lock);
	if (full) {
		U_LOG_W("stereo camera: in-process consumer %s refused — CAMERA_CONSUMER quota %u/%u exhausted",
		        req->provider_id, in_use, quota);
		internal_unpin(m);
		return XRT_ERROR_CLIENT_LIMIT_REACHED;
	}

	// The consent subject: "<service exe>#<provider id>" — never the bare
	// service, which would read "displayxr-service wants to use the 3D camera".
	char self[400] = {0};
	(void)ipc_server_peer_exe_path(own_pid(), self, sizeof(self));
	char exe[512];
	snprintf(exe, sizeof(exe), "%s#%s", self[0] != '\0' ? self : "displayxr-service", req->provider_id);

	// Kill switch / sharing toggle first, inside the policy (step 1), then the
	// component shortcut (step 4a) or the ordinary per-subject path.
	struct u_camera_consent_decision dec;
	xrt_result_t xret = consent_evaluate(m, exe, name, own_pid(),
	                                     req->component_trusted ? U_CAMERA_CONSENT_FLAG_COMPONENT_TRUSTED : 0u,
	                                     "stereo_camera_internal_open", &dec);
	if (xret != XRT_SUCCESS) {
		internal_release_quota();
		internal_unpin(m);
		return xret;
	}

	struct ipc_server_stereo_camera_internal *h = U_TYPED_CALLOC(struct ipc_server_stereo_camera_internal);
	if (h == NULL) {
		internal_release_quota();
		internal_unpin(m);
		return XRT_ERROR_ALLOCATION;
	}
	os_cond_init(&h->cond);
	h->m = m;
	snprintf(h->label, sizeof(h->label), "%s", name);

	os_mutex_lock(&m->lock);
	struct scam_camera *cam = NULL;
	struct scam_stream *s = NULL;
	int32_t reg = -1;
	if (m->shutting_down) {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	} else if (!m->sharing_enabled) {
		xret = XRT_ERROR_STEREO_CAMERA_DISABLED;
	} else if ((cam = internal_pick_camera_locked(m, req->camera_id)) == NULL || !camera_can_rectify(cam)) {
		// RECTIFIED or nothing — never the RAW-flagged fallback.
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	} else if (cam->gone || (cam->state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE && cam->src == NULL &&
	                         os_monotonic_get_ns() < cam->retry_after_ns)) {
		xret = XRT_ERROR_STEREO_CAMERA_BUSY;
	} else {
		for (int32_t i = 0; i < INTERNAL_MAX; i++) {
			if (m->internal[i] == NULL) {
				reg = i;
				break;
			}
		}
		s = reg >= 0 ? stream_claim_locked(m, cam) : NULL;
		xret = s != NULL ? XRT_SUCCESS : XRT_ERROR_CLIENT_LIMIT_REACHED;
	}
	if (xret == XRT_SUCCESS) {
		h->owner = INTERNAL_OWNER_BIT | ++m->next_owner;
		h->stream_id = s->id;
		s->owner = h->owner;
		s->ics = NULL;
		s->camera = cam->index;
		s->req.camera_id = cam->camera_id;
		s->req.output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
		s->req.format = XRT_STEREO_CAMERA_FORMAT_GRAY8;
		s->req.transport = XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY;
		s->req.max_frame_rate = req->max_frame_rate;
		s->output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
		snprintf(s->consumer, sizeof(s->consumer), "%s (in-process, \"%s\")", exe, name);
		snprintf(s->exe, sizeof(s->exe), "%s", exe);
		snprintf(s->label, sizeof(s->label), "%s", name);
		s->peer_pid = own_pid();
		s->peer_uid = -1;
		// No window by contract (the foreground rule exempts the class), never
		// a delegating client, never the front-camera live-weave rule (the
		// consumer is the runtime itself, not an app that could background).
		s->client_class = XRT_CLIENT_CLASS_CAMERA_CONSUMER;
		s->needs_presenting = false;
		s->delegating = false;
		s->consent_why = (uint32_t)dec.why;
		s->internal = h;
		xret = stream_start_locked(m, s, cam);
		if (xret != XRT_SUCCESS) {
			stream_destroy_locked(m, s);
			s = NULL;
		}
	}
	if (xret == XRT_SUCCESS) {
		m->internal[reg] = h;
		fill_properties_locked(m, cam, exe, &out_info->props);
		if (cam->have_calib) {
			fill_calibration_locked(cam, XRT_STEREO_CAMERA_OUTPUT_RECTIFIED, &out_info->calib);
		}
		stream_layout_locked(m, s, &out_info->layout);
		out_info->map = s->map;
#ifdef XRT_OS_WINDOWS
		out_info->wake = s->wake.event;
#else
		out_info->wake = s->wake.fds[0];
#endif
	}
	os_mutex_unlock(&m->lock);

	if (xret != XRT_SUCCESS) {
		U_LOG_W("stereo camera: in-process consumer %s (\"%s\") could not open a camera (%d)", req->provider_id,
		        name, (int)xret);
		os_cond_destroy(&h->cond);
		free(h);
		internal_release_quota();
		internal_unpin(m);
		return xret;
	}
	indicator_changed(m); // the tray shows "3D camera in use by <provider>"
	internal_unpin(m);
	*out_h = h;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_stereo_camera_internal_acquire(struct ipc_server_stereo_camera_internal *h,
                                          uint32_t timeout_ms,
                                          struct ipc_server_stereo_camera_internal_frame *out_frame,
                                          uint32_t *out_state)
{
	memset(out_frame, 0, sizeof(*out_frame));
	if (out_state != NULL) {
		*out_state = XRT_STEREO_CAMERA_STATE_UNAVAILABLE;
	}
	if (h == NULL) {
		return XRT_ERROR_STEREO_CAMERA_STREAM_ENDED;
	}
	struct ipc_server_stereo_camera *m = internal_pin(h);
	if (m == NULL) {
		return XRT_ERROR_STEREO_CAMERA_STREAM_ENDED; // the manager went away (shutdown)
	}
	const int64_t deadline = (int64_t)os_monotonic_get_ns() + (int64_t)timeout_ms * 1000000;
	xrt_result_t xret = XRT_TIMEOUT;
	os_mutex_lock(&m->lock);
	for (;;) {
		struct scam_stream *s = internal_stream_locked(m, h);
		if (s == NULL || s->ended || m->shutting_down) {
			xret = XRT_ERROR_STEREO_CAMERA_STREAM_ENDED;
			break;
		}
		if (out_state != NULL) {
			*out_state = effective_state_locked(m, slot_cam(m, s->camera));
		}
		int32_t slot = -1;
		uint64_t seq = 0;
		if (s->started && s->allocated && u_stereo_camera_ring_acquire(&s->ring, &slot, &seq)) {
			out_frame->info = s->slot_info[slot];
			const uint8_t *base = s->map + (uint64_t)slot * s->slot_stride;
			out_frame->plane[0] = base + out_frame->info.plane_offset[0];
			out_frame->plane[1] =
			    out_frame->info.row_pitch[1] != 0 ? base + out_frame->info.plane_offset[1] : NULL;
			xret = XRT_SUCCESS;
			break;
		}
		const int64_t now = (int64_t)os_monotonic_get_ns();
		if (now >= deadline) {
			break;
		}
		os_cond_wait_timeout_ns(&h->cond, &m->lock, (uint64_t)(deadline - now));
	}
	os_mutex_unlock(&m->lock);
	internal_unpin(m);
	return xret;
}

void
ipc_server_stereo_camera_internal_release(struct ipc_server_stereo_camera_internal *h, uint64_t frame_index)
{
	if (h == NULL) {
		return;
	}
	struct ipc_server_stereo_camera *m = internal_pin(h);
	if (m == NULL) {
		return;
	}
	os_mutex_lock(&m->lock);
	struct scam_stream *s = internal_stream_locked(m, h);
	// The pin is the ring's one consumer hold: dropping it lets the writer reuse the slot.
	if (s != NULL && s->ring.pinned >= 0 && s->slot_info[s->ring.pinned].frame_index == frame_index) {
		s->ring.pinned = -1;
	}
	os_mutex_unlock(&m->lock);
	internal_unpin(m);
}

void
ipc_server_stereo_camera_internal_close(struct ipc_server_stereo_camera_internal *h)
{
	if (h == NULL) {
		return;
	}
	struct ipc_server_stereo_camera *m = internal_pin(h);
	if (m != NULL) {
		os_mutex_lock(&m->lock);
		struct scam_stream *s = internal_stream_locked(m, h);
		if (s != NULL) {
			stream_destroy_locked(m, s);
		}
		for (uint32_t i = 0; i < INTERNAL_MAX; i++) {
			if (m->internal[i] == h) {
				m->internal[i] = NULL;
			}
		}
		os_mutex_unlock(&m->lock);
		internal_release_quota();
		U_LOG_W("stereo camera: in-process consumer \"%s\" closed its stream", h->label);
		indicator_changed(m);
		internal_unpin(m);
	}
	os_cond_destroy(&h->cond);
	free(h);
}
