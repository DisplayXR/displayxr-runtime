// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lifted weave rects on the Vulkan weave engines — see vk_conv_weave.h.
 *
 * Layouts: every helper-owned image (snapshot chain, result staging) is
 * written whole every use, so each use transitions it from UNDEFINED; the
 * engine's images stay in the layout the engine said they are in. Ordering
 * against the engine's own commands and against earlier queue submissions
 * (the glue's result writes) is a global memory barrier at the start of each
 * record; the snapshot's reads of a v6 crop finish before the crop's tiles
 * are overwritten (a second barrier), and the host reads the buffers only
 * after the engine's fence (a TRANSFER -> HOST barrier closes each record).
 *
 * @ingroup comp_vk_conv
 */

#include "vk_conv_weave.h"
#include "vk_conv_gpu.h"

#include "xrt/xrt_dp_lift.h"
#include "xrt/xrt_conversion_plugin.h"

#include "conversion/conv_core.h"
#include "conversion/conv_stream.h"

#include "vk/vk_helpers.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_lift_mailbox.h"
#include "util/u_lift_rig.h"
#include "util/u_lift_viewpoint.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Structs.
 *
 */

//! Snapshot chain depth: the 1:1 / final image plus halving steps (32x reduction).
#define VKCW_CHAIN_MAX 6u

//! Pins one client holds per frame (one per lifted rect).
#define VKCW_PINS_MAX XRT_LIFT_WEAVE_RECTS_MAX

//! Completes without use after which a stream's images go.
#define VKCW_IDLE_COMPLETES 600u

struct vkcw_img
{
	VkImage image;
	VkDeviceMemory mem;
	VkFormat format;
	uint32_t w, h;
};

struct vkcw_buf
{
	VkBuffer buf;
	VkDeviceMemory mem;
	void *map;
	VkDeviceSize size;
};

struct vkcw_stream
{
	struct vkcw_stream *next;
	uint64_t wire;
	uint64_t owner;

	//! Recorded in the frame being built (one snapshot per stream per frame).
	bool used_this_frame;
	uint32_t idle;

	//! @name Snapshot
	//! @{
	struct vkcw_img chain[VKCW_CHAIN_MAX];
	struct vkcw_buf snap_buf;
	bool snap_pending;
	//! Step E: this frame's snapshot stays on the GPU (the slot will be a
	//! glue image): no host copy recorded, complete copies chain[snap_level].
	bool snap_gpu;
	uint32_t snap_level;
	uint32_t snap_w, snap_h; //!< after crop + cap
	uint32_t act_w, act_h;   //!< the cropped area (before the cap)
	struct conv_frame_submit fr;
	uint32_t cap_logged_w, cap_logged_h;
	struct xrt_dp_lift_params last_params;
	bool has_last_params;
	uint64_t committed;
	//! @}

	//! @name Letterbox (whole rect, line samples)
	//! @{
	struct u_lift_letterbox lb;
	struct vkcw_buf lb_buf;
	bool lb_pending;
	uint32_t lb_w, lb_h;
	bool lb_bgra;
	//! @}

	//! Result staging, per result view.
	struct vkcw_img res[XRT_CONV_MAX_VIEWS];

	//! @name Host client: the engine's own copies, valid from host_done to complete
	//! @{
	const void *host_snap;
	const void *host_lines;
	bool logged_export;
	//! @}

	//! @name Diagnostics of the last record
	//! @{
	bool woven_result;
	uint32_t result_views;
	bool fmt_warned;
	bool logged_woven;
	uint32_t logged_res_w, logged_res_h, logged_res_views;
	bool logged_unresolved;
	const char *logged_fail; //!< the stage of the last failure logged (NULL = none since)
	                         //! @}
};

struct vkcw_pin
{
	uint64_t core_id;
	uint32_t ring_slot;
};

struct vk_conv_weave_client
{
	struct vk_conv_weave_client *next;
	//! Created without a device: the engine records on another graphics API
	//! (vk_conv_weave_host_plan / _host_done); never vk_conv_weave_record.
	bool host;
	struct vkcw_stream *streams;
	struct vkcw_pin pins[VKCW_PINS_MAX];
	uint32_t pin_count;
	//! Letterbox profile scratch (complete only).
	float lb_rows[U_LIFT_LETTERBOX_BINS_MAX];
	float lb_cols[U_LIFT_LETTERBOX_BINS_MAX];
};

struct vk_conv_weave
{
	struct vk_bundle *vk;
	struct vk_conv_gpu *glue;
	uint32_t max_input_edge;
	bool letterbox;

	//! Guards the bridge and the client list (a leaf).
	struct os_mutex mtx;
	struct vk_conv_weave_bridge bridge;
	struct vk_conv_weave_client *clients;
};


/*
 *
 * Small helpers.
 *
 */

static struct vk_conv_weave_bridge
bridge_get(struct vk_conv_weave *w)
{
	os_mutex_lock(&w->mtx);
	struct vk_conv_weave_bridge b = w->bridge;
	os_mutex_unlock(&w->mtx);
	return b;
}

static bool
bridge_resolve(const struct vk_conv_weave_bridge *b, uint64_t owner, uint64_t wire, uint64_t *out_core_id)
{
	uint32_t mode = 0;
	return b->core != NULL && b->resolve != NULL && b->resolve(b->ud, owner, wire, out_core_id, &mode);
}

//! The UNORM alias of a 4-byte colour format; UNDEFINED for anything else.
static VkFormat
unorm4(VkFormat f)
{
	switch (f) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
	default: return VK_FORMAT_UNDEFINED;
	}
}

static bool
is_bgra(VkFormat f)
{
	return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

static void
img_free(struct vk_bundle *vk, struct vkcw_img *i)
{
	if (i->image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, i->image, NULL);
	}
	if (i->mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, i->mem, NULL);
	}
	memset(i, 0, sizeof(*i));
}

//! (Re)create @p i at @p w x @p h of @p format (transfer src + dst). GPU idle for its old contents.
static bool
img_ensure(struct vk_bundle *vk, struct vkcw_img *i, uint32_t w, uint32_t h, VkFormat format)
{
	if (i->image != VK_NULL_HANDLE && i->w == w && i->h == h && i->format == format) {
		return true;
	}
	img_free(vk, i);
	VkExtent2D ext = {w, h};
	if (vk_create_image_simple(vk, ext, format, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                           &i->mem, &i->image) != VK_SUCCESS) {
		memset(i, 0, sizeof(*i));
		return false;
	}
	i->w = w;
	i->h = h;
	i->format = format;
	return true;
}

static void
buf_free(struct vk_bundle *vk, struct vkcw_buf *b)
{
	if (b->map != NULL) {
		vk->vkUnmapMemory(vk->device, b->mem);
	}
	if (b->buf != VK_NULL_HANDLE) {
		vk->vkDestroyBuffer(vk->device, b->buf, NULL);
	}
	if (b->mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, b->mem, NULL);
	}
	memset(b, 0, sizeof(*b));
}

//! A host-visible, coherent (cached when available) transfer-destination buffer of at least @p size.
static bool
buf_ensure(struct vk_bundle *vk, struct vkcw_buf *b, VkDeviceSize size)
{
	if (b->buf != VK_NULL_HANDLE && b->size >= size) {
		return true;
	}
	buf_free(vk, b);
	VkBufferCreateInfo bci = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = size,
	    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (vk->vkCreateBuffer(vk->device, &bci, NULL, &b->buf) != VK_SUCCESS) {
		b->buf = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements req;
	vk->vkGetBufferMemoryRequirements(vk->device, b->buf, &req);
	uint32_t type = 0;
	if (!vk_get_memory_type(vk, req.memoryTypeBits,
	                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
	                        &type) &&
	    !vk_get_memory_type(vk, req.memoryTypeBits,
	                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
		buf_free(vk, b);
		return false;
	}
	VkMemoryAllocateInfo ai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = req.size,
	    .memoryTypeIndex = type,
	};
	if (vk->vkAllocateMemory(vk->device, &ai, NULL, &b->mem) != VK_SUCCESS) {
		b->mem = VK_NULL_HANDLE;
		buf_free(vk, b);
		return false;
	}
	if (vk->vkBindBufferMemory(vk->device, b->buf, b->mem, 0) != VK_SUCCESS ||
	    vk->vkMapMemory(vk->device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) {
		b->map = NULL;
		buf_free(vk, b);
		return false;
	}
	b->size = size;
	return true;
}

static void
stream_free(struct vk_bundle *vk, struct vkcw_stream *st)
{
	for (uint32_t i = 0; i < VKCW_CHAIN_MAX; i++) {
		img_free(vk, &st->chain[i]);
	}
	for (uint32_t i = 0; i < XRT_CONV_MAX_VIEWS; i++) {
		img_free(vk, &st->res[i]);
	}
	buf_free(vk, &st->snap_buf);
	buf_free(vk, &st->lb_buf);
	free(st);
}

static struct vkcw_stream *
stream_get(struct vk_conv_weave_client *c, uint64_t owner, uint64_t wire, bool create)
{
	struct vkcw_stream *st = c->streams;
	while (st != NULL && (st->wire != wire || st->owner != owner)) {
		st = st->next;
	}
	if (st == NULL && create) {
		st = U_TYPED_CALLOC(struct vkcw_stream);
		st->wire = wire;
		st->owner = owner;
		st->next = c->streams;
		c->streams = st;
	}
	return st;
}

static void
barrier_image(struct vk_bundle *vk,
              VkCommandBuffer cmd,
              VkImage image,
              VkImageLayout from,
              VkImageLayout to,
              VkAccessFlags src_access,
              VkAccessFlags dst_access)
{
	VkImageMemoryBarrier b = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = src_access,
	    .dstAccessMask = dst_access,
	    .oldLayout = from,
	    .newLayout = to,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
	                         NULL, 1, &b);
}

static void
barrier_global(struct vk_bundle *vk,
               VkCommandBuffer cmd,
               VkPipelineStageFlags src_stage,
               VkAccessFlags src_access,
               VkPipelineStageFlags dst_stage,
               VkAccessFlags dst_access)
{
	VkMemoryBarrier mb = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
	    .srcAccessMask = src_access,
	    .dstAccessMask = dst_access,
	};
	vk->vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &mb, 0, NULL, 0, NULL);
}

//! A blit of src (x, y, w, h) to dst (x, y, w, h); both images already in their layouts.
static void
blit(struct vk_bundle *vk,
     VkCommandBuffer cmd,
     VkImage src,
     VkImageLayout src_layout,
     int32_t sx,
     int32_t sy,
     int32_t sw,
     int32_t sh,
     VkImage dst,
     VkImageLayout dst_layout,
     int32_t dx,
     int32_t dy,
     int32_t dw,
     int32_t dh,
     VkFilter filter)
{
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
		return;
	}
	const VkImageSubresourceLayers layer = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	VkImageBlit b = {
	    .srcSubresource = layer,
	    .srcOffsets = {{sx, sy, 0}, {sx + sw, sy + sh, 1}},
	    .dstSubresource = layer,
	    .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}},
	};
	vk->vkCmdBlitImage(cmd, src, src_layout, dst, dst_layout, 1, &b, filter);
}

//! The stereo pair out of an N-view result: the middle two views (the D3D11 batch rule).
static void
pick_pair(uint32_t views, uint32_t *out_l, uint32_t *out_r)
{
	if (views <= 2) {
		*out_l = 0;
		*out_r = views == 2 ? 1 : 0;
		return;
	}
	*out_l = views / 2 - 1;
	*out_r = views / 2;
}

//! Result view for weave tile @p v (the D3D11 rule: batch = the middle pair, v6 = proportional).
static uint32_t
src_view_for(bool batch, uint32_t v, uint32_t weave_views, uint32_t result_views)
{
	if (batch) {
		uint32_t l = 0, r = 0;
		pick_pair(result_views, &l, &r);
		return v == 0 ? l : r;
	}
	if (result_views == weave_views) {
		return v;
	}
	return weave_views > 1 ? (uint32_t)((float)v * (float)(result_views - 1) / (float)(weave_views - 1) + 0.5f)
	                       : result_views / 2;
}

static void
pins_release(struct vk_conv_weave_client *c, const struct vk_conv_weave_bridge *b)
{
	for (uint32_t i = 0; i < c->pin_count; i++) {
		if (b->core != NULL) {
			conv_core_stream_release(b->core, c->pins[i].core_id, c->pins[i].ring_slot);
		}
	}
	c->pin_count = 0;
}


/*
 *
 * Snapshot recording.
 *
 */

/*!
 * The rect's frame for the core (the D3D11 core_weave_submit_locked policy):
 * sticky options, explicit viewpoints, view control, rig, the display-space
 * rect and its DP, the letterbox active area, focal scaled with the cap.
 */
static void
frame_fill(struct vkcw_stream *st,
           const struct vk_conv_weave_bridge *br,
           const struct vk_conv_weave_rect *r,
           uint32_t cx,
           uint32_t cy,
           uint32_t cw,
           uint32_t ch,
           uint32_t dw,
           bool capped)
{
	const struct xrt_lift_weave_rect *b = r->binding;
	struct conv_frame_submit *fr = &st->fr;
	memset(fr, 0, sizeof(*fr));
	if (b->has_params) {
		st->last_params = b->params;
		st->has_last_params = true;
	}
	struct xrt_dp_lift_params p;
	memset(&p, 0, sizeof(p));
	p.convergence = -1.0f;
	p.strength = 1.0f;
	p.inpaint = 1;
	if (st->has_last_params) {
		p = st->last_params;
	}
	fr->source_time = (int64_t)os_monotonic_get_ns();
	fr->frame_flags = p.inpaint != 0 ? XRT_CONV_FRAME_INPAINT : 0u;
	uint32_t n = b->viewpoint_floats / 3;
	n = n > XRT_CONV_MAX_VIEWS ? XRT_CONV_MAX_VIEWS : n;
	if (n > 0) {
		fr->explicit_count = n;
		memcpy(fr->explicit_xyz, b->viewpoints, sizeof(float) * 3 * n);
	}
	if (b->has_view_control) {
		fr->has_view_control = true;
		u_lift_view_control_default(&fr->vc);
		fr->vc.ipd_factor = b->view_control.ipd_factor;
		fr->vc.parallax_factor = b->view_control.parallax_factor;
		fr->vc.axis_mode = b->view_control.axis_mode;
		fr->vc.max_offset_m = b->view_control.max_offset_m;
		fr->vc.recenter_mode = b->view_control.recenter_mode;
		if (b->view_control.hold_s >= 0.0f) {
			fr->vc.hold_s = b->view_control.hold_s;
		}
		if (b->view_control.tau_s > 0.0f) {
			fr->vc.tau_s = b->view_control.tau_s;
		}
	}
	if (b->rig.type != XRT_LIFT_RIG_NONE) {
		fr->rig.type = b->rig.type;
		fr->rig.ipd_factor = b->rig.ipd_factor;
		fr->rig.parallax_factor = b->rig.parallax_factor;
		fr->rig.perspective_factor = b->rig.perspective_factor;
		fr->rig.inv_convergence_distance = b->rig.inv_convergence_distance;
		fr->rig.half_tan_vfov = b->rig.half_tan_vfov;
		fr->rig.m2v = b->rig.m2v;
		u_lift_rig_sanitize(&fr->rig);
	}
	// No window metrics (no bound geometry): the rect's own aspect fitted in
	// the panel, as a windowless spec-4 submit gets.
	struct xrt_lift_rect_frame rf = r->frame;
	if (!rf.valid && br->fit_rect != NULL) {
		(void)br->fit_rect(&r->frame, r->w, r->h, &rf);
	}
	if (rf.valid) {
		fr->rect_valid = true;
		memcpy(fr->rect_center, rf.center, sizeof(fr->rect_center));
		fr->rect_w_m = rf.width_m;
		fr->rect_h_m = rf.height_m;
	}
	if (r->frame.eyes_dp_key != 0) {
		fr->eyes_dp_set = true; // the DP that owns the woven rect (roadmap §5.2)
		fr->eyes_dp = r->frame.eyes_dp_key;
	}
	const uint32_t x = (uint32_t)r->x, y = (uint32_t)r->y;
	fr->active[0] = (float)(cx - x) / (float)r->w;
	fr->active[1] = (float)(cy - y) / (float)r->h;
	fr->active[2] = (float)(cx - x + cw) / (float)r->w;
	fr->active[3] = (float)(cy - y + ch) / (float)r->h;
	if (p.convergence >= 0.0f) {
		fr->convergence_set = true;
		fr->convergence_rel01 = p.convergence > 1.0f ? 1.0f : p.convergence;
	}
	fr->strength = p.strength;
	fr->focal_px = p.focal_px;
	if (capped && fr->focal_px > 0.0f) {
		fr->focal_px *= (float)dw / (float)cw; // focal is in INPUT pixels (a crop keeps the scale)
	}
}

/*!
 * The area of the (clipped) rect the snapshot takes -- the letterbox crop once
 * it settled -- and the extent it is sent at (the long-edge cap). True when
 * capped.
 */
static bool
snapshot_extent(const struct vk_conv_weave *w,
                const struct vkcw_stream *st,
                const struct vk_conv_weave_rect *r,
                uint32_t *out_cx,
                uint32_t *out_cy,
                uint32_t *out_cw,
                uint32_t *out_ch,
                uint32_t *out_dw,
                uint32_t *out_dh)
{
	const uint32_t x = (uint32_t)r->x, y = (uint32_t)r->y;
	uint32_t cx = x, cy = y, cw = r->w, ch = r->h;
	if (w->letterbox && st->lb.w == r->w && st->lb.h == r->h && u_lift_crop_active(&st->lb.committed)) {
		const struct u_lift_crop *c = &st->lb.committed;
		cx = x + c->left;
		cy = y + c->top;
		cw = r->w - c->left - c->right;
		ch = r->h - c->top - c->bottom;
	}
	uint32_t dw = cw, dh = ch;
	const bool capped = u_lift_cap_dims(cw, ch, w->max_input_edge, &dw, &dh);
	*out_cx = cx;
	*out_cy = cy;
	*out_cw = cw;
	*out_ch = ch;
	*out_dw = dw;
	*out_dh = dh;
	return capped;
}

/*!
 * Snapshot the rect's (cropped) area into the chain and its final image into
 * the host buffer; measure the whole rect's lines for the letterbox.
 */
static void
record_snapshot(struct vk_conv_weave *w,
                const struct vk_conv_weave_bridge *br,
                struct vkcw_stream *st,
                uint64_t core_id,
                VkCommandBuffer cmd,
                const struct vk_conv_weave_rect *r)
{
	struct vk_bundle *vk = w->vk;
	const uint32_t x = (uint32_t)r->x, y = (uint32_t)r->y;
	const bool letterbox = w->letterbox;
	uint32_t cx, cy, cw, ch, dw, dh;
	const bool capped = snapshot_extent(w, st, r, &cx, &cy, &cw, &ch, &dw, &dh);

	// The chain: halving LINEAR blits while more than 2x away, then the final
	// blit to dw x dh (NEAREST 1:1 when uncapped: exact texels, channel order
	// converted to RGBA8).
	uint32_t lw[VKCW_CHAIN_MAX], lh[VKCW_CHAIN_MAX];
	uint32_t levels = 0;
	uint32_t cur_w = cw, cur_h = ch;
	while (levels + 1 < VKCW_CHAIN_MAX && (cur_w > 2 * dw || cur_h > 2 * dh)) {
		cur_w = (cur_w + 1) / 2 > dw ? (cur_w + 1) / 2 : dw;
		cur_h = (cur_h + 1) / 2 > dh ? (cur_h + 1) / 2 : dh;
		lw[levels] = cur_w;
		lh[levels] = cur_h;
		levels++;
	}
	lw[levels] = dw;
	lh[levels] = dh;
	levels++;
	// Step E: when the stream's binding takes a GPU kind the glue would give
	// its input slot (a composed pipeline whose depth stage imports GPU
	// memory), the snapshot never leaves the GPU: complete copies the chain's
	// last image into the slot. Otherwise GPU -> host -> slot, as before.
	uint32_t in_mem = 0, in_sync = 0;
	const bool gpu_snap = br->core != NULL && conv_core_stream_input_kinds(br->core, core_id, &in_mem, &in_sync) &&
	                      vk_conv_gpu_input_memory_kind(w->glue, in_mem, in_sync) != XRT_CONV_MEM_CPU;
	bool ok = gpu_snap || buf_ensure(vk, &st->snap_buf, (VkDeviceSize)dw * dh * 4u);
	for (uint32_t i = 0; ok && i < levels; i++) {
		ok = img_ensure(vk, &st->chain[i], lw[i], lh[i], VK_FORMAT_R8G8B8A8_UNORM);
	}
	if (ok) {
		VkImage prev = r->src.image;
		VkImageLayout prev_layout = r->src.layout;
		int32_t px = (int32_t)cx, py = (int32_t)cy, pw = (int32_t)cw, ph = (int32_t)ch;
		for (uint32_t i = 0; i < levels; i++) {
			struct vkcw_img *li = &st->chain[i];
			barrier_image(vk, cmd, li->image, VK_IMAGE_LAYOUT_UNDEFINED,
			              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
			const bool scaled = (uint32_t)pw != li->w || (uint32_t)ph != li->h;
			blit(vk, cmd, prev, prev_layout, px, py, pw, ph, li->image,
			     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0, (int32_t)li->w, (int32_t)li->h,
			     scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
			barrier_image(vk, cmd, li->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
			              VK_ACCESS_TRANSFER_READ_BIT);
			prev = li->image;
			prev_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			px = 0;
			py = 0;
			pw = (int32_t)li->w;
			ph = (int32_t)li->h;
		}
		if (!gpu_snap) {
			VkBufferImageCopy region = {
			    .bufferOffset = 0,
			    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			    .imageExtent = {dw, dh, 1},
			};
			vk->vkCmdCopyImageToBuffer(cmd, st->chain[levels - 1].image,
			                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st->snap_buf.buf, 1, &region);
		}
		st->snap_gpu = gpu_snap;
		st->snap_level = levels - 1;
		st->snap_pending = true;
		st->snap_w = dw;
		st->snap_h = dh;
		st->act_w = cw;
		st->act_h = ch;
		frame_fill(st, br, r, cx, cy, cw, ch, dw, capped);
	}

	// The letterbox profile of the WHOLE rect (bars included): its sampled
	// columns and rows, reduced on the CPU once the frame completed. Skipped
	// while the previous measurement is still unread.
	if (letterbox && !st->lb_pending) {
		const uint32_t n = U_LIFT_LETTERBOX_LINE_SAMPLES;
		const VkDeviceSize bytes = (VkDeviceSize)n * (r->w + r->h) * 4u;
		if (buf_ensure(vk, &st->lb_buf, bytes)) {
			VkBufferImageCopy regions[2 * U_LIFT_LETTERBOX_LINE_SAMPLES];
			for (uint32_t k = 0; k < n; k++) {
				regions[k] = (VkBufferImageCopy){
				    .bufferOffset = (VkDeviceSize)k * r->h * 4u,
				    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .imageOffset = {(int32_t)(x + u_lift_letterbox_sample_pos(k, r->w)), (int32_t)y, 0},
				    .imageExtent = {1, r->h, 1},
				};
				regions[n + k] = (VkBufferImageCopy){
				    .bufferOffset = (VkDeviceSize)n * r->h * 4u + (VkDeviceSize)k * r->w * 4u,
				    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .imageOffset = {(int32_t)x, (int32_t)(y + u_lift_letterbox_sample_pos(k, r->h)), 0},
				    .imageExtent = {r->w, 1, 1},
				};
			}
			vk->vkCmdCopyImageToBuffer(cmd, r->src.image, r->src.layout, st->lb_buf.buf, 2 * n, regions);
			st->lb_pending = true;
			st->lb_w = r->w;
			st->lb_h = r->h;
			st->lb_bgra = is_bgra(r->src.format);
		}
	}
}


/*
 *
 * Result drawing.
 *
 */

/*!
 * Pin the stream's latest GPU views result; false = none (flat). The pin is
 * kept in @p c until complete / begin.
 */
static bool
pin_latest(struct vk_conv_weave *w,
           struct vk_conv_weave_client *c,
           const struct vk_conv_weave_bridge *b,
           uint64_t core_id,
           struct conv_result *out_res,
           struct vk_conv_gpu_image_ref *out_ref)
{
	if (b->core == NULL || c->pin_count >= VKCW_PINS_MAX) {
		return false;
	}
	if (conv_core_stream_acquire(b->core, core_id, false, out_res) != XRT_SUCCESS) {
		return false;
	}
	// No GPU views (a CPU result the glue could not mirror, #2030): flat.
	if ((out_res->written & XRT_CONV_OUT_VIEWS) == 0 || !vk_conv_gpu_lookup(w->glue, &out_res->views, out_ref)) {
		conv_core_stream_release(b->core, core_id, out_res->ring_slot);
		return false;
	}
	c->pins[c->pin_count].core_id = core_id;
	c->pins[c->pin_count].ring_slot = out_res->ring_slot;
	c->pin_count++;
	return true;
}

//! Copy result view @p v 1:1 into its staging image (raw bytes, stays encoded). Leaves it TRANSFER_SRC.
static struct vkcw_img *
stage_view(struct vk_conv_weave *w,
           struct vkcw_stream *st,
           VkCommandBuffer cmd,
           const struct vk_conv_gpu_image_ref *ref,
           uint32_t views,
           uint32_t v,
           bool *staged)
{
	struct vk_bundle *vk = w->vk;
	if (v >= XRT_CONV_MAX_VIEWS) {
		return NULL;
	}
	struct vkcw_img *s = &st->res[v];
	if (staged[v]) {
		return s;
	}
	const bool array = ref->layers > 1;
	const uint32_t vw = array ? ref->width : ref->width / views;
	const uint32_t vh = ref->height;
	if (vw == 0 || vh == 0 || (array && v >= ref->layers) || !img_ensure(vk, s, vw, vh, unorm4(ref->format))) {
		return NULL;
	}
	barrier_image(vk, cmd, s->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
	              VK_ACCESS_TRANSFER_WRITE_BIT);
	VkImageCopy copy = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, array ? v : 0, 1},
	    .srcOffset = {array ? 0 : (int32_t)(v * vw), 0, 0},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .extent = {vw, vh, 1},
	};
	vk->vkCmdCopyImage(cmd, ref->image, VK_IMAGE_LAYOUT_GENERAL, s->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	                   &copy);
	barrier_image(vk, cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	staged[v] = true;
	return s;
}

//! Tile origin of view @p v.
static void
tile_origin(const struct vk_conv_weave_rect *r, uint32_t v, int32_t *out_x, int32_t *out_y)
{
	const uint32_t cols = r->tile_columns > 0 ? r->tile_columns : 1;
	*out_x = (int32_t)((v % cols) * r->tile_w);
	*out_y = (int32_t)((v / cols) * r->tile_h);
}

//! The batch layout's 2D frame into every tile, flat (no result yet).
static void
draw_flat(struct vk_conv_weave *w, VkCommandBuffer cmd, const struct vk_conv_weave_rect *r)
{
	for (uint32_t v = 0; v < r->views; v++) {
		int32_t tx = 0, ty = 0;
		tile_origin(r, v, &tx, &ty);
		blit(w->vk, cmd, r->src.image, r->src.layout, r->x, r->y, (int32_t)r->w, (int32_t)r->h, r->dst.image,
		     r->dst.layout, tx + r->x, ty + r->y, (int32_t)r->w, (int32_t)r->h, VK_FILTER_NEAREST);
	}
}

/*!
 * The active part of the rect (letterbox) the result covers: integer edges in
 * source pixels, the bars tile the rest of the rect exactly. False = empty.
 */
static bool
active_area(const struct vk_conv_weave_rect *r,
            const struct conv_result *res,
            int32_t *out_x0,
            int32_t *out_y0,
            int32_t *out_x1,
            int32_t *out_y1)
{
	const bool whole =
	    res->active[0] == 0.0f && res->active[1] == 0.0f && res->active[2] == 0.0f && res->active[3] == 0.0f;
	const float a0 = whole ? 0.0f : res->active[0], a1 = whole ? 0.0f : res->active[1];
	const float a2 = whole ? 1.0f : res->active[2], a3 = whole ? 1.0f : res->active[3];
	int32_t ax0 = r->x + (int32_t)lroundf(a0 * (float)r->w);
	int32_t ay0 = r->y + (int32_t)lroundf(a1 * (float)r->h);
	int32_t ax1 = r->x + (int32_t)lroundf(a2 * (float)r->w);
	int32_t ay1 = r->y + (int32_t)lroundf(a3 * (float)r->h);
	const int32_t rx1 = r->x + (int32_t)r->w, ry1 = r->y + (int32_t)r->h;
	ax0 = ax0 < r->x ? r->x : (ax0 > rx1 ? rx1 : ax0);
	ay0 = ay0 < r->y ? r->y : (ay0 > ry1 ? ry1 : ay0);
	ax1 = ax1 < ax0 ? ax0 : (ax1 > rx1 ? rx1 : ax1);
	ay1 = ay1 < ay0 ? ay0 : (ay1 > ry1 ? ry1 : ay1);
	*out_x0 = ax0;
	*out_y0 = ay0;
	*out_x1 = ax1;
	*out_y1 = ay1;
	return ax1 > ax0 && ay1 > ay0;
}

//! The "woven" line: first time, and on a result extent or view-count change.
static void
log_woven(struct vkcw_stream *st, uint32_t res_w, uint32_t res_h)
{
	if (!st->logged_woven || st->logged_res_w != res_w || st->logged_res_h != res_h ||
	    st->logged_res_views != st->result_views) {
		st->logged_woven = true;
		st->logged_res_w = res_w;
		st->logged_res_h = res_h;
		st->logged_res_views = st->result_views;
		U_LOG_I("lift rect: stream 0x%llx woven %ux%u views %u", (unsigned long long)st->wire, res_w, res_h,
		        st->result_views);
	}
}

static bool
draw_result(struct vk_conv_weave *w,
            struct vkcw_stream *st,
            VkCommandBuffer cmd,
            const struct vk_conv_weave_rect *r,
            const struct conv_result *res,
            const struct vk_conv_gpu_image_ref *ref)
{
	struct vk_bundle *vk = w->vk;
	const uint32_t rviews = res->views_info.view_count != 0 ? res->views_info.view_count : 1;
	if (unorm4(ref->format) == VK_FORMAT_UNDEFINED) {
		if (!st->fmt_warned) {
			st->fmt_warned = true;
			U_LOG_W("[lift] weave rect stream 0x%llx: result format %d is not 8-bit RGBA; woven flat",
			        (unsigned long long)st->wire, (int)ref->format);
		}
		return false;
	}
	int32_t ax0, ay0, ax1, ay1;
	if (!active_area(r, res, &ax0, &ay0, &ax1, &ay1)) {
		return false;
	}
	const int32_t rx1 = r->x + (int32_t)r->w, ry1 = r->y + (int32_t)r->h;

	vk_conv_gpu_cmd_acquire(w->glue, cmd, ref);
	bool staged[XRT_CONV_MAX_VIEWS] = {false};
	bool any = false;
	for (uint32_t v = 0; v < r->views; v++) {
		const uint32_t sv = src_view_for(r->batch, v, r->views, rviews);
		struct vkcw_img *s = stage_view(w, st, cmd, ref, rviews, sv, staged);
		if (s == NULL) {
			continue;
		}
		int32_t tx = 0, ty = 0;
		tile_origin(r, v, &tx, &ty);
		blit(vk, cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0, (int32_t)s->w, (int32_t)s->h,
		     r->dst.image, r->dst.layout, tx + ax0, ty + ay0, ax1 - ax0, ay1 - ay0, VK_FILTER_LINEAR);
		any = true;
	}
	vk_conv_gpu_cmd_release(w->glue, cmd, ref);
	if (!any) {
		return false;
	}
	// Batch: the bars are the 2D input, flat in every tile (v6: the caller's
	// tiles already hold it there).
	if (r->batch) {
		const int32_t bar[4][4] = {
		    {r->x, r->y, (int32_t)r->w, ay0 - r->y}, // top
		    {r->x, ay1, (int32_t)r->w, ry1 - ay1},   // bottom
		    {r->x, ay0, ax0 - r->x, ay1 - ay0},      // left
		    {ax1, ay0, rx1 - ax1, ay1 - ay0},        // right
		};
		for (uint32_t k = 0; k < 4; k++) {
			if (bar[k][2] <= 0 || bar[k][3] <= 0) {
				continue;
			}
			for (uint32_t v = 0; v < r->views; v++) {
				int32_t tx = 0, ty = 0;
				tile_origin(r, v, &tx, &ty);
				blit(vk, cmd, r->src.image, r->src.layout, bar[k][0], bar[k][1], bar[k][2], bar[k][3],
				     r->dst.image, r->dst.layout, tx + bar[k][0], ty + bar[k][1], bar[k][2], bar[k][3],
				     VK_FILTER_NEAREST);
			}
		}
	}
	return true;
}


/*
 *
 * Ops.
 *
 */

uint32_t
vk_conv_weave_stream_mode(struct vk_conv_weave *w, uint64_t owner, uint64_t wire_id)
{
	if (w == NULL) {
		return 0;
	}
	const struct vk_conv_weave_bridge b = bridge_get(w);
	uint64_t core_id = 0;
	uint32_t mode = 0;
	if (b.core == NULL || b.resolve == NULL || !b.resolve(b.ud, owner, wire_id, &core_id, &mode)) {
		return 0;
	}
	return mode;
}

struct vk_conv_weave_client *
vk_conv_weave_client_create(struct vk_conv_weave *w, struct vk_bundle *vk)
{
	if (w == NULL || (vk != NULL && vk != w->vk)) {
		return NULL;
	}
	struct vk_conv_weave_client *c = U_TYPED_CALLOC(struct vk_conv_weave_client);
	c->host = vk == NULL;
	os_mutex_lock(&w->mtx);
	c->next = w->clients;
	w->clients = c;
	os_mutex_unlock(&w->mtx);
	return c;
}

//! Free @p c's state (unlinked; its GPU work idle).
static void
client_free(struct vk_conv_weave *w, struct vk_conv_weave_client *c, const struct vk_conv_weave_bridge *b)
{
	pins_release(c, b);
	while (c->streams != NULL) {
		struct vkcw_stream *st = c->streams;
		c->streams = st->next;
		stream_free(w->vk, st);
	}
	free(c);
}

void
vk_conv_weave_client_destroy(struct vk_conv_weave *w, struct vk_conv_weave_client *c)
{
	if (w == NULL || c == NULL) {
		return;
	}
	os_mutex_lock(&w->mtx);
	struct vk_conv_weave_client **pp = &w->clients;
	while (*pp != NULL && *pp != c) {
		pp = &(*pp)->next;
	}
	const bool found = *pp != NULL;
	if (found) {
		*pp = c->next;
	}
	const struct vk_conv_weave_bridge b = w->bridge;
	os_mutex_unlock(&w->mtx);
	if (found) {
		client_free(w, c, &b);
	}
}

void
vk_conv_weave_begin(struct vk_conv_weave *w, struct vk_conv_weave_client *c)
{
	if (w == NULL || c == NULL) {
		return;
	}
	const struct vk_conv_weave_bridge b = bridge_get(w);
	// Anything still pending belongs to a weave that was never submitted.
	pins_release(c, &b);
	for (struct vkcw_stream *st = c->streams; st != NULL; st = st->next) {
		st->snap_pending = false;
		st->snap_gpu = false;
		st->lb_pending = false;
		st->used_this_frame = false;
		st->host_snap = NULL;
		st->host_lines = NULL;
	}
}

bool
vk_conv_weave_record(struct vk_conv_weave *w,
                     struct vk_conv_weave_client *c,
                     VkCommandBuffer cmd,
                     const struct vk_conv_weave_rect *r)
{
	if (w == NULL || c == NULL || c->host || r == NULL || r->binding == NULL || cmd == VK_NULL_HANDLE ||
	    r->src.image == VK_NULL_HANDLE || r->dst.image == VK_NULL_HANDLE || r->views == 0) {
		return false;
	}
	// Clip to the source.
	int32_t x0 = r->x < 0 ? 0 : r->x, y0 = r->y < 0 ? 0 : r->y;
	int64_t x1 = (int64_t)r->x + r->w, y1 = (int64_t)r->y + r->h;
	x1 = x1 > (int64_t)r->src.width ? (int64_t)r->src.width : x1;
	y1 = y1 > (int64_t)r->src.height ? (int64_t)r->src.height : y1;
	if (x1 <= x0 || y1 <= y0) {
		return true; // nothing visible to lift; nothing to weave either
	}
	struct vk_conv_weave_rect rc = *r;
	rc.x = x0;
	rc.y = y0;
	rc.w = (uint32_t)(x1 - x0);
	rc.h = (uint32_t)(y1 - y0);

	struct vk_bundle *vk = w->vk;
	const struct vk_conv_weave_bridge b = bridge_get(w);
	const uint64_t wire = rc.binding->stream_id;
	uint64_t core_id = 0;
	const bool resolved = bridge_resolve(&b, rc.owner, wire, &core_id);
	struct vkcw_stream *st = stream_get(c, rc.owner, wire, true);
	st->idle = 0;
	st->woven_result = false;
	st->result_views = 0;

	// Everything earlier on this queue (the engine's own writes of the source,
	// the glue's writes of the result) before this rect's transfers.
	barrier_global(vk, cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
	               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

	// 1. Snapshot into the stream (once per stream per weave).
	if (resolved && !st->used_this_frame) {
		if (b.match_weave != NULL) {
			b.match_weave(b.ud, rc.owner, wire, rc.views); // spec 4 viewCount 0 = match the weave
		}
		record_snapshot(w, &b, st, core_id, cmd, &rc);
		st->used_this_frame = true;
		// The snapshot's (and the letterbox copy's) reads finish before the
		// result overwrites the tiles (v6: the same crop image).
		barrier_global(vk, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
		               VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
	}

	// 2. The latest result at the CURRENT rect, or 3. flat until the first one.
	struct conv_result res;
	struct vk_conv_gpu_image_ref ref;
	memset(&res, 0, sizeof(res));
	bool woven = false;
	if (resolved && pin_latest(w, c, &b, core_id, &res, &ref)) {
		woven = draw_result(w, st, cmd, &rc, &res, &ref);
		if (woven) {
			st->woven_result = true;
			st->result_views = res.views_info.view_count != 0 ? res.views_info.view_count : 1;
			log_woven(st, ref.width, ref.height);
		}
	}
	if (!woven && rc.batch) {
		draw_flat(w, cmd, &rc);
	}
	if (!resolved && !st->logged_unresolved) {
		st->logged_unresolved = true;
		U_LOG_I(
		    "lift rect: stream 0x%llx failed: resolve (not a core-routed stream of this connection); woven "
		    "flat",
		    (unsigned long long)wire);
	}
	return true;
}

void
vk_conv_weave_complete(struct vk_conv_weave *w, struct vk_conv_weave_client *c)
{
	if (w == NULL || c == NULL) {
		return;
	}
	struct vk_bundle *vk = w->vk;
	const struct vk_conv_weave_bridge b = bridge_get(w);
	struct vkcw_stream **pp = &c->streams;
	while (*pp != NULL) {
		struct vkcw_stream *st = *pp;
		uint64_t core_id = 0;
		const bool alive = bridge_resolve(&b, st->owner, st->wire, &core_id);

		// Letterbox: the measurement of the whole rect, now readable.
		// (A host client's engine handed its copy over in host_done; none = it
		// could not make one this frame.)
		const uint8_t *base = c->host ? (const uint8_t *)st->host_lines : (const uint8_t *)st->lb_buf.map;
		if (st->lb_pending && base == NULL) {
			st->lb_pending = false;
		}
		if (st->lb_pending) {
			st->lb_pending = false;
			float *rows = c->lb_rows, *cols = c->lb_cols;
			uint32_t nr = 0, nc = 0;
			u_lift_letterbox_profile_from_lines(
			    base, base + (size_t)U_LIFT_LETTERBOX_LINE_SAMPLES * st->lb_h * 4u, st->lb_w, st->lb_h,
			    st->lb_bgra, rows, &nr, cols, &nc);
			if (u_lift_letterbox_update(&st->lb, st->lb_w, st->lb_h, rows, nr, cols, nc)) {
				const struct u_lift_crop *cr = &st->lb.committed;
				if (u_lift_crop_active(cr)) {
					U_LOG_W(
					    "[lift] stream 0x%llx: letterbox crop %ux%u -> active %ux%u (bars top %u "
					    "bottom "
					    "%u left %u right %u; bars woven flat)",
					    (unsigned long long)st->wire, st->lb_w, st->lb_h,
					    st->lb_w - cr->left - cr->right, st->lb_h - cr->top - cr->bottom, cr->top,
					    cr->bottom, cr->left, cr->right);
				} else {
					U_LOG_W("[lift] stream 0x%llx: letterbox crop off (%ux%u lifted whole)",
					        (unsigned long long)st->wire, st->lb_w, st->lb_h);
				}
			}
		}

		// The snapshot: begin -> copy -> commit in one go (no slot held across frames).
		if (st->snap_pending) {
			st->snap_pending = false;
			const char *fail = NULL;
			const bool gpu_snap = !c->host && st->snap_gpu;
			const uint8_t *snap =
			    c->host ? (const uint8_t *)st->host_snap : (const uint8_t *)st->snap_buf.map;
			if (!alive) {
				fail = "resolve";
			} else if (snap == NULL && !gpu_snap) {
				fail = "copy"; // the host engine's snapshot never completed
			} else {
				struct conv_submit_layout lay;
				memset(&lay, 0, sizeof(lay));
				lay.image_count = 1;
				lay.width[0] = st->snap_w;
				lay.height[0] = st->snap_h;
				lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
				lay.color_space[0] = XRT_CONV_CS_SRGB;
				lay.role[0] = XRT_CONV_IMG_COLOR;
				struct conv_submit_slot slot;
				memset(&slot, 0, sizeof(slot));
				if (conv_core_stream_begin_submit(b.core, core_id, &lay, &slot) != XRT_SUCCESS) {
					fail = "slot"; // both input slots busy: latest wins, the next weave retries
				} else {
					const struct xrt_conv_image *img = &slot.images[0];
					const uint32_t tight = st->snap_w * 4u;
					bool ok = false;
					if (gpu_snap) {
						// GPU -> GPU into a glue slot (no host buffer); a slot
						// that turned out CPU (a rebind) is read back instead.
						const struct vkcw_img *last = &st->chain[st->snap_level];
						ok = vk_conv_gpu_copy_from_vk(
						    w->glue, last->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						    last->format, 0, 0, st->snap_w, st->snap_h, img);
					} else if (img->memory_kind == XRT_CONV_MEM_CPU && img->cpu_ptr != NULL) {
						const uint32_t pitch = img->row_stride != 0 ? img->row_stride : tight;
						if ((uint64_t)pitch * st->snap_h <= img->byte_size ||
						    img->byte_size == 0) {
							for (uint32_t y = 0; y < st->snap_h; y++) {
								memcpy((uint8_t *)img->cpu_ptr + (size_t)y * pitch,
								       snap + (size_t)y * tight, tight);
							}
							ok = true;
						}
					} else {
						ok = vk_conv_gpu_write(w->glue, img, snap, tight);
					}
					uint64_t frame_id = 0;
					if (!ok) {
						conv_core_stream_abort_submit(b.core, core_id, slot.slot);
						fail = "copy";
					} else if (conv_core_stream_commit_submit(b.core, core_id, slot.slot, &st->fr,
					                                          &frame_id) != XRT_SUCCESS) {
						fail = "commit";
					} else {
						st->committed++;
						if (st->cap_logged_w != st->snap_w || st->cap_logged_h != st->snap_h) {
							st->cap_logged_w = st->snap_w;
							st->cap_logged_h = st->snap_h;
							U_LOG_I("lift rect: stream 0x%llx snapshot %ux%u (cap %u)",
							        (unsigned long long)st->wire, st->snap_w, st->snap_h,
							        w->max_input_edge);
						}
					}
				}
			}
			if (fail != NULL && fail != st->logged_fail) {
				st->logged_fail = fail;
				U_LOG_I("lift rect: stream 0x%llx failed: %s", (unsigned long long)st->wire, fail);
			} else if (fail == NULL) {
				st->logged_fail = NULL;
			}
		}
		st->used_this_frame = false;
		st->host_snap = NULL;
		st->host_lines = NULL;

		// A stream gone from the core, or idle for long, lets its images go
		// (the GPU finished everything recorded for it).
		if (!alive || ++st->idle > VKCW_IDLE_COMPLETES) {
			*pp = st->next;
			stream_free(vk, st);
			continue;
		}
		pp = &st->next;
	}
	pins_release(c, &b);
}

bool
vk_conv_weave_host_plan(struct vk_conv_weave *w,
                        struct vk_conv_weave_client *c,
                        const struct vk_conv_weave_rect *r,
                        struct vk_conv_weave_host_plan *out)
{
	memset(out, 0, sizeof(*out));
	if (w == NULL || c == NULL || !c->host || r == NULL || r->binding == NULL || r->views == 0 ||
	    r->src.width == 0 || r->src.height == 0) {
		return false;
	}
	// Clip to the source (vk_conv_weave_record's rule).
	int32_t x0 = r->x < 0 ? 0 : r->x, y0 = r->y < 0 ? 0 : r->y;
	int64_t x1 = (int64_t)r->x + r->w, y1 = (int64_t)r->y + r->h;
	x1 = x1 > (int64_t)r->src.width ? (int64_t)r->src.width : x1;
	y1 = y1 > (int64_t)r->src.height ? (int64_t)r->src.height : y1;
	if (x1 <= x0 || y1 <= y0) {
		return true; // nothing visible to lift; nothing to weave either (w == 0)
	}
	struct vk_conv_weave_rect rc = *r;
	rc.x = x0;
	rc.y = y0;
	rc.w = (uint32_t)(x1 - x0);
	rc.h = (uint32_t)(y1 - y0);
	out->x = rc.x;
	out->y = rc.y;
	out->w = rc.w;
	out->h = rc.h;

	const struct vk_conv_weave_bridge b = bridge_get(w);
	const uint64_t wire = rc.binding->stream_id;
	uint64_t core_id = 0;
	const bool resolved = bridge_resolve(&b, rc.owner, wire, &core_id);
	struct vkcw_stream *st = stream_get(c, rc.owner, wire, true);
	st->idle = 0;
	st->woven_result = false;
	st->result_views = 0;

	// 1. The snapshot the engine records (once per stream per weave).
	if (resolved && !st->used_this_frame) {
		if (b.match_weave != NULL) {
			b.match_weave(b.ud, rc.owner, wire, rc.views); // spec 4 viewCount 0 = match the weave
		}
		uint32_t cx, cy, cw, ch, dw, dh;
		const bool capped = snapshot_extent(w, st, &rc, &cx, &cy, &cw, &ch, &dw, &dh);
		out->snapshot = true;
		out->cx = cx;
		out->cy = cy;
		out->cw = cw;
		out->ch = ch;
		out->dw = dw;
		out->dh = dh;
		st->snap_pending = true;
		st->snap_w = dw;
		st->snap_h = dh;
		st->act_w = cw;
		st->act_h = ch;
		frame_fill(st, &b, &rc, cx, cy, cw, ch, dw, capped);
		if (w->letterbox && !st->lb_pending) {
			out->lines = true;
			st->lb_pending = true;
			st->lb_w = rc.w;
			st->lb_h = rc.h;
			st->lb_bgra = is_bgra(rc.src.format);
		}
		st->used_this_frame = true;
	}

	// 2. The latest result, exported as an IOSurface, or 3. flat (batch). It was
	// published only after a host-side fence wait on the Vulkan work that wrote
	// it (vk_conv_gpu.c cmd_submit_wait); the pin taken here is released only by
	// vk_conv_weave_complete, which the Metal engine calls after its command
	// buffer's waitUntilCompleted.
	struct conv_result res;
	struct vk_conv_gpu_image_ref ref;
	memset(&res, 0, sizeof(res));
	if (resolved && pin_latest(w, c, &b, core_id, &res, &ref)) {
		const struct xrt_conv_image *v = &res.views;
		const bool exported = v->memory_kind == XRT_CONV_MEM_IOSURFACE && v->handle != 0 &&
		                      v->handle != VK_CONV_GPU_NO_HANDLE && ref.layers <= 1;
		int32_t ax0, ay0, ax1, ay1;
		if (!exported) {
			// A device-private (or layered) result: the engine cannot sample it.
			// The pin goes at complete; the rect is woven flat.
			if (!st->logged_export) {
				st->logged_export = true;
				U_LOG_I(
				    "lift rect: stream 0x%llx failed: export (the result is not an IOSurface); woven "
				    "flat",
				    (unsigned long long)wire);
			}
		} else if (unorm4(ref.format) == VK_FORMAT_UNDEFINED) {
			if (!st->fmt_warned) {
				st->fmt_warned = true;
				U_LOG_W(
				    "[lift] weave rect stream 0x%llx: result format %d is not 8-bit RGBA; woven flat",
				    (unsigned long long)wire, (int)ref.format);
			}
		} else if (active_area(&rc, &res, &ax0, &ay0, &ax1, &ay1)) {
			out->result = true;
			out->result_iosurface = (void *)(uintptr_t)v->handle;
			// Step E: an async S5 result completes on the glue's shared event.
			out->result_event = ref.mtl_shared_event;
			out->result_event_value = ref.event_value;
			out->result_format = v->format;
			out->result_w = ref.width;
			out->result_h = ref.height;
			out->result_views = res.views_info.view_count != 0 ? res.views_info.view_count : 1;
			out->ax0 = ax0;
			out->ay0 = ay0;
			out->ax1 = ax1;
			out->ay1 = ay1;
			for (uint32_t t = 0; t < rc.views && t < XRT_CONV_MAX_VIEWS; t++) {
				out->tile_view[t] = src_view_for(rc.batch, t, rc.views, out->result_views);
			}
		}
	}
	if (!resolved && !st->logged_unresolved) {
		st->logged_unresolved = true;
		U_LOG_I(
		    "lift rect: stream 0x%llx failed: resolve (not a core-routed stream of this connection); woven "
		    "flat",
		    (unsigned long long)wire);
	}
	return true;
}

void
vk_conv_weave_host_done(struct vk_conv_weave *w,
                        struct vk_conv_weave_client *c,
                        uint64_t owner,
                        uint64_t wire_id,
                        const struct vk_conv_weave_host_done *d)
{
	if (w == NULL || c == NULL || !c->host || d == NULL) {
		return;
	}
	struct vkcw_stream *st = stream_get(c, owner, wire_id, false);
	if (st == NULL) {
		return;
	}
	// Several rects of one stream: only the one that recorded a copy hands it over.
	if (st->snap_pending && d->snapshot != NULL) {
		st->host_snap = d->snapshot;
	}
	if (st->lb_pending && d->lines != NULL) {
		st->host_lines = d->lines;
	}
	if (d->woven_views > 0) {
		st->woven_result = true;
		st->result_views = d->woven_views;
		log_woven(st, d->woven_w, d->woven_h);
	}
}

bool
vk_conv_weave_client_stats(struct vk_conv_weave_client *c, uint64_t wire_id, struct vk_conv_weave_stats *out)
{
	memset(out, 0, sizeof(*out));
	if (c == NULL) {
		return false;
	}
	for (struct vkcw_stream *st = c->streams; st != NULL; st = st->next) {
		if (st->wire == wire_id) {
			out->snap_w = st->snap_w;
			out->snap_h = st->snap_h;
			out->active_w = st->act_w;
			out->active_h = st->act_h;
			out->woven_result = st->woven_result;
			out->result_views = st->result_views;
			out->committed = st->committed;
			return true;
		}
	}
	return false;
}


/*
 *
 * Lifetime.
 *
 */

static uint32_t
op_stream_mode(void *ud, uint64_t owner, uint64_t wire_id)
{
	return vk_conv_weave_stream_mode((struct vk_conv_weave *)ud, owner, wire_id);
}

static struct vk_conv_weave_client *
op_client_create(void *ud, struct vk_bundle *vk)
{
	return vk_conv_weave_client_create((struct vk_conv_weave *)ud, vk);
}

static void
op_client_destroy(void *ud, struct vk_conv_weave_client *c)
{
	vk_conv_weave_client_destroy((struct vk_conv_weave *)ud, c);
}

static void
op_begin(void *ud, struct vk_conv_weave_client *c)
{
	vk_conv_weave_begin((struct vk_conv_weave *)ud, c);
}

static bool
op_record(void *ud, struct vk_conv_weave_client *c, VkCommandBuffer cmd, const struct vk_conv_weave_rect *r)
{
	return vk_conv_weave_record((struct vk_conv_weave *)ud, c, cmd, r);
}

static void
op_complete(void *ud, struct vk_conv_weave_client *c)
{
	vk_conv_weave_complete((struct vk_conv_weave *)ud, c);
}

static bool
op_host_plan(void *ud,
             struct vk_conv_weave_client *c,
             const struct vk_conv_weave_rect *r,
             struct vk_conv_weave_host_plan *out)
{
	return vk_conv_weave_host_plan((struct vk_conv_weave *)ud, c, r, out);
}

static void
op_host_done(
    void *ud, struct vk_conv_weave_client *c, uint64_t owner, uint64_t wire_id, const struct vk_conv_weave_host_done *d)
{
	vk_conv_weave_host_done((struct vk_conv_weave *)ud, c, owner, wire_id, d);
}

void
vk_conv_weave_get_ops(struct vk_conv_weave *w, struct vk_conv_weave_ops *out)
{
	memset(out, 0, sizeof(*out));
	if (w == NULL) {
		return;
	}
	out->ud = w;
	out->stream_mode = op_stream_mode;
	out->client_create = op_client_create;
	out->client_destroy = op_client_destroy;
	out->begin = op_begin;
	out->record = op_record;
	out->complete = op_complete;
	out->host_plan = op_host_plan;
	out->host_done = op_host_done;
}

bool
vk_conv_weave_create(struct vk_bundle *vk, struct vk_conv_gpu *glue, struct vk_conv_weave **out)
{
	if (vk == NULL || glue == NULL || out == NULL) {
		return false;
	}
	struct vk_conv_weave *w = U_TYPED_CALLOC(struct vk_conv_weave);
	if (os_mutex_init(&w->mtx) != 0) {
		free(w);
		return false;
	}
	w->vk = vk;
	w->glue = glue;
	// The service's weave-rect policy (ADR-042), read once, as the D3D11 service does.
	w->max_input_edge = u_lift_max_input_edge_parse(getenv("DXR_LIFT_MAX_INPUT_EDGE"));
	w->letterbox = u_lift_letterbox_parse(getenv("DXR_LIFT_LETTERBOX"));
	if (w->max_input_edge != U_LIFT_MAX_INPUT_EDGE_DEFAULT) {
		U_LOG_W("[lift] DXR_LIFT_MAX_INPUT_EDGE=%u%s", w->max_input_edge,
		        w->max_input_edge == 0 ? " (weave-rect snapshots uncapped)" : "");
	}
	if (!w->letterbox) {
		U_LOG_W("[lift] DXR_LIFT_LETTERBOX=0 (weave-rect snapshots never cropped to the active area)");
	}
	*out = w;
	return true;
}

void
vk_conv_weave_set_bridge(struct vk_conv_weave *w, const struct vk_conv_weave_bridge *bridge)
{
	if (w == NULL) {
		return;
	}
	os_mutex_lock(&w->mtx);
	if (bridge != NULL) {
		w->bridge = *bridge;
	} else {
		memset(&w->bridge, 0, sizeof(w->bridge));
	}
	os_mutex_unlock(&w->mtx);
}

void
vk_conv_weave_destroy(struct vk_conv_weave **w_ptr)
{
	if (w_ptr == NULL || *w_ptr == NULL) {
		return;
	}
	struct vk_conv_weave *w = *w_ptr;
	struct vk_bundle *vk = w->vk;
	// No engine command buffer may still read a helper image.
	vk_queue_lock(vk->main_queue);
	vk->vkQueueWaitIdle(vk->main_queue->queue);
	vk_queue_unlock(vk->main_queue);
	os_mutex_lock(&w->mtx);
	struct vk_conv_weave_client *c = w->clients;
	w->clients = NULL;
	const struct vk_conv_weave_bridge b = w->bridge;
	os_mutex_unlock(&w->mtx);
	while (c != NULL) {
		struct vk_conv_weave_client *next = c->next;
		client_free(w, c, &b);
		c = next;
	}
	os_mutex_destroy(&w->mtx);
	free(w);
	*w_ptr = NULL;
}
