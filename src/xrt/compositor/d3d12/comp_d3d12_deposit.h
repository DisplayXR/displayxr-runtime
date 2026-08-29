// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The D3D11 deposit ring for the heavy-D3D12 reroute (#1264 / ADR-039).
 * @ingroup comp_d3d12
 *
 * ## What this is
 *
 * The D3D12 flavour of the VK tier's deposit (comp_vk_native_deposit.h — read
 * its topology section first): a ring of D3D11 textures the D3D12 renderer's
 * atlas lands in directly, so the proven d3d11 fill arm (comp_vk_split — which
 * is Vulkan-free and consumes only the pure-D3D11 handoff struct) can weave and
 * present it. Measured motivation: under real app GPU load the d3d12 out arm
 * serializes (8–9 ms fires; Intel preemption is draw-granular, PRIORITY_HIGH
 * nulled) while the d3d11 arm holds 2.2–2.9 ms — the immunity is structural to
 * the d3d11 submission path, so heavy same-adapter D3D12 apps route through it.
 *
 * @verbatim
 *   D3D12 renders the atlas --(renders INTO via OpenSharedHandle import)-->
 *       D3D11 deposit texture (same adapter, NT-shared, fence-synced)
 *          |  app queue Signal()s a shared ID3D11Fence (opened as ID3D12Fence)
 *          v
 *   comp_vk_split's ingress GPU-waits on the D3D11 immediate context --> the
 *   d3d11-ends comp_xbridge --> the d3d11 out device fill arm
 * @endverbatim
 *
 * ## Synchronisation — all native fences, no keyed mutex
 *
 * Unlike the VK deposit's keyed-mutex fallback, D3D12 opens a D3D11 shared
 * fence natively (`OpenSharedHandle` → `ID3D12Fence`; both directions already
 * proven in comp_xbridge.cpp). Forward edge: the app queue Signal()s past the
 * atlas submit and the ingress `ID3D11DeviceContext4::Wait`s. Back edge (the
 * slot-rewrite-vs-in-flight-copy tear): the consumer signals a release value on
 * the D3D11 immediate context (@ref comp_d3d12_deposit_note_consumed) and the
 * app queue Wait()s it before rewriting the slot. GPU-side both ways; no CPU
 * wait anywhere in this unit.
 *
 * ## Resource states
 *
 * The imported `ID3D12Resource`s are cross-API shared and decay to COMMON at
 * command-list boundaries; the renderer's external-atlas mode enters COMMON →
 * RENDER_TARGET and exits back to COMMON every draw, so the state is
 * self-consistent regardless of decay and COMMON is what the D3D11 reader sees.
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include "vk_native/comp_vk_native_deposit.h" // struct comp_vk_deposit_handoff (pure D3D11/Win32)

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_d3d12_deposit;

//! Ring depth — mirrors the VK deposit's: producer writes N+1 while the
//! consumer still reads N.
#define COMP_D3D12_DEPOSIT_RING 2

/*!
 * Stand the deposit up: LUID-match a D3D11 device to the app's `ID3D12Device`,
 * allocate the NT-shared texture ring (R8G8B8A8_UNORM — the renderer's atlas
 * format), open each slot on the D3D12 device, and share one D3D11 fence both
 * ways.
 *
 * @param app_d3d12_device `ID3D12Device *` (the APP's device).
 * @return XRT_SUCCESS or an error with the deposit left NULL; every failure is
 *         non-fatal to the caller (the session falls back to its stock path).
 */
xrt_result_t
comp_d3d12_deposit_create(void *app_d3d12_device,
                          uint32_t width,
                          uint32_t height,
                          struct comp_d3d12_deposit **out_deposit);

//! Tear down. The caller has quiesced the GPU (both devices) first.
void
comp_d3d12_deposit_destroy(struct comp_d3d12_deposit **deposit_ptr);

/*!
 * Reallocate the ring at a new size. The caller has quiesced the app queue and
 * the consumer first (the same discipline the renderer's own atlas realloc
 * already follows). On failure the deposit is INACTIVE and the caller retires
 * the reroute.
 */
xrt_result_t
comp_d3d12_deposit_resize(struct comp_d3d12_deposit *dep, uint32_t width, uint32_t height);

/*!
 * Advance to the next ring slot for a new APP frame (never for a repaint — the
 * fill arm replays the egress, not the deposit) and take the back-pressure
 * wait: if a consumer has claimed the slot, queue a GPU-side
 * `ID3D12CommandQueue::Wait` for its release value on @p app_queue
 * (`ID3D12CommandQueue *`). Costs nothing when already satisfied.
 */
void
comp_d3d12_deposit_advance(struct comp_d3d12_deposit *dep, void *app_queue);

//! The current slot's imported `ID3D12Resource *` (the renderer's external
//! atlas for this frame), or NULL when inactive.
void *
comp_d3d12_deposit_current_resource(struct comp_d3d12_deposit *dep);

//! Allocated ring extent (the external-atlas alloc dims the renderer reports).
void
comp_d3d12_deposit_get_dims(struct comp_d3d12_deposit *dep, uint32_t *out_w, uint32_t *out_h);

/*!
 * After the atlas render has been SUBMITTED on @p app_queue
 * (`ID3D12CommandQueue *`): signal the shared fence past it. The value lands in
 * the next @ref comp_d3d12_deposit_get_handoff as `fence_value`, and the
 * ingress orders its copy behind it.
 */
void
comp_d3d12_deposit_signal(struct comp_d3d12_deposit *dep, void *app_queue);

/*!
 * Fill @p out for comp_vk_split (same contract as the VK deposit's handoff:
 * pointers BORROWED, `keyed_mutex` NULL — this flavour is always fence mode).
 */
bool
comp_d3d12_deposit_get_handoff(struct comp_d3d12_deposit *dep, struct comp_vk_deposit_handoff *out);

/*!
 * The consumer's staging copy of @p slot has been recorded on the D3D11
 * immediate context — release the slot back to D3D12 (queued signal on that
 * context; the app queue's next write into the slot waits for it via
 * @ref comp_d3d12_deposit_advance). Mirrors comp_vk_deposit_note_consumed.
 */
void
comp_d3d12_deposit_note_consumed(struct comp_d3d12_deposit *dep, uint32_t slot);

#ifdef __cplusplus
}
#endif
