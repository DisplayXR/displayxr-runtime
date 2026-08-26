// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Render-API-agnostic transparent DirectComposition present for IPC clients.
 *
 * A forced-IPC transparent client wants alpha=0 regions of the woven output to show
 * the LIVE desktop. The service runs out-of-process and a process can only create a
 * DirectComposition target / composition swap chain on a window it OWNS
 * (`E_ACCESSDENIED` on the client's HWND), so the *client* must own the present
 * (ADR-029). The service hands over a shared D3D11 NT-handle texture (premultiplied
 * `R8G8B8A8`) plus a service→client `ID3D11Fence`; this helper imports both, stands up
 * a transparent DComp swap chain on the app's HWND, and per frame polls the fence,
 * copies the matching slice of the shared output into the back buffer, and `Present` +
 * `Commit` so DWM blends the live desktop into the holes.
 *
 * The shared texture is a ring of COMP_TRANSPARENT_OUTPUT_RING array slices rather than
 * a single image (#1208) - the producer is structurally one value ahead of this consumer,
 * so with one slice it overwrote the pixels being copied out. Both sides index the ring
 * off the fence value, which is the one number they already share exactly.
 *
 * The present is pure D3D11 + DirectComposition and is **independent of the app's render
 * API**: the shared handles are openable by any D3D11 device. A D3D11 client can pass its
 * own `ID3D11Device` to avoid a second device; a D3D12/GL/VK client passes NULL and the
 * helper creates its own small D3D11 device for the present. This is why the same helper
 * serves every Windows IPC client.
 *
 * Windows-only.
 *
 * @ingroup comp_client
 */
#pragma once

#include "xrt/xrt_handles.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Depth of the service→client output ring (#1208).
 *
 * The producer runs INSIDE the consumer's commit: `client_*_compositor_layer_commit`
 * makes the layer-commit RPC — which weaves value `v+1` on this client's IPC thread in
 * the service and signals the fence — and only THEN calls the present below, which
 * copies whatever `GetCompletedValue()` reports (at most `v`). So the producer is
 * always at least one value ahead of the consumer at copy time, and with a single
 * `ArraySize=1` texture it was overwriting the very pixels being copied out. That is
 * not an unlucky interleaving; it is the steady state.
 *
 * The fix needs no mutual exclusion at all — just somewhere else to write. Both sides
 * derive the array slice from the fence value, which is the one number they already
 * share exactly:
 *
 *   producer writes slice (v + 1) % RING, then signals v + 1
 *   consumer reads   slice completed % RING, where completed <= v
 *
 * Depth 2 already separates a consumer exactly one value behind. Depth 3 also covers a
 * consumer two behind — its `CopySubresourceRegion` still in flight two commits later —
 * which is the only lag the coupling above permits. Raise this if a diagnostic ever
 * shows the consumer lagging further; nothing else has to change with it.
 *
 * NOTE: the service and the client must agree on this value. They ship in lockstep (the
 * client↔service git-tag gate rejects a mismatched pair at xrCreateInstance), so a bump
 * here needs no negotiation — unlike the XR_DXR_weave twin, whose consumer is
 * out-of-tree.
 */
#define COMP_TRANSPARENT_OUTPUT_RING 3

/*!
 * Opaque transparent-present helper. Owns the imported shared texture + fence, the DComp
 * device/target/visual, the composition swap chain, and (when no device was supplied) its
 * own D3D11 device + context.
 */
struct comp_d3d_transparent_presenter;

/*!
 * Stand up the transparent present from the IPC-provided shared handles.
 *
 * @param existing_d3d11_device An `ID3D11Device *` to reuse for the present (D3D11 client),
 *                              or NULL to have the helper create its own D3D11 device
 *                              (D3D12/GL/VK clients). Passed as `void *` to keep this
 *                              header C-includable.
 * @param hwnd                  The app's window handle (as a `uint64_t`).
 * @param width,height          Dimensions of the shared output texture.
 * @param shared_tex            Service output texture NT handle. **Consumed** (closed) by
 *                              this call regardless of success.
 * @param shared_fence          Service→client fence NT handle. **Consumed** (closed) by
 *                              this call regardless of success.
 *
 * @return A ready presenter, or NULL on any failure (caller stays on the service's opaque
 *         present — no see-through, but never a crash).
 */
struct comp_d3d_transparent_presenter *
comp_d3d_transparent_presenter_create(void *existing_d3d11_device,
                                      uint64_t hwnd,
                                      uint32_t width,
                                      uint32_t height,
                                      xrt_graphics_buffer_handle_t shared_tex,
                                      xrt_graphics_sync_handle_t shared_fence);

/*!
 * Per-frame present. Call once after the layer-commit RPC returns (the service has weaved
 * and signaled). Polls the fence, copies the ring slice that fence value names into the
 * DComp back buffer, and `Present` + `Commit`. No-op if @p p is NULL.
 */
void
comp_d3d_transparent_presenter_present(struct comp_d3d_transparent_presenter *p);

/*!
 * Tear down the presenter and release every owned resource. Sets `*p` to NULL. Safe when
 * `*p` is already NULL.
 */
void
comp_d3d_transparent_presenter_destroy(struct comp_d3d_transparent_presenter **p);

#ifdef __cplusplus
}
#endif
