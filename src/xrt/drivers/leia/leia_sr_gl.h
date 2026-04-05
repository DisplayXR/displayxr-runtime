// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia SR OpenGL weaver wrapper for native GL compositor.
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
 * Forward declaration for the GL SR weaver holder.
 */
struct leiasr_gl;

/*!
 * Create a GL SR weaver instance.
 *
 * The weaver expects a side-by-side stereo texture where:
 * - Left view is in the left half (x: 0 to view_width)
 * - Right view is in the right half (x: view_width to 2*view_width)
 *
 * No GL device/context parameters — GL context is thread-local state.
 *
 * @param max_time Maximum time in seconds to wait for SR to become ready.
 * @param hwnd Window handle (HWND), NULL for fullscreen.
 * @param view_width Width of one view (half of stereo texture width).
 * @param view_height Height of the views.
 * @param[out] out Pointer to receive the created instance.
 *
 * @return XRT_SUCCESS on success.
 *
 * @ingroup drv_leia
 */
xrt_result_t
leiasr_gl_create(double max_time,
                  void *hwnd,
                  uint32_t view_width,
                  uint32_t view_height,
                  struct leiasr_gl **out);

/*!
 * Destroy a GL SR weaver instance.
 *
 * @param leiasr_ptr Pointer to instance pointer (can be NULL).
 *
 * @ingroup drv_leia
 */
void
leiasr_gl_destroy(struct leiasr_gl **leiasr_ptr);

/*!
 * Set the input stereo texture for weaving.
 *
 * @param leiasr The GL weaver instance.
 * @param stereo_texture GL texture name of the side-by-side stereo texture (GLuint).
 * @param view_width Width of one view.
 * @param view_height Height of the views.
 * @param format GL internal format of the texture (GLenum as uint32_t).
 *
 * @ingroup drv_leia
 */
void
leiasr_gl_set_input_texture(struct leiasr_gl *leiasr,
                             uint32_t stereo_texture,
                             uint32_t view_width,
                             uint32_t view_height,
                             uint32_t format);

/*!
 * Perform weaving from the stereo texture to the currently bound framebuffer.
 *
 * Before calling this function, ensure:
 * - The output framebuffer is bound (FBO 0 for window output)
 * - The viewport is set appropriately
 * - The input texture has been set via leiasr_gl_set_input_texture
 *
 * @param leiasr The GL weaver instance.
 *
 * @ingroup drv_leia
 */
void
leiasr_gl_weave(struct leiasr_gl *leiasr);

/*!
 * Get predicted eye positions from the weaver's LookaroundFilter.
 *
 * @param leiasr The GL weaver instance.
 * @param[out] out_left_eye Left eye position in meters (x, y, z).
 * @param[out] out_right_eye Right eye position in meters (x, y, z).
 *
 * @return true if valid eye positions are available.
 *
 * @ingroup drv_leia
 */
bool
leiasr_gl_get_predicted_eye_positions(struct leiasr_gl *leiasr,
                                       float out_left_eye[3],
                                       float out_right_eye[3]);

/*!
 * Get the display dimensions for Kooima FOV calculation.
 * The dimensions are cached from SR::Display during initialization.
 *
 * @param leiasr The GL weaver instance.
 * @param[out] out_dims Pointer to receive the display dimensions (in meters).
 * @return true if valid display dimensions are available, false otherwise.
 *
 * @ingroup drv_leia
 */
bool
leiasr_gl_get_display_dimensions(struct leiasr_gl *leiasr, struct leiasr_display_dimensions *out_dims);

/*!
 * Get display pixel resolution, screen position, and physical size.
 *
 * @param leiasr The GL weaver instance.
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
leiasr_gl_get_display_pixel_info(struct leiasr_gl *leiasr,
                                  uint32_t *out_display_pixel_width,
                                  uint32_t *out_display_pixel_height,
                                  int32_t *out_display_screen_left,
                                  int32_t *out_display_screen_top,
                                  float *out_display_width_m,
                                  float *out_display_height_m);

/*!
 * Request display mode switch (2D/3D) via SR SwitchableLensHint.
 *
 * @param leiasr The GL weaver instance.
 * @param enable_3d true to switch to 3D mode, false for 2D mode.
 * @return true on success.
 *
 * @ingroup drv_leia
 */
bool
leiasr_gl_request_display_mode(struct leiasr_gl *leiasr, bool enable_3d);

/*!
 * Query hardware 3D display state from SR SwitchableLensHint.
 *
 * @param leiasr The GL weaver instance.
 * @param[out] out_is_3d true if lens is currently enabled (3D mode).
 * @return true if query succeeded (SwitchableLensHint available).
 *
 * @ingroup drv_leia
 */
bool
leiasr_gl_get_hardware_3d_state(struct leiasr_gl *leiasr, bool *out_is_3d);

#ifdef __cplusplus
}
#endif
