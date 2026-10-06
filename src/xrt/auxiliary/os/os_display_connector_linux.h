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

#ifdef __cplusplus
}
#endif
