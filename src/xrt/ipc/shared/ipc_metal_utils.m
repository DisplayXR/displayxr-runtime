// Copyright 2024, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metal/IOSurface helpers for cross-process texture sharing on macOS.
 * @author David Fattal <david.fattal@leiainc.com>
 * @ingroup ipc_shared
 */

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>

#include "ipc_metal_utils.h"


uint32_t
ipc_metal_handle_to_iosurface_id(void *metal_texture_handle)
{
	if (!metal_texture_handle) {
		return 0;
	}

	// Handle is an IOSurfaceRef (exported by vk_ic_get_handles via
	// vkExportMetalObjectsEXT). Just get its global ID directly.
	IOSurfaceRef surface = (IOSurfaceRef)metal_texture_handle;
	return IOSurfaceGetID(surface);
}

void *
ipc_metal_iosurface_from_id(uint32_t surface_id)
{
	if (surface_id == 0) {
		return NULL;
	}

	// IOSurfaceLookup returns a retained IOSurfaceRef.
	// Caller must CFRelease when done.
	IOSurfaceRef surface = IOSurfaceLookup(surface_id);
	return (void *)surface;
}
