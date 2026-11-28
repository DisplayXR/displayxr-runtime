// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop Linux, aux_os-internal: the DEVICE mode behind each X11
 *         monitor, from the compositor (Mutter DisplayConfig) or the kernel
 *         (DRM sysfs).
 * @ingroup aux_os
 *
 * X11/RandR reports a monitor's rect in X11 pixels. Under XWayland that is not
 * the device mode: with Mutter's `xwayland-native-scaling` a 3840x2160 output at
 * 150 % is a 5120x2880 X11 rect (device x ceil(scale) / scale). Anything that
 * must know what the HARDWARE is running — identifying the 3D panel, deciding
 * whether an X11 window reaches it 1:1 — needs the device mode, keyed by the
 * connector name both sides share. This file supplies it (#1831).
 *
 * Not a public aux_os interface: the result lands in
 * `os_display_desktop_info::native_width/native_height/scale`, which is what
 * every consumer reads.
 */

#pragma once

#include "os_display_desktop.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * "HDMI-A-1" -> "HDMI-1", "DP-2" -> "DP-2". The kernel names a connector with
 * a one-letter subtype segment Mutter and XWayland's RandR drop; this removes
 * it so the two can be compared.
 */
void
os_display_connector_normalise(const char *in, char *out, size_t out_size);

/*!
 * Fill `native_width`, `native_height`, `scale` and `native_source` on each
 * monitor from the best source available, matched by connector name:
 *
 * 1. Mutter `org.gnome.Mutter.DisplayConfig.GetCurrentState` on the session
 *    bus — the CURRENT mode and the logical monitor's scale. libdbus is
 *    dlopen'd (aux_os no-DSO rule) and the call is bounded; a non-GNOME
 *    session simply has no answer.
 * 2. `/sys/class/drm/card*-<connector>/modes` — the mode LIST, so the current
 *    one is inferred: the X11 size when it is itself a mode, else the
 *    preferred (first) mode. No scale.
 *
 * Monitors neither source knows keep 0, which every consumer treats as
 * "unknown — keep current behaviour".
 */
void
os_display_connector_annotate(struct os_display_desktop_info *mons, uint32_t count);

/*!
 * Infer a connector's current mode from the kernel's mode list (DRM sysfs
 * `modes`, scanout orientation, preferred first) and the monitor's rect in
 * the window-placement space, which is already turned the way the desktop
 * presents the output. In order:
 *
 * 1. the rect is itself a mode: that mode, not turned;
 * 2. the rect transposed is a mode: that mode, turned a quarter (the Steam
 *    Deck: an 800x1280 mode under a 1280x800 desktop);
 * 3. else the preferred (first) mode, turned a quarter when it and the rect
 *    disagree about portrait vs landscape (scaling keeps the aspect, so only
 *    a rotation flips it).
 *
 * @p out_w / @p out_h are in the PRESENTED orientation (swapped when
 * @p out_quarter_turn is set), as @ref os_display_desktop_info::native_width
 * requires. A rect of 0 x 0 (unknown) never infers a turn. Returns false, and
 * writes nothing, when the list is empty.
 */
bool
os_display_drm_pick_mode(const uint32_t *mode_w,
                         const uint32_t *mode_h,
                         uint32_t mode_count,
                         uint32_t rect_w,
                         uint32_t rect_h,
                         uint32_t *out_w,
                         uint32_t *out_h,
                         bool *out_quarter_turn);

/*!
 * Map Mutter's logical-monitor transform (MetaMonitorTransform: 0 normal,
 * 1/2/3 = 90/180/270, 4-7 the same flipped) to a rotation. Anything else is
 * @ref OS_DISPLAY_ROTATION_UNKNOWN.
 */
enum os_display_rotation
os_display_rotation_from_mutter(uint32_t transform);

#ifdef __cplusplus
}
#endif
