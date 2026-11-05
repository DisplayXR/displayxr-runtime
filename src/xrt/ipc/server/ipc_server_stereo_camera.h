// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043): the service's camera manager.
 *
 * The service is the single owner of each stereo camera — plug-in-provided, or
 * (Amendment 4) an opted-in plain UVC side-by-side webcam read by the service's
 * own vendor-neutral source: one runtime-owned thread per open camera pulls
 * frames from the source's wait_frame, and fans each frame out to every started, authorised stream
 * — per stream a 3-slot latest-wins shared-memory ring (u_stereo_camera_ring),
 * format conversion, decimation, and a per-stream wake handle. The source is
 * opened on the first start and closed a linger (2 s) after the last stop.
 *
 * Platform-neutral (Windows D3D11 service, macOS / Linux service, Android
 * runtime service): the only OS-specific pieces are the wake handle (event /
 * pipe) and the read-only section duplicate, both in ipc_server_stereo_camera.c.
 *
 * @ingroup ipc_server
 */

#pragma once

#include "xrt/xrt_instance.h"
#include "xrt/xrt_stereo_camera.h"
#include "xrt/xrt_share_link.h"
#include "xrt/xrt_handles.h"
#include "util/u_camera_consent.h"
#include "util/u_stereo_uvc.h"
#include "xrt/xrt_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_server_stereo_camera;
struct ipc_client_state;

/*!
 * Create the manager over the active plug-in of @p xinst (the service's native
 * instance) plus the service's own UVC side-by-side source (ADR-043 Amendment
 * 4; opt-in by the per-user stereo-cameras.json, util/u_stereo_uvc.h). Always
 * returns a manager; with no camera source or the kill switch
 * (DXR_STEREO_CAMERA=0) it simply exposes zero cameras.
 */
struct ipc_server_stereo_camera *
ipc_server_stereo_camera_create(struct xrt_instance *xinst);

/*!
 * What @ref ipc_server_stereo_camera_create_ex may replace — the seams a test
 * drives the whole manager through (tests_stereo_camera_manager). Every NULL
 * field means "the service's real one".
 */
struct ipc_server_stereo_camera_options
{
	//! Consent store (default: the per-user store, u_camera_consent_store_default()).
	const struct u_camera_consent_store_ops *consent_store;
	void *consent_store_ctx;
	//! Consent environment (default: OS camera switch, tray prompt, signer checks).
	const struct u_camera_consent_env_ops *consent_env;
	void *consent_env_ctx;
	//! UVC source config (default: u_stereo_uvc_config_load()).
	const struct u_stereo_uvc_config *uvc_config;
	//! UVC capture backend (default: os_uvc_capture_backend()).
	const struct u_stereo_uvc_backend *uvc_backend;
	/*!
	 * #1961: background re-enumeration period while a connection listens,
	 * ms. 0 = the default (2000); < 0 = none (a test drives
	 * @ref ipc_server_stereo_camera_rescan itself).
	 */
	int32_t rescan_period_ms;
	/*!
	 * The developer override (consent row 2): 0 = from
	 * DXR_STEREO_CAMERA_DEV_ALLOW (the default); > 0 forces it on, < 0 off
	 * (tests). It never lifts the front-camera rule — nor the app-share
	 * prompt (ADR-061).
	 */
	int32_t dev_override;

	/*
	 * ADR-061 app share. 0 everywhere = the service's real behaviour.
	 */
	//! DXR_APP_SHARE_DEV: 0 = from the env, > 0 on, < 0 off.
	int32_t app_share_dev;
	//! DXR_APP_SHARE_FAKE (the synthetic service-composited app): 0 = from the env, > 0 on, < 0 off.
	int32_t app_share_fake;
	/*!
	 * DXR_APP_SHARE_FAKE=link (or =both): a synthetic IN-PROCESS registrant
	 * over the share-link core — it registers, answers SHARE_REQUESTED with a
	 * CPU ring and posts frame_ready at the stream rate, exactly as an app's
	 * service link would (ADR-061 arm 2). 0 = from the env, > 0 on, < 0 off.
	 */
	int32_t app_share_fake_link;
	//! How long an exited app's entry stays listed UNAVAILABLE, ms (0 = 10000).
	int32_t app_linger_ms;
	//! `share.prompt`: 0 = the user store, 1 = always, 2 = per-app-run.
	int32_t share_prompt_mode;
	//! `share.resolution`: 0 = the user store, 1 = 720p, 2 = 1080p.
	int32_t share_resolution;
	//! The share prompt (NULL = the one the host installed, if any).
	u_app_share_prompt_fn share_prompt;
	void *share_prompt_ctx;
};

struct ipc_server_stereo_camera *
ipc_server_stereo_camera_create_ex(struct xrt_instance *xinst, const struct ipc_server_stereo_camera_options *opts);

//! Stop every camera thread, close every source, free every stream.
void
ipc_server_stereo_camera_destroy(struct ipc_server_stereo_camera **mgr_ptr);

/*!
 * #1961: re-query every camera source now (no throttle) and merge the result:
 * unchanged cameras keep their index and id, new ones are appended, ones no
 * longer listed turn UNAVAILABLE in place; any change queues
 * XrEventDataStereoCamerasChangedDXR to every listening connection. The
 * manager also does this on a client's enumerate (at most once a second) and
 * periodically while a connection listens. Returns true if the set changed.
 */
bool
ipc_server_stereo_camera_rescan(struct ipc_server_stereo_camera *mgr);

/*!
 * #1961: the display topology moved (a screen appeared or went, or the active
 * plug-in changed). Wake the background re-enumeration now instead of at its
 * next ~2 s tick, so a display's camera and its screen association follow a
 * hotplug promptly. Never blocks; does nothing while no connection listens.
 */
void
ipc_server_stereo_camera_rescan_soon(struct ipc_server_stereo_camera *mgr);

/*!
 * ADR-061 arm 2: a closing connection's service link and the share on it. Called once at
 * client teardown, BEFORE client_state is zeroed: the link is keyed by its id.
 */
void
ipc_server_client_stereo_camera_link_release(volatile struct ipc_client_state *ics);

//! Destroy every stream @p ics created. Called once at client teardown.
void
ipc_server_client_stereo_camera_release(volatile struct ipc_client_state *ics);

/*!
 * ADR-061: the set of shareable apps may have changed (a client connected,
 * a session was created / begun / ended). Wakes the background app rescan
 * now instead of at its next 1 Hz tick. Never blocks; NULL is ignored.
 */
void
ipc_server_stereo_camera_apps_changed(struct ipc_server_stereo_camera *mgr);

/*!
 * ADR-061: @p ics's session and compositor are about to be destroyed. Its
 * app entry (if any) turns UNAVAILABLE (listed for 10 s), every stream of it
 * ends SOURCE_LOST, and its share tap is DETACHED before this returns
 * (bounded: 1 s), so the platform never taps a compositor being torn down.
 * Call with no manager lock held, before the compositor goes.
 */
void
ipc_server_stereo_camera_app_session_ending(volatile struct ipc_client_state *ics);

/*!
 * ADR-061: rebuild the app table from the live connections now (tests; the
 * service does it on apps_changed and a 1 Hz tick). Returns true if the
 * listed set or an entry's labels changed.
 */
bool
ipc_server_stereo_camera_rescan_apps(struct ipc_server_stereo_camera *mgr);


/*
 *
 * R3: what the service's UI (tray / menu-bar status item) and platform hooks
 * use. All of these address the process's one manager and are safe from any
 * thread; they take the manager lock themselves.
 *
 */

/*!
 * The consent prompt ("<app> wants to use the 3D camera — Allow / Allow once /
 * Deny"). Called on an IPC client thread with no manager lock held; it must
 * BLOCK until the user answers or @p timeout_ms elapses, and return the
 * u_camera_consent_prompt_answer value (UNAVAILABLE when there is no UI).
 */
typedef enum u_camera_consent_prompt_answer (*ipc_server_stereo_camera_prompt_fn)(
    void *ctx, const char *exe, const char *app_name, long pid, uint32_t timeout_ms);

void
ipc_server_stereo_camera_set_prompt_provider(ipc_server_stereo_camera_prompt_fn fn, void *ctx);

/*!
 * ADR-061: the SHARE prompt ("Share <app> with <consumer>? Deny / Allow").
 * Same blocking contract as the camera prompt; only Allow / Allow once
 * allow. Without one, every app-share start is refused unless
 * DXR_APP_SHARE_DEV=1 (the camera prompt is never borrowed for it: its text
 * would misdescribe what is shared).
 */
void
ipc_server_stereo_camera_set_share_prompt_provider(u_app_share_prompt_fn fn, void *ctx);

/*!
 * A notice for the user, no answer expected (Windows: a tray balloon). Raised
 * the first time in a service run that a USER-level consent-delegation entry
 * allows an executable (spec §7.1.1) — @p exe is the executable, @p app_name
 * the client's declared name. Called on an IPC client thread with the consent
 * serialisation held: must NOT block (copy the strings, post, return).
 */
typedef void (*ipc_server_stereo_camera_notice_fn)(void *ctx, const char *exe, const char *app_name);

void
ipc_server_stereo_camera_set_notice_provider(ipc_server_stereo_camera_notice_fn fn, void *ctx);

/*!
 * Stereo camera A2 (stereo-camera-android §4): what a platform whose consumers
 * have no meaningful executable path supplies instead (Android: the runtime
 * service's Java side). Install it before the first client connects; every
 * field may be NULL / false.
 */
struct ipc_server_stereo_camera_platform
{
	/*!
	 * The consent SUBJECT for the peer at (@p pid, @p uid) — used in place of
	 * the executable path everywhere consent, the delegating check and the
	 * persistentId key on it (Android: "android:<package>@<cert sha256>",
	 * util/u_camera_consent_android.h). false / "" = no verifiable identity
	 * (refused, spec §7.1 step 3). Called on an IPC client thread, no lock held.
	 */
	bool (*peer_subject)(void *ctx, long pid, long uid, char *out, size_t cap);
	//! The OS camera permission of @p subject (Android: the peer package holds
	//! CAMERA and its camera app-op is not ignored). Step 4 of §7.1.
	bool (*os_camera_allowed)(void *ctx, const char *subject);
	//! The consent prompt for @p subject (same contract as the prompt provider).
	enum u_camera_consent_prompt_answer (*prompt)(
	    void *ctx, const char *subject, const char *app_name, long pid, uint32_t timeout_ms);
	/*!
	 * After consent ALLOWED a start: may THIS process open cameras? Android:
	 * the runtime package's own CAMERA permission (the runtime is the
	 * OS-visible camera client); if it is not granted the platform asks the
	 * user and BLOCKS up to @p timeout_ms. false = PERMISSION_INSUFFICIENT.
	 */
	bool (*ensure_service_camera)(void *ctx, uint32_t timeout_ms);
	/*!
	 * Option R (§1.2): a USER_FACING camera starts only for a peer whose uid
	 * presents woven 3D in this process (a weave submit in the last second,
	 * a visible session, or a weave present-owner connection while
	 * @ref consumer_foreground says its app is on top), else BUSY; a started
	 * one delivers no frames and reads SUSPENDED while that stops being true.
	 * The developer override (DXR_STEREO_CAMERA_DEV_ALLOW) does not lift it.
	 */
	bool user_facing_requires_presenting;
	/*!
	 * Is this process's consumer app on top right now, with the screen
	 * unlocked? (Android: the satellite serves one package and is BOUND_TOP
	 * exactly while that package is the top app.) Lets a weave present-owner
	 * take the front camera before its first weave submit, which needs the
	 * camera's frames. NULL = no such signal. Called without the manager lock.
	 */
	bool (*consumer_foreground)(void *ctx);
	/*!
	 * A source that gave up after it had delivered (the OS took the camera:
	 * app in the background, sensor privacy) keeps its started streams
	 * SUSPENDED instead of UNAVAILABLE while it retries (Android).
	 */
	bool gave_up_is_suspended;

	/*!
	 * @name ADR-061 share tap (the service compositor's pre-weave tap)
	 *
	 * What a service whose compositor can read a client's composed atlas
	 * supplies (the Windows D3D11 service, decision 8). All NULL = this
	 * service has no tap: every app entry is listed UNAVAILABLE ("no tap on
	 * this platform"), which is what the macOS / Linux service does until P6.
	 * @p client_id is the connection's ipc_app_state::id; the platform
	 * resolves it under its OWN lock on every call (the manager never hands
	 * out a compositor pointer), so a client that went away mid-call is an
	 * error, never a dangling read. None is called with the manager lock held.
	 * @{
	 */
	/*!
	 * Describe @p client_id's session for the enumeration: availability
	 * (enum xrt_app_share_availability, with the reason when not
	 * AVAILABLE), window rect, per-view content size, view count / active
	 * view count, zeroDisparityShiftFrac, encoding. The manager pre-fills
	 * names, ids and pid; the hook writes only the fields it knows.
	 * @p out_last_commit_ns: the client's last layer commit (os_monotonic;
	 * 0 = unknown) — older than 1 s reads SUSPENDED. false = the client is
	 * not (or no longer) a tappable session.
	 */
	bool (*share_tap_query)(void *ctx,
	                        uint32_t client_id,
	                        struct xrt_app_share_source_info *io_info,
	                        int64_t *out_last_commit_ns);
	/*!
	 * Start tapping @p client_id: SBS NV12, @p eye_w x @p eye_h per eye,
	 * decimated to at most @p fps. Called on the app entry's camera thread
	 * when its first stream starts. The tap must never make the app wait.
	 */
	xrt_result_t (*share_tap_attach)(
	    void *ctx, uint32_t client_id, uint32_t eye_w, uint32_t eye_h, float fps, void **out_tap);
	/*!
	 * The next tapped frame (XRT_PLUGIN_STEREO_CAMERA_WAIT_*), at most
	 * @p timeout_ns. On OK: @p out_frame is the full SBS NV12 frame
	 * (2 * eye_w x eye_h, planes valid until share_tap_release_frame) and
	 * @p out_info its XrAppShareFrameInfoDXR metadata. WAIT_ERROR = the
	 * client's compositor is gone (the stream then ends SOURCE_LOST).
	 */
	uint32_t (*share_tap_wait_frame)(void *ctx,
	                                 void *tap,
	                                 int64_t timeout_ns,
	                                 struct xrt_plugin_stereo_camera_frame *out_frame,
	                                 struct xrt_app_share_frame_info *out_info);
	//! Done with the frame of the last OK wait (may be NULL).
	void (*share_tap_release_frame)(void *ctx, void *tap);
	//! Stop tapping; @p tap is freed. Always called once per successful attach.
	void (*share_tap_detach)(void *ctx, void *tap);
	/*! @} */

	/*!
	 * @name ADR-061 arm 2: the share LINK (an IN-PROCESS app's published ring)
	 *
	 * An in-process app copies its pre-weave views into a 3-texture NT-shared
	 * ring with a shared fence (compositor/xbridge/comp_xbridge_share_tap.h)
	 * and posts `service_link_share_frame_ready` per frame; these hooks turn
	 * that ring into the same SBS NV12 frame the share tap above produces
	 * (ADR-061 decision 6 — the Windows D3D11 service fills them with the
	 * tap's own NV12 pass). All NULL = this service cannot import a ring:
	 * every in-process app entry is listed UNAVAILABLE ("no tap"). Called on
	 * the app entry's camera thread, never with a manager lock held.
	 * @{
	 */
	/*!
	 * The adapter the service publishes (`client_d3d_deviceLUID`, ADR-037 §7)
	 * — a ring on another adapter cannot be opened, so such an entry is listed
	 * UNAVAILABLE (OTHER_ADAPTER). false = unknown (no adapter check).
	 */
	bool (*share_link_adapter_luid)(void *ctx, uint8_t out_luid[XRT_LUID_SIZE]);
	/*!
	 * Open a published ring. Takes ownership of every handle (closes them on
	 * failure too): @p textures (@p texture_count, XRT_SHARE_LINK_RING_SLOTS
	 * on Windows) and @p fence (may be invalid). @p eye_w x @p eye_h: the
	 * stream's per-eye extent every converted frame must have.
	 */
	xrt_result_t (*share_link_import)(void *ctx,
	                                  const struct xrt_share_link_ring_desc *desc,
	                                  const xrt_graphics_buffer_handle_t *textures,
	                                  uint32_t texture_count,
	                                  xrt_graphics_sync_handle_t fence,
	                                  uint32_t eye_w,
	                                  uint32_t eye_h,
	                                  void **out_import);
	/*!
	 * Convert slot @p frame->slot once the ring's fence reached
	 * @p frame->fence_value (a GPU-side wait — never a CPU wait on the app):
	 * the content rect of each half (the frame's when its width is non-zero,
	 * else the ring's), scaled to fit the eye extent preserving aspect,
	 * letterboxed, flipped per flip_y, encoded if linear, NV12 BT.601 full
	 * range. May be pipelined like the tap (a non-blocking readback ring):
	 * XRT_PLUGIN_STEREO_CAMERA_WAIT_OK = @p out_frame holds the newest
	 * FINISHED conversion (planes valid until share_link_release_frame) and
	 * @p out_info its metadata (content rect in delivered-eye pixels, mono,
	 * content generation, zeroDisparityShiftFrac, commit time);
	 * WAIT_TIMEOUT = nothing finished yet; WAIT_ERROR = the ring is unusable
	 * (the stream ends SOURCE_LOST).
	 */
	uint32_t (*share_link_convert)(void *ctx,
	                               void *import,
	                               const struct xrt_share_link_frame *frame,
	                               struct xrt_plugin_stereo_camera_frame *out_frame,
	                               struct xrt_app_share_frame_info *out_info);
	//! Done with the frame of the last OK convert (may be NULL).
	void (*share_link_release_frame)(void *ctx, void *import);
	//! Close an imported ring; @p import is freed. Once per successful import.
	void (*share_link_unimport)(void *ctx, void *import);
	/*! @} */

	void *ctx;
};

//! ADR-061: install the D3D11 service compositor's share tap hooks.
struct ipc_server;
void
ipc_server_share_tap_d3d11_install(struct ipc_server *s);

//! Install (or clear, NULL) the platform hooks. The struct is copied.
void
ipc_server_stereo_camera_set_platform(const struct ipc_server_stereo_camera_platform *p);

//! Called (no lock held) whenever the in-use state may have changed.
typedef void (*ipc_server_stereo_camera_indicator_fn)(void *ctx);

void
ipc_server_stereo_camera_set_indicator_provider(ipc_server_stereo_camera_indicator_fn fn, void *ctx);

struct ipc_server_stereo_camera_status
{
	uint32_t camera_count;
	uint32_t started_streams; //!< > 0 = "3D camera in use"
	bool sharing_enabled;     //!< the user's persistent toggle
	bool locked;              //!< OS session locked: every stream suspended
	char consumers[256];      //!< "browser.exe, call-app.exe" (base names / Android packages, de-duplicated;
	                          //!< an in-process component by its display name)
	uint32_t component_streams; //!< of started_streams: in-process consumers (input providers)
	//! ADR-061: of started_streams, app shares; the tray reads "Sharing <shares>".
	uint32_t share_streams;
	char shares[256]; //!< the shared apps' display names, de-duplicated
};

//! false = no manager (service still starting).
bool
ipc_server_stereo_camera_get_status(struct ipc_server_stereo_camera_status *out);

//! The user's "Stop camera sharing": ends every started stream now. Returns the count.
uint32_t
ipc_server_stereo_camera_stop_all(const char *why);

//! The persistent "Share the 3D camera with apps" toggle (off ends every stream).
bool
ipc_server_stereo_camera_set_sharing(bool enabled, const char *why);

//! Platform hook: the OS session is locked / switched away (true) or back (false).
void
ipc_server_stereo_camera_set_session_locked(bool locked, const char *why);

bool
ipc_server_stereo_camera_is_session_locked(void);


/*
 *
 * In-process consumers (the input host iface's shared-camera slot,
 * `xrt_input_plugin_host_iface::open_shared_camera`): an input provider
 * loaded in the service reads the display's shared camera straight from this
 * manager — a CAMERA_CONSUMER stream with no connection (never an IPC
 * self-connection). Same ring, layout, wake handle and start path as the IPC
 * form; consent subject "<service exe>#<provider id>"; the kill switch, the
 * sharing toggle, the session lock and the indicator apply; it counts
 * against the CAMERA_CONSUMER quota; the foreground rule exempts it (no
 * window). These address the process's one manager and are safe from any
 * thread; an open may block (an unverified provider is prompted).
 *
 */

struct ipc_server;
struct ipc_server_stereo_camera_internal;

//! Give the manager its server, so in-process consumers share the CAMERA_CONSUMER quota with connections.
void
ipc_server_stereo_camera_attach_server(struct ipc_server_stereo_camera *mgr, struct ipc_server *s);

//! How many in-process consumers are open (the connection quota counts them too).
uint32_t
ipc_server_stereo_camera_internal_consumer_count(void);

struct ipc_server_stereo_camera_internal_request
{
	const char *provider_id;  //!< the provider's iface id (consent subject suffix), required
	const char *display_name; //!< what the prompt / tray name it by (NULL = provider_id)
	/*!
	 * The host's verdict on the provider binary (ADR-054 N4b signed /
	 * allowlisted / platform-trusted): consent step 4a, ALLOWED with no
	 * prompt and no store read. false = the ordinary per-subject path.
	 */
	bool component_trusted;
	uint64_t camera_id;   //!< 0 = the active display's own camera
	uint32_t output;      //!< enum xrt_stereo_camera_output: RECTIFIED only
	float max_frame_rate; //!< 0 = the source's rate
};

struct ipc_server_stereo_camera_internal_info
{
	struct xrt_stereo_camera_properties props;
	struct xrt_stereo_camera_calibration calib; //!< RECTIFIED; zeroed when the camera has none
	struct xrt_stereo_camera_stream_layout layout;
	const uint8_t *map; //!< the ring (layout.section_size bytes), valid until close
	//! The stream's wake (manager-owned, valid until close; never close it).
	xrt_graphics_sync_handle_t wake;
};

struct ipc_server_stereo_camera_internal_frame
{
	struct xrt_stereo_camera_frame_info info;
	const uint8_t *plane[2]; //!< into the ring; [1] NULL for a one-plane format
};

xrt_result_t
ipc_server_stereo_camera_internal_open(const struct ipc_server_stereo_camera_internal_request *req,
                                       struct ipc_server_stereo_camera_internal_info *out_info,
                                       struct ipc_server_stereo_camera_internal **out);

/*!
 * XRT_SUCCESS + a frame, XRT_TIMEOUT (nothing newer within @p timeout_ms), or
 * XRT_ERROR_STEREO_CAMERA_STREAM_ENDED. @p out_state (may be NULL): the
 * camera state as an app is told it (enum xrt_stereo_camera_state).
 */
xrt_result_t
ipc_server_stereo_camera_internal_acquire(struct ipc_server_stereo_camera_internal *c,
                                          uint32_t timeout_ms,
                                          struct ipc_server_stereo_camera_internal_frame *out_frame,
                                          uint32_t *out_state);

void
ipc_server_stereo_camera_internal_release(struct ipc_server_stereo_camera_internal *c, uint64_t frame_index);

//! Stop + free; NULL is ignored. Safe after the manager is gone.
void
ipc_server_stereo_camera_internal_close(struct ipc_server_stereo_camera_internal *c);

#ifdef __cplusplus
}
#endif
