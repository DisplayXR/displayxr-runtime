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
};

struct ipc_server_stereo_camera *
ipc_server_stereo_camera_create_ex(struct xrt_instance *xinst, const struct ipc_server_stereo_camera_options *opts);

//! Stop every camera thread, close every source, free every stream.
void
ipc_server_stereo_camera_destroy(struct ipc_server_stereo_camera **mgr_ptr);

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
	char consumers[256];      //!< "browser.exe, call-app.exe" (base names, de-duplicated)
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
