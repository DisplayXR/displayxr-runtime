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
#include "util/u_camera_consent.h"
#include "util/u_stereo_uvc.h"

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

//! Destroy every stream @p ics created. Called once at client teardown.
void
ipc_server_client_stereo_camera_release(volatile struct ipc_client_state *ics);


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
	 * or a visible session), else BUSY; a started one delivers no frames and
	 * reads SUSPENDED while that stops being true.
	 */
	bool user_facing_requires_presenting;
	/*!
	 * A source that gave up after it had delivered (the OS took the camera:
	 * app in the background, sensor privacy) keeps its started streams
	 * SUSPENDED instead of UNAVAILABLE while it retries (Android).
	 */
	bool gave_up_is_suspended;
	void *ctx;
};

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
	char consumers[256];      //!< "browser.exe, call-app.exe" (base names / Android packages, de-duplicated)
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

#ifdef __cplusplus
}
#endif
