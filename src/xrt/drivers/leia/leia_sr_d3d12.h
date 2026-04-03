// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia SR D3D12 weaver wrapper for native D3D12 compositor.
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_results.h"
#include "leia_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Forward declaration for the D3D12 SR weaver holder.
 */
struct leiasr_d3d12;

/*!
 * Create a D3D12 SR weaver instance.
 *
 * The weaver expects a side-by-side atlas texture where:
 * - Left view is in the left half (x: 0 to view_width)
 * - Right view is in the right half (x: view_width to 2*view_width)
 *
 * @param max_time Maximum time in seconds to wait for SR to become ready.
 * @param d3d12_device The D3D12 device (ID3D12Device*).
 * @param d3d12_command_queue The D3D12 command queue (ID3D12CommandQueue*).
 * @param hwnd Window handle (HWND), NULL for fullscreen.
 * @param view_width Width of one view (half of atlas texture width).
 * @param view_height Height of the views.
 * @param[out] out Pointer to receive the created instance.
 *
 * @return XRT_SUCCESS on success.
 *
 * @ingroup drv_leia
 */
xrt_result_t
leiasr_d3d12_create(double max_time,
                    void *d3d12_device,
                    void *d3d12_command_queue,
                    void *hwnd,
                    uint32_t view_width,
                    uint32_t view_height,
                    struct leiasr_d3d12 **out);

/*!
 * Destroy a D3D12 SR weaver instance.
 *
 * @param leiasr_ptr Pointer to the instance pointer (set to NULL on return).
 *
 * @ingroup drv_leia
 */
void
leiasr_d3d12_destroy(struct leiasr_d3d12 **leiasr_ptr);

/*!
 * Set the input atlas texture for weaving.
 *
 * @param leiasr The D3D12 weaver instance.
 * @param stereo_resource The atlas texture resource (ID3D12Resource*).
 * @param view_width Width of one view.
 * @param view_height Height of the views.
 * @param format DXGI format of the texture (DXGI_FORMAT as uint32_t).
 *
 * @ingroup drv_leia
 */
void
leiasr_d3d12_set_input_texture(struct leiasr_d3d12 *leiasr,
                               void *stereo_resource,
                               uint32_t view_width,
                               uint32_t view_height,
                               uint32_t format);

/*!
 * Set the output render target format so the weaver can create its pipeline state.
 *
 * Must be called before the first weave() call, otherwise the weaver's
 * internal pipeline state will be null and weaving will silently no-op.
 *
 * @param leiasr The D3D12 weaver instance.
 * @param format DXGI format of the output render target (DXGI_FORMAT as uint32_t).
 *
 * @ingroup drv_leia
 */
void
leiasr_d3d12_set_output_format(struct leiasr_d3d12 *leiasr, uint32_t format);

/*!
 * Perform weaving from the atlas texture to the render target.
 *
 * Records draw commands onto the provided command list. The viewport
 * parameters define a sub-rect within the render target where weaving
 * occurs. The SR SDK weaver uses viewport_x/y in its phase calculation:
 *   xOffset = window_WeavingX + viewport_x
 *   yOffset = window_WeavingY + viewport_y
 *
 * @param leiasr The D3D12 weaver instance.
 * @param command_list The command list (ID3D12GraphicsCommandList*).
 * @param viewport_x Left edge of the viewport sub-rect within the render target.
 * @param viewport_y Top edge of the viewport sub-rect within the render target.
 * @param viewport_width Width of the viewport sub-rect.
 * @param viewport_height Height of the viewport sub-rect.
 *
 * @ingroup drv_leia
 */
void
leiasr_d3d12_weave(struct leiasr_d3d12 *leiasr,
                   void *command_list,
                   int32_t viewport_x,
                   int32_t viewport_y,
                   uint32_t viewport_width,
                   uint32_t viewport_height);

/*!
 * Get predicted eye positions from the weaver's LookaroundFilter.
 *
 * @param leiasr The D3D12 weaver instance.
 * @param[out] out_left_eye Left eye position in meters (x, y, z).
 * @param[out] out_right_eye Right eye position in meters (x, y, z).
 *
 * @return true if valid eye positions are available.
 *
 * @ingroup drv_leia
 */
bool
leiasr_d3d12_get_predicted_eye_positions(struct leiasr_d3d12 *leiasr,
                                         float out_left_eye[3],
                                         float out_right_eye[3]);

/*!
 * Get the display dimensions for Kooima FOV calculation.
 *
 * @param leiasr The D3D12 weaver instance.
 * @param[out] out_dims Pointer to receive the display dimensions (in meters).
 * @return true if valid display dimensions are available.
 *
 * @ingroup drv_leia
 */
bool
leiasr_d3d12_get_display_dimensions(struct leiasr_d3d12 *leiasr,
                                    struct leiasr_display_dimensions *out_dims);

/*!
 * Get display pixel resolution, screen position, and physical size.
 *
 * @param leiasr The D3D12 weaver instance.
 * @param[out] out_display_pixel_width Display width in pixels.
 * @param[out] out_display_pixel_height Display height in pixels.
 * @param[out] out_display_screen_left Display left edge in screen coords.
 * @param[out] out_display_screen_top Display top edge in screen coords.
 * @param[out] out_display_width_m Display physical width in meters.
 * @param[out] out_display_height_m Display physical height in meters.
 * @return true if all values are valid.
 *
 * @ingroup drv_leia
 */
bool
leiasr_d3d12_get_display_pixel_info(struct leiasr_d3d12 *leiasr,
                                     uint32_t *out_display_pixel_width,
                                     uint32_t *out_display_pixel_height,
                                     int32_t *out_display_screen_left,
                                     int32_t *out_display_screen_top,
                                     float *out_display_width_m,
                                     float *out_display_height_m);

/*!
 * Request display mode switch (2D/3D) via SR SwitchableLensHint.
 *
 * @param leiasr The D3D12 weaver instance.
 * @param enable_3d true to switch to 3D mode, false for 2D mode.
 * @return true on success.
 *
 * @ingroup drv_leia
 */
bool
leiasr_d3d12_request_display_mode(struct leiasr_d3d12 *leiasr, bool enable_3d);

#ifdef __cplusplus
}
#endif
