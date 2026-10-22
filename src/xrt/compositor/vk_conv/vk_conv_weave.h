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
 * slot is CPU memory: the snapshot is GPU -> host buffer -> slot. When the
 * stream's binding takes a GPU kind the glue would give its slot (a provider
 * kind raised with vk_conv_gpu_set_provider_kinds; ADR-053 P5 step E) no host
 * copy is recorded: at completion the glue copies the chain's last image into
 * the GPU slot (vk_conv_gpu_copy_from_vk), synchronously.
 * Results are GPU images on the same device (S5 render targets, #2030
 * mirrors), sampled in place.
 *
 * comp_multi does not link this library (the runtime client links comp_multi
 * too): the IPC layer installs vk_conv_weave_get_ops' table into comp_multi
 * (comp_multi_weave_set_lift_ops), the D3D11 service's set_bridge pattern.
 *
 * HOST CLIENTS (an engine that records on another graphics API: the macOS
 * Metal weave backend). A client created with vk == NULL never records Vulkan
 * commands. Per lifted rect the engine asks vk_conv_weave_host_plan what to
 * do -- the same stream policy as vk_conv_weave_record (letterbox crop, cap,
 * frame, pin) -- records the snapshot blit, the letterbox lines and the result
 * draw itself, and once its GPU work completed hands its CPU-visible copies
 * over with vk_conv_weave_host_done, then calls complete as usual. The result
 * is reachable only as the IOSurface the glue exported it as (a device-private
 * or layered result is woven flat, "failed: export"). On macOS the glue hands
 * an S5 result over asynchronously: the plan names the MTLSharedEvent value
 * its Vulkan write signals (result_event), and the engine encodes a GPU wait
 * on it before sampling; a result planned without an event was complete
 * before it was published (SYNC_NONE). The pin keeps the provider off it until
 * complete.
 *
 * Threads: a client's calls come from its IPC thread under the engine's lock;
 * clients are independent. Never logs per frame.
 *
 * @ingroup comp_vk_conv
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_lift.h"
#include "xrt/xrt_conversion_plugin.h"
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

	/*!
	 * ADR-058 Amendment 1: how the late view synthesis places the rect this
	 * frame. 0 = the stream's eyes snapshot (no engine sample); 1 = the
	 * frame's ONE target-time sample @ref frame_eyes, the same the weave and
	 * its reply consume; 2 = keep the conversion's viewpoints (a straddling
	 * rect whose centre screen is not the sample's). = CONV_LATE_EYES_*.
	 */
	uint32_t eyes_mode;
	struct xrt_eye_positions frame_eyes;
};

/*!
 * What a host client's engine records for one lifted rect (vk_conv_weave_host_plan).
 * All coordinates are source pixels; tile v of the destination is laid out as
 * in @ref vk_conv_weave_rect.
 */
struct vk_conv_weave_host_plan
{
	//! The rect clipped to the source. w == 0: nothing visible, nothing to draw.
	int32_t x, y;
	uint32_t w, h;

	//! Snapshot: the source's (cx, cy, cw, ch) resampled to dw x dh, delivered
	//! as R G B A bytes, rows tight (dw * 4). Halving linear steps while more
	//! than 2x away (an exact 2x2 box), then one linear step to dw x dh (texel
	//! exact when 1:1). Never samples outside (cx, cy, cw, ch).
	bool snapshot;
	uint32_t cx, cy, cw, ch;
	uint32_t dw, dh;

	//! Letterbox lines: U_LIFT_LETTERBOX_LINE_SAMPLES columns (column k at
	//! x + u_lift_letterbox_sample_pos(k, w), h texels each) then as many rows
	//! (row k at y + u_lift_letterbox_sample_pos(k, h), w texels each), back to
	//! back, 4 bytes a texel in the SOURCE's channel order (its src.format).
	bool lines;

	//! Result: pinned until complete. Draw view tile_view[v] of it -- views
	//! side by side, result_w / result_views wide each, never sampled across a
	//! view's edge -- stretched
	//! over (ax0, ay0)-(ax1, ay1) of tile v. Batch: the rest of the rect (the
	//! letterbox bars) is the source, 1:1, in every tile. No result: batch =
	//! the rect of the source, 1:1, in every tile (flat); v6 = nothing.
	bool result;
	void *result_iosurface; //!< IOSurfaceRef, the glue's; valid until complete
	//! XRT_CONV_FMT_* (8-bit RGBA / BGRA, maybe _SRGB: sample the raw bytes,
	//! no decode). Take the texture format from HERE: an IOSurface the
	//! Vulkan driver created may report pixel format 0.
	uint32_t result_format;
	uint32_t result_w, result_h, result_views;
	int32_t ax0, ay0, ax1, ay1;
	//! The result view of weave tile v (batch: the middle stereo pair; v6:
	//! proportional), for v < min(views, XRT_CONV_MAX_VIEWS).
	uint32_t tile_view[XRT_CONV_MAX_VIEWS];
	/*!
	 * ADR-053 P5 step E: the result's Vulkan write completes when this
	 * id<MTLSharedEvent> reaches @ref result_event_value -- encode a GPU wait
	 * on it before sampling the result (NULL: complete when planned, the
	 * glue's SYNC_NONE hand-over). The glue owns it; valid until complete.
	 */
	void *result_event;
	uint64_t result_event_value;
};

//! A host client's frame completed (vk_conv_weave_host_done), per planned stream.
struct vk_conv_weave_host_done
{
	//! The planned snapshot (dw x dh R G B A, tight), NULL = not made.
	const void *snapshot;
	//! The planned letterbox lines, NULL = not made.
	const void *lines;
	//! The result was drawn: its extent and views (0 = woven flat).
	uint32_t woven_w, woven_h, woven_views;
};

//! The table comp_multi calls through (comp_multi_weave_set_lift_ops).
struct vk_conv_weave_ops
{
	void *ud; //!< the struct vk_conv_weave
	//! v3 mode (XRT_DP_LIFT_MODE_*) of @p owner's stream @p wire_id, 0 = not a core-routed stream.
	uint32_t (*stream_mode)(void *ud, uint64_t owner, uint64_t wire_id);
	//! Per weave engine; @p vk must be the helper's device (NULL otherwise), or NULL: a host client.
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
	//! Host clients (vk == NULL at client_create): see vk_conv_weave_host_plan.
	bool (*host_plan)(void *ud,
	                  struct vk_conv_weave_client *c,
	                  const struct vk_conv_weave_rect *r,
	                  struct vk_conv_weave_host_plan *out);
	//! Host clients: see vk_conv_weave_host_done.
	void (*host_done)(void *ud,
	                  struct vk_conv_weave_client *c,
	                  uint64_t owner,
	                  uint64_t wire_id,
	                  const struct vk_conv_weave_host_done *d);
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

/*!
 * Host client: the plan for lifted rect @p r of the weave being recorded (the
 * images in @p r are ignored; src.width / height / format describe the 2D
 * frame). Resolves the stream, decides the snapshot (crop + cap) and pins the
 * latest result as vk_conv_weave_record does. False = not lifted (weave the
 * rect as drawn).
 */
bool
vk_conv_weave_host_plan(struct vk_conv_weave *w,
                        struct vk_conv_weave_client *c,
                        const struct vk_conv_weave_rect *r,
                        struct vk_conv_weave_host_plan *out);

/*!
 * Host client: the GPU work for @p owner's stream @p wire_id completed. The
 * pointers in @p d must stay valid until vk_conv_weave_complete returns, which
 * the engine calls next (it commits the snapshot and reads the lines).
 */
void
vk_conv_weave_host_done(struct vk_conv_weave *w,
                        struct vk_conv_weave_client *c,
                        uint64_t owner,
                        uint64_t wire_id,
                        const struct vk_conv_weave_host_done *d);
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
