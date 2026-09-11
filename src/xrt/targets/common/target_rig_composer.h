// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The rig composer: the head device's pose source (#1380, ADR-034
 *         Amendment 4).
 * @ingroup xrt_target_common
 */

#pragma once

#include "xrt/xrt_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_system_devices;

/*!
 * Create the rig composer — a runtime-owned @ref xrt_device that answers
 * @p XRT_INPUT_GENERIC_HEAD_POSE with the rig pose `rig(t)`: the **voluntary**
 * fly-camera pose, un-parallaxed. It is bound as the head device's external
 * pose source through the display plug-in's existing `set_pose_source` hook,
 * in place of the qwerty HMD.
 *
 * While the rig role sits on the qwerty floor (`xrt_system_roles::rig < 0`) the
 * composer is a pass-through: `rig(t)` is exactly the qwerty HMD pose, so a box
 * with no input provider behaves byte-for-byte as it did before. When a
 * provider's @p XRT_DEVICE_TYPE_NAVIGATION device holds the role, the composer
 * owns alignment, continuity across handovers, recenter, and composition; the
 * provider only ever reports an absolute pose in its own navigation frame.
 *
 * Eye-tracked parallax is applied later, at view-pose level, and never reaches
 * this pose — which is what lets ADR-034 Amendment 2 compose provider volumes
 * against it without following parallax.
 *
 * Ownership: none of the arguments are owned; all must outlive the composer.
 * The composer is **not** added to @ref xrt_system_devices::xdevs and is never
 * a role holder itself.
 *
 * @param qwerty_hmd The qwerty HMD (the `UINT32_MAX` rig floor). Its pose at
 *                   this moment seeds `rig_initial` — the recenter "home".
 * @param xsysd      The system devices, read for roles and the navigation
 *                   device. Not stored beyond the pointer.
 * @param head       The head device this composer will drive. Used for logging
 *                   only.
 * @return The composer, or NULL on allocation failure.
 *
 * @ingroup xrt_target_common
 */
struct xrt_device *
t_rig_composer_create(struct xrt_device *qwerty_hmd, struct xrt_system_devices *xsysd, struct xrt_device *head);

/*!
 * Destroy a composer created by @ref t_rig_composer_create and NULL the handle.
 *
 * The caller must guarantee that nothing polls it any more: either the head
 * device is already gone, or its pose-source binding was cleared first with
 * `set_pose_source(inst, head, NULL)`.
 *
 * @ingroup xrt_target_common
 */
void
t_rig_composer_destroy(struct xrt_device **inout_xdev);

#ifdef __cplusplus
}
#endif
