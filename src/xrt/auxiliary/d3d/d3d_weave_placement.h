// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The ONE canonical `weave placement:` line, for every graphics API.
 * @ingroup aux_d3d
 *
 * ADR-037 §1 says render and weave may land on different adapters, and §3 says
 * every degradation off that rule must be logged with a reason. Since #918
 * Phase 3 made the split the DEFAULT, those two sentences became a support
 * contract: "send me the `weave placement:` line" has to be a complete answer
 * on any box, under any graphics API, in any regime.
 *
 * It could not be, while five compositors each formatted their own version of
 * the line and two of them (Vulkan, OpenGL) emitted nothing at all. So the line
 * lives here, once, and each compositor supplies only what it alone knows: its
 * render adapter, the panel rect, and whether its split engaged.
 *
 * Windows only — it is the only platform with more than one adapter under a
 * DisplayXR panel. Callers guard with `XRT_OS_WINDOWS`.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Emit exactly one `weave placement:` WARN naming the render adapter, the
 * scanout adapter, whether they differ, and the resulting regime.
 *
 * The regime is DERIVED here, from the two adapters, so a caller cannot report
 * a placement it did not get:
 *
 * - scanout unresolvable → `split=0 reason=scanout_unresolvable`
 * - render == scanout → `split=0 reason=same_adapter` (not a failure — the rule
 *   degenerates on a MUX'd / single-GPU box)
 * - render != scanout, @p split_active → `split=1`
 * - render != scanout, not active → `split=0 reason=<@p short_reason>`, which is
 *   the ADR-037 §3 rung-2 degradation and the only case where the caller's
 *   reason is printed.
 *
 * Costs one `QueryDisplayConfig` plus one DXGI enumeration, once per session.
 *
 * @param render_packed_luid The adapter the caller renders on, packed the way
 *        `VkPhysicalDeviceIDProperties::deviceLUID` and
 *        @ref d3d_scanout_adapter_luid pack it (raw LUID bytes, LowPart first).
 *        0 when the caller genuinely cannot know — OpenGL exposes no adapter
 *        identity — and the line then says `render=UNKNOWN` rather than guessing.
 * @param panel_screen_left Panel left edge in OS virtual-screen coordinates.
 * @param panel_screen_top Panel top edge in OS virtual-screen coordinates.
 * @param panel_pixel_width Panel width in physical pixels (0 = unknown).
 * @param panel_pixel_height Panel height in physical pixels (0 = unknown).
 * @param split_active Whether the caller's output-device split actually engaged.
 * @param short_reason One of the `COMP_SPLIT_REASON_*` tokens
 *        (comp_split_gate.h) when @p split_active is false. NULL prints
 *        `unknown`, which is a bug in the caller, not a state.
 */
void
d3d_log_weave_placement(uint64_t render_packed_luid,
                        int32_t panel_screen_left,
                        int32_t panel_screen_top,
                        uint32_t panel_pixel_width,
                        uint32_t panel_pixel_height,
                        bool split_active,
                        const char *short_reason);

#ifdef __cplusplus
}
#endif
