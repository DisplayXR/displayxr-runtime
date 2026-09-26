// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) client-side IPC calls (see
 *         ipc_client_stereo_camera.h).
 * @ingroup ipc_client
 */

#include "xrt/xrt_config_os.h"

#include "util/u_misc.h"

#include "shared/ipc_protocol.h"
#include "shared/ipc_shmem.h"
#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client_stereo_camera.h"
#include "ipc_client_generated.h"

#include <stdlib.h>
#include <string.h>

xrt_result_t
ipc_client_stereo_camera_count(struct ipc_connection *ipc_c, uint32_t *out_count)
{
	*out_count = 0;
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_stereo_camera_count(ipc_c, out_count);
}

xrt_result_t
ipc_client_stereo_camera_get_properties(struct ipc_connection *ipc_c,
                                        uint32_t index,
                                        struct xrt_stereo_camera_properties *out_props)
{
	U_ZERO(out_props);
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	xrt_result_t xret = ipc_call_stereo_camera_get_properties(ipc_c, index, out_props);
	out_props->persistent_id[sizeof(out_props->persistent_id) - 1] = '\0';
	out_props->display_name[sizeof(out_props->display_name) - 1] = '\0';
	out_props->platform_device_hint[sizeof(out_props->platform_device_hint) - 1] = '\0';
	return xret;
}

xrt_result_t
ipc_client_stereo_camera_get_calibration(struct ipc_connection *ipc_c,
                                         uint64_t camera_id,
                                         uint32_t output,
                                         struct xrt_stereo_camera_calibration *out_calib)
{
	U_ZERO(out_calib);
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_stereo_camera_get_calibration(ipc_c, camera_id, output, out_calib);
}

xrt_result_t
ipc_client_stereo_camera_stream_create(struct ipc_connection *ipc_c,
                                       const struct xrt_stereo_camera_stream_request *req,
                                       uint64_t *out_stream_id)
{
	*out_stream_id = 0;
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_stereo_camera_stream_create(ipc_c, req, out_stream_id);
}

xrt_result_t
ipc_client_stereo_camera_stream_destroy(struct ipc_connection *ipc_c, uint64_t stream_id)
{
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_stereo_camera_stream_destroy(ipc_c, stream_id);
}

xrt_result_t
ipc_client_stereo_camera_stream_start(struct ipc_connection *ipc_c, uint64_t stream_id)
{
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_stereo_camera_stream_start(ipc_c, stream_id);
}

xrt_result_t
ipc_client_stereo_camera_stream_stop(struct ipc_connection *ipc_c, uint64_t stream_id)
{
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_stereo_camera_stream_stop(ipc_c, stream_id);
}

xrt_result_t
ipc_client_stereo_camera_stream_get_transport(struct ipc_connection *ipc_c,
                                              uint64_t stream_id,
                                              struct xrt_stereo_camera_stream_layout *out_layout,
                                              xrt_shmem_handle_t *out_section,
                                              xrt_graphics_sync_handle_t *out_wake)
{
	U_ZERO(out_layout);
	*out_section = XRT_SHMEM_HANDLE_INVALID;
	*out_wake = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	xrt_shmem_handle_t section = XRT_SHMEM_HANDLE_INVALID;
	xrt_result_t xret = ipc_call_stereo_camera_stream_get_section(ipc_c, stream_id, out_layout, &section, 1);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	xrt_graphics_sync_handle_t wake = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	xret = ipc_call_stereo_camera_stream_get_wake(ipc_c, stream_id, &wake, 1);
	if (xret != XRT_SUCCESS) {
		// The section crossed; do not leak it.
		void *none = NULL;
		ipc_shmem_destroy(&section, &none, 0);
		return xret;
	}
	*out_section = section;
	*out_wake = wake;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_client_stereo_camera_acquire(struct ipc_connection *ipc_c,
                                 uint64_t stream_id,
                                 bool *out_ready,
                                 struct xrt_stereo_camera_frame_info *out_frame)
{
	*out_ready = false;
	U_ZERO(out_frame);
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_stereo_camera_acquire(ipc_c, stream_id, out_ready, out_frame);
}

xrt_result_t
ipc_client_stereo_camera_stats(struct ipc_connection *ipc_c,
                               uint64_t stream_id,
                               struct xrt_stereo_camera_stream_stats *out_stats)
{
	U_ZERO(out_stats);
	if (ipc_c == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	return ipc_call_stereo_camera_stream_stats(ipc_c, stream_id, out_stats);
}


/*
 *
 * xrt_stereo_camera_client aspect.
 *
 */

struct ipc_stereo_camera_client
{
	struct xrt_stereo_camera_client base;
	struct ipc_connection *ipc_c;
};

static inline struct ipc_connection *
conn(struct xrt_stereo_camera_client *c)
{
	return ((struct ipc_stereo_camera_client *)c)->ipc_c;
}

static xrt_result_t
c_count(struct xrt_stereo_camera_client *c, uint32_t *out_count)
{
	return ipc_client_stereo_camera_count(conn(c), out_count);
}

static xrt_result_t
c_get_properties(struct xrt_stereo_camera_client *c, uint32_t index, struct xrt_stereo_camera_properties *out)
{
	return ipc_client_stereo_camera_get_properties(conn(c), index, out);
}

static xrt_result_t
c_get_calibration(struct xrt_stereo_camera_client *c,
                  uint64_t camera_id,
                  uint32_t output,
                  struct xrt_stereo_camera_calibration *out)
{
	return ipc_client_stereo_camera_get_calibration(conn(c), camera_id, output, out);
}

static xrt_result_t
c_stream_create(struct xrt_stereo_camera_client *c,
                const struct xrt_stereo_camera_stream_request *req,
                uint64_t *out_stream_id)
{
	return ipc_client_stereo_camera_stream_create(conn(c), req, out_stream_id);
}

static xrt_result_t
c_stream_destroy(struct xrt_stereo_camera_client *c, uint64_t stream_id)
{
	return ipc_client_stereo_camera_stream_destroy(conn(c), stream_id);
}

static xrt_result_t
c_stream_start(struct xrt_stereo_camera_client *c, uint64_t stream_id)
{
	return ipc_client_stereo_camera_stream_start(conn(c), stream_id);
}

static xrt_result_t
c_stream_stop(struct xrt_stereo_camera_client *c, uint64_t stream_id)
{
	return ipc_client_stereo_camera_stream_stop(conn(c), stream_id);
}

static xrt_result_t
c_stream_get_transport(struct xrt_stereo_camera_client *c,
                       uint64_t stream_id,
                       struct xrt_stereo_camera_stream_layout *out_layout,
                       xrt_shmem_handle_t *out_section,
                       xrt_graphics_sync_handle_t *out_wake)
{
	return ipc_client_stereo_camera_stream_get_transport(conn(c), stream_id, out_layout, out_section, out_wake);
}

static xrt_result_t
c_acquire(struct xrt_stereo_camera_client *c,
          uint64_t stream_id,
          bool *out_ready,
          struct xrt_stereo_camera_frame_info *out_frame)
{
	return ipc_client_stereo_camera_acquire(conn(c), stream_id, out_ready, out_frame);
}

static xrt_result_t
c_stats(struct xrt_stereo_camera_client *c, uint64_t stream_id, struct xrt_stereo_camera_stream_stats *out)
{
	return ipc_client_stereo_camera_stats(conn(c), stream_id, out);
}

struct xrt_stereo_camera_client *
ipc_client_stereo_camera_client_create(struct ipc_connection *ipc_c)
{
	struct ipc_stereo_camera_client *c = U_TYPED_CALLOC(struct ipc_stereo_camera_client);
	c->ipc_c = ipc_c;
	c->base.count = c_count;
	c->base.get_properties = c_get_properties;
	c->base.get_calibration = c_get_calibration;
	c->base.stream_create = c_stream_create;
	c->base.stream_destroy = c_stream_destroy;
	c->base.stream_start = c_stream_start;
	c->base.stream_stop = c_stream_stop;
	c->base.stream_get_transport = c_stream_get_transport;
	c->base.acquire = c_acquire;
	c->base.stats = c_stats;
	return &c->base;
}
