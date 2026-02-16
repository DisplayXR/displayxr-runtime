// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metal/IOSurface helpers for cross-process texture sharing on macOS.
 * @author David Fattal <david.fattal@leiainc.com>
 * @ingroup ipc_shared
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * Get the IOSurfaceID from a Metal texture handle.
 *
 * The handle is expected to be a retained id<MTLTexture> (as void*) that has
 * IOSurface backing. Returns 0 if the texture has no IOSurface or is NULL.
 */
uint32_t
ipc_metal_handle_to_iosurface_id(void *metal_texture_handle);

/*!
 * Look up an IOSurface by global ID, returned as a retained void*
 * (IOSurfaceRef).
 *
 * The caller is responsible for releasing the returned handle via
 * CFRelease() when done.
 *
 * Returns NULL on failure (invalid ID or lookup failure).
 */
void *
ipc_metal_iosurface_from_id(uint32_t surface_id);


#ifdef __cplusplus
}
#endif
