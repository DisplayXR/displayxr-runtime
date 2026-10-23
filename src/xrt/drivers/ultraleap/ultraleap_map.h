// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Ultraleap → display-plane coordinate mapping + its settings.
 *
 * Pure functions with no LeapC, OS or device dependency, so the mapping the
 * provider's devices publish is unit-tested without a Leap
 * (`tests/tests_ultraleap_map.cpp`) and compiled on boxes that have no SDK.
 *
 * **Output frame:** the display plane, `XRT_TRACKING_TYPE_RIG_LOCAL`
 * (input-provider-discovery.md §4b.4) — origin at the display centre, +X
 * right, +Y up, +Z toward the viewer, metres. Leap's DESKTOP frame (device on
 * the desk, facing up) is right-handed with +X right, +Y up and +Z toward the
 * user, i.e. the same axes, so the frame change is axis-identity: rotations
 * pass through unchanged and only positions are mapped.
 *
 * **Two mappings** (`MAPPING` setting):
 *
 * - `relative` (default): `p = H·anchor_h + H·gain_h_per_m · leap_m`, with
 *   `H` the physical display height in metres. Anchor and gain are in DISPLAY
 *   HEIGHTS, so the hands feel the same on any panel size and nobody has to
 *   measure where the Leap sits. The defaults are the reference viewer app's
 *   scene (display rig virtual height 1.2 units, Leap root at y = -0.75 with
 *   scale 3): Leap origin on the display plane 0.625 H below the display
 *   centre (just under the bottom edge), 2.5 H per metre of hand motion — so a
 *   hand held 25 cm above the device lands at the display centre. Joint radii (and linear velocities) scale by
 *   the same gain, so the hand is drawn at the scene's scale.
 * - `physical`: 1:1 metres, `p = mount_m + leap_m`, for a user who measured
 *   where the device sits relative to the display centre.
 *
 * @ingroup drv_ultraleap
 */

#pragma once

#include "xrt/xrt_component_settings.h"
#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The provider's id as registered (== iface->id), what every setting lookup is keyed on.
#define UL_PROVIDER_ID "ultraleap"

//! Relative-mapping defaults (the reference viewer app's scene, in display heights).
#define UL_MAP_DEFAULT_ANCHOR_X_H 0.0f
#define UL_MAP_DEFAULT_ANCHOR_Y_H -0.625f
#define UL_MAP_DEFAULT_ANCHOR_Z_H 0.0f
#define UL_MAP_DEFAULT_GAIN_H_PER_M 2.5f

//! Physical-mapping defaults: device on the desk 15 cm below the display centre, 10 cm in front of the glass.
#define UL_MAP_DEFAULT_MOUNT_X_M 0.0f
#define UL_MAP_DEFAULT_MOUNT_Y_M -0.15f
#define UL_MAP_DEFAULT_MOUNT_Z_M 0.10f

/*!
 * Display height used when the runtime cannot say (host iface predates
 * `get_display_geometry`, or no display plug-in published geometry): a
 * 15.6" 16:9 panel, the most common 3D-display size.
 */
#define UL_MAP_FALLBACK_DISPLAY_HEIGHT_M 0.194f

enum ul_mapping_mode
{
	UL_MAPPING_RELATIVE = 0,
	UL_MAPPING_PHYSICAL = 1,
};

/*!
 * The user-facing mapping configuration, as settings describe it. Also the
 * live u_var tunables — @ref ul_map_resolve turns it into a @ref ul_map.
 */
struct ul_map_config
{
	enum ul_mapping_mode mode;

	//! Relative: where the Leap origin sits, in display heights (display-plane frame).
	struct xrt_vec3 anchor_h;

	//! Relative: display heights of motion per metre of Leap motion.
	float gain_h_per_m;

	//! Physical: where the Leap origin sits relative to the display centre, metres.
	struct xrt_vec3 mount_m;
};

/*!
 * A resolved mapping: `p = offset_m + scale · leap_m`. `scale` is also the
 * factor on radii and linear velocities (1 in physical mode).
 */
struct ul_map
{
	struct xrt_vec3 offset_m;
	float scale;
};

//! The compiled defaults (relative mapping, reference-viewer anchor + gain).
void
ul_map_config_defaults(struct ul_map_config *out);

/*!
 * Read the settings (ADR-054 decision 7) through @p get_setting — the host
 * iface's slot, keyed on @ref UL_PROVIDER_ID — over the defaults. A NULL
 * getter leaves the defaults. Unparseable values keep their default; a
 * non-positive gain keeps the default gain.
 */
void
ul_map_config_from_settings(xrt_component_get_setting_fn_t get_setting, struct ul_map_config *out);

/*!
 * Resolve @p cfg against the physical display height. @p display_height_m
 * <= 0 (unknown) uses @ref UL_MAP_FALLBACK_DISPLAY_HEIGHT_M.
 */
void
ul_map_resolve(const struct ul_map_config *cfg, float display_height_m, struct ul_map *out);

//! A Leap position (millimetres, desktop frame) → display-plane metres.
struct xrt_vec3
ul_map_position_mm(const struct ul_map *map, float x_mm, float y_mm, float z_mm);

//! A Leap linear velocity (mm/s) → display-plane m/s (scaled, never offset).
struct xrt_vec3
ul_map_velocity_mm_s(const struct ul_map *map, float x_mm_s, float y_mm_s, float z_mm_s);

//! A Leap bone width (millimetres) → joint radius in display-plane metres.
float
ul_map_radius_from_width_mm(const struct ul_map *map, float width_mm);

//! A Leap orientation → display-plane orientation (axis-identity: passes through).
struct xrt_quat
ul_map_orientation(float x, float y, float z, float w);

//! The `describe_settings` slot: this provider's declared keys.
uint32_t
ul_map_describe_settings(const struct xrt_component_setting_desc **out_descs);

#ifdef __cplusplus
}
#endif
