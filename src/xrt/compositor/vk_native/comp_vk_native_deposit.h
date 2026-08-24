// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  VK-0 — the D3D11 deposit texture for the in-process Vulkan split (#1178).
 * @ingroup comp_vk_native
 *
 * ## What this is
 *
 * The first rung of the in-process Vulkan weave-on-scanout ladder (#918 /
 * ADR-037). It makes the Vulkan compositor's **atlas** live in a D3D11 texture
 * that D3D can reach, so a later rung can hand that texture to @ref comp_xbridge
 * and move the weave onto the scanout adapter.
 *
 * ## Topology — Vulkan never touches a cross-adapter object
 *
 * Vulkan has no cross-adapter sharing, and importing a D3D12 resource
 * (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`) is unwired in this tree.
 * Neither is needed. The deposit is **same-adapter**; the existing bridge does
 * the crossing, exactly as it does for the shipped D3D11 split:
 *
 * @verbatim
 *   VK renders the atlas  --(renders INTO, no copy)-->  D3D11 deposit texture
 *        |                                              (same adapter as VkDevice,
 *        | timeline semaphore, signalled by                NT-shared, fence-synced)
 *        | the atlas submit == an ID3D11Fence                     |
 *        v                                                        v
 *   D3D consumer GPU-waits (ID3D11DeviceContext4::Wait)  -->  comp_xbridge (VK-1)
 * @endverbatim
 *
 * ## The deposit costs zero copies
 *
 * The imported `VkImage` carries `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, so the
 * D3D11 texture is a renderable target: the renderer's atlas *is* the deposit
 * slot for the frame. Nothing is copied into it.
 *
 * ## Synchronisation — GPU-side, and it does not lean on any CPU wait
 *
 * The D3D11 side creates a shared `ID3D11Fence`; Vulkan imports it as a
 * **timeline semaphore** (`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`,
 * the same mechanism `comp_vk_client.c` already uses for the workspace sync
 * fence) and signals a monotonically increasing value from the atlas submit. A
 * D3D consumer orders itself behind that write with
 * `ID3D11DeviceContext4::Wait(fence, value)` — a GPU-side wait.
 *
 * This unit adds **no CPU wait anywhere**. The pre-existing per-frame
 * `vkQueueWaitIdle` / `vkWaitForFences` on the VK render path (#837) is
 * untouched and, critically, is *not* what makes the deposit correct: when #837
 * removes it, the deposit's ordering guarantee is unchanged. That is the
 * property that keeps this ladder out of the #925 wedge family, and it is why
 * the deposit is NOT built on `IDXGIKeyedMutex::AcquireSync` the way the DComp
 * transparency bridge is (that acquire is a CPU-blocking call on the render
 * thread).
 *
 * ## Gate
 *
 * Everything here is behind `DXR_VK_DEPOSIT=1`. With the flag off no deposit is
 * created and every call site below is skipped, so the compositor runs the
 * byte-identical path it ran before this file existed.
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_vulkan_includes.h"

#include <stdint.h>
#include <stdbool.h>

struct vk_bundle;
struct comp_vk_deposit;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Ring depth. Mirrors the DComp bridge's ring: the producer writes slot N+1
 * while a consumer still reads slot N, so the deposit never has to be the thing
 * that serialises them.
 */
#define COMP_VK_DEPOSIT_RING 2

/*!
 * Is the deposit requested for this process? Reads `DXR_VK_DEPOSIT` once.
 *
 * Safe to call on any platform; always false off Windows.
 */
bool
comp_vk_deposit_requested(void);

/*!
 * Everything a D3D consumer (VK-1's @ref comp_xbridge wiring) needs to pick the
 * deposit up. Pointers are BORROWED — the deposit owns them and outlives no
 * caller.
 */
struct comp_vk_deposit_handoff
{
	void *d3d11_device;  //!< `ID3D11Device *` the deposit textures live on.
	void *d3d11_context; //!< `ID3D11DeviceContext *`, its immediate context.
	void *dxgi_adapter;  //!< `IDXGIAdapter *` — the VkDevice's adapter, LUID-matched.

	void *texture;       //!< `ID3D11Texture2D *` — the slot Vulkan last wrote.
	void *shared_handle; //!< `HANDLE` — NT share of @ref texture.

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
};

/*!
 * Stand the deposit up: LUID-match a D3D11 device to @p vk, allocate the
 * NT-shared texture ring, import each slot as a `VkImage`, and import a shared
 * `ID3D11Fence` as a timeline semaphore.
 *
 * Emits exactly one `vk deposit:` WARN naming both LUIDs and whether they
 * matched, in the style of the `weave placement:` line.
 *
 * @param vk                     The compositor's bundle (the APP's VkDevice).
 * @param app_timeline_semaphores The app's device has `VK_KHR_timeline_semaphore`
 *        enabled. The runtime cannot turn it on after the fact, and creating a
 *        timeline semaphore without it is undefined — so this is a hard gate,
 *        not a hint. False fails the create with a stated reason.
 * @param width  Atlas width in pixels.
 * @param height Atlas height in pixels.
 * @param format The atlas `VkFormat` (must have a DXGI equivalent; only
 *        `VK_FORMAT_B8G8R8A8_UNORM` is wired today).
 *
 * @return XRT_SUCCESS, or an error with the deposit left NULL. Every failure is
 *         non-fatal to the caller: the compositor runs its normal path.
 */
xrt_result_t
comp_vk_deposit_create(struct vk_bundle *vk,
                       bool app_timeline_semaphores,
                       uint32_t width,
                       uint32_t height,
                       VkFormat format,
                       struct comp_vk_deposit **out_deposit);

/*!
 * Tear the deposit down. Idles the device first — the ring is imported memory
 * and the D3D11 textures must outlive every submit that touched them.
 */
void
comp_vk_deposit_destroy(struct comp_vk_deposit **deposit_ptr);

/*!
 * Reallocate the ring at a new size (atlas resize). Same failure contract as
 * @ref comp_vk_deposit_create: on failure the deposit is left inactive and the
 * caller falls back to its own atlas.
 */
xrt_result_t
comp_vk_deposit_resize(struct comp_vk_deposit *dep, uint32_t width, uint32_t height);

/*!
 * Advance to the next ring slot and return its images. Call once per APP frame,
 * before the atlas is drawn — never for a repaint, which replays the atlas the
 * last app frame left in the current slot.
 */
void
comp_vk_deposit_advance(struct comp_vk_deposit *dep, uint64_t *out_image, uint64_t *out_view);

/*!
 * The current slot's images, without advancing.
 */
void
comp_vk_deposit_get_current(struct comp_vk_deposit *dep, uint64_t *out_image, uint64_t *out_view);

/*!
 * Claim the timeline value the NEXT atlas submit will signal, and the semaphore
 * to signal it on. Bumps the counter, so call exactly once per submit that is
 * actually issued.
 *
 * @param out_semaphore The imported timeline semaphore (`VK_NULL_HANDLE` if the
 *        deposit has no working sync — the caller then submits as before).
 * @param out_value The value to put in `VkTimelineSemaphoreSubmitInfo`.
 */
void
comp_vk_deposit_claim_signal(struct comp_vk_deposit *dep, VkSemaphore *out_semaphore, uint64_t *out_value);

/*!
 * Undo the last @ref comp_vk_deposit_claim_signal — the submit it was claimed
 * for never reached the queue, so the value will never be signalled and a
 * consumer waiting on it would hang forever.
 */
void
comp_vk_deposit_abandon_signal(struct comp_vk_deposit *dep);

/*!
 * Fill @p out with everything a D3D consumer needs. This is the VK-1 entry
 * point; nothing in the runtime calls it yet.
 *
 * @return false if the deposit is not active.
 */
bool
comp_vk_deposit_get_handoff(struct comp_vk_deposit *dep, struct comp_vk_deposit_handoff *out);

/*!
 * `DXR_VK_DEPOSIT_PROBE=1` — one-shot proof, off by default.
 *
 * Takes the GPU-side consumer wait for real (`ID3D11DeviceContext4::Wait` on the
 * imported fence), copies the deposit slot to a staging texture and reports the
 * non-transparent pixel count plus a checksum. Answers "did Vulkan's pixels
 * actually land in the D3D11 texture, and does the fence wait resolve?" without
 * needing a panel or an eyeball.
 *
 * Runs at most once per process and only after the frame has presented, so its
 * one `Map` never sits on the steady-state render path. No-op unless the env var
 * is set.
 */
void
comp_vk_deposit_probe_once(struct comp_vk_deposit *dep, VkQueue queue);

#ifdef __cplusplus
}
#endif
