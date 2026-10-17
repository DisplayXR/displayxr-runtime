// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The conversion core's CPU I/O glue (see ipc_server_conv_cpu.h).
 * @ingroup ipc_server
 */

#include "server/ipc_server_conv_cpu.h"
#include "server/ipc_server_conv.h"

#include "shared/ipc_cpu_buffer.h"

#include "xrt/xrt_conversion_plugin.h"

#include "conversion/conv_stream.h"

#include "os/os_threading.h"
#include "util/u_handles.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

//! The DXGI_FORMAT numbers the lift wire reports a CPU export's format in
//! (the client maps them, oxr_conv_pixel_format_from_dxgi).
#define CPU_IO_DXGI_RGBA16_FLOAT 10u
#define CPU_IO_DXGI_RGBA8_UNORM 28u
#define CPU_IO_DXGI_RGBA8_SRGB 29u
#define CPU_IO_DXGI_R32_FLOAT 41u
#define CPU_IO_DXGI_R16_FLOAT 54u
#define CPU_IO_DXGI_R16_UNORM 56u
#define CPU_IO_DXGI_R8_UNORM 61u
#define CPU_IO_DXGI_BGRA8_UNORM 87u
#define CPU_IO_DXGI_BGRA8_SRGB 91u

//! One stream's export section.
struct cpu_io_stream
{
	bool used;
	uint64_t wire_id;
	xrt_graphics_buffer_handle_t handle; //!< owned; the transport duplicates it into the caller
	struct ipc_cpu_buffer_map map;       //!< writable, for the stream's lifetime
	uint32_t width, height, format;      //!< format: DXGI number (the wire's)
	uint64_t bytes;
	bool have_result; //!< a result was copied since the section was made

	//! Step E: a GPU export stream (XRT_CONV_MEM_IOSURFACE / _VK_OPAQUE_FD; 0 = the section).
	uint32_t export_kind;
	struct xrt_conv_image gpu_export; //!< the glue's export image (memory_kind 0 = none yet)
};

static struct
{
	struct os_mutex mtx;
	bool mtx_ok;
	struct cpu_io_stream streams[IPC_SERVER_CONV_MAX_STREAMS];
	//! ADR-053 P5: a GPU glue's copies (guarded by mtx, held across a copy).
	struct ipc_server_conv_cpu_gpu gpu;
} g_cpu_io;

static bool
cpu_io_init(void)
{
	if (!g_cpu_io.mtx_ok) {
		if (os_mutex_init(&g_cpu_io.mtx) != 0) {
			return false;
		}
		g_cpu_io.mtx_ok = true; // process-lifetime, like the ops themselves
		for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
			g_cpu_io.streams[i].handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		}
	}
	return true;
}

static uint32_t
fmt_bpp(uint32_t conv_fmt)
{
	switch (conv_fmt) {
	case XRT_CONV_FMT_R8_UNORM: return 1;
	case XRT_CONV_FMT_R16_FLOAT:
	case XRT_CONV_FMT_R16_UNORM: return 2;
	case XRT_CONV_FMT_RGBA16_FLOAT: return 8;
	case XRT_CONV_FMT_RGBA8_UNORM:
	case XRT_CONV_FMT_RGBA8_SRGB:
	case XRT_CONV_FMT_BGRA8_UNORM:
	case XRT_CONV_FMT_BGRA8_SRGB:
	case XRT_CONV_FMT_R32_FLOAT: return 4;
	default: return 0; // NV12 / unknown: not exportable as tightly packed rows
	}
}

static uint32_t
fmt_dxgi(uint32_t conv_fmt)
{
	switch (conv_fmt) {
	case XRT_CONV_FMT_RGBA8_UNORM: return CPU_IO_DXGI_RGBA8_UNORM;
	case XRT_CONV_FMT_RGBA8_SRGB: return CPU_IO_DXGI_RGBA8_SRGB;
	case XRT_CONV_FMT_BGRA8_UNORM: return CPU_IO_DXGI_BGRA8_UNORM;
	case XRT_CONV_FMT_BGRA8_SRGB: return CPU_IO_DXGI_BGRA8_SRGB;
	case XRT_CONV_FMT_R8_UNORM: return CPU_IO_DXGI_R8_UNORM;
	case XRT_CONV_FMT_R16_FLOAT: return CPU_IO_DXGI_R16_FLOAT;
	case XRT_CONV_FMT_R16_UNORM: return CPU_IO_DXGI_R16_UNORM;
	case XRT_CONV_FMT_R32_FLOAT: return CPU_IO_DXGI_R32_FLOAT;
	case XRT_CONV_FMT_RGBA16_FLOAT: return CPU_IO_DXGI_RGBA16_FLOAT;
	default: return 0;
	}
}

//! The stream's GPU export image back to the glue (mtx held).
static void
gpu_export_free_locked(struct cpu_io_stream *s)
{
	if (s->gpu_export.memory_kind != 0 && g_cpu_io.gpu.export_free != NULL) {
		g_cpu_io.gpu.export_free(g_cpu_io.gpu.ud, &s->gpu_export);
	}
	memset(&s->gpu_export, 0, sizeof(s->gpu_export));
}

static void
stream_free_locked(struct cpu_io_stream *s)
{
	gpu_export_free_locked(s);
	ipc_cpu_buffer_unmap(&s->map);
	u_graphics_buffer_unref(&s->handle);
	memset(s, 0, sizeof(*s));
	s->handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
}

static struct cpu_io_stream *
stream_find_locked(uint64_t wire_id, bool create)
{
	struct cpu_io_stream *free_slot = NULL;
	for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
		struct cpu_io_stream *s = &g_cpu_io.streams[i];
		if (s->used && s->wire_id == wire_id) {
			return s;
		}
		if (!s->used && free_slot == NULL) {
			free_slot = s;
		}
	}
	if (!create || free_slot == NULL) {
		return NULL;
	}
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = true;
	free_slot->wire_id = wire_id;
	free_slot->handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	return free_slot;
}


/*
 *
 * The io ops.
 *
 */

//! v3 input (a D3D11 shared texture): never on this glue.
static xrt_result_t
cpu_io_import_input(void *ud,
                    uint64_t stream_id,
                    xrt_graphics_buffer_handle_t handle,
                    bool is_dxgi,
                    uint32_t width,
                    uint32_t height,
                    const struct conv_submit_slot *slot)
{
	(void)ud;
	(void)stream_id;
	(void)is_dxgi;
	(void)width;
	(void)height;
	(void)slot;
	u_graphics_buffer_unref(&handle);
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

static void
cpu_io_release_input(void *ud, xrt_graphics_buffer_handle_t handle, bool is_dxgi)
{
	(void)ud;
	(void)is_dxgi;
	u_graphics_buffer_unref(&handle);
}

static xrt_result_t
cpu_io_import_input_ex(void *ud,
                       uint64_t stream_id,
                       const struct conv_input_import *in,
                       const struct conv_submit_slot *slot)
{
	(void)ud;
	(void)stream_id;
	xrt_graphics_buffer_handle_t h = in->handle;
	xrt_graphics_buffer_handle_t sh = in->sync_handle;
	if (in->kind == CONV_INPUT_IOSURFACE || in->kind == CONV_INPUT_OPAQUE_FD || in->kind == CONV_INPUT_DMABUF) {
		// Step E: a client GPU image, copied by the GPU glue (no CPU copy when
		// the slot is a GPU image). The handles are ours: released after.
		const struct xrt_conv_image *dst = slot != NULL && slot->image_count > 0 ? &slot->images[0] : NULL;
		os_mutex_lock(&g_cpu_io.mtx);
		const bool ok = dst != NULL && in->w > 0 && in->h > 0 && g_cpu_io.gpu.import_copy != NULL &&
		                g_cpu_io.gpu.import_copy(g_cpu_io.gpu.ud, in, dst);
		os_mutex_unlock(&g_cpu_io.mtx);
		u_graphics_buffer_unref(&h);
		u_graphics_buffer_unref(&sh);
		return ok ? XRT_SUCCESS : XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	u_graphics_buffer_unref(&sh); // CPU bytes are complete at the call: no fence
	if (in->kind != CONV_INPUT_CPU) {
		u_graphics_buffer_unref(&h);
		return XRT_ERROR_INPUT_UNSUPPORTED; // a D3D kind: never offered by this glue
	}
	const struct xrt_conv_image *img = slot != NULL && slot->image_count > 0 ? &slot->images[0] : NULL;
	const uint32_t x = in->x, y = in->y, w = in->w, hh = in->h, pitch = in->cpu_row_pitch;
	// A GPU slot image (a GPU glue that gives providers GPU inputs): staged
	// through a CPU buffer and written with the glue's copy.
	const bool gpu_slot = img != NULL && img->memory_kind != XRT_CONV_MEM_CPU;
	if (img == NULL || (!gpu_slot && img->cpu_ptr == NULL) || img->width == 0 || img->height == 0 ||
	    fmt_bpp(img->format) != 4 || w == 0 || hh == 0 || pitch < (x + w) * 4u) {
		u_graphics_buffer_unref(&h);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	// The wire is not trusted: the section must hold the whole region.
	const uint64_t need = (uint64_t)pitch * (y + hh - 1) + (uint64_t)(x + w) * 4u;
	struct ipc_cpu_buffer_map m;
	const bool mapped = ipc_cpu_buffer_map(h, need, false, &m);
	u_graphics_buffer_unref(&h); // the mapping keeps the section alive
	if (!mapped) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	const uint8_t *base = (const uint8_t *)m.ptr + (size_t)y * pitch + (size_t)x * 4u;
	const uint32_t dw = img->width, dh = img->height;
	const uint32_t dstride = gpu_slot ? dw * 4u : (img->row_stride != 0 ? img->row_stride : dw * 4u);
	uint8_t *staging = gpu_slot ? malloc((size_t)dstride * dh) : NULL;
	if (gpu_slot && staging == NULL) {
		ipc_cpu_buffer_unmap(&m);
		return XRT_ERROR_ALLOCATION;
	}
	uint8_t *dst_base = gpu_slot ? staging : (uint8_t *)img->cpu_ptr;
	const bool swap =
	    in->cpu_bgra != (img->format == XRT_CONV_FMT_BGRA8_UNORM || img->format == XRT_CONV_FMT_BGRA8_SRGB);
	for (uint32_t yy = 0; yy < dh; yy++) {
		uint8_t *dst = dst_base + (size_t)yy * dstride;
		const uint8_t *src = base + (size_t)((uint64_t)yy * hh / dh) * pitch;
		if (dw == w && !swap) {
			memcpy(dst, src, (size_t)w * 4u);
			continue;
		}
		for (uint32_t xx = 0; xx < dw; xx++) {
			const uint8_t *p = src + (size_t)((uint64_t)xx * w / dw) * 4u;
			dst[4 * xx + 0] = swap ? p[2] : p[0];
			dst[4 * xx + 1] = p[1];
			dst[4 * xx + 2] = swap ? p[0] : p[2];
			dst[4 * xx + 3] = p[3];
		}
	}
	ipc_cpu_buffer_unmap(&m);
	if (gpu_slot) {
		os_mutex_lock(&g_cpu_io.mtx);
		const bool ok =
		    g_cpu_io.gpu.write != NULL && g_cpu_io.gpu.write(g_cpu_io.gpu.ud, img, staging, dstride);
		os_mutex_unlock(&g_cpu_io.mtx);
		free(staging);
		return ok ? XRT_SUCCESS : XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	return XRT_SUCCESS;
}

static xrt_result_t
cpu_io_publish_result(void *ud,
                      uint64_t stream_id,
                      const struct conv_result *res,
                      struct ipc_server_conv_published *out)
{
	(void)ud;
	memset(out, 0, sizeof(*out));
	// A VIEWS stream exports its views; a DEPTH stream its depth.
	const struct xrt_conv_image *src = (res->written & XRT_CONV_OUT_VIEWS) != 0   ? &res->views
	                                   : (res->written & XRT_CONV_OUT_DEPTH) != 0 ? &res->depth
	                                                                              : NULL;
	const uint32_t bpp = src != NULL ? fmt_bpp(src->format) : 0;
	// A GPU result (the composed views or a mirror of a GPU glue): read back
	// with the glue's copy, one layer only (the section holds one).
	const bool gpu_src = src != NULL && src->memory_kind != XRT_CONV_MEM_CPU;
	const bool layered = gpu_src && src == &res->views && res->views_info.layout == XRT_CONV_VIEWS_ARRAY &&
	                     res->views_info.view_count > 1;
	if (src == NULL || (!gpu_src && src->cpu_ptr == NULL) || layered || bpp == 0 || src->width == 0 ||
	    src->height == 0) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	const uint32_t w = src->width, h = src->height;
	const uint32_t sstride = src->row_stride != 0 ? src->row_stride : w * bpp;
	const uint64_t bytes = (uint64_t)w * h * bpp; // tightly packed rows
	if (!gpu_src && (sstride < w * bpp ||
	                 (src->byte_size != 0 && (uint64_t)sstride * (h - 1) + (uint64_t)w * bpp > src->byte_size))) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}

	os_mutex_lock(&g_cpu_io.mtx);
	struct cpu_io_stream *s = stream_find_locked(stream_id, true);
	if (s == NULL) {
		os_mutex_unlock(&g_cpu_io.mtx);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	if (s->export_kind != 0) {
		// Step E: the result into the stream's GPU export image (a GPU result
		// is copied GPU -> GPU; a CPU one uploaded), complete on return.
		const struct ipc_server_conv_cpu_gpu *gg = &g_cpu_io.gpu;
		bool re = false;
		if (s->gpu_export.memory_kind == 0 || s->gpu_export.width != w || s->gpu_export.height != h ||
		    s->gpu_export.format != src->format) {
			gpu_export_free_locked(s);
			if (gg->export_alloc == NULL ||
			    !gg->export_alloc(gg->ud, s->export_kind, src->format, w, h, stream_id, &s->gpu_export)) {
				memset(&s->gpu_export, 0, sizeof(s->gpu_export));
				os_mutex_unlock(&g_cpu_io.mtx);
				U_LOG_W(
				    "[conv] GPU export: could not make a %ux%u export image (kind 0x%x) for stream "
				    "0x%llx",
				    w, h, s->export_kind, (unsigned long long)stream_id);
				return XRT_ERROR_ALLOCATION;
			}
			re = true;
		}
		if (gg->copy == NULL || !gg->copy(gg->ud, src, &s->gpu_export)) {
			os_mutex_unlock(&g_cpu_io.mtx);
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
		}
		s->have_result = true;
		out->width = w;
		out->height = h;
		out->format = fmt_dxgi(src->format);
		out->realloc = re;
		os_mutex_unlock(&g_cpu_io.mtx);
		return XRT_SUCCESS;
	}
	bool realloc = false;
	if (s->map.ptr == NULL || s->bytes != bytes || s->width != w || s->height != h ||
	    s->format != fmt_dxgi(src->format)) {
		ipc_cpu_buffer_unmap(&s->map);
		u_graphics_buffer_unref(&s->handle);
		s->have_result = false;
		if (!ipc_cpu_buffer_create(bytes, &s->handle) || !ipc_cpu_buffer_map(s->handle, bytes, true, &s->map)) {
			stream_free_locked(s);
			os_mutex_unlock(&g_cpu_io.mtx);
			U_LOG_W("[conv] CPU export: could not make a %ux%u shared section for stream 0x%llx", w, h,
			        (unsigned long long)stream_id);
			return XRT_ERROR_ALLOCATION;
		}
		s->width = w;
		s->height = h;
		s->format = fmt_dxgi(src->format);
		s->bytes = bytes;
		realloc = true;
	}
	if (gpu_src) {
		if (g_cpu_io.gpu.read == NULL || !g_cpu_io.gpu.read(g_cpu_io.gpu.ud, src, s->map.ptr, w * bpp)) {
			os_mutex_unlock(&g_cpu_io.mtx);
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
		}
	} else {
		for (uint32_t y = 0; y < h; y++) {
			memcpy((uint8_t *)s->map.ptr + (size_t)y * w * bpp,
			       (const uint8_t *)src->cpu_ptr + (size_t)y * sstride, (size_t)w * bpp);
		}
	}
	s->have_result = true;
	out->width = w;
	out->height = h;
	out->format = s->format;
	out->realloc = realloc;
	os_mutex_unlock(&g_cpu_io.mtx);
	return XRT_SUCCESS;
}

static bool
cpu_io_export_output(void *ud,
                     uint64_t stream_id,
                     bool depth,
                     xrt_graphics_buffer_handle_t *out_handle,
                     uint32_t *out_width,
                     uint32_t *out_height,
                     uint32_t *out_format)
{
	(void)ud;
	*out_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	if (depth) {
		return false; // no auxiliary depth texture export on POSIX
	}
	// Step E: a GPU export stream's image (the glue's handle; the transport
	// duplicates it into the caller). Otherwise CPU only (export_cpu).
	os_mutex_lock(&g_cpu_io.mtx);
	struct cpu_io_stream *s = stream_find_locked(stream_id, false);
	const bool have = s != NULL && s->export_kind != 0 && s->have_result && s->gpu_export.memory_kind != 0;
	if (have) {
		*out_handle = (xrt_graphics_buffer_handle_t)(intptr_t)s->gpu_export.handle;
		if (out_width != NULL) {
			*out_width = s->gpu_export.width;
		}
		if (out_height != NULL) {
			*out_height = s->gpu_export.height;
		}
		if (out_format != NULL) {
			*out_format = fmt_dxgi(s->gpu_export.format);
		}
	}
	os_mutex_unlock(&g_cpu_io.mtx);
	return have;
}

static bool
cpu_io_export_fence(void *ud, uint64_t stream_id, xrt_graphics_sync_handle_t *out_handle)
{
	(void)ud;
	(void)stream_id;
	*out_handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	return false;
}

static void
cpu_io_stream_gone(void *ud, uint64_t stream_id)
{
	(void)ud;
	os_mutex_lock(&g_cpu_io.mtx);
	struct cpu_io_stream *s = stream_find_locked(stream_id, false);
	if (s != NULL) {
		stream_free_locked(s);
	}
	os_mutex_unlock(&g_cpu_io.mtx);
}

/*!
 * The stream's export section. publish_result already copied the result in,
 * so @p copy only asks whether there is one; the handle stays the glue's (the
 * transport duplicates it into the caller).
 */
static bool
cpu_io_export_cpu(void *ud,
                  uint64_t stream_id,
                  bool copy,
                  xrt_graphics_buffer_handle_t *out_handle,
                  uint32_t *out_width,
                  uint32_t *out_height,
                  uint32_t *out_format,
                  uint64_t *out_bytes,
                  bool *out_realloc)
{
	(void)ud;
	(void)copy;
	*out_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	*out_realloc = false; // reported by publish_result (pub.realloc)
	os_mutex_lock(&g_cpu_io.mtx);
	struct cpu_io_stream *s = stream_find_locked(stream_id, false);
	const bool have = s != NULL && s->have_result && xrt_graphics_buffer_is_valid(s->handle);
	if (have) {
		*out_handle = s->handle;
		*out_width = s->width;
		*out_height = s->height;
		*out_format = s->format;
		*out_bytes = s->bytes;
	}
	os_mutex_unlock(&g_cpu_io.mtx);
	return have;
}


/*
 *
 * Exported.
 *
 */

bool
ipc_server_conv_cpu_io_supported(void)
{
	return ipc_cpu_buffer_supported();
}

bool
ipc_server_conv_cpu_io_get(struct ipc_server_conv_io_ops *out)
{
	memset(out, 0, sizeof(*out));
	if (!ipc_cpu_buffer_supported()) {
		return false;
	}
	if (!cpu_io_init()) {
		return false;
	}
	out->import_input = cpu_io_import_input;
	out->release_input = cpu_io_release_input;
	out->publish_result = cpu_io_publish_result;
	out->export_output = cpu_io_export_output;
	out->export_fence = cpu_io_export_fence;
	out->stream_gone = cpu_io_stream_gone;
	out->import_input_ex = cpu_io_import_input_ex;
	out->export_cpu = cpu_io_export_cpu;
	out->cpu_only = true;
	out->ud = NULL;
	return true;
}

void
ipc_server_conv_cpu_io_set_gpu(const struct ipc_server_conv_cpu_gpu *gpu)
{
	if (!cpu_io_init()) {
		return;
	}
	os_mutex_lock(&g_cpu_io.mtx); // a copy in flight holds it: clearing waits for it
	if (gpu != NULL) {
		g_cpu_io.gpu = *gpu;
	} else {
		// The GPU export images are the glue's: hand them back while it exists.
		for (uint32_t i = 0; i < IPC_SERVER_CONV_MAX_STREAMS; i++) {
			if (g_cpu_io.streams[i].used) {
				gpu_export_free_locked(&g_cpu_io.streams[i]);
			}
		}
		memset(&g_cpu_io.gpu, 0, sizeof(g_cpu_io.gpu));
	}
	os_mutex_unlock(&g_cpu_io.mtx);
}

static uint64_t
mem_to_image_kinds(uint32_t mem)
{
	uint64_t k = 0;
	if ((mem & XRT_CONV_MEM_IOSURFACE) != 0) {
		k |= 1ull << IPC_SERVER_CONV_IMAGE_KIND_IOSURFACE;
	}
	if ((mem & XRT_CONV_MEM_VK_OPAQUE_FD) != 0) {
		k |= 1ull << IPC_SERVER_CONV_IMAGE_KIND_OPAQUE_FD;
	}
	if ((mem & XRT_CONV_MEM_DMABUF) != 0) {
		k |= 1ull << IPC_SERVER_CONV_IMAGE_KIND_DMABUF;
	}
	return k;
}

uint64_t
ipc_server_conv_cpu_io_gpu_image_kinds(bool export_kinds)
{
	if (!cpu_io_init()) {
		return 0;
	}
	os_mutex_lock(&g_cpu_io.mtx);
	uint64_t k = 0;
	if (export_kinds) {
		k = g_cpu_io.gpu.export_alloc != NULL && g_cpu_io.gpu.copy != NULL
		        ? mem_to_image_kinds(g_cpu_io.gpu.export_kinds & ~XRT_CONV_MEM_DMABUF)
		        : 0;
	} else {
		k = g_cpu_io.gpu.import_copy != NULL ? mem_to_image_kinds(g_cpu_io.gpu.import_kinds) : 0;
	}
	os_mutex_unlock(&g_cpu_io.mtx);
	return k;
}

void
ipc_server_conv_cpu_io_stream_export(uint64_t wire_id, uint32_t mem_kind)
{
	if (!cpu_io_init()) {
		return;
	}
	os_mutex_lock(&g_cpu_io.mtx);
	struct cpu_io_stream *s = stream_find_locked(wire_id, mem_kind != 0);
	if (s != NULL && s->export_kind != mem_kind) {
		gpu_export_free_locked(s);
		s->have_result = false;
		s->export_kind = mem_kind;
	}
	os_mutex_unlock(&g_cpu_io.mtx);
}
