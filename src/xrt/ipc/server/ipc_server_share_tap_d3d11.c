// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061: the stereo-camera manager's share tap hooks on the D3D11
 *         service compositor. A client id resolves to its compositor under
 *         global_state.lock on every call that needs one, so a client that went
 *         away is an error rather than a dangling read.
 * @ingroup ipc_server
 */

#include "ipc_server.h"
#include "ipc_server_stereo_camera.h"

#include "d3d11_service/comp_d3d11_service.h"

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_stereo_camera.h"

static volatile struct ipc_client_state *
find_client_locked(struct ipc_server *s, uint32_t client_id)
{
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		volatile struct ipc_client_state *o = &s->threads[i].ics;
		if (o->server_thread_index >= 0 && o->xs != NULL && o->xc != NULL && o->client_state.id == client_id) {
			return o;
		}
	}
	return NULL;
}

static bool
tap_query(void *ctx, uint32_t client_id, struct xrt_app_share_source_info *io_info, int64_t *out_last_commit_ns)
{
	struct ipc_server *s = (struct ipc_server *)ctx;
	bool ok = false;
	os_mutex_lock(&s->global_state.lock);
	volatile struct ipc_client_state *o = find_client_locked(s, client_id);
	if (o != NULL) {
		ok = comp_d3d11_service_share_query((struct xrt_compositor *)o->xc, io_info, out_last_commit_ns);
	}
	os_mutex_unlock(&s->global_state.lock);
	return ok;
}

static xrt_result_t
tap_attach(void *ctx, uint32_t client_id, uint32_t eye_w, uint32_t eye_h, float fps, void **out_tap)
{
	struct ipc_server *s = (struct ipc_server *)ctx;
	xrt_result_t xret = XRT_ERROR_IPC_FAILURE;
	os_mutex_lock(&s->global_state.lock);
	volatile struct ipc_client_state *o = find_client_locked(s, client_id);
	if (o != NULL) {
		xret = comp_d3d11_service_share_attach((struct xrt_compositor *)o->xc, eye_w, eye_h, fps, out_tap);
	}
	os_mutex_unlock(&s->global_state.lock);
	return xret;
}

static uint32_t
tap_wait_frame(void *ctx,
               void *tap,
               int64_t timeout_ns,
               struct xrt_plugin_stereo_camera_frame *out_frame,
               struct xrt_app_share_frame_info *out_info)
{
	(void)ctx;
	return comp_d3d11_service_share_wait_frame(tap, timeout_ns, out_frame, out_info);
}

static void
tap_release_frame(void *ctx, void *tap)
{
	(void)ctx;
	(void)tap; // the planes are the consumer's until its next wait
}

static void
tap_detach(void *ctx, void *tap)
{
	(void)ctx;
	comp_d3d11_service_share_detach(tap);
}

static bool
link_adapter_luid(void *ctx, uint8_t out_luid[XRT_LUID_SIZE])
{
	return comp_d3d11_service_share_link_adapter_luid(((struct ipc_server *)ctx)->xsysc, out_luid);
}

static xrt_result_t
link_import(void *ctx,
            const struct xrt_share_link_ring_desc *desc,
            const xrt_graphics_buffer_handle_t *textures,
            uint32_t texture_count,
            xrt_graphics_sync_handle_t fence,
            uint32_t eye_w,
            uint32_t eye_h,
            void **out_import)
{
	return comp_d3d11_service_share_link_import(((struct ipc_server *)ctx)->xsysc, desc, textures, texture_count, fence,
	                                            eye_w, eye_h, out_import);
}

static uint32_t
link_convert(void *ctx,
             void *import,
             const struct xrt_share_link_frame *frame,
             struct xrt_plugin_stereo_camera_frame *out_frame,
             struct xrt_app_share_frame_info *out_info)
{
	(void)ctx;
	return comp_d3d11_service_share_link_convert(import, frame, out_frame, out_info);
}

static void
link_unimport(void *ctx, void *import)
{
	(void)ctx;
	comp_d3d11_service_share_link_unimport(import);
}

void
ipc_server_share_tap_d3d11_install(struct ipc_server *s)
{
	struct ipc_server_stereo_camera_platform p = {0};
	p.share_tap_query = tap_query;
	p.share_tap_attach = tap_attach;
	p.share_tap_wait_frame = tap_wait_frame;
	p.share_tap_release_frame = tap_release_frame;
	p.share_tap_detach = tap_detach;
	p.share_link_adapter_luid = link_adapter_luid;
	p.share_link_import = link_import;
	p.share_link_convert = link_convert;
	p.share_link_unimport = link_unimport;
	p.ctx = s;
	ipc_server_stereo_camera_set_platform(&p);
}
