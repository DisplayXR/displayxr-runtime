// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for @ref xrt_display_processor_d3d12 interface.
 *
 * D3D12 variant of the display processor abstraction for vendor-specific
 * atlas-to-display output processing (interlacing, SBS, anaglyph, etc.).
 *
 * Unlike the D3D11 variant, this interface operates on D3D12 resources:
 * - Input is an atlas texture SRV (GPU descriptor handle)
 * - Output goes to a render target (CPU descriptor handle for RTV)
 * - Commands are recorded onto a provided command list (deferred execution)
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations for types used by optional vtable methods.
struct xrt_eye_positions;
struct xrt_window_metrics;

/*!
 * @interface xrt_display_processor_d3d12
 *
 * D3D12 display output processor that converts an atlas
 * texture into the final display output format.
 *
 * The compositor calls process_atlas() after rendering the view
 * pair into an atlas texture. The display processor records draw commands
 * onto the provided command list to write the final output.
 *
 * @ingroup xrt_iface
 */
struct xrt_display_processor_d3d12
{
	/*!
	 * Process an atlas texture into the final display output.
	 *
	 * Records draw commands onto the provided command list. The caller
	 * is responsible for executing the command list and synchronization.
	 *
	 * @param      xdp                   Pointer to self.
	 * @param      d3d12_command_list     Command list (ID3D12GraphicsCommandList*).
	 * @param      atlas_texture_resource Stereo texture resource (ID3D12Resource*), may be NULL.
	 * @param      atlas_srv_gpu_handle  Atlas texture SRV (D3D12_GPU_DESCRIPTOR_HANDLE as uint64_t).
	 * @param      target_rtv_cpu_handle  Output render target RTV (D3D12_CPU_DESCRIPTOR_HANDLE as uint64_t).
	 * @param      view_width            Width of one eye view in pixels.
	 * @param      view_height           Height of one eye view in pixels.
	 * @param      tile_columns          Number of tile columns in the atlas layout.
	 * @param      tile_rows             Number of tile rows in the atlas layout.
	 * @param      format                DXGI format of the atlas texture (DXGI_FORMAT as uint32_t).
	 * @param      target_width          Width of the output render target in pixels.
	 * @param      target_height         Height of the output render target in pixels.
	 */
	void (*process_atlas)(struct xrt_display_processor_d3d12 *xdp,
	                       void *d3d12_command_list,
	                       void *atlas_texture_resource,
	                       uint64_t atlas_srv_gpu_handle,
	                       uint64_t target_rtv_cpu_handle,
	                       uint32_t view_width,
	                       uint32_t view_height,
	                       uint32_t tile_columns,
	                       uint32_t tile_rows,
	                       uint32_t format,
	                       uint32_t target_width,
	                       uint32_t target_height);

	/*!
	 * Set the output render target format.
	 *
	 * Must be called before the first process_atlas() call so the
	 * display processor can create its internal pipeline state.
	 * Optional — NULL means format is set during creation.
	 *
	 * @param xdp    Pointer to self.
	 * @param format DXGI format of the output render target (DXGI_FORMAT as uint32_t).
	 */
	void (*set_output_format)(struct xrt_display_processor_d3d12 *xdp, uint32_t format);

	/*!
	 * Get predicted eye positions from vendor eye tracking SDK.
	 * Optional — NULL means not supported.
	 */
	bool (*get_predicted_eye_positions)(struct xrt_display_processor_d3d12 *xdp,
	                                    struct xrt_eye_positions *out_eye_pos);

	/*!
	 * Get window metrics for adaptive FOV calculation.
	 * Optional — NULL means not supported.
	 */
	bool (*get_window_metrics)(struct xrt_display_processor_d3d12 *xdp,
	                           struct xrt_window_metrics *out_metrics);

	/*!
	 * Request a display mode switch (2D/3D).
	 * Optional — NULL means not supported.
	 */
	bool (*request_display_mode)(struct xrt_display_processor_d3d12 *xdp,
	                             bool enable_3d);

	/*!
	 * Get physical display dimensions in meters.
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_dimensions)(struct xrt_display_processor_d3d12 *xdp,
	                               float *out_width_m,
	                               float *out_height_m);

	/*!
	 * Get native display pixel info (resolution and screen position).
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_pixel_info)(struct xrt_display_processor_d3d12 *xdp,
	                               uint32_t *out_pixel_width,
	                               uint32_t *out_pixel_height,
	                               int32_t *out_screen_left,
	                               int32_t *out_screen_top);

	/*!
	 * Destroy this display processor and free all resources.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*destroy)(struct xrt_display_processor_d3d12 *xdp);
};

/*!
 * @copydoc xrt_display_processor_d3d12::process_atlas
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_process_atlas(struct xrt_display_processor_d3d12 *xdp,
                                           void *d3d12_command_list,
                                           void *atlas_texture_resource,
                                           uint64_t atlas_srv_gpu_handle,
                                           uint64_t target_rtv_cpu_handle,
                                           uint32_t view_width,
                                           uint32_t view_height,
                                           uint32_t tile_columns,
                                           uint32_t tile_rows,
                                           uint32_t format,
                                           uint32_t target_width,
                                           uint32_t target_height)
{
	xdp->process_atlas(xdp, d3d12_command_list, atlas_texture_resource, atlas_srv_gpu_handle,
	                    target_rtv_cpu_handle, view_width, view_height, tile_columns, tile_rows, format,
	                    target_width, target_height);
}

/*!
 * @copydoc xrt_display_processor_d3d12::set_output_format
 * No-op if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_set_output_format(struct xrt_display_processor_d3d12 *xdp, uint32_t format)
{
	if (xdp != NULL && xdp->set_output_format != NULL) {
		xdp->set_output_format(xdp, format);
	}
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_predicted_eye_positions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_predicted_eye_positions(struct xrt_display_processor_d3d12 *xdp,
                                                        struct xrt_eye_positions *out_eye_pos)
{
	if (xdp == NULL || xdp->get_predicted_eye_positions == NULL) {
		return false;
	}
	return xdp->get_predicted_eye_positions(xdp, out_eye_pos);
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_window_metrics
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_window_metrics(struct xrt_display_processor_d3d12 *xdp,
                                               struct xrt_window_metrics *out_metrics)
{
	if (xdp == NULL || xdp->get_window_metrics == NULL) {
		return false;
	}
	return xdp->get_window_metrics(xdp, out_metrics);
}

/*!
 * @copydoc xrt_display_processor_d3d12::request_display_mode
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_request_display_mode(struct xrt_display_processor_d3d12 *xdp, bool enable_3d)
{
	if (xdp == NULL || xdp->request_display_mode == NULL) {
		return false;
	}
	return xdp->request_display_mode(xdp, enable_3d);
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_display_dimensions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_display_dimensions(struct xrt_display_processor_d3d12 *xdp,
                                                   float *out_width_m,
                                                   float *out_height_m)
{
	if (xdp == NULL || xdp->get_display_dimensions == NULL) {
		return false;
	}
	return xdp->get_display_dimensions(xdp, out_width_m, out_height_m);
}

/*!
 * @copydoc xrt_display_processor_d3d12::get_display_pixel_info
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d12
 */
static inline bool
xrt_display_processor_d3d12_get_display_pixel_info(struct xrt_display_processor_d3d12 *xdp,
                                                   uint32_t *out_pixel_width,
                                                   uint32_t *out_pixel_height,
                                                   int32_t *out_screen_left,
                                                   int32_t *out_screen_top)
{
	if (xdp == NULL || xdp->get_display_pixel_info == NULL) {
		return false;
	}
	return xdp->get_display_pixel_info(xdp, out_pixel_width, out_pixel_height, out_screen_left, out_screen_top);
}

/*!
 * Factory function type for creating a D3D12 display processor.
 *
 * Called by the compositor to create a display processor for a session.
 * The factory is set by the target builder at init time and stored in
 * xrt_system_compositor_info.
 *
 * @param d3d12_device        D3D12 device (ID3D12Device*).
 * @param d3d12_command_queue D3D12 command queue (ID3D12CommandQueue*).
 * @param window_handle       Native window handle (HWND), may be NULL.
 * @param[out] out_xdp        Created display processor on success.
 * @return XRT_SUCCESS on success.
 */
typedef xrt_result_t (*xrt_dp_factory_d3d12_fn_t)(void *d3d12_device,
                                                   void *d3d12_command_queue,
                                                   void *window_handle,
                                                   struct xrt_display_processor_d3d12 **out_xdp);

/*!
 * Destroy an xrt_display_processor_d3d12 — helper function.
 *
 * @param[in,out] xdp_ptr  A pointer to your display processor pointer.
 *
 * Will destroy the processor if *xdp_ptr is not NULL.
 * Will then set *xdp_ptr to NULL.
 *
 * @public @memberof xrt_display_processor_d3d12
 */
static inline void
xrt_display_processor_d3d12_destroy(struct xrt_display_processor_d3d12 **xdp_ptr)
{
	struct xrt_display_processor_d3d12 *xdp = *xdp_ptr;
	if (xdp == NULL) {
		return;
	}

	xdp->destroy(xdp);
	*xdp_ptr = NULL;
}


#ifdef __cplusplus
}
#endif
