// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P5: attach the conversion core's Vulkan glue to the system
 *         compositor's Vulkan device once it exists (macOS, desktop Linux).
 *         The POSIX counterpart of ipc_server_conv_d3d11.c.
 *
 * On macOS and desktop Linux the service's system compositor is the multi
 * system compositor over the null compositor, whose Vulkan bundle (the
 * target service's) is the one service-wide device: the shared spatial
 * surface, the macOS Vulkan weave backend and the desktop-Linux weave engine
 * all run on it. The glue runs on that bundle too, on its main queue (the
 * queue mutex serialises the submits), so macOS and Linux share one glue with
 * MoltenVK underneath on macOS, whichever weave backend the macOS front end
 * picked (the Metal backend owns its own device; this glue does not use it).
 *
 * ONE I/O table on POSIX: the CPU I/O glue (ipc_server_conv_cpu.c) is the
 * service's transport for client input and output (spec-4 CPU kinds,
 * cpu_only: no v3 route). This file adds the GPU side under it:
 *
 *  - ipc_server_conv_install_gpu with allocation (the service target's hook
 *    swaps its allocator to it; providers stay on the CPU lane, see
 *    vk_conv_gpu.h), S5 (composed pipelines: a depth-only provider now serves
 *    VIEWS_MONO / VIEWS_FROM_DEPTH), S6 readback, the nominal viewing
 *    distance -- and the CPU glue's own io table, so the install keeps the
 *    CPU route (cpu_only) instead of replacing it;
 *  - ipc_server_conv_cpu_io_set_gpu: the glue's CPU <-> image copies, so the
 *    CPU table can export a GPU result (the composed views, a mirror) and
 *    fill a GPU slot image.
 *
 *  - step C (lifted weave rects, XR_DXR_lift §6): the weave path's bridge to
 *    core-routed streams (vk_conv_weave, installed into comp_multi as its ops
 *    table -- comp_d3d11_service_conv_set_bridge's counterpart), spec-3 SBS /
 *    NVIEW stream create under the CPU route for those rects
 *    (ipc_server_conv_set_weave_rects), and the per-DP eyes sink
 *    (comp_multi_set_conv_eyes_sink, comp_d3d11_service_set_conv_eyes_sink's
 *    twin): every eye query comp_multi answers publishes the answering DP's
 *    eyes into the core keyed by that DP; a destroyed DP retires its key.
 *
 * Not installed:
 *
 *  - the `dp-lift` backend: the display plug-in's lift slots are D3D11-only;
 *  - a GPU client transport (textures + fences to the app): CPU bytes only.
 *
 * Detach runs in teardown_all BEFORE the system compositor (and its Vulkan
 * bundle) is destroyed.
 *
 * @ingroup ipc_server
 */

#include "server/ipc_server.h"
#include "server/ipc_server_conv.h"
#include "server/ipc_server_conv_cpu.h"

#include "conversion/conv_core.h"
#include "conversion/conv_stream.h"

#include "multi/comp_multi_interface.h"
#include "multi/comp_multi_weave_lift.h"
#include "vk_conv/vk_conv_gpu.h"
#include "vk_conv/vk_conv_weave.h"

#include "util/u_logging.h"

#include <string.h>

static struct
{
	bool attached;
	struct vk_conv_gpu *glue;
	struct vk_conv_weave *weave;
} g_conv_vk;

static bool
bridge_resolve(void *ud, uint64_t owner, uint64_t wire_id, uint64_t *out_core_id, uint32_t *out_mode)
{
	return ipc_server_conv_resolve_stream((struct ipc_server_conv *)ud, owner, wire_id, out_core_id, out_mode);
}

static void
bridge_match_weave(void *ud, uint64_t owner, uint64_t wire_id, uint32_t views)
{
	ipc_server_conv_v4_match_weave((struct ipc_server_conv *)ud, owner, wire_id, views);
}

static void
eyes_sink(void *ud, uint64_t dp_key, const struct xrt_eye_positions *e)
{
	ipc_server_conv_eyes_publish((struct ipc_server_conv *)ud, dp_key, e);
}

static void
eyes_retire(void *ud, uint64_t dp_key)
{
	ipc_server_conv_eyes_retire((struct ipc_server_conv *)ud, dp_key);
}

//! The weave stream tag the bridge resolves is the v3 route's (as on D3D11).
_Static_assert(IPC_SERVER_CONV_STREAM_TAG == (1ull << 62), "the weave path's core-stream tag");

static bool
gpu_write(void *ud, const struct xrt_conv_image *dst, const void *pixels, uint32_t row_pitch)
{
	return vk_conv_gpu_write((struct vk_conv_gpu *)ud, dst, pixels, row_pitch);
}

static bool
gpu_read(void *ud, const struct xrt_conv_image *src, void *pixels, uint32_t row_pitch)
{
	return vk_conv_gpu_read((struct vk_conv_gpu *)ud, src, pixels, row_pitch);
}

//! Step E: a client GPU image (the transport's handles, borrowed) -> the slot image.
static bool
gpu_import_copy(void *ud, const struct conv_input_import *in, const struct xrt_conv_image *dst)
{
	return vk_conv_gpu_import_input((struct vk_conv_gpu *)ud, in, dst);
}

static bool
gpu_export_alloc(
    void *ud, uint32_t kind, uint32_t conv_fmt, uint32_t w, uint32_t h, uint64_t stream_id, struct xrt_conv_image *out)
{
	return vk_conv_gpu_export_alloc((struct vk_conv_gpu *)ud, kind, conv_fmt, w, h, stream_id, out);
}

static void
gpu_export_free(void *ud, struct xrt_conv_image *img)
{
	struct conv_image_alloc_ops ops;
	vk_conv_gpu_alloc_ops((struct vk_conv_gpu *)ud, &ops);
	ops.free(ops.ud, img);
}

static bool
gpu_copy(void *ud, const struct xrt_conv_image *src, const struct xrt_conv_image *dst)
{
	return vk_conv_gpu_copy((struct vk_conv_gpu *)ud, src, dst);
}

void
ipc_server_conv_attach_vk(struct xrt_system_compositor *xsysc)
{
	struct ipc_server_conv *sc = ipc_server_get_conv();
	if (sc == NULL || g_conv_vk.attached || xsysc == NULL) {
		return;
	}
	// The one POSIX I/O table: the CPU glue (service_conversion.c installed it
	// already; the install below re-installs the same table, cpu_only).
	struct ipc_server_conv_gpu gpu;
	memset(&gpu, 0, sizeof(gpu));
	if (!ipc_server_conv_cpu_io_get(&gpu.io)) {
		U_LOG_W("[conv] Vulkan glue: no CPU I/O transport on this platform; not attached");
		return;
	}
	struct vk_bundle *vk = comp_multi_system_get_vk(xsysc);
	if (vk == NULL) {
		U_LOG_W(
		    "[conv] Vulkan glue: the system compositor has no Vulkan device; conversion stays CPU-only "
		    "(no composed pipelines)");
		return;
	}
	if (!vk_conv_gpu_create(vk, &g_conv_vk.glue)) {
		U_LOG_W("[conv] Vulkan glue: create failed; conversion stays CPU-only (no composed pipelines)");
		return;
	}

	vk_conv_gpu_alloc_ops(g_conv_vk.glue, &gpu.alloc);
	vk_conv_gpu_stage_ops(g_conv_vk.glue, &gpu.viewsyn, &gpu.readback);
	// gpu.dp_lift: none (D3D11 only).
	gpu.nominal_z_m = xsysc->info.nominal_viewer_z_m;
	gpu.adapter_luid = 0; // satellites are CPU-lane only on POSIX

	struct vk_conv_gpu_caps caps;
	vk_conv_gpu_get_caps(g_conv_vk.glue, &caps);
	U_LOG_W(
	    "[conv] Vulkan glue attached: memory kinds 0x%x (CPU%s%s%s), sync kinds 0x%x; providers on the CPU lane",
	    caps.memory_kinds, (caps.memory_kinds & XRT_CONV_MEM_VK_OPAQUE_FD) != 0 ? " OPAQUE_FD" : "",
	    (caps.memory_kinds & XRT_CONV_MEM_DMABUF) != 0 ? " DMABUF" : "",
	    (caps.memory_kinds & XRT_CONV_MEM_IOSURFACE) != 0 ? " IOSURFACE" : "", caps.sync_kinds);

	// The copies first: by the time GPU results can exist, the CPU table can export them.
	struct ipc_server_conv_cpu_gpu copies;
	memset(&copies, 0, sizeof(copies));
	copies.write = gpu_write;
	copies.read = gpu_read;
	copies.ud = g_conv_vk.glue;
	// Step E: the client GPU kinds (IOSurface on macOS; OPAQUE_FD / dma-buf on Linux).
	copies.import_kinds = vk_conv_gpu_import_kinds(g_conv_vk.glue);
	copies.export_kinds = vk_conv_gpu_export_kinds(g_conv_vk.glue);
	copies.import_copy = gpu_import_copy;
	copies.export_alloc = gpu_export_alloc;
	copies.export_free = gpu_export_free;
	copies.copy = gpu_copy;
	ipc_server_conv_cpu_io_set_gpu(&copies);
	U_LOG_W("[conv] Vulkan glue: client GPU kinds in 0x%x, out 0x%x (XRT_CONV_MEM_*; CPU always)",
	        copies.import_kinds, copies.export_kinds);
	ipc_server_conv_install_gpu(sc, &gpu);

	// Step C: the per-DP eyes sink, then lifted weave rects.
	struct conv_core *core = ipc_server_conv_get_core(sc);
	comp_multi_set_conv_eyes_sink(eyes_sink, eyes_retire, sc);
	if (vk_conv_weave_create(vk, g_conv_vk.glue, &g_conv_vk.weave)) {
		struct vk_conv_weave_bridge b;
		memset(&b, 0, sizeof(b));
		b.core = core;
		b.resolve = bridge_resolve;
		b.match_weave = bridge_match_weave;
		b.fit_rect = ipc_server_conv_submit_rect; // no window metrics: the windowless spec-4 rule
		b.ud = sc;
		vk_conv_weave_set_bridge(g_conv_vk.weave, &b);
		struct vk_conv_weave_ops ops;
		vk_conv_weave_get_ops(g_conv_vk.weave, &ops);
		comp_multi_weave_set_lift_ops(&ops);
		ipc_server_conv_set_weave_rects(sc, true);
		U_LOG_W("[conv] Vulkan glue: lifted weave rects on (spec-3 SBS / NVIEW streams for weave rects)");
	} else {
		U_LOG_W("[conv] Vulkan glue: lifted weave rects unavailable (helper create failed); woven as drawn");
	}
	g_conv_vk.attached = true;
}

void
ipc_server_conv_detach_vk(void)
{
	if (!g_conv_vk.attached) {
		return;
	}
	struct ipc_server_conv *sc = ipc_server_get_conv();
	// Step C first: no new lifted rects, no more eyes; comp_multi lets go of
	// the helper (waits for calls in flight), then the helper drops its pins
	// and pending snapshots (the queue idle) while the core's streams exist.
	ipc_server_conv_set_weave_rects(sc, false);
	comp_multi_weave_set_lift_ops(NULL);
	comp_multi_set_conv_eyes_sink(NULL, NULL, NULL);
	vk_conv_weave_destroy(&g_conv_vk.weave);
	// The core lets go first (the route, every core-routed stream -- their
	// images are the glue's --, the allocator, S5 / S6), then the copies
	// (waits for one in flight), then the glue.
	ipc_server_conv_install_gpu(sc, NULL);
	ipc_server_conv_cpu_io_set_gpu(NULL);
	vk_conv_gpu_destroy(&g_conv_vk.glue);
	// The CPU route outlives the GPU side.
	struct ipc_server_conv_io_ops cpu_io;
	if (sc != NULL && ipc_server_conv_cpu_io_get(&cpu_io)) {
		ipc_server_conv_set_io(sc, &cpu_io);
	}
	memset(&g_conv_vk, 0, sizeof(g_conv_vk));
}
