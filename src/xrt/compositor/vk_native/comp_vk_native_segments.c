// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen segment display processors (multi-screen M2) — see
 *         comp_vk_native_segments.h.
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_segments.h"

#include "xrt/xrt_display_processor_vk.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_plugin.h"

#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * State.
 *
 */

//! Per-screen state: the screen's segment DP (NULL for the primary screen,
//! whose DP the session owns) and the cropped DP input for its segment.
struct seg_screen_state
{
	struct xrt_display_processor *dp;
	bool tolerates_resample;
	//! The per-session one-time DP setup (encoding, transparency) is done.
	bool configured;

	VkImage crop_image;
	VkDeviceMemory crop_memory;
	VkImageView crop_view;
	uint32_t crop_w;
	uint32_t crop_h;
	VkFormat crop_format;
};

//! What the last split frame did with one segment (atlas capture).
struct seg_capture
{
	uint32_t screen_index;
	bool woven;
	uint32_t w;
	uint32_t h;
};

struct comp_vk_native_segments
{
	struct vk_bundle *vk;
	VkCommandPool cmd_pool;
	//! #868: the runtime-owned queue a DP must capture at creation (see
	//! vk_make_dp_vk); VK_NULL_HANDLE = none, the DP sees the app's queue.
	VkQueue dp_queue;

	bool enabled;

	uint32_t screen_count;
	struct comp_segments_screen screens[COMP_SEGMENTS_MAX_SCREENS];
	struct xrt_screen_binding bindings[COMP_SEGMENTS_MAX_SCREENS];
	//! Owning plug-in iface / instance per screen (registry).
	const struct xrt_plugin_iface *iface[COMP_SEGMENTS_MAX_SCREENS];
	struct xrt_plugin_instance *inst[COMP_SEGMENTS_MAX_SCREENS];
	struct seg_screen_state st[COMP_SEGMENTS_MAX_SCREENS];

	struct comp_segments_lifecycle lc;
	struct comp_segment_table table;
	struct comp_segment_table logged_table;
	bool have_logged_table;
	bool logged_split;

	struct seg_capture cap[COMP_SEGMENTS_MAX];
	uint32_t cap_count;
};


/*
 *
 * Helpers.
 *
 */

static int
screen_index_of(const struct comp_vk_native_segments *segs, uint64_t id)
{
	for (uint32_t i = 0; i < segs->screen_count; i++) {
		if (segs->screens[i].id == id) {
			return (int)i;
		}
	}
	return -1;
}

static void
crop_release(struct vk_bundle *vk, struct seg_screen_state *st)
{
	if (st->crop_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, st->crop_view, NULL);
		st->crop_view = VK_NULL_HANDLE;
	}
	if (st->crop_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, st->crop_image, NULL);
		st->crop_image = VK_NULL_HANDLE;
	}
	if (st->crop_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, st->crop_memory, NULL);
		st->crop_memory = VK_NULL_HANDLE;
	}
	st->crop_w = 0;
	st->crop_h = 0;
}

/*!
 * (Re)create a segment's DP input image at exactly @p w x @p h — a DP reads its
 * atlas as `tile_columns * view_width` wide, so the image must be exactly the
 * segment's tile grid (the same reason the single-DP path crops, ADR-030).
 */
static bool
crop_ensure(struct vk_bundle *vk, struct seg_screen_state *st, uint32_t w, uint32_t h, VkFormat format)
{
	if (st->crop_image != VK_NULL_HANDLE && st->crop_w == w && st->crop_h == h && st->crop_format == format) {
		return true;
	}
	crop_release(vk, st);

	VkImageCreateInfo ici = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = format,
	    .extent = {w, h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    // TRANSFER_SRC: a DP may read its input with a transfer (the Leia
	    // Linux DP's 2D path blits it), and the atlas capture reads it back.
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vk->vkCreateImage(vk->device, &ici, NULL, &st->crop_image) != VK_SUCCESS) {
		st->crop_image = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryRequirements req;
	vk->vkGetImageMemoryRequirements(vk->device, st->crop_image, &req);
	VkPhysicalDeviceMemoryProperties props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &props);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((req.memoryTypeBits & (1u << i)) &&
		    (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) {
		crop_release(vk, st);
		return false;
	}
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = req.size,
	    .memoryTypeIndex = type,
	};
	if (vk->vkAllocateMemory(vk->device, &mai, NULL, &st->crop_memory) != VK_SUCCESS ||
	    vk->vkBindImageMemory(vk->device, st->crop_image, st->crop_memory, 0) != VK_SUCCESS) {
		crop_release(vk, st);
		return false;
	}

	VkImageViewCreateInfo vci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = st->crop_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = format,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	if (vk->vkCreateImageView(vk->device, &vci, NULL, &st->crop_view) != VK_SUCCESS) {
		crop_release(vk, st);
		return false;
	}
	st->crop_w = w;
	st->crop_h = h;
	st->crop_format = format;
	return true;
}

static void
dp_release(struct comp_vk_native_segments *segs, uint32_t i)
{
	struct seg_screen_state *st = &segs->st[i];
	if (st->dp != NULL) {
		U_LOG_I("segments: destroying the segment DP for screen 0x%016llx ('%s')",
		        (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name);
		xrt_display_processor_destroy(&st->dp);
	}
	st->configured = false;
	st->tolerates_resample = false;
	crop_release(segs->vk, st);
}

/*!
 * Create screen @p i's segment DP through the plug-in's per-screen factory.
 * Windowless (NULL window) on purpose: a segment DP's phase comes from
 * set_present_origin (ADR-033).
 *
 * There is deliberately NO fallback to the plain `create_dp_vk`: that DP would
 * describe the plug-in's process-wide panel instead of this screen, and
 * nothing promises it honours a sub-rect canvas or leaves the rest of the
 * target alone — it could clear the primary's segment after the primary wove.
 * A plug-in without the slot gets flat 2D on its other screens
 * (has_dp_factory is false, so this is never reached for it).
 */
static bool
dp_create(struct comp_vk_native_segments *segs, uint32_t i, int32_t target_format)
{
	struct seg_screen_state *st = &segs->st[i];
	const struct xrt_plugin_iface *iface = segs->iface[i];
	xrt_result_t xret = XRT_ERROR_DEVICE_CREATION_FAILED;
	const char *how = "none";

	if (xrt_plugin_iface_has_create_dp_vk_for_screen(iface)) {
		how = "create_dp_vk_for_screen";
		/*
		 * #868: same queue swap as vk_make_dp_vk — a vendor DP captures
		 * vk->main_queue once, at creation, for its internal submits, and the
		 * weave also runs on the repaint thread; the runtime-owned queue keeps
		 * those submits off the app's queue. Restored immediately.
		 */
		VkQueue saved = segs->vk->main_queue->queue;
		if (segs->dp_queue != VK_NULL_HANDLE) {
			segs->vk->main_queue->queue = segs->dp_queue;
		}
		xret = iface->create_dp_vk_for_screen(segs->inst[i], segs->vk, (void *)(uintptr_t)segs->cmd_pool, NULL,
		                                      target_format, &segs->bindings[i], &st->dp);
		segs->vk->main_queue->queue = saved;
	}

	if (xret != XRT_SUCCESS || st->dp == NULL || st->dp->process_atlas == NULL) {
		if (st->dp != NULL) {
			xrt_display_processor_destroy(&st->dp);
		}
		U_LOG_W("segments: could not create a DP for screen 0x%016llx ('%s', plug-in '%s') via %s (%d) — "
		        "that segment stays flat 2D",
		        (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name,
		        segs->screens[i].plugin_id, how, (int)xret);
		return false;
	}
	st->tolerates_resample = xrt_display_processor_vk_tolerates_resample((struct xrt_display_processor_vk *)st->dp);
	st->configured = false;
	// Lifecycle event (hysteresis-gated), not per frame.
	U_LOG_W("segments: created a segment DP for screen 0x%016llx ('%s', plug-in '%s') via %s — %s",
	        (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name, segs->screens[i].plugin_id,
	        how, st->tolerates_resample ? "tolerates a resample" : "needs 1:1 pixels");
	return true;
}

static int32_t
scale_round(int64_t v, int64_t num, int64_t den)
{
	// round(v * num / den), v/num/den >= 0
	return den > 0 ? (int32_t)((v * num + den / 2) / den) : 0;
}

/*!
 * A segment's rect in VIEW-TILE pixels (the atlas holds the canvas at view
 * resolution). Edges are rounded independently, so two segments sharing a
 * seam share the tile column too: no gap, no overlap.
 */
static bool
segment_tile_rect(const struct comp_seg_rect *wr,
                  const struct comp_seg_rect *canvas,
                  uint32_t view_w,
                  uint32_t view_h,
                  int32_t *x0,
                  int32_t *y0,
                  uint32_t *w,
                  uint32_t *h)
{
	if (canvas->w == 0 || canvas->h == 0) {
		return false;
	}
	const int64_t rx = (int64_t)wr->x - canvas->x;
	const int64_t ry = (int64_t)wr->y - canvas->y;
	if (rx < 0 || ry < 0) {
		return false;
	}
	const int32_t ax = scale_round(rx, view_w, canvas->w);
	const int32_t ay = scale_round(ry, view_h, canvas->h);
	int32_t bx = scale_round(rx + wr->w, view_w, canvas->w);
	int32_t by = scale_round(ry + wr->h, view_h, canvas->h);
	if (bx > (int32_t)view_w) {
		bx = (int32_t)view_w;
	}
	if (by > (int32_t)view_h) {
		by = (int32_t)view_h;
	}
	if (ax < 0 || ay < 0 || bx <= ax || by <= ay) {
		return false;
	}
	*x0 = ax;
	*y0 = ay;
	*w = (uint32_t)(bx - ax);
	*h = (uint32_t)(by - ay);
	return true;
}

static void
image_barrier(struct vk_bundle *vk,
              VkCommandBuffer cmd,
              VkImage image,
              VkImageLayout from,
              VkImageLayout to,
              VkAccessFlags src_access,
              VkAccessFlags dst_access,
              VkPipelineStageFlags src_stage,
              VkPipelineStageFlags dst_stage)
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
	vk->vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}


/*
 *
 * Exported.
 *
 */

struct comp_vk_native_segments *
comp_vk_native_segments_create(struct vk_bundle *vk, VkCommandPool cmd_pool, VkQueue dp_queue)
{
	struct comp_vk_native_segments *segs = U_TYPED_CALLOC(struct comp_vk_native_segments);
	if (segs == NULL) {
		return NULL;
	}
	segs->vk = vk;
	segs->cmd_pool = cmd_pool;
	segs->dp_queue = dp_queue;
	comp_segments_lifecycle_init(&segs->lc, 0, 0);
	return segs;
}

void
comp_vk_native_segments_destroy(struct comp_vk_native_segments **segs_ptr)
{
	if (segs_ptr == NULL || *segs_ptr == NULL) {
		return;
	}
	struct comp_vk_native_segments *segs = *segs_ptr;
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		dp_release(segs, i);
	}
	free(segs);
	*segs_ptr = NULL;
}

void
comp_vk_native_segments_set_screens(struct comp_vk_native_segments *segs,
                                    const struct xrt_screen_list *list,
                                    const struct xrt_system_compositor_info *info,
                                    uint64_t pinned_display_id)
{
	if (segs == NULL) {
		return;
	}
	// A new screen set invalidates every segment DP built for the old one.
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		dp_release(segs, i);
	}
	comp_segments_lifecycle_init(&segs->lc, 0, 0);
	segs->enabled = false;
	segs->screen_count = 0;
	segs->have_logged_table = false;
	segs->logged_split = false;
	segs->cap_count = 0;
	memset(&segs->table, 0, sizeof(segs->table));

	const char *env = getenv("DXR_SEGMENTS");
	if (env != NULL && env[0] == '0') {
		U_LOG_W("segments: DXR_SEGMENTS=0 — a window spanning screens keeps the single-DP path");
		return;
	}
	if (list == NULL || info == NULL || list->count < 2) {
		return; // one screen: nothing to segment
	}
	if (pinned_display_id != 0) {
		U_LOG_I("segments: session pinned to display 0x%016llx (XrSessionDisplayBindingDXR) — no segmentation",
		        (unsigned long long)pinned_display_id);
		return;
	}

	// The primary: the system-default screen, woven by the session's own DP.
	const struct xrt_dp_registry_entry *primary_entry = NULL;
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX; i++) {
		if ((list->screens[i].flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) == 0) {
			continue;
		}
		for (uint32_t e = 0; e < info->dp_registry.entry_count; e++) {
			if (info->dp_registry.entries[e].monitor_id == list->screens[i].id) {
				primary_entry = &info->dp_registry.entries[e];
				break;
			}
		}
	}
	if (primary_entry == NULL) {
		U_LOG_I("segments: the DP registry does not know the system-default screen — no segmentation");
		return;
	}

	uint32_t n = 0;
	bool mixed = false;
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX && n < COMP_SEGMENTS_MAX_SCREENS; i++) {
		const struct xrt_screen *s = &list->screens[i];
		if (s->desktop_width == 0 || s->desktop_height == 0) {
			continue; // no desktop placement (pure-DRM record): cannot be segmented against
		}
		const struct xrt_dp_registry_entry *e = NULL;
		for (uint32_t k = 0; k < info->dp_registry.entry_count; k++) {
			if (info->dp_registry.entries[k].monitor_id == s->id) {
				e = &info->dp_registry.entries[k];
				break;
			}
		}

		struct comp_segments_screen *cs = &segs->screens[n];
		memset(cs, 0, sizeof(*cs));
		cs->id = s->id;
		cs->desktop.x = s->desktop_left;
		cs->desktop.y = s->desktop_top;
		cs->desktop.w = s->desktop_width;
		cs->desktop.h = s->desktop_height;
		cs->native_w = s->native_width;
		cs->native_h = s->native_height;
		cs->is_primary = (e == primary_entry);
		snprintf(cs->plugin_id, sizeof(cs->plugin_id), "%s", s->plugin_id);

		segs->iface[n] = NULL;
		segs->inst[n] = NULL;
		if (e != NULL && !cs->is_primary) {
			const bool same_vendor = strcmp(e->plugin_id, primary_entry->plugin_id) == 0;
			if (!same_vendor) {
				mixed = true;
			} else {
				segs->iface[n] = (const struct xrt_plugin_iface *)e->owning_iface;
				segs->inst[n] = (struct xrt_plugin_instance *)e->owning_instance;
				// The per-screen slot, and only it (see dp_create). The
				// registry's VK factory being set is what says the claim
				// covers Vulkan and the vk_bundle ABI matched (#1243).
				cs->has_dp_factory = e->dp_factory_vk != NULL &&
				                     xrt_plugin_iface_has_create_dp_vk_for_screen(segs->iface[n]);
			}
		}

		struct xrt_screen_binding *b = &segs->bindings[n];
		memset(b, 0, sizeof(*b));
		b->struct_size = (uint32_t)sizeof(*b);
		b->monitor_id = s->id;
		b->desktop_left = s->desktop_left;
		b->desktop_top = s->desktop_top;
		b->desktop_width = s->desktop_width;
		b->desktop_height = s->desktop_height;
		b->native_pixel_width = s->native_width;
		b->native_pixel_height = s->native_height;
		b->physical_width_mm = s->physical_width_mm;
		b->physical_height_mm = s->physical_height_mm;
		b->desktop_scale = s->desktop_scale;
		snprintf(b->device_name, sizeof(b->device_name), "%s", s->device_name);
		if (e != NULL) {
			snprintf(b->serial, sizeof(b->serial), "%s", e->serial);
		}
		n++;
	}
	segs->screen_count = n;

	if (mixed) {
		// One-off session setup line. Mixed vendors (a Leia panel next to a
		// sim_display laptop panel) are multi-screen M4: the vendor DP's
		// behaviour on a sub-rect canvas next to another vendor's segment is
		// not established yet, so the window keeps the shipped single-DP
		// path rather than risk a broken weave on the 3D panel.
		U_LOG_W("segments: the screens belong to different plug-ins (primary '%s') — mixed-vendor segments are "
		        "multi-screen M4; a window spanning screens keeps the single-DP path",
		        primary_entry->plugin_id);
		return;
	}
	segs->enabled = n >= 2;
	if (segs->enabled) {
		U_LOG_W("segments: %u screens, plug-in '%s' — a window spanning screens is woven per screen "
		        "(multi-screen M2)",
		        n, primary_entry->plugin_id);
	}
}

bool
comp_vk_native_segments_enabled(const struct comp_vk_native_segments *segs)
{
	return segs != NULL && segs->enabled;
}

bool
comp_vk_native_segments_update(struct comp_vk_native_segments *segs,
                               const struct comp_seg_rect *window_desktop,
                               const struct comp_seg_rect *canvas,
                               int32_t target_format)
{
	if (segs == NULL || !segs->enabled || window_desktop == NULL) {
		return false;
	}

	comp_segments_compute(window_desktop, canvas, segs->screens, segs->screen_count, &segs->table);

	struct comp_segments_actions act;
	comp_segments_lifecycle_update(&segs->lc, &segs->table, &act);
	for (uint32_t a = 0; a < act.count; a++) {
		const int i = screen_index_of(segs, act.items[a].screen_id);
		if (i < 0) {
			continue;
		}
		if (act.items[a].action == COMP_SEG_ACTION_CREATE) {
			const bool ok = dp_create(segs, (uint32_t)i, target_format);
			comp_segments_lifecycle_set_created(&segs->lc, act.items[a].screen_id, ok);
		} else if (act.items[a].action == COMP_SEG_ACTION_DESTROY) {
			dp_release(segs, (uint32_t)i);
		}
	}

	const uint32_t cw = canvas != NULL && canvas->w > 0 ? canvas->w : window_desktop->w;
	const uint32_t ch = canvas != NULL && canvas->h > 0 ? canvas->h : window_desktop->h;
	const bool split = comp_segments_table_is_split(&segs->table, cw, ch);

	// One INFO line per table change while split, and one on leaving it —
	// a drag produces a burst, a still window logs once. Never WARN.
	if (split && (!segs->have_logged_table || !comp_segments_table_equal(&segs->table, &segs->logged_table))) {
		// The capability can follow a runtime mode switch (sim's 1/2/3 keys):
		// re-ask on change, never per frame.
		for (uint32_t i = 0; i < segs->screen_count; i++) {
			if (segs->st[i].dp != NULL) {
				segs->st[i].tolerates_resample = xrt_display_processor_vk_tolerates_resample(
				    (struct xrt_display_processor_vk *)segs->st[i].dp);
			}
		}
		char buf[768];
		comp_segments_table_format(&segs->table, buf, sizeof(buf));
		U_LOG_I("segments: window %d,%d %ux%u -> %s", window_desktop->x, window_desktop->y, window_desktop->w,
		        window_desktop->h, buf);
		segs->logged_table = segs->table;
		segs->have_logged_table = true;
	} else if (!split && segs->logged_split) {
		U_LOG_I("segments: window %d,%d %ux%u is on the primary screen only — single-DP path",
		        window_desktop->x, window_desktop->y, window_desktop->w, window_desktop->h);
		segs->have_logged_table = false;
	}
	segs->logged_split = split;
	if (!split) {
		segs->cap_count = 0;
	}
	return split;
}

bool
comp_vk_native_segments_record(struct comp_vk_native_segments *segs,
                               const struct comp_vk_native_segments_frame *f)
{
	if (segs == NULL || f == NULL || f->cmd == VK_NULL_HANDLE || f->target_image == VK_NULL_HANDLE ||
	    f->src_image == VK_NULL_HANDLE || f->tile_columns == 0 || f->tile_rows == 0) {
		return false;
	}
	struct vk_bundle *vk = segs->vk;
	VkCommandBuffer cmd = f->cmd;
	const struct comp_segment_table *t = &segs->table;

	/*
	 * 1. Decide each segment: which DP (if any) and whether it may weave.
	 */
	struct xrt_display_processor *dp_of[COMP_SEGMENTS_MAX] = {0};
	struct seg_screen_state *st_of[COMP_SEGMENTS_MAX] = {0};
	bool weave[COMP_SEGMENTS_MAX] = {false};
	int32_t tx[COMP_SEGMENTS_MAX], ty[COMP_SEGMENTS_MAX];
	uint32_t tw[COMP_SEGMENTS_MAX], th[COMP_SEGMENTS_MAX];
	bool have_tile[COMP_SEGMENTS_MAX] = {false};

	for (uint32_t k = 0; k < t->count; k++) {
		const struct comp_segment *g = &t->seg[k];
		have_tile[k] =
		    segment_tile_rect(&g->window_rect, &f->canvas, f->view_width, f->view_height, &tx[k], &ty[k], &tw[k], &th[k]);
		const int i = (int)g->screen_index;
		st_of[k] = (i >= 0 && (uint32_t)i < segs->screen_count) ? &segs->st[i] : NULL;
		if (g->is_primary) {
			// The primary screen's 1:1 gate is the session-level one
			// (#1595 / #1831), already applied to the frame's layout.
			dp_of[k] = f->primary_dp;
			weave[k] = f->primary_dp != NULL;
		} else if (st_of[k] != NULL) {
			dp_of[k] = st_of[k]->dp;
			weave[k] = comp_segments_decide(dp_of[k] != NULL, st_of[k]->tolerates_resample, g->screen_1to1) ==
			           COMP_SEG_RENDER_WEAVE;
		}
		if (!have_tile[k] || st_of[k] == NULL) {
			weave[k] = false;
		}
	}

	/*
	 * 2. Clear the target. A segment DP confines its render pass to its
	 * canvas, so whatever no segment covers (the part of the window on no
	 * screen) would otherwise be undefined.
	 */
	{
		image_barrier(vk, cmd, f->target_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		              VK_PIPELINE_STAGE_TRANSFER_BIT);
		const VkClearColorValue clear = {.float32 = {0.0f, 0.0f, 0.0f, f->transparent_background ? 0.0f : 1.0f}};
		const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vk->vkCmdClearColorImage(cmd, f->target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
		image_barrier(vk, cmd, f->target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
		              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	}

	/*
	 * 3. Crop each woven segment's views out of the atlas — crop before the
	 * DP is the law (ADR-030): a DP's atlas holds exactly its canvas.
	 */
	image_barrier(vk, cmd, f->src_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	for (uint32_t k = 0; k < t->count; k++) {
		if (!weave[k]) {
			continue;
		}
		struct seg_screen_state *st = st_of[k];
		const uint32_t cw = tw[k] * f->tile_columns;
		const uint32_t ch = th[k] * f->tile_rows;
		if (!crop_ensure(vk, st, cw, ch, f->src_format)) {
			weave[k] = false; // no input image: flat 2D instead
			continue;
		}
		image_barrier(vk, cmd, st->crop_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		              VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkImageCopy regions[XRT_MAX_VIEWS];
		uint32_t rc = 0;
		for (uint32_t row = 0; row < f->tile_rows; row++) {
			for (uint32_t col = 0; col < f->tile_columns && rc < XRT_MAX_VIEWS; col++) {
				regions[rc++] = (VkImageCopy){
				    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .srcOffset = {(int32_t)(col * f->view_width) + tx[k],
				                  (int32_t)(row * f->view_height) + ty[k], 0},
				    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .dstOffset = {(int32_t)(col * tw[k]), (int32_t)(row * th[k]), 0},
				    .extent = {tw[k], th[k], 1},
				};
			}
		}
		vk->vkCmdCopyImage(cmd, f->src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st->crop_image,
		                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rc, regions);
		image_barrier(vk, cmd, st->crop_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		              VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}
	image_barrier(vk, cmd, f->src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
	              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	/*
	 * 4. Weave: the primary first (a DP built before M2 may still clear the
	 * whole target, so it must not run after a sibling), then left to right.
	 * Each gets canvas = its segment; a DP confines viewport AND scissor to
	 * it (plan risk 7). Between two DPs the target goes back to
	 * COLOR_ATTACHMENT_OPTIMAL with its contents preserved.
	 */
	bool any_woven = false;
	for (uint32_t pass = 0; pass < 2; pass++) {
		for (uint32_t k = 0; k < t->count; k++) {
			const struct comp_segment *g = &t->seg[k];
			if (!weave[k] || g->is_primary != (pass == 0)) {
				continue;
			}
			struct xrt_display_processor *dp = dp_of[k];
			struct seg_screen_state *st = st_of[k];
			if (any_woven) {
				image_barrier(vk, cmd, f->target_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
				              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
			}
			if (!g->is_primary) {
				struct xrt_display_processor_vk *vdp = (struct xrt_display_processor_vk *)dp;
				if (!st->configured) {
					st->configured = true;
					xrt_display_processor_set_atlas_encoding(dp, XRT_ATLAS_ENCODING_ENCODED);
					xrt_display_processor_vk_set_transparent_background(vdp, f->transparent_background,
					                                                    false);
					xrt_display_processor_set_background_2d(dp, VK_NULL_HANDLE, 0, 0);
				}
				// Phase only where window px ARE device px; a resampled
				// screen's origin is in the wrong units (and only a
				// resample-tolerant DP weaves there, which has no phase).
				if (g->screen_1to1 != COMP_SEG_1TO1_NO) {
					xrt_display_processor_vk_set_present_origin(vdp, g->present_origin_x,
					                                            g->present_origin_y);
				}
				xrt_display_processor_set_target_color_view(dp, f->target_view);
			}
			xrt_display_processor_process_atlas(
			    dp, cmd, (VkImage_XDP)st->crop_image, st->crop_view, tw[k], th[k], f->tile_columns,
			    f->tile_rows, (VkFormat_XDP)f->src_format, f->target_fb, (VkImage_XDP)f->target_image,
			    f->target_width, f->target_height, (VkFormat_XDP)f->target_format, g->window_rect.x,
			    g->window_rect.y, g->window_rect.w, g->window_rect.h);
			any_woven = true;
		}
	}
	if (!any_woven) {
		image_barrier(vk, cmd, f->target_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
		              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	}

	/*
	 * 5. Flat 2D for every segment that could not be woven, and for the
	 * canvas no segment covers (a monitor no plug-in claimed, or off every
	 * screen): one view (the middle one — the left eye of a stereo pair),
	 * linearly scaled from the view tile to the window rect. Same content and
	 * seam registration as the shipped off-panel band (#1654).
	 */
	struct comp_seg_rect flat_dst[COMP_SEGMENTS_MAX + COMP_SEGMENTS_MAX_UNCOVERED];
	uint32_t flat_n = 0;
	for (uint32_t k = 0; k < t->count; k++) {
		if (!weave[k] && have_tile[k]) {
			flat_dst[flat_n++] = t->seg[k].window_rect;
		}
	}
	flat_n += comp_segments_uncovered(t, &f->canvas, &flat_dst[flat_n], COMP_SEGMENTS_MAX_UNCOVERED);
	if (flat_n > 0) {
		const uint32_t views = f->tile_columns * f->tile_rows;
		const uint32_t vi = views > 0 ? (views - 1) / 2 : 0;
		const int32_t base_x = (int32_t)((vi % f->tile_columns) * f->view_width);
		const int32_t base_y = (int32_t)((vi / f->tile_columns) * f->view_height);
		image_barrier(vk, cmd, f->src_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
		              VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		              VK_PIPELINE_STAGE_TRANSFER_BIT);
		image_barrier(vk, cmd, f->target_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		              VK_PIPELINE_STAGE_TRANSFER_BIT);
		for (uint32_t k = 0; k < flat_n; k++) {
			const struct comp_seg_rect *d = &flat_dst[k];
			int32_t sx = 0, sy = 0;
			uint32_t sw = 0, sh = 0;
			if (!segment_tile_rect(d, &f->canvas, f->view_width, f->view_height, &sx, &sy, &sw, &sh)) {
				continue; // thinner than one source pixel
			}
			VkImageBlit blit = {
			    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			    .srcOffsets = {{base_x + sx, base_y + sy, 0},
			                   {base_x + sx + (int32_t)sw, base_y + sy + (int32_t)sh, 1}},
			    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			    .dstOffsets = {{d->x, d->y, 0}, {d->x + (int32_t)d->w, d->y + (int32_t)d->h, 1}},
			};
			vk->vkCmdBlitImage(cmd, f->src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f->target_image,
			                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
		}
		image_barrier(vk, cmd, f->target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
		              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		image_barrier(vk, cmd, f->src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
		              VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	// What the capture can show for this frame.
	segs->cap_count = 0;
	for (uint32_t k = 0; k < t->count && segs->cap_count < COMP_SEGMENTS_MAX; k++) {
		struct seg_capture *c = &segs->cap[segs->cap_count++];
		c->screen_index = t->seg[k].screen_index;
		c->woven = weave[k];
		c->w = weave[k] ? tw[k] * f->tile_columns : 0;
		c->h = weave[k] ? th[k] * f->tile_rows : 0;
	}
	return any_woven;
}

bool
comp_vk_native_segments_get_capture(const struct comp_vk_native_segments *segs,
                                    uint32_t index,
                                    VkImage *out_image,
                                    uint32_t *out_w,
                                    uint32_t *out_h,
                                    uint64_t *out_screen_id,
                                    bool *out_woven)
{
	if (segs == NULL || index >= segs->cap_count) {
		return false;
	}
	const struct seg_capture *c = &segs->cap[index];
	if (c->screen_index >= segs->screen_count) {
		return false;
	}
	const struct seg_screen_state *st = &segs->st[c->screen_index];
	*out_image = c->woven ? st->crop_image : VK_NULL_HANDLE;
	*out_w = c->w;
	*out_h = c->h;
	*out_screen_id = segs->screens[c->screen_index].id;
	*out_woven = c->woven;
	return true;
}
