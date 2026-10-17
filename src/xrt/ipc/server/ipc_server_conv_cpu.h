// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The conversion core's CPU I/O glue for services without a D3D11
 *         device (desktop Linux, macOS): XR_DXR_lift spec-4 CPU kinds.
 * @ingroup ipc_server
 *
 * The Windows D3D11 service installs a GPU glue once its device exists
 * (d3d11_conv_gpu.cpp). A POSIX service has no such device, and until this
 * glue nothing engaged the core route there: the core ran, providers were
 * described, but no stream could be created. This glue is the CPU-first v1
 * of the POSIX route (roadmap conversion-providers.md §4.2):
 *
 *  - input: CPU bytes in a shared section (ipc_cpu_buffer.h — a POSIX shm fd
 *    on Linux, a global IOSurface on macOS), copied into the core's CPU slot
 *    image (RGBA8; BGRA8 swizzled; a smaller slot — inputScale — is filled by
 *    nearest-neighbour sampling);
 *  - output: the published result (views for a VIEWS stream, depth for a
 *    DEPTH one) copied, tightly packed, into a per-stream shared section the
 *    client maps (conv_get_cpu_output);
 *  - no v3 route (cpu_only): v3 is D3D11 shared textures by definition.
 *
 * The core has no CPU view synthesis: a VIEWS task here is served only by a
 * provider that emits views itself. Depth-only providers need the Vulkan GPU
 * glue (step B) before a COMPOSED views task exists on POSIX.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct conv_input_import;
struct ipc_server_conv_io_ops;
struct xrt_conv_image;

//! Does this platform have the CPU glue (ipc_cpu_buffer_supported)?
bool
ipc_server_conv_cpu_io_supported(void);

/*!
 * Fill @p out with the CPU glue (its state is process-wide; install it with
 * ipc_server_conv_set_io). False when the platform has no CPU buffer transport.
 */
bool
ipc_server_conv_cpu_io_get(struct ipc_server_conv_io_ops *out);

/*!
 * ADR-053 P5: a GPU glue's CPU <-> image copies (the POSIX Vulkan glue,
 * vk_conv_gpu_write / _read), for the images it allocated. With them the CPU
 * table stays the one I/O table on POSIX while a GPU glue owns allocation, S5
 * and S6: publish_result reads a GPU result (the composed views, a mirror)
 * back into the export section, and import_input_ex writes into a GPU slot
 * image. Both copies are synchronous; layered (ARRAY) GPU results are refused.
 */
struct ipc_server_conv_cpu_gpu
{
	//! @p dst (the glue's GPU image) <- rows @p row_pitch apart (0 = tight).
	bool (*write)(void *ud, const struct xrt_conv_image *dst, const void *pixels, uint32_t row_pitch);
	//! @p src (the glue's GPU image) -> rows @p row_pitch apart (0 = tight).
	bool (*read)(void *ud, const struct xrt_conv_image *src, void *pixels, uint32_t row_pitch);
	void *ud;

	/*!
	 * @name Step E: the client GPU kinds (all optional; NULL / 0 = CPU only)
	 *
	 * The kinds a GPU glue takes from and gives to the client: the service
	 * then lists them in imageKinds / exportKinds (beside CPU), and spec-4
	 * streams may be created with a GPU export kind.
	 * @{
	 */
	//! XRT_CONV_MEM_* the glue imports (import_copy) / exports (export_alloc).
	uint32_t import_kinds;
	uint32_t export_kinds;
	/*!
	 * Client image @p in (a CONV_INPUT_IOSURFACE / _OPAQUE_FD / _DMABUF; its
	 * handles stay the caller's) -> slot image @p dst, a GPU or CPU image.
	 * Synchronous; honours @p in's sync on the GPU.
	 */
	bool (*import_copy)(void *ud, const struct conv_input_import *in, const struct xrt_conv_image *dst);
	//! A per-stream export image of @p kind (one of export_kinds); freed with export_free.
	bool (*export_alloc)(void *ud,
	                     uint32_t kind,
	                     uint32_t conv_fmt,
	                     uint32_t w,
	                     uint32_t h,
	                     uint64_t stream_id,
	                     struct xrt_conv_image *out);
	void (*export_free)(void *ud, struct xrt_conv_image *img);
	//! @p src (a GPU or CPU result) -> export image @p dst, same extent. Synchronous.
	bool (*copy)(void *ud, const struct xrt_conv_image *src, const struct xrt_conv_image *dst);
	//! @}
};

/*!
 * The XrLiftImageKindDXR mask (1 << kind, IPC_SERVER_CONV_IMAGE_KIND_*) of
 * the GPU kinds the installed GPU glue takes (@p export_kinds false) or gives;
 * 0 without one. CPU is not included (the table always takes and gives it).
 */
uint64_t
ipc_server_conv_cpu_io_gpu_image_kinds(bool export_kinds);

/*!
 * A spec-4 stream (@p wire_id) was created with GPU export kind @p mem_kind
 * (XRT_CONV_MEM_IOSURFACE / _VK_OPAQUE_FD): publish_result copies its results
 * into a per-stream export image of that kind (GPU -> GPU for a GPU result)
 * and export_output hands its handle out. 0 = the CPU section (the default).
 */
void
ipc_server_conv_cpu_io_stream_export(uint64_t wire_id, uint32_t mem_kind);

/*!
 * Install (or, with NULL, clear) the GPU copies. Copied. Clearing waits for
 * any copy in flight: after it returns the glue may be destroyed.
 */
void
ipc_server_conv_cpu_io_set_gpu(const struct ipc_server_conv_cpu_gpu *gpu);

#ifdef __cplusplus
}
#endif
