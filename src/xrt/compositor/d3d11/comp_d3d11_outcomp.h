// Copyright 2024-2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 OUTPUT composite: the masked 2D-over-3D pass on an explicit device.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations (C++ structs)
struct comp_d3d11_outcomp;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Composite modes for @ref comp_d3d11_outcomp_composite_2d_masked. See its doc
 * comment for the per-mode blend.
 */
#define COMP_D3D11_COMPOSITE_MODE_LERP 0u
#define COMP_D3D11_COMPOSITE_MODE_ALPHA_OVER 1u
#define COMP_D3D11_COMPOSITE_MODE_ZONES 2u

/*!
 * Create the output composite unit on an EXPLICIT device (#918 Phase 2a).
 *
 * The unit owns the masked-composite shaders, its constant buffer, the point
 * sampler, and the opaque blend / rasterizer / depth-stencil states — every one
 * of them a device-scoped object, which is the whole reason this is a unit and
 * not a renderer method: the composite's destination is the WEAVE TARGET, and
 * under the output-device split that target lives on the scanout device while
 * the renderer stays on the app's.
 *
 * The device/context are passed explicitly and are NOT owned (no
 * AddRef/Release) — the caller outlives the unit, exactly as for
 * @ref comp_d3d11_target_create.
 *
 * @param device The ID3D11Device* every resource here is created on.
 * @param context The ID3D11DeviceContext* every draw/copy here is recorded on.
 * @param out_outcomp Pointer to receive the created unit.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_outcomp_create(void *device, void *context, struct comp_d3d11_outcomp **out_outcomp);

/*!
 * Destroy the unit and everything it owns (including the weave scratch).
 * Idempotent; nulls the caller's pointer.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_outcomp_destroy(struct comp_d3d11_outcomp **outcomp_ptr);

/*!
 * (Re)allocate the unit-owned weave scratch to @p w × @p h @p dxgi_format
 * (no-op when it already matches).
 *
 * The scratch is the SRV-capable snapshot of the weave target's window region
 * that the authored-mask lerp reads (the target itself is RTV-only), so it is
 * allocated on the SAME device as the composite — see
 * @ref comp_d3d11_outcomp_snapshot_weave.
 *
 * Separate from the snapshot so a caller can do its own allocation-failure
 * bail before any work that depends on the scratch existing.
 *
 * @param dxgi_format A DXGI_FORMAT (kept as uint32_t so the header stays free
 *        of D3D headers). Must be the UNORM sibling of the target's format —
 *        the whole composite runs on sRGB-passthrough bytes.
 *
 * @return true when the scratch is allocated at the requested shape.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_outcomp_ensure_weave_scratch(struct comp_d3d11_outcomp *outcomp,
                                        uint32_t w,
                                        uint32_t h,
                                        uint32_t dxgi_format);

/*!
 * Copy the @p region_w × @p region_h top-left region of @p dst_texture (which
 * the display processor just wove) into the weave scratch and return its SRV,
 * ready to be passed back as @p weave_srv.
 *
 * @param dst_texture The weave target (ID3D11Texture2D*).
 *
 * @return The scratch's ID3D11ShaderResourceView*, or NULL when no scratch is
 *         allocated (call @ref comp_d3d11_outcomp_ensure_weave_scratch first).
 *
 * @ingroup comp_d3d11
 */
void *
comp_d3d11_outcomp_snapshot_weave(struct comp_d3d11_outcomp *outcomp,
                                  void *dst_texture,
                                  uint32_t region_w,
                                  uint32_t region_h);

/*!
 * Composite a 2D layer over the weaved 3D output, gated by a per-pixel
 * region mask (unified 2D/3D compositing, #439 Phase 0 + Phase 1).
 *
 * The weave target (@p dst_texture) already holds the weaved 3D output.
 *
 * Phase 0 (rect path, @p mask_srv == NULL): derives a hard mask from the
 * canvas sub-rect: pixels INSIDE the canvas keep the weave (the pixel shader
 * discards), pixels OUTSIDE are written from @p twod_srv at 1:1. With a point
 * sampler + opaque output this is a hard-edged 1:1 fill of the non-canvas region.
 *
 * Phase 1 (authored-mask path, @p mask_srv != NULL): samples the scalar mask
 * M and blends per @p composite_mode — soft edges work, so the weave must be
 * readable: pass an SRV snapshot of dst as @p weave_srv (see
 * @ref comp_d3d11_outcomp_snapshot_weave). Modes: LERP = the hard M-lerp
 * (M·weave + (1−M)·twod, explicit authored mask — designer cutout/portal);
 * ALPHA_OVER = #491 premul over (implicit legacy mask: twod + (1−twod.a)·weave,
 * mask unused); ZONES = ADR-027/#801 (twod + (1−twod.a)·(M·weave) — M gates
 * only the weave by zone geometry, the 2D composites on top by its own alpha).
 * Ignored on the rect path.
 *
 * The pass writes only the @p region_w × @p region_h viewport at the top-left
 * of dst (#464: the 2D layer + mask are window-sized; the window content is
 * anchored top-left inside the worst-case surface). All SRVs are sampled with
 * uv spanning that region, so window-sized inputs map 1:1. Phase 0 passes
 * region == dst dims (full-surface behavior, unchanged).
 *
 * EVERY texture handed in must belong to the unit's device — that single rule
 * is what the #918 Phase 2 plane transports exist to satisfy.
 *
 * @param outcomp The output composite unit.
 * @param dst_texture Weave target (ID3D11Texture2D*) — holds the weave; a
 *        temporary RTV is created on it.
 * @param twod_srv The 2D layer (ID3D11ShaderResourceView*): an SRV-capable
 *        scratch copy of the 2D source (Local2D / zones content), region-sized.
 * @param mask_srv Authored scalar mask (R8_UNORM SRV), or NULL for the
 *        Phase 0 analytic rect path.
 * @param weave_srv SRV snapshot of dst's region (weave readable for the
 *        lerp); required iff @p mask_srv is non-NULL.
 * @param region_w Composite region width in pixels (window dims, #464).
 * @param region_h Composite region height in pixels.
 * @param cx,cy,cw,ch The 3D canvas sub-rect (region px) → the Phase 0 mask.
 * @param composite_mode One of COMP_D3D11_COMPOSITE_MODE_*.
 * @param opaque_present #833/#116 opaque present (DXR_PRESENT_OPAQUE on a
 *        transparent session): DWM completes no blends, so the composite
 *        flattens against the weave (which the DP's flattened gate already
 *        completed against the captured desktop) and emits α=1 — ZONES /
 *        ALPHA_OVER collapse to a premul-over of the 2D onto the weave; LERP
 *        completes its 2D side the same way. Ignored on the rect path.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_outcomp_composite_2d_masked(struct comp_d3d11_outcomp *outcomp,
                                       void *dst_texture,
                                       void *twod_srv,
                                       void *mask_srv,
                                       void *weave_srv,
                                       uint32_t region_w,
                                       uint32_t region_h,
                                       int32_t cx,
                                       int32_t cy,
                                       uint32_t cw,
                                       uint32_t ch,
                                       uint32_t composite_mode,
                                       bool opaque_present);

#ifdef __cplusplus
}
#endif
