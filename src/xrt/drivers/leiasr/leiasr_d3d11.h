// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia SR D3D11 weaver wrapper for native D3D11 compositor.
 * @author David Fattal
 * @ingroup drv_leiasr
 */

#pragma once

#include "xrt/xrt_results.h"
#include "leiasr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Forward declaration for the D3D11 SR weaver holder.
 */
struct leiasr_d3d11;

/*!
 * Create a D3D11 SR weaver instance.
 *
 * The weaver expects a side-by-side stereo texture where:
 * - Left view is in the left half (x: 0 to view_width)
 * - Right view is in the right half (x: view_width to 2*view_width)
 *
 * @param max_time Maximum time in seconds to wait for SR to become ready.
 * @param d3d11_device The D3D11 device (ID3D11Device*).
 * @param d3d11_context The D3D11 immediate context (ID3D11DeviceContext*).
 * @param hwnd Window handle (HWND), NULL for fullscreen.
 * @param view_width Width of one view (half of stereo texture width).
 * @param view_height Height of the views.
 * @param[out] out Pointer to receive the created instance.
 *
 * @return XRT_SUCCESS on success.
 *
 * @ingroup drv_leiasr
 */
xrt_result_t
leiasr_d3d11_create(double max_time,
                    void *d3d11_device,
                    void *d3d11_context,
                    void *hwnd,
                    uint32_t view_width,
                    uint32_t view_height,
                    struct leiasr_d3d11 **out);

/*!
 * Destroy a D3D11 SR weaver instance.
 *
 * @param leiasr The instance to destroy (can be NULL).
 *
 * @ingroup drv_leiasr
 */
void
leiasr_d3d11_destroy(struct leiasr_d3d11 **leiasr_ptr);

/*!
 * Set the input stereo texture for weaving.
 *
 * @param leiasr The D3D11 weaver instance.
 * @param stereo_srv Shader resource view of the side-by-side stereo texture (ID3D11ShaderResourceView*).
 * @param view_width Width of one view.
 * @param view_height Height of the views.
 * @param format DXGI format of the texture.
 *
 * @ingroup drv_leiasr
 */
void
leiasr_d3d11_set_input_texture(struct leiasr_d3d11 *leiasr,
                               void *stereo_srv,
                               uint32_t view_width,
                               uint32_t view_height,
                               uint32_t format);

/*!
 * Perform weaving from the stereo texture to the current render target.
 *
 * Before calling this function, ensure:
 * - The output render target is bound via OMSetRenderTargets
 * - The viewport and scissor rect are set
 * - The input texture has been set via leiasr_d3d11_set_input_texture
 *
 * @param leiasr The D3D11 weaver instance.
 *
 * @ingroup drv_leiasr
 */
void
leiasr_d3d11_weave(struct leiasr_d3d11 *leiasr);

/*!
 * Get predicted eye positions from the weaver's LookaroundFilter.
 *
 * @param leiasr The D3D11 weaver instance.
 * @param[out] out_left_eye Left eye position in meters (x, y, z).
 * @param[out] out_right_eye Right eye position in meters (x, y, z).
 *
 * @return true if valid eye positions are available.
 *
 * @ingroup drv_leiasr
 */
bool
leiasr_d3d11_get_predicted_eye_positions(struct leiasr_d3d11 *leiasr,
                                         float out_left_eye[3],
                                         float out_right_eye[3]);

/*!
 * Configure sRGB conversion in the weaver shader.
 *
 * @param leiasr The D3D11 weaver instance.
 * @param read_srgb Convert from sRGB to linear when reading input.
 * @param write_srgb Convert from linear to sRGB when writing output.
 *
 * @ingroup drv_leiasr
 */
void
leiasr_d3d11_set_srgb_conversion(struct leiasr_d3d11 *leiasr,
                                 bool read_srgb,
                                 bool write_srgb);

/*!
 * Set the weaver latency in frames.
 *
 * @param leiasr The D3D11 weaver instance.
 * @param latency_frames Number of frames of latency to predict for.
 *
 * @ingroup drv_leiasr
 */
void
leiasr_d3d11_set_latency_in_frames(struct leiasr_d3d11 *leiasr,
                                   uint64_t latency_frames);

/*!
 * Check if the weaver is ready to use.
 *
 * @param leiasr The D3D11 weaver instance.
 * @return true if the weaver is ready.
 *
 * @ingroup drv_leiasr
 */
bool
leiasr_d3d11_is_ready(struct leiasr_d3d11 *leiasr);

/*!
 * Get the display dimensions for Kooima FOV calculation.
 * The dimensions are cached from SR::Display during initialization.
 *
 * @param leiasr The D3D11 weaver instance.
 * @param[out] out_dims Pointer to receive the display dimensions (in meters).
 * @return true if valid display dimensions are available, false otherwise.
 *
 * @ingroup drv_leiasr
 */
bool
leiasr_d3d11_get_display_dimensions(struct leiasr_d3d11 *leiasr, struct leiasr_display_dimensions *out_dims);

/*!
 * Get display pixel resolution, screen position, and physical size.
 * Used for computing window metrics (adaptive FOV and eye offset).
 *
 * @param leiasr The D3D11 weaver instance.
 * @param[out] out_display_pixel_width Display width in pixels.
 * @param[out] out_display_pixel_height Display height in pixels.
 * @param[out] out_display_screen_left Display left edge in screen coords.
 * @param[out] out_display_screen_top Display top edge in screen coords.
 * @param[out] out_display_width_m Display physical width in meters.
 * @param[out] out_display_height_m Display physical height in meters.
 * @return true if all values are valid.
 *
 * @ingroup drv_leiasr
 */
bool
leiasr_d3d11_get_display_pixel_info(struct leiasr_d3d11 *leiasr,
                                     uint32_t *out_display_pixel_width,
                                     uint32_t *out_display_pixel_height,
                                     int32_t *out_display_screen_left,
                                     int32_t *out_display_screen_top,
                                     float *out_display_width_m,
                                     float *out_display_height_m);

/*!
 * Get the recommended view texture dimensions from the SR display.
 * These dimensions are queried from the SR SDK during weaver creation and should
 * be used for creating swapchains and the compositor stereo texture.
 *
 * @param leiasr The D3D11 weaver instance.
 * @param[out] out_width Recommended width per view (single eye).
 * @param[out] out_height Recommended height per view.
 * @return true if valid dimensions are available, false otherwise.
 *
 * @ingroup drv_leiasr
 */
bool
leiasr_d3d11_get_recommended_view_dimensions(struct leiasr_d3d11 *leiasr,
                                              uint32_t *out_width,
                                              uint32_t *out_height);

/*!
 * Query recommended view texture dimensions and display refresh rate from SR
 * display without creating a weaver.
 *
 * This is a standalone function that can be called early during initialization
 * to get the recommended dimensions before creating swapchains. It creates a
 * temporary SR context, queries the display, and cleans up.
 *
 * @param max_time Maximum time in seconds to wait for SR to become ready.
 * @param[out] out_width Recommended width per view (single eye).
 * @param[out] out_height Recommended height per view.
 * @param[out] out_refresh_rate_hz Display refresh rate in Hz (NULL to skip).
 * @param[out] out_native_width Native display width in pixels (NULL to skip).
 * @param[out] out_native_height Native display height in pixels (NULL to skip).
 * @return true if valid dimensions were obtained, false otherwise.
 *
 * @ingroup drv_leiasr
 */
bool
leiasr_query_recommended_view_dimensions(double max_time,
                                          uint32_t *out_width,
                                          uint32_t *out_height,
                                          float *out_refresh_rate_hz,
                                          uint32_t *out_native_width,
                                          uint32_t *out_native_height);

#ifdef __cplusplus
}
#endif
