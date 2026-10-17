// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lifted weave rects on the Vulkan weave engines (XR_DXR_lift §6,
 *         ADR-053 P5 step C): the POSIX twin of the D3D11 service's
 *         lift_weave_rect_batch / lift_weave_rects_nview +
 *         core_weave_submit_locked / core_pin_latest.
 *
 * The desktop-Linux weave engine (comp_multi_weave_linux.c) and the macOS
 * Vulkan weave backend (comp_multi_weave_macos_vk.c) call in here while they
 * record a weave, with their own command buffer. Per lifted rect:
 *
 *  1. SNAPSHOT. The rect's 2D frame (batch: the input; v6: tile 0 of the crop)
 *     is blitted into a per-stream staging image -- the letterbox-cropped
 *     active area (u_lift_letterbox), downsampled so the long edge is at most
 *     DXR_LIFT_MAX_INPUT_EDGE (u_lift_cap_dims, default 1920; halving blits, so
 *     a 2x step is an exact 2x2 box and nothing outside the rect is sampled) --
 *     and copied to a host-visible buffer in the same command buffer. When the
 *     engine reports the frame COMPLETE (its fence signalled) the pixels go
 *     into the stream's input slot (begin -> copy -> commit inside one call,
 *     never holding a slot across frames), with the frame's options, the rect's
 *     display-space frame and the DP whose eyes the stream reads. The letterbox
 *     profile of the WHOLE rect rides along: the
 *     U_LIFT_LETTERBOX_LINE_SAMPLES sampled columns and rows are copied to a
 *     second buffer and reduced on the CPU at completion
 *     (u_lift_letterbox_profile_from_lines -- the D3D11 shader's profile).
 *  2. RESULT. The stream's latest result is pinned (held until the frame
 *     completes, so the provider can never overwrite what the GPU still reads)
 *     and each needed view is copied 1:1 into a per-view UNORM staging image
 *     (raw bytes: an _SRGB result stays encoded, no decode in a blit) and
 *     stretched from there into the rect's ACTIVE area of every weave tile. On
 *     the batch layout the letterbox bars are the 2D input, flat (the same
 *     pixels in both tiles), tiling the rect exactly around the active area.
 *  3. FLAT until the first result: the batch layout writes the 2D frame into
 *     both tiles; v6 leaves the caller's identical tiles as drawn.
 *
 * Providers on POSIX run on the CPU lane (vk_conv_gpu.h), so a stream's input
 * slot is CPU memory: the snapshot is GPU -> host buffer -> slot. A GPU slot
 * (a provider kind raised with vk_conv_gpu_set_provider_kinds) is filled from
 * the same host buffer through vk_conv_gpu_write, synchronously, at completion.
 * Results are GPU images on the same device (S5 render targets, #2030
 * mirrors), sampled in place.
 *
 * comp_multi does not link this library (the runtime client links comp_multi
 * too): the IPC layer installs vk_conv_weave_get_ops' table into comp_multi
 * (comp_multi_weave_set_lift_ops), the D3D11 service's set_bridge pattern.
 *
 * Threads: a client's calls come from its IPC thread under the engine's lock;
 * clients are independent. Never logs per frame.
 *
 * @ingroup comp_vk_conv
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_lift.h"
#include "xrt/xrt_vulkan_includes.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct conv_core;
struct vk_bundle;
struct vk_conv_gpu;
struct vk_conv_weave;
struct vk_conv_weave_client;

/*!
 * The weave path's bridge to core-routed streams (the IPC conversion module's
 * resolver; comp_d3d11_conv_bridge's twin).
 */
struct vk_conv_weave_bridge
{
	struct conv_core *core;
	//! A wire stream id of connection @p owner -> its core stream and v3 mode. False = not ours.
	bool (*resolve)(void *ud, uint64_t owner, uint64_t wire_id, uint64_t *out_core_id, uint32_t *out_mode);
	//! Spec 4 viewCount 0: the weave's view count for a lifted rect's stream (may be NULL).
	void (*match_weave)(void *ud, uint64_t owner, uint64_t wire_id, uint32_t views);
	/*!
	 * A rect frame without window metrics (valid = false, the panel's size) ->
	 * the rect's own aspect fitted in the panel (ipc_server_conv_submit_rect,
	 * the windowless spec-4 rule). May be NULL: the frame stays unknown.
	 */
	bool (*fit_rect)(const struct xrt_lift_rect_frame *frame,
	                 uint32_t extent_w,
	                 uint32_t extent_h,
	                 struct xrt_lift_rect_frame *out);
	void *ud;
};

//! An image of the weave engine as the helper reads / writes it this frame.
struct vk_conv_weave_image
{
	VkImage image;
	VkImageLayout layout; //!< the layout it is in; reads AND writes use it (GENERAL when src == dst)
	VkFormat format;      //!< a 4-byte RGBA / BGRA format
	uint32_t width, height;
};

/*!
 * One lifted rect of one weave. The rect is in @ref src pixels; tile v of
 * @ref dst starts at ((v % tile_columns) * tile_w, (v / tile_columns) * tile_h)
 * and the rect sits at the same (x, y) inside every tile (batch: the 2x1 SBS
 * scratch, tiles = window-sized; v6: the crop, tiles = content views).
 */
struct vk_conv_weave_rect
{
	const struct xrt_lift_weave_rect *binding; //!< the stream + the rect's options
	uint64_t owner;                            //!< the connection (lift owner) the stream belongs to

	struct vk_conv_weave_image src; //!< the 2D frame (batch: the input; v6: the crop)
	int32_t x, y;                   //!< rect origin in @ref src (clipped to it)
	uint32_t w, h;                  //!< rect extent

	struct vk_conv_weave_image dst; //!< the weave atlas (may be the same image as @ref src)
	uint32_t views;                 //!< tiles = the weave's views (2 on the batch layout)
	uint32_t tile_columns;
	uint32_t tile_w, tile_h;

	//! The batch (v3) layout: tiles get the result's middle stereo pair, and the
	//! flat 2D frame (no result yet) and the letterbox bars come from @ref src.
	//! v6: false (view v -> tile v, proportionally; the tiles already hold the
	//! caller's flat frame).
	bool batch;

	//! Where the rect is (display space) and whose eyes its stream reads.
	struct xrt_lift_rect_frame frame;
};

//! The table comp_multi calls through (comp_multi_weave_set_lift_ops).
struct vk_conv_weave_ops
{
	void *ud; //!< the struct vk_conv_weave
	//! v3 mode (XRT_DP_LIFT_MODE_*) of @p owner's stream @p wire_id, 0 = not a core-routed stream.
	uint32_t (*stream_mode)(void *ud, uint64_t owner, uint64_t wire_id);
	//! Per weave engine; @p vk must be the helper's device (NULL otherwise).
	struct vk_conv_weave_client *(*client_create)(void *ud, struct vk_bundle *vk);
	//! The engine's GPU work is idle. Aborts what was never completed.
	void (*client_destroy)(void *ud, struct vk_conv_weave_client *c);
	//! A new weave starts recording: everything still pending was never submitted (dropped).
	void (*begin)(void *ud, struct vk_conv_weave_client *c);
	//! Record one lifted rect into @p cmd. False = not lifted (weave the rect as drawn).
	bool (*record)(void *ud,
	               struct vk_conv_weave_client *c,
	               VkCommandBuffer cmd,
	               const struct vk_conv_weave_rect *r);
	//! The GPU finished everything recorded since begin: commit snapshots, read profiles, unpin.
	void (*complete)(void *ud, struct vk_conv_weave_client *c);
};

/*!
 * Create the helper on @p vk (the glue's device; borrowed) over @p glue
 * (borrowed: result images are its images). Reads DXR_LIFT_MAX_INPUT_EDGE and
 * DXR_LIFT_LETTERBOX once.
 */
bool
vk_conv_weave_create(struct vk_bundle *vk, struct vk_conv_gpu *glue, struct vk_conv_weave **out);

/*!
 * Waits the device's main queue idle, then frees every client still alive
 * (aborting their pending snapshots, releasing their pins). Call after
 * comp_multi let go of the ops and before the core's streams go.
 */
void
vk_conv_weave_destroy(struct vk_conv_weave **w_ptr);

//! Install (NULL: clear) the bridge.
void
vk_conv_weave_set_bridge(struct vk_conv_weave *w, const struct vk_conv_weave_bridge *bridge);

void
vk_conv_weave_get_ops(struct vk_conv_weave *w, struct vk_conv_weave_ops *out);

//! @name The ops, directly (tests)
//! @{
uint32_t
vk_conv_weave_stream_mode(struct vk_conv_weave *w, uint64_t owner, uint64_t wire_id);

struct vk_conv_weave_client *
vk_conv_weave_client_create(struct vk_conv_weave *w, struct vk_bundle *vk);

void
vk_conv_weave_client_destroy(struct vk_conv_weave *w, struct vk_conv_weave_client *c);

void
vk_conv_weave_begin(struct vk_conv_weave *w, struct vk_conv_weave_client *c);

bool
vk_conv_weave_record(struct vk_conv_weave *w,
                     struct vk_conv_weave_client *c,
                     VkCommandBuffer cmd,
                     const struct vk_conv_weave_rect *r);

void
vk_conv_weave_complete(struct vk_conv_weave *w, struct vk_conv_weave_client *c);
//! @}

//! Diagnostics (tests): what the last record of @p wire_id did.
struct vk_conv_weave_stats
{
	uint32_t snap_w, snap_h;     //!< the snapshot extent sent to the stream (after crop + cap)
	uint32_t active_w, active_h; //!< the rect area it was taken from (after the letterbox crop)
	bool woven_result;           //!< the stream's result was woven (false = flat)
	uint32_t result_views;       //!< views of that result
	uint64_t committed;          //!< snapshots committed to the stream so far
};

bool
vk_conv_weave_client_stats(struct vk_conv_weave_client *c, uint64_t wire_id, struct vk_conv_weave_stats *out);

#ifdef __cplusplus
}
#endif
