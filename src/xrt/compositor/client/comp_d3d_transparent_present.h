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
 * a transparent DComp swap chain on the app's HWND, and per frame waits the fence,
 * copies the shared output into the back buffer, and `Present` + `Commit` so DWM blends
 * the live desktop into the holes.
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
 * and signaled). GPU-waits the lockstep fence, copies the shared output into the DComp
 * back buffer, and `Present` + `Commit`. No-op if @p p is NULL.
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
