// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The D3D11 hub's deposit handoff: what a producer hands the hub each frame.
 * @ingroup comp_hub_d3d11
 *
 * Pure D3D11/Win32 (every COM pointer is a `void *`, every format an integer),
 * so this header needs neither Vulkan nor `d3d11.h`. Every deposit flavour fills
 * it: the Vulkan tier's (comp_vk_native_deposit.h: Vulkan renders INTO the
 * NT-shared D3D11 texture), the D3D12 tier's (comp_d3d12_deposit.h, #1264) and
 * the D3D11 tier's own atlas (#2317 P1b). It was `struct comp_vk_deposit_handoff`
 * in comp_vk_native_deposit.h until the hub was extracted from comp_vk_native
 * (#2317 P1a); the fields and their contract are unchanged.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Everything a D3D consumer (the hub's @ref comp_xbridge wiring) needs to pick the
 * deposit up. Pointers are BORROWED — the deposit owns them and outlives no
 * caller.
 */
struct comp_hub_d3d11_handoff
{
	void *d3d11_device;  //!< `ID3D11Device *` the deposit textures live on.
	void *d3d11_context; //!< `ID3D11DeviceContext *`, its immediate context.
	void *dxgi_adapter;  //!< `IDXGIAdapter *` — the VkDevice's adapter, LUID-matched.

	void *texture;       //!< `ID3D11Texture2D *` — the slot Vulkan last wrote.
	void *shared_handle; //!< `HANDLE` — NT share of @ref texture.

	/*!
	 * `IDXGIKeyedMutex *` of @ref texture, or NULL in fence mode. Non-NULL
	 * means the deposit is in KEYED-MUTEX mode (no D3D12_FENCE import on this
	 * driver — ADR-039 Phase A): @ref fence is NULL, and the consumer MUST
	 * bracket its read with `AcquireSync(0, bounded_timeout)` /
	 * `ReleaseSync(0)` — key 0, and SKIP the frame on timeout rather than
	 * wait. The rationale for key 0 both sides (mutual exclusion, never a 0/1
	 * ready-handshake) lives at the mode selection in
	 * @ref comp_vk_deposit_create.
	 */
	void *keyed_mutex;

	void *fence;               //!< `ID3D11Fence *` Vulkan signals from the atlas submit.
	void *fence_shared_handle; //!< `HANDLE` — NT share of @ref fence.

	/*!
	 * The value @ref fence reaches once Vulkan's write to @ref texture has
	 * completed on the GPU. A consumer waits for exactly this:
	 * `ctx4->Wait(fence, fence_value)`. Zero before the first atlas submit.
	 */
	uint64_t fence_value;

	uint64_t adapter_luid; //!< Packed LUID (HighPart<<32 | LowPart) of the shared adapter.
	uint32_t width;
	uint32_t height;
	uint32_t slot; //!< Ring index of @ref texture.
	/*!
	 * The `DXGI_FORMAT` of @ref texture, as an integer so this header stays
	 * free of `dxgi.h`.
	 *
	 * Published because a CONSUMER's copy of this texture is format-checked,
	 * not merely size-checked: `ID3D11DeviceContext::CopySubresourceRegion`
	 * requires source and destination to share a typeless family, and
	 * `B8G8R8A8` and `R8G8B8A8` do not. A cross-family copy is not an error the
	 * API reports — it is dropped, and the destination keeps whatever it held
	 * (#1178). Consumers must compare this against their own chain's format
	 * rather than assume.
	 */
	uint32_t dxgi_format;
};

#ifdef __cplusplus
}
#endif
