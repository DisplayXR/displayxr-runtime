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

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * State.
 *
 */

//! One cropped DP input image.
struct seg_crop
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	uint32_t w;
	uint32_t h;
	VkFormat format;
};

//! Which frame class records into a crop image (see seg_screen_state::crop).
enum seg_frame_class
{
	SEG_CLASS_APP = 0,
	SEG_CLASS_REPAINT = 1,
	SEG_CLASS_COUNT = 2,
};

//! What a retired handle is (comp_segments_retire kinds).
enum seg_retire_kind
{
	SEG_RETIRE_DP = 1,
	SEG_RETIRE_VIEW = 2,
	SEG_RETIRE_IMAGE = 3,
	SEG_RETIRE_MEMORY = 4,
};

//! Per-screen state: the screen's segment DP (NULL for the primary screen,
//! whose DP the session owns) and the cropped DP input for its segment.
struct seg_screen_state
{
	struct xrt_display_processor *dp;
	bool tolerates_resample;
	//! The one-time DP setup (no 2D-under backdrop) is done.
	bool configured;
	//! Last atlas encoding / transparency declared to @ref dp (-1 = never),
	//! re-declared on change, like the primary's (#1484, #573).
	int encoding_latched;
	int transparent_latched;
	//! The session-wide 2D/3D mode last sent to this DP (-1 = never).
	int mode_sent;
	//! Windows (ADR-047 Amendment 2): this DP holds the session's window —
	//! it is the owner, and phases from the window, not the present origin.
	bool has_hwnd;

	/*!
	 * One DP input image PER FRAME CLASS. The repaint ("fill") parks on its
	 * fence with the compositor lock released (#1264 S1), so an app frame can
	 * record while the fill's command buffer is still executing: sharing one
	 * image would let the app's copy overwrite what the in-flight fill samples
	 * (and a resize would destroy it). Each class only ever touches its own,
	 * and each class waits its own previous submission before recording, so a
	 * class may recreate its own image immediately.
	 */
	struct seg_crop crop[SEG_CLASS_COUNT];
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
	//! Display info per screen (physical size, nominal viewer) — M3 views.
	struct xrt_screen_info info[COMP_SEGMENTS_MAX_SCREENS];
	//! Owning plug-in iface / instance per screen (registry).
	const struct xrt_plugin_iface *iface[COMP_SEGMENTS_MAX_SCREENS];
	struct xrt_plugin_instance *inst[COMP_SEGMENTS_MAX_SCREENS];
	struct seg_screen_state st[COMP_SEGMENTS_MAX_SCREENS];

	struct comp_segments_lifecycle lc;
	struct comp_segment_table table;
	struct comp_segment_table logged_table;
	bool have_logged_table;
	bool logged_split;

	//! The session-wide hardware 2D/3D mode every segment DP follows (-1 = not set yet).
	int display_mode;

	struct seg_capture cap[COMP_SEGMENTS_MAX];
	uint32_t cap_count;

	//! Releases that must wait until no fill is in flight (see
	//! comp_segments_retire). Drained by update() when the caller says it is
	//! safe, and by destroy.
	struct comp_segments_retire retire;
	//! True only inside an update() the caller declared release-safe, and in
	//! destroy — the debug assert in release_now() checks it.
	bool release_safe;

	//! Rendering-mode index the segment DPs' tolerance was last read at.
	uint32_t mode_index;

	/*
	 * Who holds the window handle (ADR-047 Amendment 2, Windows). Off the
	 * primary, the owner's DP lives in st[owner_index].dp with has_hwnd set,
	 * and the session's primary DP is a windowless per-screen DP the
	 * compositor swapped in through hooks.swap_primary.
	 */
	struct comp_vk_native_segments_hwnd_hooks hooks;
	struct comp_segments_owner owner;
	uint32_t primary_index;  //!< index of the primary screen, UINT32_MAX = none
	int owner_index;         //!< index of the screen holding the handle, -1 = none
	bool primary_for_screen; //!< the primary's plug-in has create_dp_vk_for_screen
	bool primary_windowless; //!< the session's primary DP is windowless right now
	//! The last update's swapchain format (the hand-back at destroy / set_screens).
	int32_t target_format;
	bool have_target_format;

	/*!
	 * Guards every seg_screen_state::dp pointer swap against
	 * @ref comp_vk_native_segments_get_eyes, which the state tracker calls
	 * from the app thread (xrLocateViews) while the weave may create or
	 * destroy segment DPs.
	 */
	struct os_mutex dp_mutex;
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

/*!
 * Destroy one handle NOW. Only legal when no submitted work can reference it:
 * a fill parked with the lock released may still be executing a command buffer
 * that does.
 */
static void
release_now(struct comp_vk_native_segments *segs, uint32_t kind, uint64_t item)
{
	assert(segs->release_safe && "segments: release while a fill may be in flight");
	struct vk_bundle *vk = segs->vk;
	switch (kind) {
	case SEG_RETIRE_DP: {
		struct xrt_display_processor *dp = (struct xrt_display_processor *)(uintptr_t)item;
		xrt_display_processor_destroy(&dp);
		break;
	}
	case SEG_RETIRE_VIEW: vk->vkDestroyImageView(vk->device, (VkImageView)(uintptr_t)item, NULL); break;
	case SEG_RETIRE_IMAGE: vk->vkDestroyImage(vk->device, (VkImage)(uintptr_t)item, NULL); break;
	case SEG_RETIRE_MEMORY: vk->vkFreeMemory(vk->device, (VkDeviceMemory)(uintptr_t)item, NULL); break;
	default: break;
	}
}

/*!
 * Release @p item, deferred onto the retire list. A full list (a pathological
 * pile-up while a fill stays parked) leaks the handle with one WARN rather than
 * free something the GPU may still read.
 */
static void
release_deferred(struct comp_vk_native_segments *segs, uint32_t kind, uint64_t item)
{
	if (item == 0) {
		return;
	}
	if (!comp_segments_retire_push(&segs->retire, kind, item)) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			U_LOG_W("segments: retire list full — leaking a released object rather than freeing it under "
			        "an in-flight fill");
		}
	}
}

//! Hand everything retired back to Vulkan, if @p safe.
static void
retire_drain(struct comp_vk_native_segments *segs, bool safe)
{
	uint32_t kinds[COMP_SEGMENTS_RETIRE_MAX];
	uint64_t items[COMP_SEGMENTS_RETIRE_MAX];
	const uint32_t n = comp_segments_retire_take(&segs->retire, safe, kinds, items, COMP_SEGMENTS_RETIRE_MAX);
	for (uint32_t i = 0; i < n; i++) {
		release_now(segs, kinds[i], items[i]);
	}
}

//! Retire a crop image (deferred: another frame class may still read it).
static void
crop_retire(struct comp_vk_native_segments *segs, struct seg_crop *c)
{
	release_deferred(segs, SEG_RETIRE_VIEW, (uint64_t)(uintptr_t)c->view);
	release_deferred(segs, SEG_RETIRE_IMAGE, (uint64_t)(uintptr_t)c->image);
	release_deferred(segs, SEG_RETIRE_MEMORY, (uint64_t)(uintptr_t)c->memory);
	memset(c, 0, sizeof(*c));
}

/*!
 * Destroy a crop image immediately. Only for the recording class's OWN image:
 * that class waited its previous submission before this frame, and no other
 * class ever touches it.
 */
static void
crop_destroy_own(struct vk_bundle *vk, struct seg_crop *c)
{
	if (c->view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, c->view, NULL);
	}
	if (c->image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->image, NULL);
	}
	if (c->memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->memory, NULL);
	}
	memset(c, 0, sizeof(*c));
}

/*!
 * (Re)create a segment's DP input image at exactly @p w x @p h — a DP reads its
 * atlas as `tile_columns * view_width` wide, so the image must be exactly the
 * segment's tile grid (the same reason the single-DP path crops, ADR-030).
 */
static bool
crop_ensure(struct vk_bundle *vk, struct seg_crop *c, uint32_t w, uint32_t h, VkFormat format)
{
	if (c->image != VK_NULL_HANDLE && c->w == w && c->h == h && c->format == format) {
		return true;
	}
	crop_destroy_own(vk, c);

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
	if (vk->vkCreateImage(vk->device, &ici, NULL, &c->image) != VK_SUCCESS) {
		c->image = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryRequirements req;
	vk->vkGetImageMemoryRequirements(vk->device, c->image, &req);
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
		crop_destroy_own(vk, c);
		return false;
	}
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = req.size,
	    .memoryTypeIndex = type,
	};
	if (vk->vkAllocateMemory(vk->device, &mai, NULL, &c->memory) != VK_SUCCESS ||
	    vk->vkBindImageMemory(vk->device, c->image, c->memory, 0) != VK_SUCCESS) {
		crop_destroy_own(vk, c);
		return false;
	}

	VkImageViewCreateInfo vci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = c->image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = format,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	if (vk->vkCreateImageView(vk->device, &vci, NULL, &c->view) != VK_SUCCESS) {
		crop_destroy_own(vk, c);
		return false;
	}
	c->w = w;
	c->h = h;
	c->format = format;
	return true;
}

static void
dp_release(struct comp_vk_native_segments *segs, uint32_t i)
{
	struct seg_screen_state *st = &segs->st[i];
	if (st->dp != NULL) {
		U_LOG_I("segments: retiring the segment DP for screen 0x%016llx ('%s')",
		        (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name);
		// The app thread reads st->dp for eyes (M3): swap under the lock,
		// release (deferred past any in-flight fill) after it.
		os_mutex_lock(&segs->dp_mutex);
		struct xrt_display_processor *dp = st->dp;
		st->dp = NULL;
		os_mutex_unlock(&segs->dp_mutex);
		release_deferred(segs, SEG_RETIRE_DP, (uint64_t)(uintptr_t)dp);
	}
	st->configured = false;
	st->encoding_latched = -1;
	st->transparent_latched = -1;
	st->tolerates_resample = false;
	st->has_hwnd = false;
	for (uint32_t k = 0; k < SEG_CLASS_COUNT; k++) {
		crop_retire(segs, &st->crop[k]);
	}
	st->mode_sent = -1;
}

/*!
 * Make (not install) a DP for screen @p i through the plug-in's per-screen
 * factory. @p window is NULL for a windowless segment DP — its phase comes from
 * set_present_origin (ADR-033) — and, on Windows only, the session's real HWND
 * for the screen that owns it (the majority screen, whose DP keeps the vendor's
 * drag phase-snap, ADR-047 Amendment 2).
 *
 * There is deliberately NO fallback to the plain `create_dp_vk`: that DP would
 * describe the plug-in's process-wide panel instead of this screen, and
 * nothing promises it honours a sub-rect canvas or leaves the rest of the
 * target alone — it could clear the primary's segment after the primary wove.
 * A plug-in without the slot gets flat 2D on its other screens
 * (has_dp_factory is false, so this is never reached for it).
 */
static struct xrt_display_processor *
dp_make(struct comp_vk_native_segments *segs, uint32_t i, int32_t target_format, void *window)
{
	const struct xrt_plugin_iface *iface = segs->iface[i];
	const bool can = segs->screens[i].is_primary ? segs->primary_for_screen : segs->screens[i].has_dp_factory;
	xrt_result_t xret = XRT_ERROR_DEVICE_CREATION_FAILED;

	struct xrt_display_processor *dp = NULL;
	if (can && xrt_plugin_iface_has_create_dp_vk_for_screen(iface)) {
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
		xret = iface->create_dp_vk_for_screen(segs->inst[i], segs->vk, (void *)(uintptr_t)segs->cmd_pool,
		                                      window, target_format, &segs->bindings[i], &dp);
		segs->vk->main_queue->queue = saved;
	}

	if (xret != XRT_SUCCESS || dp == NULL || dp->process_atlas == NULL) {
		if (dp != NULL) {
			xrt_display_processor_destroy(&dp);
		}
		U_LOG_W(
		    "segments: could not create a %s DP for screen 0x%016llx ('%s', plug-in '%s') via "
		    "create_dp_vk_for_screen (%d)",
		    window != NULL ? "window-holding" : "windowless", (unsigned long long)segs->screens[i].id,
		    segs->bindings[i].device_name, segs->screens[i].plugin_id, (int)xret);
		return NULL;
	}
	return dp;
}

/*!
 * Install @p dp (may be NULL) as screen @p i's segment DP, latches reset so
 * the next weave configures it. Returns the DP it replaced, for the caller to
 * retire (once this returns, get_eyes can no longer reach it).
 */
static struct xrt_display_processor *
dp_install(struct comp_vk_native_segments *segs, uint32_t i, struct xrt_display_processor *dp, bool has_hwnd)
{
	struct seg_screen_state *st = &segs->st[i];
	// The app thread reads st->dp for eyes (M3): swap under the lock.
	os_mutex_lock(&segs->dp_mutex);
	struct xrt_display_processor *old = st->dp;
	st->dp = dp;
	os_mutex_unlock(&segs->dp_mutex);
	st->tolerates_resample =
	    dp != NULL && xrt_display_processor_vk_tolerates_resample((struct xrt_display_processor_vk *)dp);
	st->has_hwnd = dp != NULL && has_hwnd;
	st->configured = false;
	st->encoding_latched = -1;
	st->transparent_latched = -1;
	st->mode_sent = -1;
	if (dp == NULL) {
		for (uint32_t k = 0; k < SEG_CLASS_COUNT; k++) {
			crop_retire(segs, &st->crop[k]);
		}
	}
	return old;
}

/*!
 * The lifecycle's CREATE: a windowless segment DP for screen @p i. A screen
 * that already has a DP (it holds the window handle) keeps it.
 */
static bool
dp_create(struct comp_vk_native_segments *segs, uint32_t i, int32_t target_format)
{
	if (segs->st[i].dp != NULL) {
		return true;
	}
	struct xrt_display_processor *dp = dp_make(segs, i, target_format, NULL);
	if (dp == NULL) {
		U_LOG_W("segments: screen 0x%016llx's segment stays flat 2D", (unsigned long long)segs->screens[i].id);
		return false;
	}
	(void)dp_install(segs, i, dp, false);
	// Lifecycle event (hysteresis-gated), not per frame.
	U_LOG_W(
	    "segments: created a segment DP for screen 0x%016llx ('%s', plug-in '%s') via "
	    "create_dp_vk_for_screen — %s",
	    (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name, segs->screens[i].plugin_id,
	    segs->st[i].tolerates_resample ? "tolerates a resample" : "needs 1:1 pixels");
	return true;
}

static const char *
screen_name(const struct comp_vk_native_segments *segs, int i)
{
	return i >= 0 ? segs->bindings[i].device_name : "(none)";
}

/*!
 * Put @p dp in screen @p i's slot — as the DP holding the window handle
 * (@p has_hwnd) or as a windowless one — and retire what it replaces (deferred:
 * a parked fill may still execute a command buffer that references it). The
 * primary's slot is the session's own DP, swapped by the compositor.
 */
static void
slot_replace(struct comp_vk_native_segments *segs, uint32_t i, struct xrt_display_processor *dp, bool has_hwnd)
{
	struct xrt_display_processor *old = NULL;
	if (i == segs->primary_index) {
		old = segs->hooks.swap_primary(segs->hooks.userdata, dp);
		segs->primary_windowless = !has_hwnd;
	} else {
		old = dp_install(segs, i, dp, has_hwnd);
		if (dp != NULL) {
			// The lifecycle tracks this screen's DP as live: no second create.
			comp_segments_lifecycle_set_created(&segs->lc, segs->screens[i].id, true);
		}
	}
	release_deferred(segs, SEG_RETIRE_DP, (uint64_t)(uintptr_t)old);
}

/*!
 * Move the window handle from the current owner to screen @p t (ADR-047
 * Amendment 2) — the D3D11 manager's hwnd_handoff, in Vulkan terms. One weaver
 * per window, and a live DP's window cannot be re-pointed, so:
 *   1. make a windowless replacement for the old owner (the primary always
 *      needs one; another screen only while the window still covers it) —
 *      failing that, abort with nothing changed;
 *   2. swap it in and retire the old owner's DP, which frees the window;
 *   3. make the target's DP WITH the window and swap it in — failing that,
 *      roll back (the old owner takes the window again).
 * All of it runs on the weave thread between two weaves (the caller holds the
 * compositor lock), so a frame weaves with the old pair or the new pair and
 * never with a gap.
 *
 * Retiring is deferred while a fill may be in flight, and the WINDOW is
 * released only when the old DP is really destroyed. So the old owner's DP is
 * handed back to Vulkan before step 3 whenever the caller declared this update
 * release-safe (the common case); otherwise a vendor that refuses a second
 * weaver per window refuses step 3, the hand-off rolls back, and the owner
 * hysteresis does not retry until the majority leaves and comes back — never
 * a gap, never a flap.
 */
static void
hwnd_handoff(struct comp_vk_native_segments *segs, uint32_t t, int32_t target_format)
{
	const int o = segs->owner_index;
	if (o == (int)t) {
		return;
	}
	const uint64_t t_id = segs->screens[t].id;
	const uint64_t t0 = os_monotonic_get_ns();

	// 1. The old owner's windowless replacement.
	struct xrt_display_processor *w = NULL;
	const bool o_needs_dp = o >= 0 && comp_segments_owner_needs_windowless_replacement(
	                                      &segs->table, segs->screens[o].id, (uint32_t)o == segs->primary_index,
	                                      segs->screens[o].has_dp_factory);
	if (o_needs_dp) {
		w = dp_make(segs, (uint32_t)o, target_format, NULL);
		if (w == NULL) {
			U_LOG_W("segments: the window handle stays with '%s' — no windowless DP to replace it with",
			        screen_name(segs, o));
			comp_segments_owner_set_result(&segs->owner, t_id, false, segs->screens[o].id);
			return;
		}
	}

	// 2. Free the window.
	if (segs->hooks.bracket != NULL) {
		segs->hooks.bracket(segs->hooks.userdata, true, NULL);
	}
	if (o >= 0) {
		slot_replace(segs, (uint32_t)o, w, false);
	}
	// The old owner's DP is on the retire list; it releases the window only
	// when destroyed, so hand it back now if no fill is in flight.
	if (segs->release_safe) {
		retire_drain(segs, true);
	}

	// 3. The target takes the window.
	struct xrt_display_processor *h = dp_make(segs, t, target_format, segs->hooks.hwnd);
	int now_owner = -1;
	if (h != NULL) {
		slot_replace(segs, t, h, true);
		now_owner = (int)t;
	} else if (o >= 0) {
		// Roll back: the old owner takes it again.
		struct xrt_display_processor *back = dp_make(segs, (uint32_t)o, target_format, segs->hooks.hwnd);
		if (back != NULL) {
			slot_replace(segs, (uint32_t)o, back, true);
			now_owner = o;
		}
	}
	segs->owner_index = now_owner;
	if (segs->hooks.bracket != NULL) {
		// The DP the drag snap asks now; NULL = the session's primary DP.
		struct xrt_display_processor *hdp = NULL;
		if (now_owner >= 0 && (uint32_t)now_owner != segs->primary_index) {
			hdp = segs->st[now_owner].dp;
		}
		segs->hooks.bracket(segs->hooks.userdata, false, hdp);
	}
	comp_segments_owner_set_result(&segs->owner, t_id, h != NULL, now_owner >= 0 ? segs->screens[now_owner].id : 0);

	// One line per flip (the flip itself is hysteresis-gated).
	const double ms = (double)(os_monotonic_get_ns() - t0) / 1.0e6;
	if (h != NULL) {
		U_LOG_W(
		    "segments: window handle handed from '%s' to '%s' (screen 0x%016llx holds the majority) in "
		    "%.1f ms — drag snap follows it; '%s' weaves windowless (Vulkan)",
		    screen_name(segs, o), screen_name(segs, (int)t), (unsigned long long)t_id, ms,
		    screen_name(segs, o));
	} else {
		U_LOG_W("segments: window handle hand-off to '%s' FAILED after %.1f ms — %s (Vulkan)",
		        screen_name(segs, (int)t), ms,
		        now_owner >= 0 ? "rolled back to the previous owner" : "no DP holds the window now");
	}
}

/*!
 * Hand the window back to the primary screen's DP (a screen-list rebuild or a
 * teardown while another screen owns it). No-op when the primary owns it.
 */
static void
hwnd_return_to_primary(struct comp_vk_native_segments *segs)
{
	if (segs->hooks.swap_primary == NULL || segs->primary_index == UINT32_MAX || !segs->have_target_format ||
	    (segs->owner_index == (int)segs->primary_index && !segs->primary_windowless)) {
		return;
	}
	hwnd_handoff(segs, segs->primary_index, segs->target_format);
}

/*!
 * A segment's rect in VIEW-TILE pixels — comp_segments_tile_rect, the one
 * mapping the M3 view routing shares (so a segment's views land exactly where
 * this crop reads them).
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
	struct comp_seg_rect r;
	if (!comp_segments_tile_rect(wr, canvas, view_w, view_h, &r)) {
		return false;
	}
	*x0 = r.x;
	*y0 = r.y;
	*w = r.w;
	*h = r.h;
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
	segs->display_mode = -1;
	segs->primary_index = UINT32_MAX;
	segs->owner_index = -1;
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		segs->st[i].mode_sent = -1;
	}
	comp_segments_lifecycle_init(&segs->lc, 0, 0);
	if (os_mutex_init(&segs->dp_mutex) != 0) {
		free(segs);
		return NULL;
	}
	return segs;
}

void
comp_vk_native_segments_destroy(struct comp_vk_native_segments **segs_ptr)
{
	if (segs_ptr == NULL || *segs_ptr == NULL) {
		return;
	}
	struct comp_vk_native_segments *segs = *segs_ptr;
	// The caller guarantees nothing in flight (see the header).
	segs->release_safe = true;
	// Another screen holds the window: hand it back to the session's primary
	// DP first (no hooks = compositor teardown, where the primary DP goes
	// too). An empty table: the old owner needs no windowless replacement.
	segs->table.count = 0;
	hwnd_return_to_primary(segs);
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		dp_release(segs, i);
	}
	retire_drain(segs, true);
	segs->release_safe = false;
	os_mutex_destroy(&segs->dp_mutex);
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
	// The window goes back to the primary DP first (an empty table: the old
	// owner needs no windowless replacement). Deferred releases only: a fill
	// may be in flight.
	segs->table.count = 0;
	hwnd_return_to_primary(segs);
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		dp_release(segs, i);
	}
	comp_segments_lifecycle_init(&segs->lc, 0, 0);
	segs->primary_index = UINT32_MAX;
	segs->owner_index = -1;
	segs->primary_for_screen = false;
	segs->primary_windowless = false;
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
	char vendors[256] = {0};
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
		if (cs->is_primary) {
			// The primary's own plug-in, for the window-handle hand-off
			// (Windows): a windowless primary DP while another screen
			// holds the window.
			segs->iface[n] = (const struct xrt_plugin_iface *)e->owning_iface;
			segs->inst[n] = (struct xrt_plugin_instance *)e->owning_instance;
			segs->primary_for_screen =
			    e->dp_factory_vk != NULL && xrt_plugin_iface_has_create_dp_vk_for_screen(segs->iface[n]);
			segs->primary_index = n;
		} else if (e != NULL) {
			/*
			 * Multi-screen M3 (closes the M4 runtime gap): a segment's DP
			 * comes from ITS screen's registry entry, whichever plug-in
			 * that is — the active one or a claim source the loader keeps
			 * resident (M0). `dp_factory_vk` is only set when the plug-in
			 * passed the vk_bundle ABI check (#1243), so it gates the
			 * per-screen slot too: a plug-in whose VK factory was refused
			 * gets no segment DP (that segment is flat 2D).
			 */
			if (strcmp(e->plugin_id, primary_entry->plugin_id) != 0) {
				mixed = true;
				if (strstr(vendors, e->plugin_id) == NULL) {
					size_t used = strlen(vendors);
					snprintf(vendors + used, sizeof(vendors) - used, "%s%s", used > 0 ? ", " : "",
					         e->plugin_id);
				}
			}
			segs->iface[n] = (const struct xrt_plugin_iface *)e->owning_iface;
			segs->inst[n] = (struct xrt_plugin_instance *)e->owning_instance;
			// The per-screen slot, and only it (see dp_create). The
			// registry's VK factory being set is what says the claim
			// covers Vulkan and the vk_bundle ABI matched (#1243).
			cs->has_dp_factory =
			    e->dp_factory_vk != NULL && xrt_plugin_iface_has_create_dp_vk_for_screen(segs->iface[n]);
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
		segs->info[n] = s->info;
		n++;
	}
	segs->screen_count = n;

	if (mixed) {
		// One-off session setup line: each screen is woven by its own
		// plug-in's DP (a Leia panel next to a sim_display laptop panel).
		U_LOG_I(
		    "segments: mixed-vendor screen table — primary '%s', other screens by '%s'; each segment DP "
		    "comes from its own screen's plug-in",
		    primary_entry->plugin_id, vendors);
	}
	segs->enabled = n >= 2;
	if (segs->primary_index == UINT32_MAX) {
		segs->primary_for_screen = false; // no primary segment: nothing to hand off from
	}
	if (segs->enabled) {
		if (segs->primary_index != UINT32_MAX) {
			segs->owner_index = (int)segs->primary_index;
			comp_segments_owner_init(&segs->owner, segs->screens[segs->primary_index].id, 0, 0);
		}
		const bool handoff = segs->hooks.swap_primary != NULL && segs->primary_for_screen;
		U_LOG_W(
		    "segments: %u screens, plug-in '%s' — a window spanning screens is woven per screen "
		    "(multi-screen, Vulkan); the window handle %s",
		    n, primary_entry->plugin_id,
		    handoff ? "follows the majority screen"
		            : (segs->hooks.swap_primary == NULL
		                   ? "stays with the primary DP (no window to hand off)"
		                   : "stays with the primary DP (its plug-in has no create_dp_vk_for_screen)"));
	}
}

void
comp_vk_native_segments_set_hwnd_hooks(struct comp_vk_native_segments *segs,
                                       const struct comp_vk_native_segments_hwnd_hooks *hooks)
{
	if (segs == NULL) {
		return;
	}
	if (hooks == NULL || hooks->hwnd == NULL || hooks->swap_primary == NULL) {
		memset(&segs->hooks, 0, sizeof(segs->hooks));
		return;
	}
	segs->hooks = *hooks;
}

uint64_t
comp_vk_native_segments_get_owner(const struct comp_vk_native_segments *segs)
{
	if (segs == NULL || !segs->enabled || segs->owner_index < 0 ||
	    (uint32_t)segs->owner_index >= segs->screen_count) {
		return 0;
	}
	return segs->screens[segs->owner_index].id;
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
                               int32_t target_format,
                               bool release_safe,
                               uint32_t mode_index)
{
	if (segs == NULL) {
		return false;
	}
	// Retired DPs / images go back to Vulkan only when no fill is in flight.
	segs->release_safe = release_safe;
	retire_drain(segs, release_safe);
	if (!segs->enabled || window_desktop == NULL) {
		segs->release_safe = false;
		return false;
	}
	segs->target_format = target_format;
	segs->have_target_format = true;

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
		} else if (act.items[a].action == COMP_SEG_ACTION_DESTROY && !segs->st[i].has_hwnd) {
			// The DP holding the window outlives its segment: it moves
			// only by a hand-off (below), never by a retire.
			dp_release(segs, (uint32_t)i);
		}
	}

	/*
	 * ADR-047 Amendment 2 (Windows): the window handle follows the majority
	 * screen, hysteresis in comp_segments_owner (a clear margin held for
	 * 0.5 s). No hooks (Linux, a windowless session) = never.
	 */
	if (segs->hooks.swap_primary != NULL && segs->primary_for_screen) {
		const uint64_t target = comp_segments_owner_update(&segs->owner, &segs->table, os_monotonic_get_ns());
		const int ti = target != 0 ? screen_index_of(segs, target) : -1;
		if (ti >= 0) {
			hwnd_handoff(segs, (uint32_t)ti, target_format);
		}
	}
	segs->release_safe = false;

	const uint32_t cw = canvas != NULL && canvas->w > 0 ? canvas->w : window_desktop->w;
	const uint32_t ch = canvas != NULL && canvas->h > 0 ? canvas->h : window_desktop->h;
	const bool split = comp_segments_table_is_split(&segs->table, cw, ch);

	// A DP's resample tolerance can follow the rendering mode (sim_display:
	// INTERLACED needs 1:1). Re-ask on a mode change — never per frame.
	if (mode_index != segs->mode_index) {
		segs->mode_index = mode_index;
		for (uint32_t i = 0; i < segs->screen_count; i++) {
			if (segs->st[i].dp != NULL) {
				segs->st[i].tolerates_resample = xrt_display_processor_vk_tolerates_resample(
				    (struct xrt_display_processor_vk *)segs->st[i].dp);
			}
		}
	}

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
	// A windowless primary DP needs its present origin, which only the split
	// path sends: keep it while another screen (or nobody) holds the window,
	// even with the window back on the primary alone (until the hand-back).
	return split || segs->primary_windowless;
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
	const uint32_t cls = f->is_repaint ? SEG_CLASS_REPAINT : SEG_CLASS_APP;

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
		// #1883: where THIS segment's views were painted — for a routed frame
		// the partition it was located for, not the live seam.
		{
			struct comp_seg_rect r;
			have_tile[k] =
			    comp_segments_source_rect(g, &f->canvas, f->view_width, f->view_height, f->content, &r);
			tx[k] = r.x;
			ty[k] = r.y;
			tw[k] = r.w;
			th[k] = r.h;
		}
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
		struct seg_crop *crop = &st->crop[cls];
		if (!crop_ensure(vk, crop, cw, ch, f->src_format)) {
			weave[k] = false; // no input image: flat 2D instead
			continue;
		}
		image_barrier(vk, cmd, crop->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
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
		vk->vkCmdCopyImage(cmd, f->src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, crop->image,
		                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rc, regions);
		image_barrier(vk, cmd, crop->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
			if (g->is_primary) {
				// The session feeds its own DP everything else; windowless
				// (another screen holds the window), it needs its phase here.
				if (segs->primary_windowless && g->screen_1to1 != COMP_SEG_1TO1_NO) {
					xrt_display_processor_vk_set_present_origin(
					    (struct xrt_display_processor_vk *)dp, g->present_origin_x,
					    g->present_origin_y);
				}
			} else {
				struct xrt_display_processor_vk *vdp = (struct xrt_display_processor_vk *)dp;
				if (!st->configured) {
					st->configured = true;
					xrt_display_processor_set_background_2d(dp, VK_NULL_HANDLE, 0, 0);
				}
				// Same declarations the primary gets, on change only: the
				// atlas encoding (DXR_VK_ATLAS_ENCODING honoured by the
				// caller; -1 = do not declare) and the session's
				// transparency, which can toggle mid-session.
				if (f->atlas_encoding >= 0 && st->encoding_latched != f->atlas_encoding) {
					st->encoding_latched = f->atlas_encoding;
					xrt_display_processor_set_atlas_encoding(dp,
					                                         (enum xrt_atlas_encoding)f->atlas_encoding);
				}
				if (st->transparent_latched != (int)f->transparent_background) {
					st->transparent_latched = (int)f->transparent_background;
					xrt_display_processor_vk_set_transparent_background(vdp, f->transparent_background,
					                                                    false);
				}
				// Phase only where window px ARE device px; a resampled
				// screen's origin is in the wrong units (and only a
				// resample-tolerant DP weaves there, which has no phase).
				// The DP holding the window phases from it, like the
				// primary always has.
				if (!st->has_hwnd && g->screen_1to1 != COMP_SEG_1TO1_NO) {
					xrt_display_processor_vk_set_present_origin(vdp, g->present_origin_x,
					                                            g->present_origin_y);
				}
				xrt_display_processor_set_target_color_view(dp, f->target_view);
			}
			xrt_display_processor_process_atlas(
			    dp, cmd, (VkImage_XDP)st->crop[cls].image, st->crop[cls].view, tw[k], th[k], f->tile_columns,
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
	struct comp_seg_rect flat_src[COMP_SEGMENTS_MAX] = {0};
	uint32_t flat_n = 0;
	for (uint32_t k = 0; k < t->count; k++) {
		if (!weave[k] && have_tile[k]) {
			// The segment's own views (#1883), like the crop.
			flat_src[flat_n] = (struct comp_seg_rect){tx[k], ty[k], tw[k], th[k]};
			flat_dst[flat_n++] = t->seg[k].window_rect;
		}
	}
	const uint32_t flat_seg_n = flat_n;
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
			if (k < flat_seg_n) {
				sx = flat_src[k].x;
				sy = flat_src[k].y;
				sw = flat_src[k].w;
				sh = flat_src[k].h;
			} else if (!segment_tile_rect(d, &f->canvas, f->view_width, f->view_height, &sx, &sy, &sw,
			                              &sh)) {
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
	// The capture fires on app frames: the app class's image.
	*out_image = c->woven ? st->crop[SEG_CLASS_APP].image : VK_NULL_HANDLE;
	*out_w = c->w;
	*out_h = c->h;
	*out_screen_id = segs->screens[c->screen_index].id;
	*out_woven = c->woven;
	return true;
}


/*
 *
 * Multi-screen M3: per-segment views.
 *
 */

bool
comp_vk_native_segments_get_metrics(const struct comp_vk_native_segments *segs,
                                    const struct comp_seg_rect *window_desktop,
                                    const struct comp_seg_rect *canvas,
                                    bool primary_has_dp,
                                    struct xrt_segment_metrics *out)
{
	memset(out, 0, sizeof(*out));
	if (segs == NULL || !segs->enabled || !segs->logged_split || window_desktop == NULL || canvas == NULL) {
		return false;
	}
	const struct comp_segment_table *t = &segs->table;
	if (t->count == 0 || t->count > XRT_MAX_SEGMENTS) {
		// More screens than view sets: the window keeps one view set (M2).
		return false;
	}
	for (uint32_t k = 0; k < t->count; k++) {
		const struct comp_segment *g = &t->seg[k];
		if (g->screen_index >= segs->screen_count) {
			return false;
		}
		const uint32_t i = g->screen_index;
		const struct seg_screen_state *st = &segs->st[i];
		struct xrt_segment_metric *m = &out->seg[k];
		m->screen_id = g->screen_id;
		m->window_rect = (struct xrt_rect){{g->window_rect.x, g->window_rect.y},
		                                   {(int)g->window_rect.w, (int)g->window_rect.h}};
		m->screen_rect = (struct xrt_rect){{g->screen_rect.x, g->screen_rect.y},
		                                   {(int)g->screen_rect.w, (int)g->screen_rect.h}};
		m->screen_desktop_left = segs->screens[i].desktop.x;
		m->screen_desktop_top = segs->screens[i].desktop.y;
		m->screen_desktop_width = segs->screens[i].desktop.w;
		m->screen_desktop_height = segs->screens[i].desktop.h;
		m->screen_width_m = segs->info[i].width_m;
		m->screen_height_m = segs->info[i].height_m;
		m->nominal_viewer_x_m = segs->info[i].nominal_viewer_x_m;
		m->nominal_viewer_y_m = segs->info[i].nominal_viewer_y_m;
		m->nominal_viewer_z_m = segs->info[i].nominal_viewer_z_m;
		m->is_primary = g->is_primary;
		if (g->is_primary) {
			m->has_dp = primary_has_dp;
			m->tolerates_resample = false; // the primary keeps the session-level gates
			m->woven = primary_has_dp;
		} else {
			m->has_dp = st->dp != NULL;
			m->tolerates_resample = st->tolerates_resample;
			m->woven = comp_segments_decide(m->has_dp, st->tolerates_resample, g->screen_1to1) ==
			           COMP_SEG_RENDER_WEAVE;
		}
	}
	out->count = t->count;
	out->canvas = (struct xrt_rect){{canvas->x, canvas->y}, {(int)canvas->w, (int)canvas->h}};
	out->window_screen_left = window_desktop->x;
	out->window_screen_top = window_desktop->y;
	out->window_pixel_width = window_desktop->w;
	out->window_pixel_height = window_desktop->h;
	return true;
}

bool
comp_vk_native_segments_get_eyes(struct comp_vk_native_segments *segs,
                                 uint64_t screen_id,
                                 struct xrt_eye_positions *out)
{
	if (segs == NULL || out == NULL) {
		return false;
	}
	const int i = screen_index_of(segs, screen_id);
	if (i < 0) {
		return false;
	}
	bool ok = false;
	os_mutex_lock(&segs->dp_mutex);
	if (segs->st[i].dp != NULL) {
		ok = xrt_display_processor_get_predicted_eye_positions(segs->st[i].dp, out) && out->valid;
	}
	os_mutex_unlock(&segs->dp_mutex);
	return ok;
}

void
comp_vk_native_segments_set_display_mode(struct comp_vk_native_segments *segs, bool enable_3d)
{
	if (segs == NULL) {
		return;
	}
	const int want = enable_3d ? 1 : 0;
	segs->display_mode = want;
	for (uint32_t i = 0; i < segs->screen_count; i++) {
		struct seg_screen_state *st = &segs->st[i];
		if (st->dp == NULL || st->mode_sent == want) {
			continue;
		}
		st->mode_sent = want;
		// A lifecycle event per DP (create, or a session 2D/3D switch).
		const bool ok = xrt_display_processor_request_display_mode(st->dp, enable_3d);
		U_LOG_I("segments: screen 0x%016llx ('%s') DP follows the session mode: %s%s",
		        (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name, enable_3d ? "3D" : "2D",
		        ok ? "" : " (this DP has no 2D/3D switch — nothing to do)");
	}
}
