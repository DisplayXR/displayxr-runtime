// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Derive the OpenXR `grip_surface` pose from a controller's `grip` pose.
 *
 * OpenXR 1.1 promoted `XR_EXT_palm_pose`'s `.../input/palm_ext/pose` to the
 * core `.../input/grip_surface/pose` identifier (also reachable on a 1.0
 * instance through `XR_KHR_maintenance1`). Both name the same thing and the
 * runtime binds both to @ref XRT_INPUT_GENERIC_PALM_POSE.
 *
 * A device with no palm sensor still has to supply that pose: it is a fixed,
 * hand-specific rigid offset from `grip`, not an independent measurement. This
 * is the one place that offset is written down, so every controller-class
 * device derives the same geometry instead of each driver inventing its own.
 *
 * The convention, from the OpenXR standard pose identifiers:
 *
 * - `grip` +X is the palm NORMAL — pointing AWAY from the palm on the left
 *   hand, INTO the palm on the right hand.
 * - `grip_surface` sits on the palm surface rather than inside the held
 *   object, so it is displaced from `grip` along that normal TOWARDS the palm:
 *   -X for the left hand, +X for the right hand.
 * - `grip_surface` +X must be within 10° of `grip` +X. Its -Z is the extended
 *   index finger's pointing direction, which on a physical controller is
 *   typically rotated about X relative to `grip` by however much the handle's
 *   geometry demands.
 *
 * The devices this serves are synthetic — keyboard-driven, scripted or
 * network-fed hands with no handle to model — so there is no geometry to
 * rotate about X and the derived pose keeps the grip orientation. The
 * displacement is a real measurement: half the palm thickness, the same
 * 32 mm caliper figure `u_hand_tracking.c` uses for the palm joint radius.
 *
 * Pure geometry: no device types, no state, no clock.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Distance from the `grip` origin to the palm surface along the palm normal,
 * in metres. Half of the 32 mm palm thickness measured for
 * @ref XRT_HAND_JOINT_PALM.
 *
 * @ingroup aux_util
 */
#define U_GRIP_SURFACE_OFFSET_M 0.016f

/*!
 * The fixed `grip` → `grip_surface` transform, expressed IN the grip frame —
 * i.e. exactly what `xrLocateSpace(grip_surface_space, grip_space, ...)` must
 * report.
 *
 * @param is_left  True for the left hand; the offset mirrors about X.
 * @param out_pose Written with the offset. Ignored when NULL.
 *
 * @ingroup aux_util
 */
void
u_grip_surface_offset(bool is_left, struct xrt_pose *out_pose);

/*!
 * Derive the `grip_surface` relation from a `grip` relation.
 *
 * Relation flags are mirrored verbatim: the derived pose is exactly as valid
 * and exactly as tracked as the grip pose it came from, never more. When both
 * velocity bits are valid the linear velocity gains the rigid lever-arm term
 * (ω × r) so the palm point's velocity is the one it actually has.
 *
 * @param is_left      True for the left hand.
 * @param grip         The device's grip relation. NULL yields a zero relation.
 * @param out_relation Written with the derived relation. Ignored when NULL.
 *                     May alias @p grip.
 *
 * @ingroup aux_util
 */
void
u_grip_surface_from_grip(bool is_left, const struct xrt_space_relation *grip, struct xrt_space_relation *out_relation);

#ifdef __cplusplus
}
#endif
