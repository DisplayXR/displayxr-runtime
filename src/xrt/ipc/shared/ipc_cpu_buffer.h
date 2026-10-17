// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CPU bytes that cross the IPC transport as a graphics-buffer handle
 *         (XR_DXR_lift's CPU image kind on POSIX).
 * @ingroup ipc_shared
 *
 * The XR_DXR_lift CPU kind moves tightly packed pixel bytes between an app and
 * the service in a shared section that rides the ordinary
 * xrt_graphics_buffer_handle_t transport (conv_submit_frame in_handles,
 * conv_get_cpu_output out_handles):
 *  - desktop Linux: an unlinked POSIX shared-memory fd (SCM_RIGHTS);
 *  - macOS: a global IOSurface used as a plain byte buffer (the macOS
 *    transport carries IOSurface IDs, ipc_message_channel_unix.c).
 *
 * Windows keeps its file mappings (oxr_conv_gpu_d3d.c, d3d11_conv_gpu.cpp) and
 * does not build this file; on any other platform every call fails.
 *
 * Ownership: create returns a handle the caller owns (u_graphics_buffer_unref
 * releases it). map never takes the handle: the mapping keeps the section
 * alive on its own, so the handle can be released right after mapping.
 */

#pragma once

#include "xrt/xrt_handles.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A live mapping of a CPU buffer.
struct ipc_cpu_buffer_map
{
	void *ptr;      //!< the first byte; NULL = not mapped
	uint64_t bytes; //!< the bytes mapped (what map was asked for)
	bool writable;
	void *priv; //!< platform state (macOS: the retained IOSurface)
};

//! Does this platform carry the CPU kind over IPC?
bool
ipc_cpu_buffer_supported(void);

/*!
 * A new shared section of at least @p bytes, zero-filled. *out_handle is the
 * caller's (release with u_graphics_buffer_unref).
 */
bool
ipc_cpu_buffer_create(uint64_t bytes, xrt_graphics_buffer_handle_t *out_handle);

/*!
 * Map @p bytes of @p handle (read-only unless @p writable). Fails when the
 * section is smaller than @p bytes: a handle off the wire is not trusted.
 * Does not take @p handle.
 */
bool
ipc_cpu_buffer_map(xrt_graphics_buffer_handle_t handle, uint64_t bytes, bool writable, struct ipc_cpu_buffer_map *out);

//! Undo ipc_cpu_buffer_map (NULL / unmapped is a no-op).
void
ipc_cpu_buffer_unmap(struct ipc_cpu_buffer_map *map);

#ifdef __cplusplus
}
#endif
