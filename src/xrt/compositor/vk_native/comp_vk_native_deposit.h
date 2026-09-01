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
 * **KEYED-MUTEX mode (ADR-039 Phase A)** is the documented exception: a driver
 * that exposes no D3D12_FENCE import at all (this box's Intel UHD VK ICD) gets
 * a per-slot keyed mutex instead — key 0 on both sides (mutual exclusion, not a
 * ready-handshake; the wedge argument lives at the mode selection in the
 * create), bounded timeouts, skip-on-timeout. The consumer's acquire IS a
 * short CPU block on the frame thread, comparable to the per-frame CPU wait
 * the VK path already carries (#837) — accepted for Phase A bring-up, revisited
 * if #837 lands. Plane deposits run TIMING-ONLY in this mode (#1274): their
 * fence edges no-op and ordering rides the #837 wait plus the on-change
 * cadence — see the note at comp_vk_deposit_plane_ensure.
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
 *        not a hint. False fails the create with a stated reason. FENCE mode
 *        only: keyed-mutex mode does not create a timeline and ignores it.
 * @param app_keyed_mutex The app's device has `VK_KHR_win32_keyed_mutex`
 *        enabled. Consulted only when the device cannot import a D3D12_FENCE
 *        (ADR-039 Phase A): true selects KEYED-MUTEX mode, false fails the
 *        create with a stated reason.
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
                       bool app_keyed_mutex,
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
 * KEYED-MUTEX mode only (ADR-039 Phase A): chain the current slot's keyed-mutex
 * acquire/release (key 0 both, bounded timeout) onto @p submit_info's pNext.
 * No-op in fence mode or with no deposit — the caller invokes it unconditionally
 * wherever @ref comp_vk_deposit_claim_signal returned no semaphore. The chain
 * storage lives in the deposit and must not be raced by a second concurrent
 * submit build (the renderer builds one at a time).
 */
void
comp_vk_deposit_chain_km(struct comp_vk_deposit *dep, VkSubmitInfo *submit_info);

/*!
 * VK-1 (#1178) — the consumer has taken its copy of @p slot; release it back to
 * Vulkan.
 *
 * Signals the shared fence from the D3D11 immediate context to a fresh value past
 * every claim so far, and remembers it as @p slot's release value. Purely a queue
 * operation: this thread does not wait, and neither does the GPU until something
 * asks it to.
 *
 * **Why the ring needs this at all.** VK-0's two-slot ring separates a producer
 * reading slot N from Vulkan writing slot N+1, which is enough while Vulkan is
 * also the thing that weaves and presents — the frame loop is self-pacing. Under
 * the split it is not: Vulkan renders the atlas and returns, and the present it
 * used to block behind now belongs to the output device. Vulkan can then reach
 * slot N again (two frames later) while the bridge's copy of the older slot N is
 * still executing, and nothing anywhere would order them. That is a torn atlas,
 * not a stall, so it cannot be left to timing.
 *
 * Deliberately NOT a CPU wait, an `IDXGIKeyedMutex::AcquireSync`, or a deeper
 * ring: the imported semaphore is a D3D12 fence and is therefore bidirectional,
 * so the correct answer costs one queued signal and one queued wait.
 *
 * No-op when the deposit has no working sync.
 */
void
comp_vk_deposit_note_consumed(struct comp_vk_deposit *dep, uint32_t slot);

/*!
 * The value the CURRENT slot's last @ref comp_vk_deposit_note_consumed signalled,
 * or 0 when nothing has consumed this slot yet (warmup, or no consumer at all).
 *
 * Vulkan's next submit into this slot waits for it on the timeline semaphore —
 * see the back-pressure argument above. Call after
 * @ref comp_vk_deposit_advance has chosen the frame's slot.
 */
uint64_t
comp_vk_deposit_current_slot_wait(struct comp_vk_deposit *dep);

/*!
 * The imported timeline semaphore, or `VK_NULL_HANDLE` when the deposit has no
 * working sync. Exposed for the wait half above; the signal half claims it
 * through @ref comp_vk_deposit_claim_signal.
 */
VkSemaphore
comp_vk_deposit_get_timeline(struct comp_vk_deposit *dep);

/*!
 * Fill @p out with everything a D3D consumer needs. This is the VK-1 entry
 * point; nothing in the runtime calls it yet.
 *
 * @return false if the deposit is not active.
 */
bool
comp_vk_deposit_get_handoff(struct comp_vk_deposit *dep, struct comp_vk_deposit_handoff *out);

/*!
 * @name VK-1b (#1178) — the PLANE deposits.
 *
 * ## Why these exist at all, and why only the Vulkan leg needs them
 *
 * The masked composite needs three more app-device images on the scanout
 * adapter beside the atlas: the Local2D over-flatten, the 2D-under backdrop and
 * a Tier-3 authored mask (`COMP_XBRIDGE_PLANE_*`). @ref comp_xbridge transports
 * all three already — but its D3D11-ends flavour binds a plane by NT handle to an
 * `ID3D11Texture2D`, and the D3D12-ends flavour binds an `ID3D12Resource` by
 * pointer. **Both flavours assume the compositor's flatten scratch already IS a
 * D3D resource.** For the two D3D legs it is, for free. For Vulkan it is a plain
 * `VkImage` and there is nothing to bind.
 *
 * That asymmetry is the whole reason VK-1b is a rung of its own rather than a
 * transcription of the D3D12 leg's D12-4: VK-1a needed no transport change only
 * because VK-0 had *already* made the atlas a D3D11 texture. The planes have no
 * VK-0. So this is VK-0 again, three more times — same recipe (NT-shared
 * `SHARED | SHARED_NTHANDLE` D3D11 texture, imported as a **renderable**
 * `VkImage`), same zero copies, same fence.
 *
 * ## Not a ring, and why that is safe
 *
 * The atlas deposit is a ring because the atlas is rewritten every frame while a
 * consumer may still be reading the previous one. A plane is single-buffered,
 * exactly as the D3D11 leg's `local2d_scratch` / `backdrop_scratch` are — the
 * back-pressure is a fence edge, not a spare texture. See
 * @ref comp_vk_deposit_note_planes_consumed for the edge and why the app
 * immediate context is the only place it can be taken.
 *
 * ## Sized ONCE at the panel (the two 2D planes)
 *
 * `comp_xbridge_info::panel_width/height` documents the rule and the reason: a
 * window can never exceed the panel, so a panel-sized plane fits every region the
 * session will ever composite, and allocating once puts the planes structurally
 * outside the R2 resize hysteresis. The composite writes only the region's
 * top-left sub-rect (#464). The authored MASK is the documented exception and is
 * sized at the mask.
 * @{
 */
//! Local2D OVER flatten (RGBA). Mirrors `COMP_XBRIDGE_PLANE_LOCAL2D`.
#define COMP_VK_DEPOSIT_PLANE_LOCAL2D 0u
//! 2D-under backdrop flatten (RGBA). Mirrors `COMP_XBRIDGE_PLANE_BACKDROP`.
#define COMP_VK_DEPOSIT_PLANE_BACKDROP 1u
//! Tier-3 app-authored zone mask (R8). Mirrors `COMP_XBRIDGE_PLANE_MASK`.
#define COMP_VK_DEPOSIT_PLANE_MASK 2u
#define COMP_VK_DEPOSIT_PLANE_COUNT 3u

/*!
 * One plane surface, as both halves see it: Vulkan renders into @ref image, the
 * bridge opens @ref shared_handle. Pointers are BORROWED and live as long as the
 * deposit does.
 */
struct comp_vk_deposit_plane
{
	void *texture;       //!< `ID3D11Texture2D *` — what the bridge's producer opens.
	void *shared_handle; //!< `HANDLE` for `comp_xbridge_bind_plane`.
	uint64_t image;      //!< `VkImage` the flatten renders into.
	uint64_t view;       //!< `VkImageView` over @ref image.
	uint32_t width;      //!< Allocated extent — the PANEL for the 2D planes.
	uint32_t height;
	/*!
	 * Bumped on every REALLOCATION, never on a content change. This is what
	 * `comp_xbridge_bind_plane`'s @p generation wants: a change re-opens the
	 * handle (and drains the producer first), so bumping it per frame would
	 * re-transport the whole plane every frame.
	 */
	uint64_t generation;
};

/*!
 * Allocate (or keep) @p plane at @p width x @p height @p format.
 *
 * A no-op returning true when the plane already matches. A reallocation frees the
 * old surface and bumps @ref comp_vk_deposit_plane::generation, so it must not be
 * called at a size that changes per frame — the 2D planes pass the PANEL for
 * exactly that reason.
 *
 * @return false when the plane could not be allocated. That degrades THAT
 *         FEATURE and nothing else: the caller stops staging the plane, the
 *         recipe stamps it invalid, and the 3D weave is untouched.
 */
bool
comp_vk_deposit_plane_ensure(
    struct comp_vk_deposit *dep, uint32_t plane, uint32_t width, uint32_t height, VkFormat format);

/*!
 * Fill @p out with @p plane's surface. False when the plane is not allocated.
 */
bool
comp_vk_deposit_plane_get(struct comp_vk_deposit *dep, uint32_t plane, struct comp_vk_deposit_plane *out);

/*!
 * VK-1b — release every plane back to Vulkan, and the ONE ordering edge that
 * makes the plane transport correct on this leg.
 *
 * ## The hole this closes
 *
 * A plane's ingress is **Option I**: the bridge's producer copy queue opens the
 * app-device texture and reads it in place (see `comp_xbridge_bind_plane`). The
 * bridge's own back-fence for that — `comp_xbridge_pre_render` /
 * `comp_xbridge_pre_plane_write` — issues a GPU-side wait **on the app's D3D11
 * immediate context**, which is exactly right for the two D3D legs because the
 * immediate context is what writes their scratch. On this leg the plane is
 * written by the **Vulkan queue**, which that wait does not order at all. Left
 * alone, Vulkan's next flatten would race the producer's in-flight read and tear
 * the 2D band — the same defect class VK-1a's atlas release edge closed, one
 * level down.
 *
 * ## How it closes, without a deeper ring or a different ingress mode
 *
 * The imported semaphore is a D3D12 fence and is therefore bidirectional, so the
 * answer is one queued signal and one queued wait, as it was for the atlas:
 *
 *  1. the caller takes `comp_xbridge_pre_plane_write` for each live plane, which
 *     makes the app immediate context wait for the producer's read;
 *  2. this call signals the shared fence past every claim so far **on that same
 *     immediate context**, so the value is unreachable until (1) has resolved;
 *  3. Vulkan's next flatten submit waits for that value on the timeline —
 *     @ref comp_vk_deposit_plane_wait_value.
 *
 * The immediate context being one ordered stream is what makes a single signal
 * cover every plane, and is why this is not a per-plane call.
 *
 * No-op when the deposit has no working sync. Never a CPU wait.
 */
void
comp_vk_deposit_note_planes_consumed(struct comp_vk_deposit *dep);

/*!
 * The value Vulkan's next write to ANY plane must wait for on the timeline, or 0
 * when nothing has consumed a plane yet (warmup, or no consumer at all).
 */
uint64_t
comp_vk_deposit_plane_wait_value(struct comp_vk_deposit *dep);

/*! @} */

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
