// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-owned window region for shaped transparent windows, applied
 *         right after the present it was cut from (Windows, DComp readback route).
 *
 * A shaped transparent app (`dxr::ClickThroughRegion`, the Unity provider)
 * calls SetWindowRgn from its render thread with a region cut from an EARLIER
 * frame (the coverage readback is pipelined one update), and DWM applies a
 * region at its next composition while the woven frame reaches the glass one
 * or two vsyncs after its Present. Region and pixels therefore drift apart by
 * up to an app frame in either direction, and a region that is behind or ahead
 * of the pixels on glass DELETES opaque content: the silhouette visibly leads
 * or trails the object.
 *
 * Under live (DComp, premultiplied) presentation the region changes nothing
 * the user sees where the frame is transparent — alpha 0 already shows the
 * live desktop. Its only visible effect is clipping opaque pixels, which it
 * must never do. So the runtime takes the region over, per session window:
 *
 *  - the app's SetWindowRgn(hwnd, non-NULL) is swallowed (an import-table hook
 *    on user32!SetWindowRgn, scoped to registered windows only — every other
 *    window passes through untouched); SetWindowRgn(hwnd, NULL) still
 *    unshapes and hands ownership back, so leaving transparent mode, framed
 *    modes and style changes behave exactly as before;
 *  - after each presented app frame, the present worker cuts the region from
 *    the exact premultiplied bytes it just handed DWM (alpha != 0, 4-px
 *    texels, 1-texel dilation), unions the last few presented frames so a
 *    frame still on glass is never clipped by a region cut for a newer one,
 *    and applies it on a helper thread (never on a thread the frame path
 *    waits for), only when it changed.
 *
 * Shaping starts only after the app asked for it (its first non-NULL
 * SetWindowRgn), so unshaped apps are unaffected. DXR_VK_RUNTIME_REGION=0
 * disables the whole mechanism (the app's own SetWindowRgn calls go through).
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_vk_region_owner;

/*!
 * Register @p hwnd for runtime-owned shaping. Returns NULL when disabled
 * (DXR_VK_RUNTIME_REGION=0) or the hook could not be installed.
 */
struct comp_vk_region_owner *
comp_vk_region_owner_create(void *hwnd);

/*!
 * Present worker, right after the Present of a frame: @p bgra is the
 * premultiplied BGRA8 frame just presented (tightly packed, @p w x @p h).
 * @p is_repaint frames carry the previous app frame's footprint and are
 * skipped. Cheap when the app never shaped the window.
 */
void
comp_vk_region_owner_on_present(
    struct comp_vk_region_owner *o, const uint8_t *bgra, uint32_t w, uint32_t h, bool is_repaint);

/*! Unregister and stop. Safe with NULL. */
void
comp_vk_region_owner_destroy(struct comp_vk_region_owner **o);

#ifdef __cplusplus
}
#endif
