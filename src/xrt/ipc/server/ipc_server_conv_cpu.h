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

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_server_conv_io_ops;

//! Does this platform have the CPU glue (ipc_cpu_buffer_supported)?
bool
ipc_server_conv_cpu_io_supported(void);

/*!
 * Fill @p out with the CPU glue (its state is process-wide; install it with
 * ipc_server_conv_set_io). False when the platform has no CPU buffer transport.
 */
bool
ipc_server_conv_cpu_io_get(struct ipc_server_conv_io_ops *out);

#ifdef __cplusplus
}
#endif
