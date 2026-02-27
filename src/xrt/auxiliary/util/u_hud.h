// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime HUD overlay for diagnostic display.
 *
 * Renders diagnostic info (FPS, eye positions, display mode, etc.) as a
 * bitmap text overlay. The CPU renders to an RGBA pixel buffer which the
 * compositor uploads and blits post-weave for crisp, readable text.
 *
 * Toggle with TAB key. Only active for runtime-owned windows.
 *
 * @author David Fattal
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Toggle HUD visibility (thread-safe for single bool).
 * @ingroup aux_util
 */
void
u_hud_toggle(void);

/*!
 * Check if HUD is currently visible.
 * @ingroup aux_util
 */
bool
u_hud_is_visible(void);

/*!
 * Diagnostic data to display on the HUD.
 * Filled by the compositor from its current state.
 * @ingroup aux_util
 */
struct u_hud_data
{
	const char *device_name;                       //!< Device identifier (e.g. "Sim 3D Display")
	float fps;
	float frame_time_ms;
	bool mode_3d;
	const char *output_mode;                       //!< "Weaved", "Fallback", "SBS", etc.
	uint32_t render_width;
	uint32_t render_height;
	uint32_t window_width;
	uint32_t window_height;
	float display_width_mm;
	float display_height_mm;
	float nominal_x, nominal_y, nominal_z;         //!< Display nominal position (mm)
	float left_eye_x, left_eye_y, left_eye_z;      //!< Left eye position (mm)
	float right_eye_x, right_eye_y, right_eye_z;   //!< Right eye position (mm)
	bool eye_tracking_active;
	float zoom_scale;                               //!< Zoom factor (1.0 = no zoom)
	float vdisp_x, vdisp_y, vdisp_z;               //!< Virtual display position (m)
	float forward_x, forward_y, forward_z;         //!< Head forward direction

	// Stereo controls (dual camera/display state)
	bool camera_mode;                //!< true=camera, false=display
	float cam_ipd_factor;            //!< Camera: IPD factor [0.01,1]
	float cam_parallax_factor;       //!< Camera: parallax factor [0.01,1]
	float cam_convergence;           //!< Camera: convergence in diopters [0,2]
	float cam_half_tan_vfov;         //!< Camera: half_tan_vfov (derived)
	float disp_ipd_factor;           //!< Display: IPD factor [0.01,1]
	float disp_parallax_factor;      //!< Display: parallax factor [0.01,1]
	float disp_vHeight;              //!< Display: virtual display height (m) [0.1,10]
	float nominal_viewer_z;          //!< Hardware: nominal viewer distance (m)
	float screen_height_m;           //!< Hardware: screen height (m)
};

struct u_hud;

/*!
 * Create a HUD renderer.
 *
 * @param out_hud Receives the created HUD.
 * @param target_width Display/swapchain width in physical pixels.
 *        Used to compute integer scale factor (1x at <=1920, 2x at >1920).
 * @return true on success.
 * @ingroup aux_util
 */
bool
u_hud_create(struct u_hud **out_hud, uint32_t target_width);

/*!
 * Destroy a HUD renderer.
 * @ingroup aux_util
 */
void
u_hud_destroy(struct u_hud **hud_ptr);

/*!
 * Update the HUD with new diagnostic data.
 * Rate-limited to ~2Hz internally.
 *
 * @param hud The HUD.
 * @param data Current diagnostic data.
 * @return true if pixels changed (caller should re-upload to GPU).
 * @ingroup aux_util
 */
bool
u_hud_update(struct u_hud *hud, const struct u_hud_data *data);

/*!
 * Get the RGBA pixel buffer (4 bytes per pixel, row-major).
 * @ingroup aux_util
 */
const uint8_t *
u_hud_get_pixels(struct u_hud *hud);

/*!
 * Get pixel buffer width.
 * @ingroup aux_util
 */
uint32_t
u_hud_get_width(struct u_hud *hud);

/*!
 * Get pixel buffer height.
 * @ingroup aux_util
 */
uint32_t
u_hud_get_height(struct u_hud *hud);

#ifdef __cplusplus
}
#endif
