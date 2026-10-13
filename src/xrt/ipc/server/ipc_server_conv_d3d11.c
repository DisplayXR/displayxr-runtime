// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P1a (#2003): attach the D3D11 service compositor's GPU side
 *         to the conversion core once the service's device exists.
 *
 * The service target owns the core (service_conversion.c) and registers it
 * with ipc_server_set_conv before ipc_server_main; the D3D11 device only
 * exists after init_all created the system compositor. This file joins the
 * two (it is the only code that sees both the compositor and the core):
 *
 *  - comp_d3d11_service_conv_glue -> ipc_server_conv_install_gpu: image
 *    allocation, S5 / S6, the v3 route's I/O and the `dp-lift` backend (the
 *    service target's hook registers the provider row + swaps its allocator);
 *  - the weave path's bridge to core-routed stream ids;
 *  - the per-DP eyes sink (G7, roadmap §5.2): every eye query the compositor
 *    answers publishes the answering DP's snapshot into the core, keyed by
 *    that DP; a destroyed client DP retires its key.
 *
 * Detach runs in teardown_all BEFORE the system compositor is destroyed.
 *
 * @ingroup ipc_server
 */

#include "server/ipc_server.h"
#include "server/ipc_server_conv.h"

#include "conversion/conv_core.h"

#include "d3d11_service/comp_d3d11_service.h"
#include "d3d11_service/comp_d3d11_service_conv.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <string.h>

// The v3 route's stream ids are what the weave path recognises.
_Static_assert(IPC_SERVER_CONV_STREAM_TAG == (1ull << 62),
               "the weave path's core-stream tag (D3D11_LIFT_CORE_STREAM_TAG)");

static struct
{
	bool attached;
	struct xrt_system_compositor *xsysc;
	struct comp_d3d11_conv_glue glue;
} g_conv_d3d11;

static xrt_result_t
io_publish(void *ud, uint64_t stream_id, const struct conv_result *res, struct ipc_server_conv_published *out)
{
	(void)ud;
	struct comp_d3d11_conv_published p;
	memset(&p, 0, sizeof(p));
	const xrt_result_t r = g_conv_d3d11.glue.publish_result(g_conv_d3d11.glue.io_ud, stream_id, res, &p);
	out->fence_value = p.fence_value;
	out->width = p.width;
	out->height = p.height;
	out->format = p.format;
	out->realloc = p.realloc;
	out->depth_width = p.depth_width;
	out->depth_height = p.depth_height;
	out->depth_format = p.depth_format;
	out->depth_realloc = p.depth_realloc;
	return r;
}

static bool
bridge_resolve(void *ud, uint64_t owner, uint64_t wire_id, uint64_t *out_core_id, uint32_t *out_mode)
{
	return ipc_server_conv_resolve_stream((struct ipc_server_conv *)ud, owner, wire_id, out_core_id, out_mode);
}

static void
eyes_sink(void *ud, uint64_t dp_key, const struct xrt_eye_positions *e)
{
	struct conv_eyes ce;
	memset(&ce, 0, sizeof(ce));
	ce.valid = e->valid;
	ce.is_tracking = e->is_tracking;
	ce.count = e->count > CONV_EYES_MAX ? CONV_EYES_MAX : e->count;
	for (uint32_t i = 0; i < ce.count; i++) {
		ce.xyz[i][0] = e->eyes[i].x;
		ce.xyz[i][1] = e->eyes[i].y;
		ce.xyz[i][2] = e->eyes[i].z;
	}
	ce.timestamp_ns = os_monotonic_get_ns();
	conv_core_eyes_publish((struct conv_core *)ud, dp_key, &ce);
}

static void
eyes_retire(void *ud, uint64_t dp_key)
{
	conv_core_eyes_retire((struct conv_core *)ud, dp_key);
}

void
ipc_server_conv_attach_d3d11(struct xrt_system_compositor *xsysc)
{
	struct ipc_server_conv *sc = ipc_server_get_conv();
	if (sc == NULL || g_conv_d3d11.attached || !comp_d3d11_service_is_d3d11_service(xsysc)) {
		return;
	}
	struct comp_d3d11_conv_glue *g = &g_conv_d3d11.glue;
	if (!comp_d3d11_service_conv_glue(xsysc, g)) {
		U_LOG_W("[conv] D3D11 service: no GPU glue (no device); lift stays on the pre-core path");
		return;
	}
	struct conv_core *core = ipc_server_conv_get_core(sc);

	struct ipc_server_conv_gpu gpu;
	memset(&gpu, 0, sizeof(gpu));
	gpu.alloc = g->alloc;
	gpu.viewsyn = g->viewsyn;
	gpu.readback = g->readback;
	gpu.dp_lift = g->dp_lift;
	gpu.io.import_input = g->import_input;
	gpu.io.import_input_cpu = g->import_input_cpu;
	gpu.io.release_input = g->release_input;
	gpu.io.input_content_key = g->input_content_key;
	gpu.io.publish_result = io_publish;
	gpu.io.export_output = g->export_output;
	gpu.io.export_fence = g->export_fence;
	gpu.io.stream_gone = g->stream_gone;
	gpu.io.import_input_ex = g->import_input_ex;
	gpu.io.export_cpu = g->export_cpu;
	gpu.io.ud = g->io_ud;
	gpu.nominal_z_m = g->nominal_z_m;
	gpu.adapter_luid = g->adapter_luid;

	comp_d3d11_service_set_conv_eyes_sink(eyes_sink, eyes_retire, core);
	struct comp_d3d11_conv_bridge b;
	memset(&b, 0, sizeof(b));
	b.core = core;
	b.resolve = bridge_resolve;
	b.ud = sc;
	comp_d3d11_service_conv_set_bridge(xsysc, &b);
	ipc_server_conv_install_gpu(sc, &gpu);
	g_conv_d3d11.attached = true;
	g_conv_d3d11.xsysc = xsysc;
}

void
ipc_server_conv_detach_d3d11(void)
{
	if (!g_conv_d3d11.attached) {
		return;
	}
	ipc_server_conv_install_gpu(ipc_server_get_conv(), NULL);
	comp_d3d11_service_conv_set_bridge(g_conv_d3d11.xsysc, NULL);
	comp_d3d11_service_set_conv_eyes_sink(NULL, NULL, NULL);
	memset(&g_conv_d3d11, 0, sizeof(g_conv_d3d11));
}
