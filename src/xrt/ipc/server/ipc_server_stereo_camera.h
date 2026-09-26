// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043): the service's camera manager.
 *
 * The service is the single owner of each plug-in-provided stereo camera: one
 * runtime-owned thread per open camera pulls frames from the plug-in's
 * wait_frame slot, and fans each frame out to every started, authorised stream
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

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_server_stereo_camera;
struct ipc_client_state;

/*!
 * Create the manager over the active plug-in of @p xinst (the service's native
 * instance). Always returns a manager; with no plug-in, no camera slots, or the
 * kill switch (DXR_STEREO_CAMERA=0) it simply exposes zero cameras.
 */
struct ipc_server_stereo_camera *
ipc_server_stereo_camera_create(struct xrt_instance *xinst);

//! Stop every camera thread, close every source, free every stream.
void
ipc_server_stereo_camera_destroy(struct ipc_server_stereo_camera **mgr_ptr);

//! Destroy every stream @p ics created. Called once at client teardown.
void
ipc_server_client_stereo_camera_release(volatile struct ipc_client_state *ics);

#ifdef __cplusplus
}
#endif
