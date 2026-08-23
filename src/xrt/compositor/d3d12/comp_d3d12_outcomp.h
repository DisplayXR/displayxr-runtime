// Copyright 2024-2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 OUTPUT composite: the masked 2D-over-3D pass on an explicit device.
 * @author David Fattal
 * @ingroup comp_d3d12
 *
 * #918 D12-1 — the D3D12 twin of @ref comp_d3d11_outcomp. Same entry points,
 * same parameter meanings, same composition order, same shaders (both units
 * compile `d3d_shared/comp_masked_composite_shaders.h`), so the D12-3 Stage A
 * that runs this on a native D3D12 output device on the scanout adapter can be
 * written symmetric to the D3D11 one.
 *
 * WHAT DIFFERS, AND WHY — the four places D3D12 forces a deliberate choice
 * rather than a transcription (each is called out again on the entry point it
 * affects):
 *
 * 1. NO CONTEXT, A COMMAND LIST PER CALL. The D3D11 unit borrows an
 *    ID3D11DeviceContext at create and records onto it implicitly, which bakes
 *    in D3D11's "one immediate context, caller serialises" threading policy.
 *    D3D12 has no immediate context, so the equivalent policy has to be stated:
 *    the unit is a pure RECORDER. Every call that touches the GPU takes the
 *    caller's OPEN ID3D12GraphicsCommandList and records into it; the unit
 *    never closes, submits, signals or waits on anything, and owns no queue,
 *    allocator or fence. Threading is therefore the command list's rule, not a
 *    device rule: one thread per list at a time, and one unit may serve several
 *    lists as long as no two record concurrently (the SRV ring below is what
 *    makes several IN-FLIGHT lists safe, not several concurrent recorders).
 *
 * 2. EXPLICIT RESOURCE STATES. D3D11 has implicit state promotion/decay; D3D12
 *    does not. The two recording entry points therefore take @p dst_state, the
 *    state the CALLER keeps the weave target in, and ROUND-TRIP out of and back
 *    into it (the transition is elided when it already matches what the step
 *    needs). Round-tripping costs one barrier more than a fused
 *    snapshot-then-composite sequence would; it buys that the two calls compose
 *    in any order, with anything in between, which is what a unit the split
 *    tail assembles has to offer. The caller-owned SOURCES (2D layer, mask) are
 *    NOT transitioned — the unit cannot know their steady state — and must be
 *    in PIXEL_SHADER_RESOURCE when the draw executes. The unit-owned weave
 *    scratch is the exception it fully owns: its steady state is
 *    PIXEL_SHADER_RESOURCE, and the snapshot round-trips it through COPY_DEST.
 *
 * 3. VIEWS LIVE IN HEAPS. D3D11 takes ready-made ID3D11ShaderResourceView* and
 *    creates its destination RTV inline; a D3D12 view is a descriptor in a
 *    unit-owned heap. So the source parameters are ID3D12Resource* (the unit
 *    writes the SRVs itself, from each resource's OWN format — see the sRGB
 *    note below) and the destination RTV is created per call, from the
 *    resource's own format, exactly as D3D11's `CreateRenderTargetView(dst,
 *    nullptr, ...)` does.
 *
 * 4. PSO VARIANTS INSTEAD OF STATE TOGGLES. D3D11 binds blend / rasterizer /
 *    depth-stencil objects at draw time and its RTV is format-agnostic. D3D12
 *    bakes all of it — including the render-target format — into the PSO, so
 *    the unit builds one PSO per weave-target format it accepts
 *    (R8G8B8A8_UNORM and B8G8R8A8_UNORM, the two seen in the wild: DXGI targets
 *    and app shared textures). Everything else is identical across the
 *    variants, and the shader bytecode is shared.
 *
 * sRGB: unchanged from the D3D11 unit, deliberately. The whole composite runs
 * on sRGB-PASSTHROUGH bytes — the display processor wants ENCODED pixels, so no
 * step here may insert a linear<->encoded conversion. Concretely: SRVs are
 * created with each source resource's OWN format (never coerced to a UNORM
 * sibling, never to an _SRGB one), the destination RTV takes the weave target's
 * own format, and only UNORM target formats get a PSO — an _SRGB-typed target
 * is REJECTED rather than quietly given an _SRGB RTV variant, because that RTV
 * would encode on write and the D3D11 twin performs no such encode.
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations (C++ structs)
struct comp_d3d12_outcomp;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Composite modes for @ref comp_d3d12_outcomp_composite_2d_masked. See its doc
 * comment for the per-mode blend.
 *
 * Same names and values as the triple comp_d3d12_renderer.h already publishes —
 * one composite, one mode enumeration, whichever unit records it. (D12-3 folds
 * the renderer's copy of this pass into this unit and its copy of these macros
 * goes with it; until then the two definitions are token-identical, which is
 * what keeps including both headers legal.)
 */
#define COMP_D3D12_COMPOSITE_MODE_LERP 0u
#define COMP_D3D12_COMPOSITE_MODE_ALPHA_OVER 1u
#define COMP_D3D12_COMPOSITE_MODE_ZONES 2u

/*!
 * Create the output composite unit on an EXPLICIT device (#918 D12-1).
 *
 * The unit owns the masked-composite shaders, the root signature, the
 * per-format PSOs, the descriptor heaps for its SRV sets and its destination
 * RTV, and the weave scratch — every one of them a device-scoped object, which
 * is the whole reason this is a unit and not a renderer method: the composite's
 * destination is the WEAVE TARGET, and under the output-device split that
 * target lives on the scanout device while the renderer stays on the app's.
 *
 * The device is passed explicitly and is NOT owned (no AddRef/Release) — the
 * caller outlives the unit, exactly as for @ref comp_d3d11_outcomp_create.
 *
 * There is no context parameter (the D3D11 twin's second argument): D3D12 has
 * no immediate context, and the unit records into the caller's open command
 * list per call instead. See point 1 of the file comment.
 *
 * @param device The ID3D12Device* every resource here is created on.
 * @param out_outcomp Pointer to receive the created unit.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_outcomp_create(void *device, struct comp_d3d12_outcomp **out_outcomp);

/*!
 * #1151 — tell the unit that everything recorded so far has been SUBMITTED, so
 * its descriptor ring may safely wrap past this point.
 *
 * The unit hands out one descriptor set per composite from a ring of four and
 * wraps when it runs out. A wrap is only safe once the set being reused has
 * finished executing — and this unit is a pure recorder (point 1 of the file
 * comment): it owns no queue and no fence, so it cannot see a submission
 * boundary and cannot tell a safe wrap from one that overwrites descriptors the
 * GPU is still reading. Only the caller knows where its `ExecuteCommandLists`
 * are.
 *
 * Call it wherever the list the composites were recorded into is executed. Too
 * many composites between two calls is then reported (once, as an ERROR) rather
 * than silently corrupting a frame; the unit never waits, because a stall on the
 * weave thread would be worse than the corruption it is warning about.
 *
 * NULL-tolerant, so a caller can wire its submission boundaries before it wires
 * its first composite.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_outcomp_note_execute(struct comp_d3d12_outcomp *outcomp);

/*!
 * Destroy the unit and everything it owns (including the weave scratch).
 * Idempotent; nulls the caller's pointer.
 *
 * The unit signals no fence and waits on nothing, so — like every other
 * D3D12-owned object — the caller must not destroy it while a command list that
 * references its resources is still in flight.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_outcomp_destroy(struct comp_d3d12_outcomp **outcomp_ptr);

/*!
 * (Re)allocate the unit-owned weave scratch to @p w × @p h @p dxgi_format
 * (no-op when it already covers the request).
 *
 * The scratch is the sampleable snapshot of the weave target's window region
 * that the authored-mask lerp reads (the target is a render target, and a
 * D3D12 resource cannot be read and written by one draw), so it is allocated on
 * the SAME device as the composite — see @ref comp_d3d12_outcomp_snapshot_weave.
 *
 * Separate from the snapshot so a caller can do its own allocation-failure
 * bail before any work that depends on the scratch existing.
 *
 * GROW-ONLY within a session (#918 F10), byte-for-byte the D3D11 rule; a format
 * change still reallocates. The scratch is created in — and steady-states at —
 * PIXEL_SHADER_RESOURCE (point 2 of the file comment).
 *
 * @param dxgi_format A DXGI_FORMAT (kept as uint32_t so the header stays free
 *        of D3D headers). Must be the UNORM sibling of the target's format —
 *        the whole composite runs on sRGB-passthrough bytes.
 *
 * @return true when the scratch is allocated at least at the requested shape.
 *
 * @ingroup comp_d3d12
 */
bool
comp_d3d12_outcomp_ensure_weave_scratch(struct comp_d3d12_outcomp *outcomp,
                                        uint32_t w,
                                        uint32_t h,
                                        uint32_t dxgi_format);

/*!
 * Record a copy of the @p region_w × @p region_h top-left region of
 * @p dst_resource (which the display processor just wove) into the weave
 * scratch, and return the scratch, ready to be passed back as
 * @p weave_resource.
 *
 * Records into @p cmd_list only — nothing is submitted, so the returned
 * resource holds the snapshot once that list executes, not when this returns.
 *
 * Barriers (point 2 of the file comment): @p dst_resource round-trips
 * @p dst_state → COPY_SOURCE → @p dst_state, and the unit's scratch round-trips
 * PIXEL_SHADER_RESOURCE → COPY_DEST → PIXEL_SHADER_RESOURCE. Both ends are
 * therefore exactly as the caller left them, except that the scratch now holds
 * the snapshot.
 *
 * @param cmd_list The caller's OPEN ID3D12GraphicsCommandList*.
 * @param dst_resource The weave target (ID3D12Resource*).
 * @param dst_state D3D12_RESOURCE_STATES the caller keeps @p dst_resource in
 *        (uint32_t so the header stays free of D3D headers).
 *
 * @return The scratch's ID3D12Resource*, or NULL when no scratch is allocated
 *         (call @ref comp_d3d12_outcomp_ensure_weave_scratch first) or the
 *         clamped copy region is empty.
 *
 * @ingroup comp_d3d12
 */
void *
comp_d3d12_outcomp_snapshot_weave(struct comp_d3d12_outcomp *outcomp,
                                  void *cmd_list,
                                  void *dst_resource,
                                  uint32_t dst_state,
                                  uint32_t region_w,
                                  uint32_t region_h);

/*!
 * Composite a 2D layer over the weaved 3D output, gated by a per-pixel
 * region mask (unified 2D/3D compositing, #439 Phase 0 + Phase 1).
 *
 * The weave target (@p dst_resource) already holds the weaved 3D output.
 *
 * Phase 0 (rect path, @p mask_resource == NULL): derives a hard mask from the
 * canvas sub-rect: pixels INSIDE the canvas keep the weave (the pixel shader
 * discards), pixels OUTSIDE are written from @p twod_resource at 1:1. With a
 * point sampler + opaque output this is a hard-edged 1:1 fill of the non-canvas
 * region.
 *
 * Phase 1 (authored-mask path, @p mask_resource != NULL): samples the scalar
 * mask M and blends per @p composite_mode — soft edges work, so the weave must
 * be readable: pass the snapshot of dst as @p weave_resource (see
 * @ref comp_d3d12_outcomp_snapshot_weave). Modes: LERP = the hard M-lerp
 * (M·weave + (1−M)·twod, explicit authored mask — designer cutout/portal);
 * ALPHA_OVER = #491 premul over (implicit legacy mask: twod + (1−twod.a)·weave,
 * mask unused); ZONES = ADR-027/#801 (twod + (1−twod.a)·(M·weave) — M gates
 * only the weave by zone geometry, the 2D composites on top by its own alpha).
 * Ignored on the rect path.
 *
 * The pass writes only the @p region_w × @p region_h viewport at the top-left
 * of dst (#464: the 2D layer + mask are window-sized; the window content is
 * anchored top-left inside the worst-case surface). All sources are sampled
 * with uv spanning that region, so window-sized inputs map 1:1. Phase 0 passes
 * region == dst dims (full-surface behavior, unchanged).
 *
 * EVERY resource handed in must belong to the unit's device — that single rule
 * is what the #918 Phase 2 plane transports exist to satisfy.
 *
 * Records into @p cmd_list only. @p dst_resource round-trips @p dst_state →
 * RENDER_TARGET → @p dst_state; the caller-owned sources are NOT transitioned
 * and must be in PIXEL_SHADER_RESOURCE when the draw executes (the weave
 * scratch already is, by @ref comp_d3d12_outcomp_snapshot_weave). Leaves the
 * unit's descriptor heap, root signature and PSO bound on the list — downstream
 * recording re-binds what it needs, exactly as the renderer's copy of this pass
 * documents.
 *
 * @param outcomp The output composite unit.
 * @param cmd_list The caller's OPEN ID3D12GraphicsCommandList*.
 * @param dst_resource Weave target (ID3D12Resource*) — holds the weave; an RTV
 *        is created on it per call, in the unit's own RTV heap. Its format
 *        selects the PSO and must be R8G8B8A8_UNORM or B8G8R8A8_UNORM (see the
 *        sRGB note in the file comment).
 * @param dst_state D3D12_RESOURCE_STATES the caller keeps @p dst_resource in.
 * @param twod_resource The 2D layer (ID3D12Resource*): the region-sized scratch
 *        holding the flattened 2D source (Local2D / zones content).
 * @param mask_resource Authored scalar mask (R8_UNORM ID3D12Resource*), or NULL
 *        for the Phase 0 analytic rect path.
 * @param weave_resource Snapshot of dst's region (weave readable for the lerp);
 *        required iff @p mask_resource is non-NULL.
 * @param region_w Composite region width in pixels (window dims, #464).
 * @param region_h Composite region height in pixels.
 * @param cx,cy,cw,ch The 3D canvas sub-rect (region px) → the Phase 0 mask.
 * @param composite_mode One of COMP_D3D12_COMPOSITE_MODE_*.
 * @param opaque_present #833/#116 opaque present (DXR_PRESENT_OPAQUE on a
 *        transparent session): DWM completes no blends, so the composite
 *        flattens against the weave (which the DP's flattened gate already
 *        completed against the captured desktop) and emits α=1 — ZONES /
 *        ALPHA_OVER collapse to a premul-over of the 2D onto the weave; LERP
 *        completes its 2D side the same way. Ignored on the rect path.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_outcomp_composite_2d_masked(struct comp_d3d12_outcomp *outcomp,
                                       void *cmd_list,
                                       void *dst_resource,
                                       uint32_t dst_state,
                                       void *twod_resource,
                                       void *mask_resource,
                                       void *weave_resource,
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
