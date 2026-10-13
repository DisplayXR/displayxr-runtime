// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host D3D11 lane (Windows): the host's own
 *         device on the adapter LUID the service passed, and the
 *         runtime-allocated output images (NT-handle textures + one shared
 *         fence) the service pulls with DuplicateHandle (roadmap §3.5).
 *
 * Never the default adapter: on a hybrid box the service renders on one GPU
 * and weaves on the other, and a texture shared across adapters can fail
 * silently. A LUID with no matching adapter, or an adapter that cannot create
 * a feature-level-11 device with ID3D11Device5 (shared fences), is "no usable
 * device" (exit 13).
 *
 * @ingroup conv_host
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct conv_host_d3d11;

/*!
 * Create the device on @p luid. @p out_reason (optional) says why not.
 * @return NULL when there is no usable device.
 */
struct conv_host_d3d11 *
conv_host_d3d11_create(uint64_t luid, char *out_reason, uint32_t reason_size);

void
conv_host_d3d11_destroy(struct conv_host_d3d11 **d_ptr);

//! The adapter's description (UTF-8), for the log.
const char *
conv_host_d3d11_adapter_name(const struct conv_host_d3d11 *d);

/*!
 * One output image: a shareable 2D texture (NT handle, no keyed mutex: the
 * service and the host sync with fence values only).
 */
struct conv_host_d3d11_image
{
	void *texture;      //!< ID3D11Texture2D*
	uint64_t nt_handle; //!< HANDLE the service duplicates into itself
	uint32_t width, height, array_size, dxgi_format;
};

/*!
 * Allocate an output image. @p dxgi_format is a DXGI_FORMAT value. The host
 * owns it until @ref conv_host_d3d11_free_image.
 */
bool
conv_host_d3d11_alloc_image(struct conv_host_d3d11 *d,
                            uint32_t width,
                            uint32_t height,
                            uint32_t array_size,
                            uint32_t dxgi_format,
                            struct conv_host_d3d11_image *out);

void
conv_host_d3d11_free_image(struct conv_host_d3d11 *d, struct conv_host_d3d11_image *img);

/*!
 * The host's shared output fence (created on first use): its NT handle, which
 * the service duplicates once, and the next value to signal. Completion
 * records carry the value; the service observes it from the CPU.
 */
bool
conv_host_d3d11_fence(struct conv_host_d3d11 *d, uint64_t *out_nt_handle);

/*!
 * Reserve the next output-fence value (strictly increasing). The provider
 * signals it as the output images' release_value; if the convert fails, the
 * host signals it itself (@ref conv_host_d3d11_signal_value) so the service's
 * CPU observe never waits for a value nobody will write. 0 = no fence.
 */
uint64_t
conv_host_d3d11_reserve(struct conv_host_d3d11 *d);

//! Signal @p value on the output fence from the host's immediate context.
bool
conv_host_d3d11_signal_value(struct conv_host_d3d11 *d, uint64_t value);

//! The output fence's completed value (tests / diagnostics).
uint64_t
conv_host_d3d11_completed(struct conv_host_d3d11 *d);

#ifdef __cplusplus
}
#endif
