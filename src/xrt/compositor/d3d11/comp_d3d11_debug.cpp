// Copyright 2024-2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 debug GUI readback implementation.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_debug.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_sink.h"
#include "util/u_var.h"
#include "util/u_frame.h"
#include "xrt/xrt_frame.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>

#include <cstring>
#include <cstdlib>

/*!
 * D3D11 debug readback structure.
 */
struct comp_d3d11_debug
{
	//! Staging texture for CPU readback.
	ID3D11Texture2D *staging_texture;

	//! Width of the texture.
	uint32_t width;

	//! Height of the texture.
	uint32_t height;

	//! Debug sink for pushing frames to the GUI.
	struct u_sink_debug debug_sink;

	//! CPU-side frame buffer.
	uint8_t *frame_buffer;

	//! Size of the frame buffer.
	size_t frame_buffer_size;

	//! Frame counter for timestamps.
	uint64_t frame_count;

	//! Whether to push frames (controlled by u_var).
	bool push_frames;

	//! Push every N frames (to reduce overhead).
	int32_t push_every_n_frames;
};

extern "C" xrt_result_t
comp_d3d11_debug_create(ID3D11Device *device, uint32_t width, uint32_t height, struct comp_d3d11_debug **out_debug)
{
	struct comp_d3d11_debug *debug = new comp_d3d11_debug();
	memset(debug, 0, sizeof(*debug));

	debug->width = width;
	debug->height = height;
	debug->push_frames = true;
	debug->push_every_n_frames = 2; // Push every 2nd frame by default

	// Initialize the debug sink
	u_sink_debug_init(&debug->debug_sink);

	// Create staging texture for CPU readback
	D3D11_TEXTURE2D_DESC staging_desc = {};
	staging_desc.Width = width;
	staging_desc.Height = height;
	staging_desc.MipLevels = 1;
	staging_desc.ArraySize = 1;
	staging_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	staging_desc.SampleDesc.Count = 1;
	staging_desc.Usage = D3D11_USAGE_STAGING;
	staging_desc.BindFlags = 0;
	staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &debug->staging_texture);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create staging texture for debug readback: 0x%08x", hr);
		delete debug;
		return XRT_ERROR_D3D;
	}

	// Allocate CPU-side frame buffer (RGBX format for xrt_frame)
	debug->frame_buffer_size = static_cast<size_t>(width) * height * 4;
	debug->frame_buffer = static_cast<uint8_t *>(malloc(debug->frame_buffer_size));
	if (debug->frame_buffer == nullptr) {
		U_LOG_E("Failed to allocate frame buffer for debug readback");
		debug->staging_texture->Release();
		delete debug;
		return XRT_ERROR_ALLOCATION;
	}

	U_LOG_I("D3D11 debug readback created: %ux%u", width, height);

	*out_debug = debug;
	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_debug_add_vars(struct comp_d3d11_debug *debug)
{
	if (debug == nullptr) {
		return;
	}

	u_var_add_root(debug, "D3D11 Debug Readback", false);
	u_var_add_bool(debug, &debug->push_frames, "Push frames to debug GUI");
	u_var_add_i32(debug, &debug->push_every_n_frames, "Push every N frames");
	u_var_add_sink_debug(debug, &debug->debug_sink, "Stereo Preview");
}

extern "C" bool
comp_d3d11_debug_is_active(struct comp_d3d11_debug *debug)
{
	if (debug == nullptr) {
		return false;
	}

	return debug->push_frames && u_sink_debug_is_active(&debug->debug_sink);
}

extern "C" void
comp_d3d11_debug_update_preview(struct comp_d3d11_debug *debug,
                                ID3D11DeviceContext *context,
                                ID3D11Texture2D *source_texture)
{
	if (debug == nullptr || context == nullptr || source_texture == nullptr) {
		return;
	}

	// Check if we should push this frame
	if (!debug->push_frames) {
		return;
	}

	if (!u_sink_debug_is_active(&debug->debug_sink)) {
		return;
	}

	debug->frame_count++;

	// Only push every N frames to reduce overhead
	if (debug->push_every_n_frames > 1 && (debug->frame_count % debug->push_every_n_frames) != 0) {
		return;
	}

	// Copy from GPU to staging texture
	context->CopyResource(debug->staging_texture, source_texture);

	// Map the staging texture for CPU read
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = context->Map(debug->staging_texture, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		U_LOG_W("Failed to map staging texture: 0x%08x", hr);
		return;
	}

	// Copy data to our frame buffer (handle row pitch differences)
	const uint8_t *src = static_cast<const uint8_t *>(mapped.pData);
	uint8_t *dst = debug->frame_buffer;
	uint32_t row_size = debug->width * 4;

	if (mapped.RowPitch == row_size) {
		// Fast path: no pitch adjustment needed
		memcpy(dst, src, debug->frame_buffer_size);
	} else {
		// Slow path: copy row by row
		for (uint32_t y = 0; y < debug->height; y++) {
			memcpy(dst + y * row_size, src + y * mapped.RowPitch, row_size);
		}
	}

	context->Unmap(debug->staging_texture, 0);

	// Create an xrt_frame to push to the debug sink
	struct xrt_frame *xf = nullptr;
	u_frame_create_one_off(XRT_FORMAT_R8G8B8X8, debug->width, debug->height, &xf);
	if (xf == nullptr) {
		U_LOG_W("Failed to create xrt_frame for debug preview");
		return;
	}

	// Copy our frame buffer into the xrt_frame
	memcpy(xf->data, debug->frame_buffer, debug->frame_buffer_size);

	// Set frame metadata
	xf->timestamp = static_cast<int64_t>(debug->frame_count);
	xf->source_timestamp = xf->timestamp;
	xf->source_sequence = debug->frame_count;

	// Push to the debug sink
	u_sink_debug_push_frame(&debug->debug_sink, xf);

	// Release our reference
	xrt_frame_reference(&xf, nullptr);
}

extern "C" void
comp_d3d11_debug_destroy(struct comp_d3d11_debug **debug_ptr)
{
	if (debug_ptr == nullptr || *debug_ptr == nullptr) {
		return;
	}

	struct comp_d3d11_debug *debug = *debug_ptr;

	// Remove u_var root
	u_var_remove_root(debug);

	// Destroy the debug sink
	u_sink_debug_destroy(&debug->debug_sink);

	// Free frame buffer
	if (debug->frame_buffer != nullptr) {
		free(debug->frame_buffer);
		debug->frame_buffer = nullptr;
	}

	// Release staging texture
	if (debug->staging_texture != nullptr) {
		debug->staging_texture->Release();
		debug->staging_texture = nullptr;
	}

	delete debug;
	*debug_ptr = nullptr;

	U_LOG_I("D3D11 debug readback destroyed");
}
