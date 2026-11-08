// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 arm 2: the in-process share tap at the D3D11 fill arm — the
 *         hook surface the fill arm calls (Windows body: comp_xbridge_share_tap_win.cpp).
 * @ingroup comp_xbridge
 *
 * An in-process session's pre-weave views reach a 3D-screen-share consumer
 * through the session's service link (ipc_client_service_link, ADR-049
 * Amendment 1): the service queues SHARE_REQUESTED, the app copies its views
 * into a 3-texture NT-shared ring with a shared fence and posts one
 * frame_ready per frame, and the service imports the ring and runs the same
 * NV12 pass as its own share tap (ADR-061 decisions 6, 9). On Windows the
 * D3D11, D3D12 and Vulkan in-process tiers all feed the ADR-059 D3D11 fill
 * arm, so that is the ONE tap point; these three calls are its contract.
 * They reach the link only through util/u_share_link.h (the compositor layer
 * never links IPC).
 *
 * @par Contract (what the Windows body must do)
 *
 * - **xbridge_share_tap_begin** — once, when the fill arm engages for a
 *   session (compositor create / tier engage). Decide whether this session
 *   can be tapped and publish it with u_share_link_set_tap(): available + the
 *   LUID of @ref comp_xbridge_share_tap_info::device's adapter, or not
 *   available with the reason (enum xrt_app_share_availability):
 *   UNSUPPORTED_API (GL), SHARED_TEXTURE (a texture-class session the fill
 *   arm declines without DXR_HUB=1), SPLIT_DISABLED
 *   (DXR_SPLIT_SAME_ADAPTER=0 / DXR_WEAVE_ON_SCANOUT=0), LEGACY_STANDALONE,
 *   OTHER_ADAPTER (neither fill-arm device is on the service's adapter — the
 *   service checks the LUID too). Allocates nothing GPU-side: the ring is
 *   created on the first SHARE_REQUESTED. Returns XRT_SUCCESS and a tap, or
 *   XRT_ERROR_FEATURE_NOT_SUPPORTED and NULL ("no tap" — every caller treats
 *   that as "never call commit").
 *
 * - **xbridge_share_tap_commit** — once per committed frame, on the fill
 *   arm's thread, AFTER the pre-weave atlas is final (cropped per ADR-030,
 *   the atlas the DP will weave) and before the DP consumes it. Never waits
 *   on the GPU, never blocks on the service:
 *   1. u_share_link_poll() (cheap: one IPC round trip at most every 250 ms;
 *      every frame_ready reply refreshes it). NONE → return. SHARE_STOPPED →
 *      release the ring, return.
 *   2. SHARE_REQUESTED: decimate to the poll's fps (skip frames earlier than
 *      1/fps after the last published one).
 *   3. (Re)create the ring when there is none, its stream_generation differs
 *      from the poll's, or the per-view content size changed: 3 textures
 *      `2·content_w × content_h`, typed (UNORM / UNORM_SRGB as the atlas's
 *      encoding says — never TYPELESS), `D3D11_RESOURCE_MISC_SHARED |
 *      SHARED_NTHANDLE` (no keyed mutex), plus one `ID3D11Fence` created
 *      `D3D11_FENCE_FLAG_SHARED`, all on the device that is on the service's
 *      adapter. Bump the ring generation and u_share_link_publish_ring()
 *      with the 3 NT handles + the fence handle (the transport duplicates
 *      them; the tap keeps and later closes its own).
 *   4. Pick the views: the middle pair when view_count > 2, view 0 twice
 *      when one view is active (ADR-041, mono = 1). CopySubresourceRegion
 *      each view's content rect (tile grid × content dims, at the atlas's
 *      slot stride) into the left / right half of slot N — no scaling, no
 *      shader: the service scales and letterboxes.
 *   5. Signal the fence with ++value on the same context, then
 *      u_share_link_frame_ready({ring generation, N, value, commit_ns,
 *      zero_disparity_shift_frac (0 in v1), mono, content_generation}).
 *      N cycles 0,1,2: latest-wins, a slot the service has not read yet is
 *      simply overwritten (the service waits the fence value GPU-side).
 *   6. A frame_ready that fails (link gone) releases the ring; the next
 *      SHARE_REQUESTED makes a new one.
 *
 * - **xbridge_share_tap_end** — when the fill arm disengages / the compositor
 *   is destroyed: release the ring and fence (no wait: the service holds its
 *   own references), u_share_link_set_tap(false, NO_TAP), free the tap.
 *
 * Off Windows begin reports "no tap" and the session is listed UNAVAILABLE
 * (no_tap) by the service. On Windows a fill arm that never calls begin (a
 * D3D12 or Vulkan tier on its own legs, not the hub) reports the same.
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_xbridge_share_tap;

//! What the fill arm knows when it engages.
struct comp_xbridge_share_tap_info
{
	void *device;  //!< `ID3D11Device *` the ring would live on (Windows; NULL elsewhere)
	void *context; //!< its immediate context
	uint8_t luid[XRT_LUID_SIZE];
	bool luid_valid;
	uint32_t tier; //!< enum xrt_share_link_tier of the session
	//! Non-zero: the fill arm already knows it declines (enum xrt_app_share_availability).
	uint32_t unavailable_reason;
};

//! One committed frame: the pre-weave atlas as the DP will weave it.
struct comp_xbridge_share_tap_frame
{
	void *atlas; //!< `ID3D11Texture2D *` (typed or TYPELESS; the tap copies, never views)
	uint32_t atlas_width, atlas_height;
	uint32_t format;                        //!< DXGI_FORMAT of @ref atlas
	uint32_t tile_columns, tile_rows;       //!< the atlas grid
	uint32_t view_count;                    //!< the fixed view count (ADR-041)
	uint32_t active_view_count;             //!< 1 = mono
	uint32_t content_width, content_height; //!< per-view content dims of the active mode (ADR-030)
	bool flip_y;
	bool encoding_is_linear;
	int64_t commit_ns; //!< os_monotonic
	float zero_disparity_shift_frac;
	/*!
	 * Where tiles sit. The hub's producers pack them at the view size (the hub
	 * crops the top-left content box); 0 = the slot stride, atlas / grid.
	 */
	uint32_t tile_stride_x, tile_stride_y;
	void *wait_fence;          //!< `ID3D11Fence *` to wait before reading @ref atlas, or NULL
	uint64_t wait_fence_value; //!< the value @ref wait_fence reaches once the atlas is written
	void *keyed_mutex;         //!< `IDXGIKeyedMutex *` guarding @ref atlas (key 0), or NULL
};

/*!
 * The fill arm engaged for a session: decide and publish the tap state.
 * @return XRT_SUCCESS + a tap, or XRT_ERROR_FEATURE_NOT_SUPPORTED + NULL (no tap).
 */
xrt_result_t
xbridge_share_tap_begin(const struct comp_xbridge_share_tap_info *info, struct comp_xbridge_share_tap **out_tap);

/*!
 * One committed frame (see the contract above). Cheap when nobody shares.
 * XRT_ERROR_FEATURE_NOT_SUPPORTED with a NULL tap.
 */
xrt_result_t
xbridge_share_tap_commit(struct comp_xbridge_share_tap *tap, const struct comp_xbridge_share_tap_frame *frame);

//! Release the ring, publish "no tap", free. NULL-safe; clears @p *tap_ptr.
void
xbridge_share_tap_end(struct comp_xbridge_share_tap **tap_ptr);

#ifdef __cplusplus
}
#endif
